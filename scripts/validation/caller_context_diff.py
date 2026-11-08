#!/usr/bin/env python3
"""Caller-register contexts: machine fixtures, strict UC goldens and read-only facts.

The native matrix uses unchanged finite_dispatch_diff.py with this JSON and
--prove-leaf-calls. This script never launches LLVM or compiles a program.
--self-test is pure Python. --oracle executes only Unicorn. --analyze is an
explicit separate action and compares the existing public finite-values API.
Tigress prefix actions preserve its real memset and the caller loop; they do
not execute megaInit or claim whole-function recovery.
"""
from __future__ import annotations
import argparse
import collections
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / 'tests/fixtures/caller-context.json'
ENTRY, CASE_A, CASE_B, DATA, STACK, SENTINEL = 0x1000, 0x1100, 0x1120, 0x8000, 0xc800, 0xf000
TG_SHA = '366080723ae39eabd97b44bd2f0667ef17bff441ff8c09af0cd781f4d895a9d2'


def sha(data): return hashlib.sha256(data).hexdigest()
def file_sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1048576), b''): digest.update(chunk)
    return digest.hexdigest()
def load(path): return json.loads(Path(path).read_text())
def save(path, data): Path(path).write_text(json.dumps(data, indent=2) + '\n')


def validate_fixture(document):
    if document['schema'] != 1 or document['entry'] != ENTRY or len(document['cases']) != 6:
        raise ValueError('expected six permanent callers at1000')
    expected = {(arch, shape) for arch in ('x86_32', 'x86_64') for shape in ('single', 'two_calls', 'nested')}
    seen = set()
    for fixture in document['cases']:
        arch, meta = fixture['architecture'], fixture['caller_context']
        key = arch, meta['shape']
        if key not in expected or key in seen: raise ValueError('missing/duplicate caller shape')
        seen.add(key)
        code = bytes.fromhex(fixture['code_hex']); width = 8 if arch == 'x86_64' else 4
        target = CASE_A if meta['shape'] == 'single' else CASE_B
        if len(code) != 0x280 or fixture['undefined_flags'] or fixture['stubs']:
            raise ValueError('fixture layout/exclusions changed')
        if fixture['expected_targets'] != [target] or meta['pointer_width'] != width:
            raise ValueError('wrong complete target specification')
        at = fixture['indirect_site'] - ENTRY
        if code[at:at + 4] != bytes((0xff, 0x64, 0x24, meta['dispatcher_slot_offset'])):
            raise ValueError('dispatcher is not the actual stack LOAD/JMP')
        functions = {row['address']: bytes.fromhex(row['code_hex']) for row in fixture['local_functions']}
        if functions[0x1200] != bytes.fromhex('488937c3' if width == 8 else '8937c3'):
            raise ValueError('helper bytes changed')
        for address, body in functions.items():
            if code[address - ENTRY:address - ENTRY + len(body)] != body:
                raise ValueError('local body differs from actual code region')
        call_sites = meta['top_level_call_sites'] + meta['nested_call_sites']
        if code.count(b'\xe8') != len(call_sites): raise ValueError('unlisted machine call')
        for pc in call_sites:
            index = pc - ENTRY
            if code[index] != 0xe8 or pc + 5 + int.from_bytes(code[index + 1:index + 5], 'little', signed=True) not in functions:
                raise ValueError('real CALL target differs')
        for address, value in [(CASE_A, 7), (CASE_B, 9)]:
            if code[address - ENTRY:address - ENTRY + 8] != b'\xb8' + value.to_bytes(4, 'little') + b'\x89\x03\xc3':
                raise ValueError('case result/store/RET changed')
    if seen != expected: raise ValueError('fixture coverage differs')
    return document


def edge_errors(published, expected, observed):
    """Empty publication is a failure in a positive recovery test."""
    errors = []
    if set(published) != set(expected): errors.append('inexact-target-set')
    if any(target not in published for target in observed): errors.append('missing-executed-target')
    return errors


