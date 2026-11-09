#!/usr/bin/env python3
"""Finite write footprints: fixed machine inputs and independent full-state UC.

--self-test uses only Python. --oracle executes the five fixtures, with no
Dragon analysis or LLVM execution; the shared Guest adapter imports Dragon
only as a dependency. --analyze is a separate read-only public finite-values
check against the frozen reference. No observed target becomes a Graph hint.

Unchanged finite_dispatch_diff.py can execute the four x86 cases with --case
for each native_case_ids() entry and --trials 16. The A64 STR Q0 case exercises
a real 128-bit STORE through full-state CPU and finite queries only: the
existing finite-dispatch Pass explicitly supports x86 CFG recovery.
"""
from __future__ import annotations

import argparse
import collections
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / 'tests/fixtures/finite-write-footprints.json'
ENTRY, TARGET, TABLE, DEST, SENTINEL = 0x1000, 0x1100, 0x3000, 0x4000, 0xf000
TRIALS = 16
UC_SHA = 'c1c8ec79f8ac7f0e92614e30c84d9ab2bbec8d79662aa6e3c3ea1265ce2d415d'
EDGES = (0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0x100000000,
         0x8000000000000000, 0xffffffffffffffff)


def sha(data): return hashlib.sha256(data).hexdigest()
def file_sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for part in iter(lambda: stream.read(1048576), b''): digest.update(part)
    return digest.hexdigest()
def load(path): return json.loads(Path(path).read_text())
def save(path, value): Path(path).write_text(json.dumps(value, indent=2) + '\n')
def mask(width): return (1 << width) - 1


def native_case_ids():
    return [arch + '_' + shape for arch in ('x86_32', 'x86_64')
            for shape in ('snapshot_masked_store', 'masked_store_only')]


def make_fixtures():
    cases = []
    for arch in ('x86_32', 'x86_64'):
        wide = arch == 'x86_64'
        width = 64 if wide else 32
        for shape in ('snapshot_masked_store', 'masked_store_only'):
            code = bytearray(b'\xcc' * 0x101)
            # A Guest STORE establishes the dispatcher cell. Initial image or
            # CPU-observed contents are never supplied as analysis evidence.
            body = bytearray(bytes.fromhex('48c704250030000000110000' if wide
                                           else 'c7050030000000110000'))
            body += bytes.fromhex('89f8')  # MOV EAX,EDI zero-extends in x64.
            if shape == 'snapshot_masked_store':
                body += bytes.fromhex('83c80729f8')  # OR EAX,7; SUB EAX,EDI.
            else:
                # ADD defines AF after AND, so no flag exclusion is needed.
                body += bytes.fromhex('83e00783c000')
            store_pc = ENTRY + len(body)
            body += bytes.fromhex('891485') + struct.pack('<I', DEST)
            displacement_offset = store_pc - ENTRY + 3
            jump_pc = ENTRY + len(body)
            body += bytes.fromhex('ff242500300000' if wide else 'ff2500300000')
            code[:len(body)] = body
            code[TARGET - ENTRY] = 0xc3
            # First eight are literal architectural boundaries. The next
            # eight cover every actual index while retaining high input bits.
            source = [v & mask(width) for v in EDGES]
            source += [((EDGES[i] & ~7) | (i if shape == 'masked_store_only'
                                               else i ^ 7)) & mask(width)
                       for i in range(8)]
            data = [EDGES[(i + 2) % 8] & mask(width) for i in range(TRIALS)]
            names = ('RDI', 'RDX') if wide else ('EDI', 'EDX')
            cases.append(dict(id=arch + '_' + shape, architecture=arch,
                code_hex=bytes(code).hex(), undefined_flags=[], stubs=[], local_functions=[],
                indirect_site=jump_pc, expected_targets=[TARGET],
                set_gprs={names[0]: source, names[1]: data},
                finite_write_footprints=dict(shape=shape, required_trials=TRIALS,
                    native_finite_dispatch=True, table_address=TABLE,
                    table_width=8 if wide else 4, target=TARGET,
                    variable_store_pc=store_pc, variable_store_width=4,
                    variable_store_displacement_offset=displacement_offset,
                    possible_written_bytes=[DEST, DEST + 31],
                    input_registers=list(names),
                    flags='Final SUB or ADD defines all arithmetic flags; DF is preserved.')))
    # MOVZ X0,#1100; MOVZ X1,#3000; STR X0,[X1]; MOVZ X2,#4000;
    # STR Q0,[X2]; LDR X3,[X1]; BR X3. Q0 supplies all 16 stored bytes.
    words = (0xd2822000, 0xd2860001, 0xf9000020, 0xd2880002,
             0x3d800040, 0xf9400023, 0xd61f0060)
    code = bytearray(struct.pack('<I', 0xd4200000) * 65)
    code[:4 * len(words)] = struct.pack('<' + 'I' * len(words), *words)
    code[TARGET - ENTRY:TARGET - ENTRY + 4] = struct.pack('<I', 0xd65f03c0)
    cases.append(dict(id='arm64_wide_store', architecture='arm64', code_hex=bytes(code).hex(),
        undefined_flags=[], stubs=[], local_functions=[], indirect_site=ENTRY + 24,
        expected_targets=[TARGET], set_gprs={},
        finite_write_footprints=dict(shape='wide_store', required_trials=TRIALS,
            native_finite_dispatch=False, table_address=TABLE, table_width=8, target=TARGET,
            variable_store_pc=ENTRY + 16, variable_store_width=16,
            possible_written_bytes=[DEST, DEST + 15],
            flags='No instruction changes NZCV; the real RET preserves SP.',
            native_limit='FiniteDispatchPass currently supports x86 only; no A64 native recovery claim.')))
    return dict(schema=1, entry=ENTRY, cases=cases,
        scope='Guest-established table cell, precise disjoint STORE footprints; no image-memory contract or observed CFG hints.')


