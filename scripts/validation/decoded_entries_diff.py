#!/usr/bin/env python3
"""Decoded-entry fixtures: complete roots and separate real target-zero boundaries.

--self-test is pure Python. --oracle uses the unchanged Guest Adapter to run
64 complete Unicorn roots and 64 explicit transfer boundaries, never Dragon
analysis or LLVM. Boundary observations are not root returns or CFG hints.
Unchanged finite_dispatch_diff.py can use this fixture file with --trials 16.
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
DEFAULT = ROOT / 'tests/fixtures/decoded-entries.json'
ENTRY, DIRECT, CONTINUATION = 0x1000, 0x1007, 0x100c
TABLE, TARGET, SENTINEL, STACK = 0x3000, 0x1100, 0xf000, 0xc800
TRIALS, MEMORY_SIZE, REGS_SIZE = 16, 65536, 512
UC_SHA = 'c1c8ec79f8ac7f0e92614e30c84d9ab2bbec8d79662aa6e3c3ea1265ce2d415d'


def sha(data): return hashlib.sha256(data).hexdigest()
def file_sha(path): return sha(Path(path).read_bytes())
def encoded(value): return json.dumps(value, sort_keys=True, separators=(',', ':'))
def save(path, value): Path(path).write_text(json.dumps(value, indent=2) + '\n')
def get(buf, offset, size=8): return int.from_bytes(buf[offset:offset + size], 'little')
def put(buf, offset, value, size=8): buf[offset:offset + size] = value.to_bytes(size, 'little')


def make_fixtures():
    cases = []
    for arch in ('x86_32', 'x86_64'):
        width = 4 if arch == 'x86_32' else 8
        for call in (True, False):
            # XOR; ADD 0 defines AF too; JE skips the real external transfer.
            body = bytes.fromhex('31c083c0007405')
            body += bytes([0xe8 if call else 0xe9]) + struct.pack('<i', -CONTINUATION)
            body += bytes.fromhex('c7050030000000110000' if width == 4
                                  else '48c704250030000000110000')
            site = ENTRY + len(body)
            body += bytes.fromhex('ff2500300000' if width == 4 else 'ff242500300000')
            code = bytearray(b'\xcc' * 0x101)
            code[:len(body)] = body
            code[TARGET - ENTRY] = 0xc3
            cases.append(dict(id=arch + ('_call0' if call else '_jmp0'),
                architecture=arch, code_hex=bytes(code).hex(), undefined_flags=[],
                stubs=[], local_functions=[], set_gprs={}, indirect_site=site,
                expected_targets=[TARGET], decoded_entries=dict(required_trials=TRIALS,
                    direct_site=DIRECT, direct_target=0, boundary_kind=2 if call else 1,
                    continuation=CONTINUATION, table_address=TABLE, table_width=width,
                    root_target=TARGET, flags='ADD EAX,0 defines CF/PF/AF/ZF/SF/OF; DF is preserved.')))
    return dict(schema=1, entry=ENTRY, cases=cases,
        scope='Real CALL/JMP to zero stays decoded evidence; complete roots skip it. '
              'Separate CPU-only boundary observations never become Graph hints.')


def validate(document):
    # Canonical JSON distinguishes booleans from integers as well as bytes,
    # exclusions and the complete fixture domain.
    if encoded(document) != encoded(make_fixtures()):
        raise ValueError('fixture bytes, trial domain, boundaries or exclusions changed')
    return document


def model(fixture, regs, memory, boundary=False):
    """Literal machine specification, independent of Dragon and Unicorn."""
    if len(regs) != REGS_SIZE or len(memory) != MEMORY_SIZE:
        raise ValueError('incomplete initial machine state')
    code = bytes.fromhex(fixture['code_hex'])
    if memory[ENTRY:ENTRY + len(code)] != code:
        raise ValueError('initial machine bytes differ from the fixture')
    if get(regs, 128) != (DIRECT if boundary else ENTRY):
        raise ValueError('initial PC differs from the requested observation')
    width = fixture['decoded_entries']['table_width']
    out, mem = bytearray(regs), bytearray(memory)
    if width == 4:
        for offset in range(0, 64, 8): put(out, offset, get(regs, offset, 4))
    sp = get(regs, 32, width)
    if sp < width or sp + width > len(mem): raise ValueError('stack outside comparison memory')
    if boundary:
        call = fixture['decoded_entries']['boundary_kind'] == 2
        if call:
            put(out, 32, sp - width)
            put(mem, sp - width, CONTINUATION, width)
        put(out, 128, 0)
        exit_record = (3, 2 if call else 1, DIRECT, 0, 1,
                       CONTINUATION if call else 0, 0)
    else:
        if get(mem, sp, width) != SENTINEL:
            raise ValueError('root RET destination is not the declared sentinel')
        put(out, 0, 0)  # XOR EAX,EAX clears the x64 parent as well.
        # ADD 0+0: all six arithmetic flags are defined, including AF.
        for offset, value in ((136, 0), (137, 1), (138, 0),
                              (139, 1), (140, 0), (141, 0)):
            out[offset] = value
        put(mem, TABLE, TARGET, width)
        put(out, 32, sp + width)
        put(out, 128, SENTINEL)
        exit_record = (1, 3, TARGET, SENTINEL, 1, 0, 0)
    return bytes(out), bytes(mem), collections.Counter(), exit_record


def check_trace(fixture, trace, boundary):
    if boundary:
        if trace != [DIRECT, 0]: raise ValueError('real transfer did not stop exactly at zero')
        return
    site = fixture['indirect_site']
    if trace[:4] != [ENTRY, ENTRY + 2, ENTRY + 5, CONTINUATION] or DIRECT in trace:
        raise ValueError('complete root failed to skip its external transfer')
    targets = [b for a, b in zip(trace, trace[1:]) if a == site]
    if targets != [TARGET]: raise ValueError('observed indirect transfer differs from the single target')


def corrupt(actual, kind):
    regs, memory = bytearray(actual[0]), bytearray(actual[1])
    if kind == 'result': regs[0] ^= 1
    elif kind == 'memory': memory[0x8000] ^= 1
    elif kind == 'AF': regs[138] ^= 1
    else: raise ValueError('unknown single-field control')
    return bytes(regs), bytes(memory)


def check_control(differences, field):
    if len(differences) != 1 or differences[0]['field'] != field or \
            field == 'memory' and differences[0].get('count') != 1:
        raise ValueError('single-field control failed to detect exactly its intended corruption')


def self_test(path):
    document = validate(json.loads(path.read_text()))
    checks = dict(four_cases=len(document['cases']) == 4,
                  no_exclusions=all(f['undefined_flags'] == [] for f in document['cases']))
    for name, edit in (
        ('code', lambda d: d['cases'][0].__setitem__('code_hex', 'c3')),
        ('AF_exclusion', lambda d: d['cases'][0]['undefined_flags'].append('AF')),
        ('target', lambda d: d['cases'][0].__setitem__('expected_targets', [0])),
        ('trials', lambda d: d['cases'][0]['decoded_entries'].__setitem__('required_trials', 8)),
        ('hidden_stub', lambda d: d['cases'][0]['stubs'].append(dict(address=0, code_hex='c3'))),
        ('typed_schema', lambda d: d.__setitem__('schema', True))):
        bad = copy.deepcopy(document); edit(bad)
        try: validate(bad)
        except ValueError: checks[name + '_rejected'] = True
        else: checks[name + '_rejected'] = False
    for fixture in document['cases']:
        meta, code = fixture['decoded_entries'], bytes.fromhex(fixture['code_hex'])
        prefix = fixture['id'] + '_'
        checks[prefix + 'machine_layout'] = code[:7] == bytes.fromhex('31c083c0007405') and \
            code[7] == (0xe8 if meta['boundary_kind'] == 2 else 0xe9) and \
            CONTINUATION + struct.unpack('<i', code[8:12])[0] == 0 and \
            code[TARGET - ENTRY:] == b'\xc3' and \
            code[meta['continuation'] - ENTRY:fixture['indirect_site'] - ENTRY] == \
            bytes.fromhex('c7050030000000110000' if meta['table_width'] == 4
                          else '48c704250030000000110000')
        trace = [ENTRY, ENTRY + 2, ENTRY + 5, CONTINUATION, fixture['indirect_site'], TARGET]
        check_trace(fixture, trace, False); check_trace(fixture, [DIRECT, 0], True)
        for boundary in (False, True):
            bad_trace = [DIRECT, 1] if boundary else trace[:-1] + [TARGET + 1]
            try: check_trace(fixture, bad_trace, boundary)
            except ValueError: checks[prefix + ('boundary' if boundary else 'root') + '_wrong_target_rejected'] = True
            else: checks[prefix + ('boundary' if boundary else 'root') + '_wrong_target_rejected'] = False
        for df in (0, 1):
            regs, memory = bytearray(REGS_SIZE), bytearray(MEMORY_SIZE)
            put(regs, 0, 0xffffffffffffffff); put(regs, 32, STACK); put(regs, 128, ENTRY)
            regs[138], regs[142] = 1, df
            memory[ENTRY:ENTRY + len(code)] = code
            put(memory, STACK, SENTINEL, meta['table_width'])
            expected = model(fixture, regs, memory)
            checks[prefix + f'root_AF_DF_{df}'] = expected[0][138] == 0 and \
                expected[0][142] == df and get(expected[0], 0) == 0 and \
                get(expected[1], TABLE, meta['table_width']) == TARGET and \
                expected[3] == (1, 3, TARGET, SENTINEL, 1, 0, 0)
            put(regs, 128, DIRECT)
            boundary = model(fixture, regs, memory, True)
            call = meta['boundary_kind'] == 2
            checks[prefix + f'boundary_model_{df}'] = boundary[0][138] == 1 and \
                boundary[0][142] == df and get(boundary[0], 128) == 0 and \
                get(boundary[0], 32) == STACK - (meta['table_width'] if call else 0) and \
                boundary[3] == (3, meta['boundary_kind'], DIRECT, 0, 1, CONTINUATION if call else 0, 0) and \
                (get(boundary[1], STACK - meta['table_width'], meta['table_width']) == CONTINUATION
                 if call else boundary[1] == bytes(memory))
        for kind, field in (('result', 'EAX' if meta['table_width'] == 4 else 'RAX'),
                            ('memory', 'memory'), ('AF', 'AF')):
            damaged = corrupt(expected, kind)
            changes = [(i, a, b) for i, (a, b) in enumerate(zip(expected[0] + expected[1], damaged[0] + damaged[1])) if a != b]
            checks[prefix + kind + '_one_byte_control'] = len(changes) == 1 and changes[0][1] ^ changes[0][2] == 1
            check_control([dict(field=field, count=1)], field)
    for name, differences, field in (
        ('undetected', [], 'AF'), ('wrong_field', [dict(field='CF')], 'AF'),
        ('multiple_bytes', [dict(field='memory', count=2)], 'memory')):
        try: check_control(differences, field)
        except ValueError: checks[name + '_control_rejected'] = True
        else: checks[name + '_control_rejected'] = False
    if not all(checks.values()): raise RuntimeError(repr(checks))
    return dict(ok=True, checks=checks, pure_checks=len(checks),
        planned_full_CPU_returns=64, planned_CPU_boundaries=64, CPU_Graph_LLVM_executions=0)


def traced_reference(guest, adapter, fixture, regs, memory, entry):
    original, trace = guest.Uc, []
    def tracked(*args, **kwargs):
        uc = original(*args, **kwargs)
        uc.hook_add(guest.UC_HOOK_CODE,
                    lambda u, pc, size, data: trace.append(u.reg_read(adapter.pc_reg)))
        return uc
    guest.Uc = tracked
    try: actual = adapter.reference(fixture, regs, memory, entry)
    finally: guest.Uc = original
    return actual, trace


def oracle(path, output, expected_uc_sha):
    if os.environ.get('DRAGON_TEST_PYTHON'):
        sys.path.insert(0, os.environ['DRAGON_TEST_PYTHON'])
        import dragon  # Pin the requested import-only wrapper before shared imports.
    import function_guest_diff as guest
    import finite_write_footprints_diff as prior
    document = validate(json.loads(path.read_text()))
    sources = [Path(__file__).resolve(), path.resolve(), *[ROOT / 'scripts/validation' / n for n in
        ('function_guest_diff.py', 'function_dispatch_diff.py', 'function_llvm_diff.py',
         'finite_write_footprints_diff.py', 'ir_unreachable.py')]]
    dependencies = {str(p): file_sha(p) for p in sources}
    before = prior.provenance(expected_uc_sha)
    output.mkdir(parents=True, exist_ok=False)
    rows, controls = [], []
    for fixture in document['cases']:
        adapter = guest.Adapter(fixture['architecture'])
        if len(adapter.fields) != (52 if adapter.x32 else 76) or \
                not {'AF', 'DF', 'PC'}.issubset({n for n, _, _ in adapter.fields}):
            raise RuntimeError('unexpected complete Guest field layout')
        for trial in range(TRIALS):
            for boundary in (False, True):
                observed = copy.deepcopy(fixture)
                if boundary:
                    observed.update(missing_targets=[0], boundary_kind=fixture['decoded_entries']['boundary_kind'])
                regs, memory = adapter.initial(observed, trial, ENTRY)
                start = DIRECT if boundary else ENTRY
                put(regs, adapter.pc_offset, start)
                expected = model(fixture, regs, memory, boundary)
                actual, trace = traced_reference(guest, adapter, observed, regs, memory, start)
                if len(actual[0]) != REGS_SIZE or len(actual[1]) != MEMORY_SIZE:
                    raise RuntimeError('reference returned an incomplete machine state')
                packed = struct.pack('<8Q', *actual[3], 0)
                differences, unused = adapter.compare(observed, expected, (*actual[:3], packed), False)
                if differences: raise RuntimeError(json.dumps(dict(case=fixture['id'], trial=trial, boundary=boundary, differences=differences)))
                check_trace(fixture, trace, boundary)
                code = bytes.fromhex(fixture['code_hex'])
                if actual[1][ENTRY:ENTRY + len(code)] != code:
                    raise RuntimeError('CPU changed the frozen fixture code')
                role = 'boundary' if boundary else 'root'
                work = output / 'snapshots' / fixture['id'] / role / str(trial)
                work.mkdir(parents=True)
                snapshots = {}
                for name, data in (('initial-registers.bin', regs), ('initial-memory.bin', memory),
                    ('expected-registers.bin', expected[0]), ('expected-memory.bin', expected[1]),
                    ('actual-registers.bin', actual[0]), ('actual-memory.bin', actual[1])):
                    destination = work / name; destination.write_bytes(data)
                    snapshots[name] = dict(path=str(destination.relative_to(output)), size=len(data), sha256=sha(data))
                for kind in ('result', 'memory', 'AF'):
                    damaged = corrupt(actual, kind)
                    bad, unused = adapter.compare(observed, expected, (*damaged, actual[2], packed), False)
                    field = 'EAX' if adapter.x32 else 'RAX'
                    field = field if kind == 'result' else kind
                    check_control(bad, field)
                    controls.append(dict(case=fixture['id'], trial=trial, role=role, kind=kind, detected=True, differences=bad))
                bad_trace = list(trace)
                bad_trace[1 if boundary else bad_trace.index(fixture['indirect_site']) + 1] = 1 if boundary else TARGET + 1
                try: check_trace(fixture, bad_trace, boundary)
                except ValueError: pass
                else: raise RuntimeError('wrong-target control was not detected')
                controls.append(dict(case=fixture['id'], trial=trial, role=role, kind='target', detected=True, bad_trace=bad_trace))
                rows.append(dict(case=fixture['id'], trial=trial, role=role, status='match', trace=trace,
                    expected_exit=expected[3], actual_exit=actual[3], modeled_fields=len(adapter.fields),
                    memory_bytes=MEMORY_SIZE, exclusions=0, skips=0, normalizations=0,
                    initial_DF=regs[142], code_unchanged=True, snapshots=snapshots))
    after = prior.provenance(expected_uc_sha)
    if before != after or any(file_sha(p) != value for p, value in dependencies.items()):
        raise RuntimeError('source or actually mapped library changed during the oracle')
    domain = {(f['id'], t, r) for f in document['cases'] for t in range(TRIALS) for r in ('root', 'boundary')}
    if len(rows) != len(domain) or {(r['case'], r['trial'], r['role']) for r in rows} != domain:
        raise RuntimeError('incomplete or duplicated CPU domain')
    for fixture in document['cases']:
        if {r['initial_DF'] for r in rows if r['case'] == fixture['id']} != {0, 1}:
            raise RuntimeError('direction-flag inputs do not cover both values')
    counts = dict(collections.Counter(c['kind'] for c in controls))
    if counts != {k: len(domain) for k in ('result', 'memory', 'AF', 'target')}:
        raise RuntimeError('incomplete negative-control domain')
    summary = dict(full_CPU_returns=sum(r['role'] == 'root' for r in rows),
        explicit_CPU_boundaries=sum(r['role'] == 'boundary' for r in rows), CPU_executions=len(rows),
        controls=counts, modeled_fields=sum(r['modeled_fields'] for r in rows),
        memory_bytes=sum(r['memory_bytes'] for r in rows), exclusions=0, skips=0, normalizations=0,
        Graph_LLVM_executions=0)
    result = dict(schema=1, scope=__doc__, fixture_sha256=file_sha(path), script_sha256=file_sha(__file__),
        dependencies=dependencies, provenance_before=before, provenance_after=after,
        summary=summary, rows=rows, controls=controls)
    save(output / 'oracle.json', result)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path, default=DEFAULT)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument('--self-test', action='store_true')
    actions.add_argument('--oracle', type=Path)
    parser.add_argument('--expected-unicorn-sha256', default=UC_SHA)
    args = parser.parse_args()
    result = self_test(args.fixtures) if args.self_test else oracle(args.fixtures, args.oracle, args.expected_unicorn_sha256)
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