def finite_status(value, targets):
    if value['kind'] == 2:
        if value['values']: raise ValueError('Unknown must not carry values')
        return 'unknown'
    if value['kind'] != 1 or set(value['values']) != set(targets):
        return 'wrong-or-incomplete-finite'
    return 'finite-exact'


def snapshot_differences(fields, expected_regs, expected_memory, actual_regs, actual_memory):
    differences = []
    for name, offset, size in fields:
        if expected_regs[offset:offset + size] != actual_regs[offset:offset + size]:
            differences.append(name)
    if expected_memory != actual_memory: differences.append('memory')
    return differences


def self_test(path):
    document = validate_fixture(load(path)); checks = {}
    checks['six_real_callers'] = len(document['cases']) == 6
    checks['good_edge_set'] = not edge_errors([CASE_A], [CASE_A], [CASE_A])
    checks['empty_targets_are_not_a_positive_pass'] = bool(edge_errors([], [CASE_A], [CASE_A]))
    checks['missing_singleton_edge_detected'] = 'missing-executed-target' in edge_errors([], [CASE_A], [CASE_A])
    checks['wrong_edge_detected'] = bool(edge_errors([CASE_B], [CASE_A], [CASE_A]))
    checks['old_Unknown_not_called_finite'] = finite_status(dict(kind=2, values=[]), [CASE_A]) == 'unknown'
    checks['wrong_constant_detected'] = finite_status(dict(kind=1, values=[CASE_A]), [CASE_B]) != 'finite-exact'
    fields = [('RAX', 0, 8), ('DF', 8, 1)]
    regs, memory = bytes(9), bytes(16)
    damaged = bytearray(regs); damaged[0] ^= 1
    checks['one_result_field_control'] = snapshot_differences(fields, regs, memory, damaged, memory) == ['RAX']
    damaged = bytearray(regs); damaged[8] ^= 1
    checks['one_other_register_field_control'] = snapshot_differences(fields, regs, memory, damaged, memory) == ['DF']
    damaged_mem = bytearray(memory); damaged_mem[3] ^= 1
    checks['one_memory_byte_control'] = snapshot_differences(fields, regs, memory, regs, damaged_mem) == ['memory']
    def rejected(name, action):
        try: action()
        except (ValueError, KeyError): checks[name] = True
        else: checks[name] = False
    bad = copy.deepcopy(document); bad['cases'][1] = copy.deepcopy(bad['cases'][0])
    rejected('duplicate_fixture_rejected', lambda: validate_fixture(bad))
    bad = copy.deepcopy(document); body = bytearray.fromhex(bad['cases'][0]['code_hex']); call = bad['cases'][0]['caller_context']['top_level_call_sites'][0] - ENTRY
    body[call + 1] ^= 1; bad['cases'][0]['code_hex'] = body.hex()
    rejected('changed_CALL_target_rejected', lambda: validate_fixture(bad))
    return dict(ok=all(checks.values()), checks=checks, fixture_sha256=file_sha(path),
                CPU_executions_started=0, Graph_analyses_started=0, LLVM_executions_started=0)