def index_for(fixture, source):
    source &= mask(32)
    shape = fixture['finite_write_footprints']['shape']
    return ((source | 7) - source) & mask(32) if shape == 'snapshot_masked_store' else source & 7


def validate(document):
    if document != make_fixtures():
        raise ValueError('fixture bytes, inputs, target contract or exclusions changed')
    for fixture in document['cases']:
        meta = fixture['finite_write_footprints']
        if meta['native_finite_dispatch']:
            indices = {index_for(fixture, v) for v in fixture['set_gprs'][meta['input_registers'][0]]}
            if indices != set(range(8)): raise ValueError('actual profiles do not cover every index')
            if not (DEST + 31 < TABLE or TABLE + meta['table_width'] <= DEST):
                raise ValueError('fixture is not disjoint from its dispatcher cell')
    return document


def edge_errors(published, expected, observed):
    errors = []
    if set(published) != set(expected): errors.append('inexact-target-set')
    if any(target not in published for target in observed): errors.append('missing-executed-target')
    return errors


def compare_set(value, complete, expect_unknown=False):
    if type(complete) is not bool or not complete:
        raise ValueError('tiny finite analysis must complete within the default budget')
    expected_kind, expected_values = (2, []) if expect_unknown else (1, [TARGET])
    if type(value['kind']) is not int or type(value['values']) is not list or \
            any(type(v) is not int for v in value['values']) or \
            value['kind'] != expected_kind or value['values'] != expected_values:
        raise ValueError('finite target query differs from the declared proof contract')


def alias_code(fixture):
    code = bytearray.fromhex(fixture['code_hex'])
    meta = fixture['finite_write_footprints']
    if fixture['architecture'] == 'arm64':
        at = meta['variable_store_pc'] - ENTRY
        code[at:at + 4] = struct.pack('<I', 0x3d800020)  # STR Q0,[X1], over table.
    else:
        at = meta['variable_store_displacement_offset']
        code[at:at + 4] = struct.pack('<I', TABLE)
    return bytes(code)


