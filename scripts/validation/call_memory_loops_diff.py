#!/usr/bin/env python3
"""Caller-memory inputs and bounded callee loops, with strict machine references.

--self-test is pure Python, --oracle executes only Unicorn, --analyze uses only
read-only finite-values. Native execution uses unchanged finite_dispatch_diff.py
--fixtures tests/fixtures/call-memory-loops.json --prove-leaf-calls, with V2 or
static/product FunctionStart. CPU observations never supply Graph successors.
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

import caller_context_diff as context

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / 'tests/fixtures/call-memory-loops.json'
ENTRY, CASE_A, CASE_B, CHILD, WRAPPER, LOOP = 0x1000, 0x1100, 0x1120, 0x1200, 0x1240, 0x1280
DATA, SENTINEL = 0x8000, 0xf000
TARGETS = {CASE_A: 7, CASE_B: 9}


def sha(data): return hashlib.sha256(data).hexdigest()
def file_sha(path): return context.file_sha(path)
def load(path): return json.loads(Path(path).read_text())
def save(path, value): Path(path).write_text(json.dumps(value, indent=2) + '\n')


class Code:
    def __init__(self, address):
        self.address = address; self.data = bytearray(); self.instructions = []; self.labels = {}; self.fixups = []
    def emit(self, blob, text, **extra):
        at = self.address + len(self.data)
        self.instructions.append(dict(address=at, size=len(blob), text=text, **extra)); self.data += blob
        return at
    def mark(self, name): self.labels[name] = self.address + len(self.data)
    def jump(self, opcode, label, text):
        at = self.emit(bytes([opcode, 0]), text); self.fixups.append((at, label))
    def call(self, address):
        at = self.address + len(self.data)
        return self.emit(b'\xe8' + struct.pack('<i', address - at - 5), 'CALL', target=address)
    def finish(self):
        for pc, label in self.fixups:
            delta = self.labels[label] - pc - 2
            if not -128 <= delta <= 127: raise ValueError('short branch overflow')
            self.data[pc - self.address + 1] = delta & 255
            next(i for i in self.instructions if i['address'] == pc)['target'] = self.labels[label]
        for insn in self.instructions:
            pos = insn['address'] - self.address; insn['code_hex'] = bytes(self.data[pos:pos + insn['size']]).hex()
        return bytes(self.data)


def fixture_for(arch, family, count=None, overlap=None):
    width = 8 if arch == 'x86_64' else 4; rex = b'\x48' if width == 8 else b''
    cells = {}; src, dst = 0x20, 0x60
    def init(offset, value): cells[offset] = value
    if family in ('read_copy', 'two_reads'):
        dst = 0x40; init(src, CASE_B); init(dst, CASE_A); slot = dst
    elif family == 'nested_read':
        dst = 0x40; init(src, CASE_B); init(dst, CASE_A); init(0x50, CASE_A); slot = 0x50
    elif family == 'counted_fill':
        for i in range(max(count, 1)): init(dst + width * i, CASE_A)
        slot = dst + width * max(count - 1, 0)
    elif family == 'counted_copy':
        if overlap == 'forward':
            dst = src + width
            for i, value in enumerate([CASE_A, CASE_B, CASE_B, CASE_B]): init(src + i * width, value)
        elif overlap == 'backward':
            dst = src; src += width
            for i, value in enumerate([CASE_B, CASE_A, CASE_B, CASE_B]): init(dst + i * width, value)
        else:
            # Count zero intentionally has no initialized source bytes.
            for i in range(count): init(src + i * width, CASE_B if i != 0 or count == 1 else CASE_A)
            for i in range(max(count, 1)): init(dst + i * width, CASE_A)
        slot = dst + width * max(count - 1, 0)
    else: raise ValueError('unknown family')
    root = Code(ENTRY)
    for offset, value in sorted(cells.items()):
        root.emit(rex + b'\xc7\x44\x24' + bytes([offset]) + struct.pack('<I', value), 'caller STORE')
    root.emit(rex + b'\x8d\x7c\x24' + bytes([src]), 'LEA source')
    root.emit(rex + b'\x8d\x74\x24' + bytes([dst]), 'LEA destination')
    if family in ('counted_fill', 'counted_copy'):
        root.emit(b'\xb9' + struct.pack('<I', count), 'MOV count')
    if family == 'counted_fill': root.emit(b'\xba' + struct.pack('<I', CASE_B), 'MOV fill value')
    if family == 'two_reads': root.emit(b'\xb8\x00\x00\x00\x00', 'MOV same AX context')
    root.call(WRAPPER if family == 'nested_read' else LOOP if family.startswith('counted_') else CHILD)
    if family == 'two_reads':
        root.emit(rex + b'\xc7\x44\x24' + bytes([src]) + struct.pack('<I', CASE_A), 'change source memory only')
        root.emit(b'\xb8\x00\x00\x00\x00', 'MOV same AX context')
        root.call(CHILD)
    dispatcher = root.emit(b'\xff\x64\x24' + bytes([slot]), 'indirect JMP')
    helpers = []
    if family in ('read_copy', 'two_reads', 'nested_read'):
        child = Code(CHILD); child.emit(rex + b'\x8b\x07', 'LOAD through caller pointer')
        child.emit(rex + b'\x89\x06', 'STORE through caller pointer'); child.emit(b'\xc3', 'RET'); helpers.append(child)
        if family == 'nested_read':
            wrapper = Code(WRAPPER); wrapper.call(CHILD)
            wrapper.emit(rex + b'\x89\xf7', 'MOV DI SI'); wrapper.emit(rex + b'\x83\xc6\x10', 'ADD destination16')
            wrapper.call(CHILD); wrapper.emit(b'\xc3', 'RET'); helpers.append(wrapper)
    else:
        helper = Code(LOOP); helper.emit(b'\x83\xf9\x00', 'CMP ECX0'); helper.jump(0x74, 'ret', 'JE zero-count')
        helper.mark('body')
        if family == 'counted_copy': helper.emit(rex + b'\x8b\x07', 'sequential LOAD')
        helper.emit(rex + (b'\x89\x16' if family == 'counted_fill' else b'\x89\x06'), 'sequential STORE')
        if family == 'counted_copy': helper.emit(rex + b'\x83\xc7' + bytes([width]), 'ADD source')
        helper.emit(rex + b'\x83\xc6' + bytes([width]), 'ADD destination')
        helper.emit(b'\x83\xe9\x01', 'SUB count1'); helper.jump(0x75, 'body', 'JNE repeat')
        helper.mark('ret'); helper.emit(b'\xc3', 'RET'); helpers.append(helper)
    body = root.finish(); code = bytearray(b'\xcc' * 0x300); code[:len(body)] = body
    regions = [dict(address=ENTRY, code_hex=body.hex(), instructions=root.instructions)]
    local = []
    for helper in helpers:
        blob = helper.finish(); code[helper.address - ENTRY:helper.address - ENTRY + len(blob)] = blob
        local.append(dict(address=helper.address, code_hex=blob.hex()))
        regions.append(dict(address=helper.address, code_hex=blob.hex(), instructions=helper.instructions))
    for address, value in TARGETS.items():
        blob = b'\xb8' + struct.pack('<I', value) + b'\x89\x03\xc3'
        code[address - ENTRY:address - ENTRY + len(blob)] = blob
    # Compute sequential memory independently of the encoded machine operations.
    model = dict(cells)
    if family == 'read_copy': model[dst] = model[src]
    elif family == 'two_reads': model[dst] = model[src]; model[src] = CASE_A; model[dst] = model[src]
    elif family == 'nested_read': model[dst] = model[src]; model[0x50] = model[dst]
    elif family == 'counted_fill':
        for i in range(count): model[dst + i * width] = CASE_B
    else:
        for i in range(count): model[dst + i * width] = model[src + i * width]
    target = model[slot]; suffix = family + (('_n' + str(count)) if count is not None else '') + (('_' + overlap) if overlap else '')
    return dict(id=arch + '_' + suffix, architecture=arch, code_hex=bytes(code).hex(), undefined_flags=[], stubs=[],
        indirect_site=dispatcher, expected_targets=[target], local_functions=local,
        call_memory_loop=dict(family=family, pointer_width=width, iterations=count, overlap=overlap,
            source_offset=src, destination_offset=dst, dispatcher_slot_offset=slot,
            initial_stack_cells=[dict(offset=o, value=v, width=width) for o, v in sorted(cells.items())],
            expected_final_stack_cells=[dict(offset=o, value=v, width=width) for o, v in sorted(model.items())],
            top_level_call_sites=[i['address'] for i in root.instructions if i['text'] == 'CALL'],
            nested_call_sites=[i['address'] for h in helpers for i in h.instructions if i['text'] == 'CALL'],
            loop_body=next((h.labels['body'] for h in helpers if 'body' in h.labels), None),
            machine_regions=regions, no_initial_API_facts=True))


def make_fixtures():
    cases = []
    for arch in ['x86_32', 'x86_64']:
        for family in ['read_copy', 'nested_read', 'two_reads']: cases.append(fixture_for(arch, family))
        for count in [0, 1, 3]: cases.append(fixture_for(arch, 'counted_fill', count))
        for count in [0, 1, 3]: cases.append(fixture_for(arch, 'counted_copy', count))
        for overlap in ['forward', 'backward']: cases.append(fixture_for(arch, 'counted_copy', 3, overlap))
    return dict(schema=1, entry=ENTRY,
        scope='Caller machine STOREs supply callee memory. Direct E8/C3, exact count guards and sequential overlapping copies. No API names, image facts, heap snapshots or CPU-fed CFG edges.', cases=cases)


def validate(document):
    canonical = make_fixtures()
    if document != canonical: raise ValueError('fixture bytes, layout, facts or exact22-case plan differ')
    if len(document['cases']) != 22: raise ValueError('expected22 permanent cases')
    for f in document['cases']:
        code = bytes.fromhex(f['code_hex']); meta = f['call_memory_loop']; width = meta['pointer_width']
        if f['undefined_flags'] or f['stubs']: raise ValueError('no exclusions or model stubs allowed')
        for region in meta['machine_regions']:
            start = region['address'] - ENTRY; body = bytes.fromhex(region['code_hex'])
            if code[start:start + len(body)] != body: raise ValueError('body differs from embedded code')
            for instruction in region['instructions']:
                at = instruction['address'] - ENTRY
                if code[at:at + instruction['size']].hex() != instruction['code_hex']: raise ValueError('machine instruction differs')
                if instruction['text'] == 'CALL':
                    if code[at] != 0xe8 or ENTRY + at + 5 + struct.unpack('<i', code[at + 1:at + 5])[0] != instruction['target']: raise ValueError('CALL target differs')
        at = f['indirect_site'] - ENTRY
        if code[at:at + 4] != bytes([0xff, 0x64, 0x24, meta['dispatcher_slot_offset']]): raise ValueError('dispatcher changed')
        if width not in [4, 8] or not meta['no_initial_API_facts']: raise ValueError('unexpected conditional facts')
    return document


def word(memory, at, width): return int.from_bytes(memory[at:at + width], 'little')
def put(memory, at, value, width): memory[at:at + width] = value.to_bytes(width, 'little')


def expected_machine(adapter, fixture, initial_regs, initial_memory):
    meta = fixture['call_memory_loop']; family = meta['family']; width = meta['pointer_width']
    regs = bytearray(initial_regs); memory = bytearray(initial_memory); sp = word(regs, 32, width)
    src, dst = sp + meta['source_offset'], sp + meta['destination_offset']; count = meta['iterations']
    for cell in meta['initial_stack_cells']: put(memory, sp + cell['offset'], cell['value'], width)
    final_src, final_dst = src, dst
    if family in ('read_copy', 'two_reads', 'nested_read'):
        put(memory, dst, word(memory, src, width), width)
        if family == 'two_reads': put(memory, src, CASE_A, width); put(memory, dst, word(memory, src, width), width)
        elif family == 'nested_read':
            final_src, final_dst = dst, dst + 16; put(memory, final_dst, word(memory, final_src, width), width)
    elif family == 'counted_fill':
        for i in range(count): put(memory, dst + i * width, CASE_B, width)
        final_dst += count * width; put(regs, 8, 0, 8); put(regs, 16, CASE_B, 8)
    else:
        for i in range(count): put(memory, dst + i * width, word(memory, src + i * width, width), width)
        final_src += count * width; final_dst += count * width; put(regs, 8, 0, 8)
    target = word(memory, sp + meta['dispatcher_slot_offset'], width)
    if target not in TARGETS or [target] != fixture['expected_targets']: raise ValueError('independent sequential model target differs')
    for offset, value in [(0, TARGETS[target]), (32, sp + width), (48, final_dst), (56, final_src), (128, SENTINEL)]: put(regs, offset, value, 8)
    if family.startswith('counted_'):
        values = dict(CF=0, PF=1, AF=0, ZF=1, SF=0, OF=0)
    elif family == 'nested_read':
        bits = width * 8; a, b = dst, 16; mask = (1 << bits) - 1; result = (a + b) & mask
        values = dict(CF=int(a + b > mask), PF=int((result & 255).bit_count() % 2 == 0), AF=((a ^ b ^ result) >> 4) & 1,
                      ZF=int(result == 0), SF=result >> (bits - 1), OF=int(bool((~(a ^ b) & (a ^ result)) & (1 << (bits - 1)))))
    else: values = {}
    for name, offset, unused in adapter.flags:
        if name in values: regs[offset] = values[name]
    put(memory, sp - width, meta['top_level_call_sites'][-1] + 5, width)
    if meta['nested_call_sites']: put(memory, sp - 2 * width, meta['nested_call_sites'][-1] + 5, width)
    put(memory, DATA, TARGETS[target], 4)
    return bytes(regs), bytes(memory), collections.Counter(), (1, 3, target + 7, SENTINEL, 1, 0, 0)


def self_test(path):
    document = validate(load(path)); checks = {}
    checks['exact22_immutable_machine_cases'] = len(document['cases']) == 22
    checks['all_three_fill_counts_in_both_architectures'] = sum(f['call_memory_loop']['family'] == 'counted_fill' for f in document['cases']) == 6
    zeros = [f for f in document['cases'] if f['call_memory_loop']['family'] == 'counted_copy' and f['call_memory_loop']['iterations'] == 0]
    checks['zero_copy_source_is_not_initialized'] = all(all(c['offset'] != f['call_memory_loop']['source_offset'] for c in f['call_memory_loop']['initial_stack_cells']) for f in zeros)
    checks['empty_target_publication_detected'] = bool(context.edge_errors([], [CASE_A], [CASE_A]))
    checks['wrong_target_publication_detected'] = bool(context.edge_errors([CASE_B], [CASE_A], [CASE_A]))
    checks['Unknown_not_a_positive_recovery'] = context.finite_status(dict(kind=2, values=[]), [CASE_A]) == 'unknown'
    for arch in ['x86_32', 'x86_64']:
        f = next(f for f in document['cases'] if f['architecture'] == arch and f['call_memory_loop']['overlap'] == 'forward')
        m = f['call_memory_loop']; cells = {c['offset']: c['value'] for c in m['initial_stack_cells']}
        snapshot_last = cells[m['source_offset'] + (m['iterations'] - 1) * m['pointer_width']]
        checks[arch + '_snapshot_copy_predicts_wrong_target'] = snapshot_last == CASE_B and f['expected_targets'] == [CASE_A]
    for kind in ['result', 'memory', 'single-field']:
        regs, memory = bytes(9), bytes(16); r, m = bytearray(regs), bytearray(memory)
        if kind == 'result': r[0] ^= 1; expected = ['RAX']
        elif kind == 'memory': m[3] ^= 1; expected = ['memory']
        else: r[8] ^= 1; expected = ['DF']
        checks[kind + '_single_field_detected'] = context.snapshot_differences([('RAX', 0, 8), ('DF', 8, 1)], regs, memory, r, m) == expected
    def corrupt_call(fixture):
        body = bytearray.fromhex(fixture['code_hex'])
        at = fixture['call_memory_loop']['top_level_call_sites'][0] - ENTRY
        body[at + 1] ^= 1
        fixture['code_hex'] = body.hex()
    for name, edit in [('changed_CALL', corrupt_call),
                       ('wrong_expected_target', lambda f: f.__setitem__('expected_targets', [0x1140])),
                       ('hidden_flag_exclusion', lambda f: f.__setitem__('undefined_flags', ['AF']))]:
        bad = copy.deepcopy(document); edit(bad['cases'][0])
        try: validate(bad)
        except ValueError: checks[name + '_rejected'] = True
        else: checks[name + '_rejected'] = False
    return dict(ok=all(checks.values()), checks=checks, fixture_sha256=file_sha(path), CPU_executions_started=0, Graph_analyses_started=0, LLVM_executions_started=0)


def oracle(path, output, trials):
    import function_guest_diff as guest
    document = validate(load(path)); output.mkdir(parents=True, exist_ok=False); rows = []; controls = collections.Counter()
    dependencies = {name: file_sha(ROOT / 'scripts/validation' / name) for name in ['function_guest_diff.py', 'function_llvm_diff.py', 'function_dispatch_diff.py', 'caller_context_diff.py']}
    for fixture in document['cases']:
        adapter = guest.Adapter(fixture['architecture']); meta = fixture['call_memory_loop']
        for trial in range(trials):
            regs, memory = adapter.initial(fixture, trial, ENTRY); trace = []; original_uc = guest.Uc
            def tracked(*args, **kwargs):
                uc = original_uc(*args, **kwargs)
                uc.hook_add(guest.UC_HOOK_CODE, lambda machine, pc, size, unused: trace.append(machine.reg_read(adapter.pc_reg)))
                return uc
            guest.Uc = tracked; error = None; actual = None
            try: actual = adapter.reference(fixture, regs, memory, ENTRY)
            except Exception as failure: error = repr(failure)
            finally: guest.Uc = original_uc
            work = output / fixture['id'] / str(trial); work.mkdir(parents=True)
            (work / 'initial-registers.bin').write_bytes(regs); (work / 'initial-memory.bin').write_bytes(memory)
            row = dict(case=fixture['id'], architecture=fixture['architecture'], trial=trial, trace=trace, reference_error=error)
            if actual is not None:
                wanted = expected_machine(adapter, fixture, regs, memory); packed = struct.pack('<8Q', *actual[3], 0)
                differences, unused = adapter.compare(fixture, wanted, (*actual[:3], packed), False)
                observed = [b for a, b in zip(trace, trace[1:]) if a == fixture['indirect_site']]
                checks = dict(independent_full_state=not differences, exact_dispatch=observed == fixture['expected_targets'],
                    actual_CALLs=all(pc in trace for pc in meta['top_level_call_sites'] + meta['nested_call_sites']),
                    code_unchanged=actual[1][ENTRY:ENTRY + 0x300] == bytes.fromhex(fixture['code_hex']))
                if meta['loop_body'] is not None: checks['exact_loop_trip_count'] = trace.count(meta['loop_body']) == meta['iterations']
                row.update(checks=checks, complete_CPU_return=all(checks.values()), differences=differences, actual_dispatch=observed,
                    exit=list(actual[3]), register_fields_compared=len(adapter.fields), memory_bytes_compared=65536)
                (work / 'final-registers.bin').write_bytes(actual[0]); (work / 'final-memory.bin').write_bytes(actual[1])
                for kind in ['result', 'memory', 'single-field']:
                    damaged_regs, damaged_mem = bytearray(actual[0]), bytearray(actual[1]); field = 'EAX' if fixture['architecture'] == 'x86_32' else 'RAX'
                    if kind == 'result': damaged_regs[0] ^= 1
                    elif kind == 'memory': damaged_mem[DATA] ^= 1; field = 'memory'
                    else: damaged_regs[142] ^= 1; field = 'DF'
                    diff, unused = adapter.compare(fixture, actual, (bytes(damaged_regs), bytes(damaged_mem), actual[2], packed), False)
                    if len(diff) != 1 or diff[0]['field'] != field or kind == 'memory' and diff[0]['count'] != 1: raise RuntimeError('single-field negative control failed')
                    controls[kind] += 1
                if not context.edge_errors([], fixture['expected_targets'], observed): raise RuntimeError('empty singleton edge escaped checker')
                controls['edge-empty-singleton'] += 1
                if meta['overlap'] == 'forward':
                    snapshot = {c['offset']: c['value'] for c in meta['initial_stack_cells']}
                    wrong = snapshot[meta['source_offset'] + (meta['iterations'] - 1) * meta['pointer_width']]
                    if not context.edge_errors([wrong], fixture['expected_targets'], observed): raise RuntimeError('snapshot-overlap wrong target escaped checker')
                    controls['snapshot-copy-wrong-target'] += 1
            save(work / 'cpu.json', row); rows.append(row); save(output / 'progress.json', dict(rows=rows, controls=dict(controls)))
            if error or not row.get('complete_CPU_return'): raise RuntimeError('CPU premise failed: ' + repr(row))
    if any(file_sha(ROOT / 'scripts/validation' / n) != v for n, v in dependencies.items()): raise RuntimeError('reference dependency changed')
    result = dict(schema=1, rows=rows, controls=dict(controls), fixture_sha256=file_sha(path), script_sha256=file_sha(__file__), dependencies=dependencies,
        complete_CPU_returns=len(rows), Graph_analyses_started=0, LLVM_executions_started=0, exclusions=0, skips=0, normalizations=0)
    save(output / 'oracle.json', result)
    return dict(complete_CPU_returns=len(rows), controls=dict(controls), Graph_analyses_started=0, LLVM_executions_started=0)


def analyze(path, output, pin_path, old):
    sys.path.insert(0, os.environ.get('DRAGON_TEST_PYTHON', str(ROOT / 'python'))); import dragon
    document = validate(load(path)); pin = load(pin_path); context.actual_maps(dragon, pin); output.mkdir(parents=True, exist_ok=False); rows = []
    for f in document['cases']:
        for strategy in [0, 1]:
            with dragon.Disassembler(1 if f['architecture'] == 'x86_32' else 2, dragon.Configuration(strategy=strategy)) as dis:
                with dis.analyze(bytes.fromhex(f['code_hex']), ENTRY) as graph:
                    before = context.graph_fingerprint(graph); sites = []
                    for pc in graph.block_addresses():
                        ir = graph.get_ignil_block(pc)
                        if ir and ir.ops() and ir.ops()[-1][:2] == (f['indirect_site'], dragon.IGNILOpcode.JMP): sites.append(pc)
                    if len(sites) != 1: raise RuntimeError('dispatcher must identify one real JMP')
                    result = graph.finite_values(entry_points=[ENTRY], prove_leaf_calls=True)
                    if result['complete'] is not True: raise RuntimeError('tiny caller did not reach its fixed point')
                    after = context.graph_fingerprint(graph)
                    if before != after: raise RuntimeError('read-only query mutated Graph')
                    status = context.finite_status(result['jump_targets'][sites[0]], f['expected_targets'])
                    row = dict(case=f['id'], strategy=strategy, status=status, query_block=sites[0], result=result, prove_leaf_calls=True,
                        graph_before=before, graph_after=after, actual_maps=context.actual_maps(dragon, pin),
                        decoded_call_sites=[dict(pc=i.address, callees=i.callees) for i in graph.instructions() if i.is_call],
                        counter_scope='All independent Graph roots; nested rejections are not root failure')
                    rows.append(row); save(output / 'progress.json', dict(rows=rows))
    counts = dict(collections.Counter(r['status'] for r in rows)); ok = all(r['status'] == ('unknown' if old else 'finite-exact') for r in rows)
    save(output / 'analysis.json', dict(schema=1, rows=rows, counts=counts, expectation_met=ok, old_Unknown_is_not_wrong_semantics=old,
        fixture_sha256=file_sha(path), script_sha256=file_sha(__file__), libraries=pin, CPU_executions_started=0, LLVM_executions_started=0))
    return dict(expectation_met=ok, statuses=counts, Graph_unchanged=True, CPU_executions_started=0, LLVM_executions_started=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--fixtures', type=Path, default=DEFAULT)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument('--self-test', action='store_true'); actions.add_argument('--oracle', type=Path); actions.add_argument('--analyze', type=Path)
    parser.add_argument('--trials', type=int, default=8); parser.add_argument('--library-pin', type=Path); parser.add_argument('--expect-old-unknown', action='store_true')
    args = parser.parse_args()
    if args.self_test: result = self_test(args.fixtures)
    elif args.oracle:
        if args.trials < 1: parser.error('positive trials required')
        result = oracle(args.fixtures, args.oracle, args.trials)
    else:
        if not args.library_pin: parser.error('--analyze requires --library-pin')
        result = analyze(args.fixtures, args.analyze, args.library_pin, args.expect_old_unknown)
    print(json.dumps(result, indent=2)); return 0 if result.get('ok', result.get('expectation_met', True)) else 1


if __name__ == '__main__': raise SystemExit(main())