def expected_machine_output(guest, adapter, fixture, regs, memory):
    """Independent effects of these six tiny callers, including real CALL slots."""
    meta = fixture['caller_context']; width = meta['pointer_width']; sp = guest.base.get(regs, 32)
    target = fixture['expected_targets'][0]; result = 7 if target == CASE_A else 9
    want_regs, want_mem = bytearray(regs), bytearray(memory)
    for offset, value in [(0, result), (32, sp + width), (48, target), (56, sp + meta['dispatcher_slot_offset']), (128, SENTINEL)]:
        guest.base.put(want_regs, offset, value, 8)
    if meta['shape'] == 'nested':
        bits = width * 8; a, b = sp + 0x20, 8; mask = (1 << bits) - 1; total = a + b; value = total & mask
        flags = dict(CF=int(total > mask), PF=int((value & 255).bit_count() % 2 == 0),
                     AF=((a ^ b ^ value) >> 4) & 1, ZF=int(value == 0),
                     SF=(value >> (bits - 1)) & 1, OF=int(bool((~(a ^ b) & (a ^ value)) & (1 << (bits - 1)))))
        for name, offset, bit in adapter.flags:
            if name in flags: want_regs[offset] = flags[name]
    for slot in meta['expected_final_stack_slots']:
        guest.base.put(want_mem, sp + slot['offset'], slot['value'], slot['width'])
    guest.base.put(want_mem, sp - width, meta['top_level_call_sites'][-1] + 5, width)
    if meta['nested_call_sites']:
        guest.base.put(want_mem, sp - 2 * width, meta['nested_call_sites'][-1] + 5, width)
    guest.base.put(want_mem, DATA, result, 4)
    return bytes(want_regs), bytes(want_mem), collections.Counter(), (1, 3, target + 7, SENTINEL, 1, 0, 0)


def oracle(path, output, trials):
    # These unchanged adapters define the full Guest ABI. Their Dragon module is
    # loaded as a dependency but no Dragon analysis/lifting API is called here.
    import function_guest_diff as guest
    document = validate_fixture(load(path)); rows = []; controls = collections.Counter()
    dependencies = {str(ROOT / 'scripts/validation' / name): file_sha(ROOT / 'scripts/validation' / name)
                    for name in ['function_guest_diff.py', 'function_llvm_diff.py', 'function_dispatch_diff.py']}
    output.mkdir(parents=True, exist_ok=False)
    for fixture in document['cases']:
        adapter = guest.Adapter(fixture['architecture'])
        for trial in range(trials):
            regs, memory = adapter.initial(fixture, trial, ENTRY); trace = []; writes = []
            original_uc = guest.Uc
            def tracked(*args, **kwargs):
                uc = original_uc(*args, **kwargs)
                uc.hook_add(guest.UC_HOOK_CODE, lambda machine, pc, size, unused: trace.append(dict(pc=machine.reg_read(adapter.pc_reg), size=size)))
                import unicorn
                uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, lambda machine, access, address, size, value, unused: writes.append(dict(pc=machine.reg_read(adapter.pc_reg), address=address, size=size, value=value)))
                return uc
            guest.Uc = tracked; error = None
            try: actual = adapter.reference(fixture, regs, memory, ENTRY)
            except Exception as failure: error = repr(failure); actual = None
            finally: guest.Uc = original_uc
            work = output / fixture['id'] / str(trial); work.mkdir(parents=True)
            (work / 'initial-registers.bin').write_bytes(regs); (work / 'initial-memory.bin').write_bytes(memory)
            row = dict(case=fixture['id'], trial=trial, architecture=fixture['architecture'], trace=trace, writes=writes,
                       code_sha256=sha(bytes.fromhex(fixture['code_hex'])), reference_error=error)
            if actual is not None:
                want = expected_machine_output(guest, adapter, fixture, regs, memory)
                packed = struct.pack('<8Q', *actual[3], 0)
                differences, record = adapter.compare(fixture, want, (*actual[:3], packed), False)
                observed = [b['pc'] for a, b in zip(trace, trace[1:]) if a['pc'] == fixture['indirect_site']]
                checks = dict(independent_full_state=not differences, exact_dispatch=observed == fixture['expected_targets'],
                              all_real_call_sites_visited=all(pc in {t['pc'] for t in trace} for pc in fixture['caller_context']['top_level_call_sites'] + fixture['caller_context']['nested_call_sites']),
                              code_unchanged=actual[1][ENTRY:ENTRY + len(bytes.fromhex(fixture['code_hex']))] == bytes.fromhex(fixture['code_hex']))
                row.update(checks=checks, differences=differences, actual_dispatch=observed, exit=list(actual[3]),
                           register_fields_compared=len(adapter.fields), memory_bytes_compared=len(memory),
                           complete_CPU_return=all(checks.values()))
                (work / 'final-registers.bin').write_bytes(actual[0]); (work / 'final-memory.bin').write_bytes(actual[1])
                for kind in ['result', 'memory', 'single-field']:
                    damaged_regs, damaged_mem = bytearray(actual[0]), bytearray(actual[1])
                    field = 'EAX' if fixture['architecture'] == 'x86_32' else 'RAX'
                    if kind == 'result': damaged_regs[0] ^= 1
                    elif kind == 'memory': damaged_mem[DATA] ^= 1; field = 'memory'
                    else: damaged_regs[142] ^= 1; field = 'DF'
                    diff, unused = adapter.compare(fixture, actual, (bytes(damaged_regs), bytes(damaged_mem), actual[2], packed), False)
                    if len(diff) != 1 or diff[0]['field'] != field or kind == 'memory' and diff[0]['count'] != 1:
                        raise RuntimeError('single-field oracle control failed: ' + repr((kind, diff)))
                    controls[kind] += 1
                if not edge_errors([], fixture['expected_targets'], observed): raise RuntimeError('empty target set escaped edge control')
                controls['edge-empty-singleton'] += 1
            save(work / 'cpu.json', row); rows.append(row)
            save(output / 'progress.json', dict(rows=rows, controls=dict(controls)))
            if error or not row.get('complete_CPU_return'): raise RuntimeError('fixture CPU premise failed: ' + repr(row))
    if any(file_sha(p) != h for p, h in dependencies.items()): raise RuntimeError('unchanged reference dependency changed')
    result = dict(schema=1, fixture_sha256=file_sha(path), script_sha256=file_sha(__file__), rows=rows,
                  controls=dict(controls), complete_CPU_returns=len(rows), Graph_analyses_started=0,
                  LLVM_executions_started=0, dependencies=dependencies, exclusions=0, skips=0, normalizations=0,
                  reference_backend='Unicorn via unchanged full Guest Adapter; no Dragon analysis called')
    save(output / 'oracle.json', result)
    return dict(complete_CPU_returns=len(rows), controls=dict(controls), Graph_analyses_started=0, LLVM_executions_started=0)