def self_test(path):
    document = validate(load(path))
    checks = dict(five_fixed_cases=len(document['cases']) == 5,
                  four_native_cases=native_case_ids() == [f['id'] for f in document['cases'][:4]],
                  all_eight_indices=True, no_exclusions=all(not f['undefined_flags'] for f in document['cases']))
    for name, edit in (
        ('removed_target', lambda d: d['cases'][0]['expected_targets'].clear()),
        ('changed_code', lambda d: d['cases'][0].__setitem__('code_hex', 'c3')),
        ('hidden_AF_exclusion', lambda d: d['cases'][0]['undefined_flags'].append('AF')),
        ('changed_input', lambda d: d['cases'][0]['set_gprs']['EDI'].__setitem__(0, 9)),
        ('false_A64_native_claim', lambda d: d['cases'][-1]['finite_write_footprints'].__setitem__('native_finite_dispatch', True))):
        bad = copy.deepcopy(document); edit(bad)
        try: validate(bad)
        except ValueError: checks[name + '_rejected'] = True
        else: checks[name + '_rejected'] = False
    for name, value, complete in (
        ('wrong', dict(kind=1, values=[TARGET + 1]), True),
        ('empty', dict(kind=1, values=[]), True),
        ('unknown', dict(kind=2, values=[]), True),
        ('partial', dict(kind=1, values=[TARGET]), False),
        ('boolean_kind', dict(kind=True, values=[TARGET]), True),
        ('boolean_complete', dict(kind=1, values=[TARGET]), 1)):
        try: compare_set(value, complete)
        except ValueError: checks[name + '_rejected'] = True
        else: checks[name + '_rejected'] = False
    checks['missing_edge_rejected'] = bool(edge_errors([], [TARGET], [TARGET]))
    checks['exact_edge_accepted'] = not edge_errors([TARGET], [TARGET], [TARGET])
    checks['alias_changes_real_store'] = all(alias_code(f) != bytes.fromhex(f['code_hex']) for f in document['cases'])
    checks['snapshot_scalar_identity'] = all(((x | 7) - x) & mask(32) == ((~x) & 7)
                                               for x in [*range(256), *EDGES])
    if not all(checks.values()): raise RuntimeError(repr(checks))
    return dict(ok=True, checks=checks, native_cases=native_case_ids(),
                planned_full_CPU_returns=len(document['cases']) * TRIALS,
                CPU_Graph_LLVM_executions=0)


def provenance(expected_uc_sha):
    import unicorn
    import dragon
    maps = Path('/proc/self/maps').read_text().splitlines()
    uc_rows = [r for r in maps if '/libunicorn.so' in r]
    if not uc_rows or any('(deleted)' in r for r in uc_rows):
        raise RuntimeError('live Unicorn library mapping is missing or deleted')
    uc_paths = {str(Path(r.split()[-1]).resolve()) for r in uc_rows}
    if len(uc_paths) != 1 or any(file_sha(p) != expected_uc_sha for p in uc_paths):
        raise RuntimeError('actual Unicorn DSO differs from the independent expected SHA')
    dragon_rows = [r for r in maps if '/libdragon' in r]
    if any('(deleted)' in r for r in dragon_rows): raise RuntimeError('import-only Dragon DSO was deleted')
    dragon_paths = {str(Path(r.split()[-1]).resolve()) for r in dragon_rows}
    if str(Path(dragon._LIB).resolve()) not in dragon_paths:
        raise RuntimeError('wrapper does not identify its actually mapped import-only C shim')
    return dict(interpreter=str(Path(sys.executable).resolve()),
                interpreter_sha256=file_sha(sys.executable), unicorn_version=unicorn.__version__,
                unicorn_maps=uc_rows, unicorn_libraries={p: file_sha(p) for p in sorted(uc_paths)},
                dragon_import_only_maps=dragon_rows,
                dragon_import_only_libraries={p: file_sha(p) for p in sorted(dragon_paths)},
                python_wrapper=str(Path(dragon.__file__).resolve()),
                python_wrapper_sha256=file_sha(dragon.__file__), Dragon_analysis_calls=0)


def flags32(a, b, result, subtract):
    sign = 1 << 31
    overflow = ((a ^ b) if subtract else ~(a ^ b)) & (a ^ result) & sign
    return (int(a < b) if subtract else int(a + b > mask(32)),
            int((result & 255).bit_count() % 2 == 0), int(bool((a ^ b ^ result) & 16)),
            int(result == 0), int(bool(result & sign)), int(bool(overflow)))


def model(adapter, fixture, regs, memory):
    """Sequential machine specification, independent of finite-set algorithms."""
    import function_guest_diff as guest
    out, mem = bytearray(regs), bytearray(memory)
    meta = fixture['finite_write_footprints']
    if mem[ENTRY:ENTRY + len(bytes.fromhex(fixture['code_hex']))] != bytes.fromhex(fixture['code_hex']):
        raise ValueError('CPU/model initial code differs from frozen fixture')
    guest.base.put(mem, TABLE, TARGET, meta['table_width'])
    if adapter.a64:
        for offset, value in ((0, TARGET), (8, TABLE), (16, DEST), (24, TARGET)):
            guest.base.put(out, offset, value)
        mem[DEST:DEST + 16] = regs[272:288]
        guest.base.put(out, adapter.pc_offset, SENTINEL)
        index = None
    else:
        if adapter.x32:
            for unused, offset, width, unused_reg in adapter.gprs:
                guest.base.put(out, offset, guest.base.get(regs, offset, width))
        source = guest.base.get(regs, 56, 4)
        if meta['shape'] == 'snapshot_masked_store':
            left, right = source | 7, source
            index = (left - right) & mask(32)
            flags = flags32(left, right, index, True)
        else:
            left, right = source & 7, 0
            index = (left + right) & mask(32)
            flags = flags32(left, right, index, False)
        guest.base.put(out, 0, index)
        for offset, value in zip((136, 137, 138, 139, 140, 141), flags): out[offset] = value
        guest.base.put(mem, DEST + 4 * index, guest.base.get(regs, 16, 4), 4)
        pointer_width = 4 if adapter.x32 else 8
        guest.base.put(out, 32, (guest.base.get(regs, 32, pointer_width) + pointer_width) & mask(pointer_width * 8))
        guest.base.put(out, adapter.pc_offset, SENTINEL)
    target = guest.base.get(mem, TABLE, meta['table_width'])
    if target != TARGET: raise ValueError('sequential model overwrote the dispatcher cell')
    return (bytes(out), bytes(mem), collections.Counter(),
            (1, 3, target, SENTINEL, 1, 0, 0)), index


def oracle(path, output, expected_uc_sha):
    if os.environ.get('DRAGON_TEST_PYTHON'): sys.path.insert(0, os.environ['DRAGON_TEST_PYTHON'])
    import function_guest_diff as guest
    document = validate(load(path))
    dependencies = {str(p): file_sha(p) for p in [Path(__file__), path,
        *[ROOT / 'scripts/validation' / name for name in (
            'function_guest_diff.py', 'function_dispatch_diff.py', 'function_llvm_diff.py', 'ir_unreachable.py')]]}
    before = provenance(expected_uc_sha)
    output.mkdir(parents=True, exist_ok=False)
    rows, controls = [], collections.Counter()
    for fixture in document['cases']:
        adapter = guest.Adapter(fixture['architecture'])
        for trial in range(TRIALS):
            regs, memory = adapter.initial(fixture, trial, ENTRY)
            expected, index = model(adapter, fixture, regs, memory)
            actual = adapter.reference(fixture, regs, memory, ENTRY)
            packed = struct.pack('<8Q', *actual[3], 0)
            differences, unused = adapter.compare(fixture, expected, (*actual[:3], packed), False)
            if differences: raise RuntimeError(json.dumps(dict(case=fixture['id'], trial=trial, differences=differences)))
            if actual[1][ENTRY:ENTRY + len(bytes.fromhex(fixture['code_hex']))] != bytes.fromhex(fixture['code_hex']):
                raise RuntimeError('actual CPU modified frozen code')
            work = output / 'snapshots' / fixture['id'] / str(trial)
            work.mkdir(parents=True)
            snapshots = {}
            for name, data in (('initial-registers.bin', regs), ('initial-memory.bin', memory),
                               ('final-registers.bin', actual[0]), ('final-memory.bin', actual[1])):
                (work / name).write_bytes(data)
                snapshots[name] = dict(path=str((work / name).relative_to(output)), size=len(data), sha256=sha(data))
            for kind in ('result', 'memory', 'NF' if adapter.a64 else 'DF'):
                bad_regs, bad_mem = bytearray(actual[0]), bytearray(actual[1])
                if kind == 'result':
                    bad_regs[0] ^= 1
                    field = 'X0' if adapter.a64 else 'EAX' if adapter.x32 else 'RAX'
                elif kind == 'memory': bad_mem[0x8000] ^= 1; field = 'memory'
                else: bad_regs[784 if adapter.a64 else 142] ^= 1; field = kind
                bad, unused = adapter.compare(fixture, expected, (bytes(bad_regs), bytes(bad_mem), actual[2], packed), False)
                if len(bad) != 1 or bad[0]['field'] != field or kind == 'memory' and bad[0]['count'] != 1:
                    raise RuntimeError('single-field reference control did not detect exactly its intended corruption')
                controls[kind] += 1
            if edge_errors(fixture['expected_targets'], [TARGET], [actual[3][2]]) or not edge_errors([], [TARGET], [actual[3][2]]):
                raise RuntimeError('missing/empty target publication escaped edge control')
            controls['edge'] += 1
            rows.append(dict(case=fixture['id'], trial=trial, index=index, complete=True,
                target=actual[3][2], exit=actual[3], modeled_fields=len(adapter.fields),
                memory_bytes=65536, exclusions=0, normalizations=0, snapshots=snapshots))
    after = provenance(expected_uc_sha)
    if before != after or any(file_sha(p) != value for p, value in dependencies.items()):
        raise RuntimeError('reference dependency or actually mapped library changed during execution')
    for fixture in document['cases'][:4]:
        if {r['index'] for r in rows if r['case'] == fixture['id']} != set(range(8)):
            raise RuntimeError('CPU profiles did not exercise all eight actual store indices')
    result = dict(schema=1, fixture_sha256=file_sha(path), script_sha256=file_sha(__file__),
        dependencies=dependencies, provenance_before=before, provenance_after=after,
        rows=rows, full_CPU_returns=len(rows), controls=dict(controls),
        modeled_fields=sum(r['modeled_fields'] for r in rows), memory_bytes=sum(r['memory_bytes'] for r in rows),
        skips=0, exclusions=0, normalizations=0, Graph_LLVM_executions=0)
    save(output / 'oracle.json', result)
    return {key: result[key] for key in ('full_CPU_returns', 'controls', 'modeled_fields', 'memory_bytes', 'skips', 'exclusions', 'normalizations', 'Graph_LLVM_executions')}