def actual_maps(dragon, pin):
    if type(pin) is not dict or len(pin) != 2 or {Path(path).name for path in pin} != {'libdragon.so', 'libdragon_c.so'}:
        raise ValueError('pin must name exactly the backend and C shim')
    lines = [line for line in Path('/proc/self/maps').read_text().splitlines() if '/libdragon' in line]
    paths = {str(Path(line.split()[-1]).resolve()) for line in lines}
    if not lines or any('(deleted)' in line for line in lines) or paths != set(pin) or str(Path(dragon._LIB).resolve()) not in paths:
        raise RuntimeError('actual library mapping differs from independent pin')
    if any(file_sha(path) != value for path, value in pin.items()): raise RuntimeError('mapped library bytes changed')
    wrapper = Path(dragon.__file__).resolve()
    headers = [wrapper.parent.parent / 'c_shim/dragon_c.h',
               wrapper.parent.parent / 'include/dragon/lifters/llvm/Execution.h']
    return dict(libraries=pin, maps=lines,
                interface_sources={str(path): file_sha(path) for path in [wrapper, *headers]})


def graph_fingerprint(graph):
    rows = [dict(pc=i.address, code=i.bytes.hex(), branches=i.branches, callees=i.callees,
                 block=i.is_block_start, function=i.is_function_start) for i in graph.instructions()]
    blocks = []
    for pc in graph.block_addresses():
        ir = graph.get_ignil_block(pc)
        blocks.append((pc, ir.to_string() if ir else None, ir.unlifted() if ir else None))
    return sha(json.dumps(dict(rows=rows, blocks=blocks, functions=graph.function_addresses()), sort_keys=True).encode())