def validate_cpu(cpu, evidence, document, path):
    if cpu['fixture_sha256'] != file_sha(path) or cpu['script_sha256'] != file_sha(__file__):
        raise ValueError('CPU evidence was generated from different fixture/script bytes')
    if any(type(r['trial']) is not int or type(r['case']) is not str for r in cpu['rows']):
        raise ValueError('CPU case/trial fields are not strict scalar keys')
    keys = [(r['case'], r['trial']) for r in cpu['rows']]
    expected = {(f['id'], trial) for f in document['cases'] for trial in range(TRIALS)}
    if len(keys) != len(expected) or set(keys) != expected or cpu['full_CPU_returns'] != len(expected):
        raise ValueError('CPU evidence has missing or duplicated actual input profiles')
    fixtures = {f['id']: f for f in document['cases']}
    controls = dict(result=80, memory=80, DF=64, NF=16, edge=80)
    if cpu['controls'] != controls or any(type(v) is not int for v in cpu['controls'].values()):
        raise ValueError('CPU single-field/edge controls are incomplete')
    for name in ('skips', 'exclusions', 'normalizations', 'Graph_LLVM_executions'):
        if type(cpu[name]) is not int or cpu[name] != 0: raise ValueError('CPU reference scope differs')
    for row in cpu['rows']:
        fixture = fixtures[row['case']]
        arch = fixture['architecture']
        if row['complete'] is not True or row['target'] != TARGET or row['exit'] != [1, 3, TARGET, SENTINEL, 1, 0, 0]:
            raise ValueError('CPU reference is not a complete real root return')
        fields = {'x86_32': 52, 'x86_64': 76, 'arm64': 102}[arch]
        if row['modeled_fields'] != fields or row['memory_bytes'] != 65536 or row['exclusions'] != 0 or row['normalizations'] != 0:
            raise ValueError('CPU reference reduced its state or memory comparison scope')
        if arch == 'arm64': expected_index = None
        else:
            name = fixture['finite_write_footprints']['input_registers'][0]
            expected_index = index_for(fixture, fixture['set_gprs'][name][row['trial']])
        if row['index'] != expected_index: raise ValueError('CPU evidence input/index pairing differs')
        names = {'initial-registers.bin', 'initial-memory.bin', 'final-registers.bin', 'final-memory.bin'}
        if set(row['snapshots']) != names: raise ValueError('CPU raw snapshot set is incomplete')
        for name, item in row['snapshots'].items():
            relative = Path(item['path'])
            if relative.is_absolute() or '..' in relative.parts: raise ValueError('unsafe snapshot path')
            expected_path = Path('snapshots') / row['case'] / str(row['trial']) / name
            expected_size = 65536 if name.endswith('memory.bin') else 800 if arch == 'arm64' else 512
            if relative != expected_path or type(item['size']) is not int or item['size'] != expected_size:
                raise ValueError('CPU snapshot identity or complete byte extent differs')
            actual = evidence / relative
            if actual.stat().st_size != item['size'] or file_sha(actual) != item['sha256']:
                raise ValueError('CPU raw snapshot bytes differ from the frozen reference')