def analyze(path, output, pin_path, expected_old):
    sys.path.insert(0, os.environ.get('DRAGON_TEST_PYTHON', str(ROOT / 'python'))); import dragon
    pin = load(pin_path); actual_maps(dragon, pin)
    document = validate_fixture(load(path)); output.mkdir(parents=True, exist_ok=False); rows = []
    for fixture in document['cases']:
        for strategy in (0, 1):
            with dragon.Disassembler(1 if fixture['architecture'] == 'x86_32' else 2, dragon.Configuration(strategy=strategy)) as dis:
                with dis.analyze(bytes.fromhex(fixture['code_hex']), ENTRY) as graph:
                    before = graph_fingerprint(graph); sites = []
                    for pc in graph.block_addresses():
                        ir = graph.get_ignil_block(pc)
                        if ir and ir.ops() and ir.ops()[-1][:2] == (fixture['indirect_site'], dragon.IGNILOpcode.JMP): sites.append(pc)
                    if len(sites) != 1: raise RuntimeError('dispatcher does not identify one real IGNIL JMP')
                    result = graph.finite_values(entry_points=[ENTRY], prove_leaf_calls=True)
                    if result['complete'] is not True: raise RuntimeError('tiny caller analysis did not reach its fixed point')
                    after = graph_fingerprint(graph)
                    if before != after: raise RuntimeError('read-only call query mutated Graph')
                    value = result['jump_targets'][sites[0]]; status = finite_status(value, fixture['expected_targets'])
                    row = dict(case=fixture['id'], strategy=strategy, code_sha256=sha(bytes.fromhex(fixture['code_hex'])),
                               prove_leaf_calls=True, status=status, query_block=sites[0], result=result,
                               graph_before=before, graph_after=after, actual_maps=actual_maps(dragon, pin),
                               decoded_call_sites=[dict(pc=i.address, callees=i.callees) for i in graph.instructions() if i.is_call],
                               counter_scope='Unique analysed machine call sites across all independent roots; nested rejected sites do not imply root failure')
                    rows.append(row); save(output / 'progress.json', dict(rows=rows))
    counts = dict(collections.Counter(row['status'] for row in rows))
    ok = all(row['status'] == ('unknown' if expected_old else 'finite-exact') for row in rows)
    save(output / 'analysis.json', dict(schema=1, rows=rows, counts=counts, expectation_met=ok,
        old5a_without_context=expected_old, old_Unknown_is_not_wrong_semantics=expected_old,
        fixture_sha256=file_sha(path), script_sha256=file_sha(__file__), libraries=pin, CPU_or_LLVM_runs=0))
    return dict(expectation_met=ok, statuses=counts, old5a_without_context=expected_old, Graph_unchanged=True)


def tg_image(path):
    import io
    from elftools.elf.elffile import ELFFile
    data = Path(path).read_bytes()
    if sha(data) != TG_SHA: raise ValueError('not the frozen MBA O2 ELF')
    image = ELFFile(io.BytesIO(data)); regions = []
    for section in image.iter_sections():
        if section['sh_flags'] & 2 and section['sh_size']:
            blob = bytes(section['sh_size']) if section['sh_type'] == 'SHT_NOBITS' else section.data()
            regions.append((section['sh_addr'], blob))
    table = next(blob[0x11060 - address:0x11090 - address] for address, blob in regions
                 if address <= 0x11060 and 0x11090 <= address + len(blob))
    rodata = next(blob for address, blob in regions if address == 0x11000)
    return data, regions, table, rodata[0x10:0x30]


def tg_prefix_expected(x, initial_out):
    output = [int.from_bytes(initial_out[i:i + 4], 'little') for i in range(0, 32, 4)]
    state = x ^ 0xdcc99397; codes = []
    for pc in range(12):
        code = (state ^ (state >> 13) ^ pc) % 9; codes.append(code)
        if code <= 5: return b''.join(value.to_bytes(4, 'little') for value in output), codes
        for unused in range(state & 15):
            state = ((state << 1) & 0xffffffff) ^ (0x6a6ae769 if state >> 31 else 0)
        output[pc & 7] ^= state
    raise ValueError('planned prefix never reaches its table LOAD')


def tg_cases(table):
    return [dict(id=('alias' if alias else 'disjoint') + '-x%08x' % x, x=x,
                 out=0x11060 if alias else 0x8000, alias=alias,
                 entry_states=[dict(entry=0x10000, registers=[dict(offset=32, width=64, value=STACK),
                      dict(offset=48, width=64, value=0x11060 if alias else 0x8000),
                      dict(offset=56, width=64, value=x)],
                      memory=[dict(address=0x11060, bytes=table.hex()),
                              dict(address=STACK, bytes=SENTINEL.to_bytes(8, 'little').hex())])])
            for alias in (False, True) for x in (0, 1, 0x80000000, 0xffffffff)]


def tg_oracle(path, output):
    import unicorn as U
    from unicorn import x86_const as X
    data, regions, table, initial_out = tg_image(path); rows = []
    output.mkdir(parents=True, exist_ok=False)
    for case in tg_cases(table):
        memory = bytearray(1048576)
        for address, blob in regions: memory[address:address + len(blob)] = blob
        memory[STACK:STACK + 8] = SENTINEL.to_bytes(8, 'little')
        uc = U.Uc(U.UC_ARCH_X86, U.UC_MODE_64); uc.mem_map(0, len(memory)); uc.mem_write(0, bytes(memory))
        for register, value in [(X.UC_X86_REG_RSP, STACK), (X.UC_X86_REG_RDI, case['x']),
                                (X.UC_X86_REG_RSI, case['out']), (X.UC_X86_REG_EFLAGS, 0x202)]: uc.reg_write(register, value)
        trace, points = [], []
        def hook(machine, pc, size, unused):
            trace.append(pc)
            if pc in (0x1004f, 0x1011b):
                points.append(dict(pc=pc, table_hex=bytes(machine.mem_read(0x11060, 48)).hex(),
                                   values=[int.from_bytes(bytes(machine.mem_read(0x11060 + i * 8, 8)), 'little') for i in range(6)]))
            if pc == 0x1011b: machine.emu_stop()
        uc.hook_add(U.UC_HOOK_CODE, hook); error = None
        try: uc.emu_start(0x10000, SENTINEL, count=20000)
        except U.UcError as failure: error = repr(failure)
        final = bytes(uc.mem_read(0, len(memory))); expected_out, codes = tg_prefix_expected(case['x'], initial_out)
        expected_table = expected_out + table[32:] if case['alias'] else table
        checks = dict(no_CPU_error=error is None, both_exact_points=[p['pc'] for p in points] == [0x1004f, 0x1011b],
            real_memset_called_and_returned=0x10420 in trace and 0x104bd in trace and trace.index(0x104bd) < trace.index(0x1004f),
            entry_and_stop=trace[0] == 0x10000 and uc.reg_read(X.UC_X86_REG_RIP) == 0x1011b,
            no_megaInit=not any(0x102e0 <= pc < 0x10319 or 0x1000 <= pc < 0x1021 for pc in trace),
            SBOX_zero=memory[0x11090:0x110d0] == final[0x11090:0x110d0] == bytes(64),
            complete_source_prefix_output=final[case['out']:case['out'] + 32] == expected_out,
            guard_visit_count=trace.count(0x10115) == len(codes),
            all_code_unchanged=all(final[address:address + len(blob)] == blob for address, blob in regions if address in (0x1000, 0x10000)))
        if len(points) == 2:
            checks['after_CALL_table_unchanged'] = points[0]['table_hex'] == table.hex()
            checks['before_JMP_table_matches_real_out_writes'] = points[1]['table_hex'] == expected_table.hex()
        work = output / case['id']; work.mkdir()
        (work / 'initial-memory.bin').write_bytes(memory); (work / 'before-JMP-memory.bin').write_bytes(final)
        row = dict(**case, points=points, trace=trace, checks=checks, reference_error=error,
                   successful_prefix=all(checks.values()), complete_CPU_return=False,
                   entry=0x10000, stop_before=0x1011b, ELF_sha256=sha(data),
                   initialization='Allocated ELF sections plus zero .bss/rest; no megaInit/SBOX/heap snapshot. Source formula predicts output only.')
        save(work / 'cpu.json', row); rows.append(row); save(output / 'progress.json', dict(rows=rows))
        if not row['successful_prefix']: raise RuntimeError('TG prefix premise failed: ' + repr(checks))
    save(output / 'oracle.json', dict(schema=1, rows=rows, ELF_sha256=sha(data), script_sha256=file_sha(__file__),
        successful_prefixes=8, complete_CPU_returns=0, Graph_analyses_started=0, LLVM_executions_started=0))
    return dict(successful_TG_prefixes=8, complete_CPU_returns=0, both_LOAD_points_observed=True, Graph_analyses_started=0)