def analyze(path, output, evidence, pin_path, expect_old):
    sys.path.insert(0, os.environ.get('DRAGON_TEST_PYTHON', str(ROOT / 'python')))
    import dragon
    import caller_context_diff as context
    document = validate(load(path)); cpu = load(evidence / 'oracle.json')
    validate_cpu(cpu, evidence, document, path)
    pin = load(pin_path); context.actual_maps(dragon, pin)
    output.mkdir(parents=True, exist_ok=False); rows = []
    for fixture in document['cases']:
        for alias in (False, True):
            code = alias_code(fixture) if alias else bytes.fromhex(fixture['code_hex'])
            for strategy in (0, 1):
                arch = {'x86_32': 1, 'x86_64': 2, 'arm64': 4}[fixture['architecture']]
                with dragon.Disassembler(arch, dragon.Configuration(strategy=strategy)) as dis:
                    with dis.analyze(code, ENTRY) as graph:
                        sites = []
                        for at in graph.block_addresses():
                            block = graph.get_ignil_block(at)
                            if block and block.ops() and block.ops()[-1][:2] == (fixture['indirect_site'], dragon.IGNILOpcode.JMP):
                                sites.append(at)
                        if len(sites) != 1: raise RuntimeError('not exactly one actual dispatcher query block')
                        for calls in (False, True):
                            before = context.graph_fingerprint(graph)
                            result = graph.finite_values(entry_points=[ENTRY], prove_leaf_calls=calls)
                            value = result['jump_targets'][sites[0]]
                            compare_set(value, result['complete'], expect_unknown=alias or expect_old)
                            after = context.graph_fingerprint(graph)
                            if before != after: raise RuntimeError('read-only finite query mutated Graph')
                            rows.append(dict(case=fixture['id'], alias_control=alias, strategy=strategy,
                                prove_leaf_calls=calls, result=result, code_sha256=sha(code),
                                graph_before=before, graph_after=after, actual_maps=context.actual_maps(dragon, pin),
                                scope='Read-only finite target facts; no CPU target/seed/table callback is supplied.'))
    save(output / 'analysis.json', dict(schema=1, rows=rows, expect_old=expect_old,
        script_sha256=file_sha(__file__), fixture_sha256=file_sha(path),
        oracle_sha256=file_sha(evidence / 'oracle.json'), libraries=pin, CPU_LLVM_executions=0))
    return dict(read_only_reports=len(rows), positive_reports=sum(not r['alias_control'] for r in rows),
                alias_control_reports=sum(r['alias_control'] for r in rows),
                expected_positive_kind='Unknown' if expect_old else 'Finite', CPU_LLVM_executions=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path, default=DEFAULT)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument('--self-test', action='store_true')
    actions.add_argument('--oracle', type=Path)
    actions.add_argument('--analyze', type=Path)
    parser.add_argument('--oracle-evidence', type=Path)
    parser.add_argument('--library-pin', type=Path)
    parser.add_argument('--expected-unicorn-sha256', default=UC_SHA)
    parser.add_argument('--expect-old', action='store_true')
    args = parser.parse_args()
    if args.self_test: result = self_test(args.fixtures)
    elif args.oracle:
        if args.expect_old: parser.error('--expect-old applies only to analysis')
        result = oracle(args.fixtures, args.oracle, args.expected_unicorn_sha256)
    else:
        if not args.oracle_evidence or not args.library_pin:
            parser.error('--analyze needs frozen --oracle-evidence and --library-pin')
        result = analyze(args.fixtures, args.analyze, args.oracle_evidence, args.library_pin, args.expect_old)
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