def normalized_states(states):
    return [dict(entry=s['entry'], registers=s['registers'],
                 memory=[dict(address=m['address'], bytes=bytes.fromhex(m['bytes']).hex()) for m in s['memory']]) for s in states]


def tg_analyze(path, output, pin_path, observations):
    sys.path.insert(0, os.environ.get('DRAGON_TEST_PYTHON', str(ROOT / 'python'))); import dragon
    data, regions, table, unused = tg_image(path); pin = load(pin_path); actual_maps(dragon, pin)
    CPU = load(observations); cases = tg_cases(table)
    if CPU['ELF_sha256'] != TG_SHA or len(CPU['rows']) != 8: raise ValueError('TG oracle scope differs')
    output.mkdir(parents=True, exist_ok=False); rows = []; statuses = collections.Counter()
    for case in cases:
        cpu = next(r for r in CPU['rows'] if r['id'] == case['id'])
        if not cpu['successful_prefix'] or cpu['complete_CPU_return'] or normalized_states(cpu['entry_states']) != case['entry_states']:
            raise ValueError('TG CPU assumptions/prefix are not valid')
        for strategy in (0, 1):
            with dragon.LoadedImage.load_bytes(data) as image:
                with image.disassemble(dragon.Configuration(strategy=strategy)) as graph:
                    sites, queries = [], []
                    for point, count in [(0x1004f, 2), (0x1011b, 1)]:
                        candidates = []
                        for block in graph.block_addresses():
                            ir = graph.get_ignil_block(block)
                            if ir:
                                candidates += [dict(block=block, index=index, text=ir.op_to_string(index))
                                    for pc, opcode, index in ir.ops() if pc == point and opcode == dragon.IGNILOpcode.LOAD]
                        if len(candidates) != count or any(not re.match(r't\d+:i64 = LOAD\(', q['text']) for q in candidates):
                            raise RuntimeError('TG exact LOAD64 layout differs')
                        # MOVAPS has two real64-bit LOADs; ordinal0 is explicit.
                        site = sorted(candidates, key=lambda q: (q['block'], q['index']))[0]
                        sites.append(dict(pc=point, LOAD_ordinal=0, actual_LOAD_count=count, **site))
                        queries += [(site['block'], site['index'], 0x11060 + i * 8, 64) for i in range(6)]
                    states = [dragon.FiniteEntryState(s['entry'],
                        registers=[(r['offset'], r['width'], r['value']) for r in s['registers']],
                        memory=[(m['address'], bytes.fromhex(m['bytes'])) for m in s['memory']]) for s in case['entry_states']]
                    before = graph_fingerprint(graph)
                    result = graph.finite_memory_values_with_initial_state(queries, states, entry_points=[0x10000], prove_leaf_calls=True)
                    after = graph_fingerprint(graph)
                    if before != after or result.get('conditional') is not True or result.get('assumptions_valid') is not True or normalized_states(result['entry_states']) != case['entry_states']:
                        raise RuntimeError('TG read-only/conditional contract differs')
                    if len(result['values']) != 12: raise RuntimeError('TG query count differs')
                    checks = []
                    for index, value in enumerate(result['values']):
                        actual = cpu['points'][index // 6]['values'][index % 6]
                        if value['kind'] == 1:
                            if not result['complete'] or actual not in value['values']: raise RuntimeError('TG finite set omitted actual LOAD-time cell value')
                            status = 'finite-covered'
                        elif value['kind'] == 2 and not value['values']: status = 'unknown'
                        else: raise RuntimeError('executed TG query reported Unreachable/invalid values')
                        statuses[(hex(sites[index // 6]['pc']), status)] += 1; checks.append(status)
                    row = dict(case=case['id'], strategy=strategy, sites=sites, result=result, statuses=checks,
                        prove_leaf_calls=True, graph_before=before, graph_after=after, actual_maps=actual_maps(dragon, pin),
                        decoded_call_sites=[dict(pc=i.address, callees=i.callees) for i in graph.instructions() if i.is_call],
                        scope='Conditional kernel prefix only; real memset and caller-loop/store barriers retained. No automatic switch recovery claim.')
                    rows.append(row); save(output / 'progress.json', dict(rows=rows))
    result = dict(schema=1, rows=rows, per_point_statuses=[dict(point=k[0], status=k[1], count=v) for k, v in sorted(statuses.items())],
        ELF_sha256=sha(data), script_sha256=file_sha(__file__), libraries=pin, prove_leaf_calls=True,
        CPU_executions_started=0, LLVM_executions_started=0, semantic_complete=False)
    save(output / 'analysis.json', result)
    return dict(read_only_reports=len(rows), fixed_cell_queries=192, prove_leaf_calls=True,
                per_point_statuses=result['per_point_statuses'], wrong=0, CPU_executions_started=0, LLVM_executions_started=0)


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--fixtures', type=Path, default=DEFAULT)
    action = p.add_mutually_exclusive_group(required=True)
    action.add_argument('--self-test', action='store_true'); action.add_argument('--oracle', type=Path)
    action.add_argument('--analyze', type=Path)
    action.add_argument('--tg-oracle', type=Path); action.add_argument('--tg-analyze', type=Path)
    p.add_argument('--library-pin', type=Path); p.add_argument('--expect-old-unknown', action='store_true')
    p.add_argument('--tg-elf', type=Path); p.add_argument('--tg-observations', type=Path)
    p.add_argument('--trials', type=int, default=8)
    args = p.parse_args()
    if args.self_test: result = self_test(args.fixtures)
    elif args.oracle:
        if args.trials < 1: p.error('positive trials required')
        result = oracle(args.fixtures, args.oracle, args.trials)
    elif args.analyze:
        if args.library_pin is None: p.error('--analyze requires --library-pin')
        result = analyze(args.fixtures, args.analyze, args.library_pin, args.expect_old_unknown)
    else:
        if args.tg_elf is None: p.error('TG actions require --tg-elf')
        if args.tg_oracle: result = tg_oracle(args.tg_elf, args.tg_oracle)
        else:
            if args.library_pin is None or args.tg_observations is None: p.error('--tg-analyze requires library pin and actual TG observations')
            result = tg_analyze(args.tg_elf, args.tg_analyze, args.library_pin, args.tg_observations)
    print(json.dumps(result, indent=2))
    return 0 if result.get('ok', result.get('expectation_met', True)) else 1


if __name__ == '__main__': raise SystemExit(main())
