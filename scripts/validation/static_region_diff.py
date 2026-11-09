"""Small three-architecture decoded-region oracle using unchanged Guest checks.

Each fixture stores its input target into Guest DATA before loading/jumping;
the initial table contains a different target. No values become Graph hints.
Two decoded cases write 7/99 and
return. A third target is an explicit external Transfer boundary. Interior
external entries must execute nothing. All fields, 64 KiB and seven Exit fields
are compared by StaticAdapter. --without-region keeps ordinary lift and records
covered-case stops as incomplete, never as complete matches (exit status 1).
The ordinary six LLVM steps and opt-in control-flow step are unchanged.
"""
import argparse
import collections
import copy
import hashlib
import json
import sys
from pathlib import Path

ENTRY, EXTERNAL = 0x1000, 0x2000


def fixtures():
    result = []
    for arch in ('x86_32', 'x86_64', 'arm64'):
        if arch == 'arm64':
            root = bytes.fromhex('600200f9700240f900021fd6')  # STR X0; LDR X16; BR X16 via [X19]
            bodies = [bytes.fromhex('e00080d2600600f9c0035fd6'),
                      bytes.fromhex('600c80d2600600f9c0035fd6')]
        else:
            root = bytes.fromhex('893bff23' if arch == 'x86_32' else '48893bff23')
            bodies = [bytes.fromhex('b807000000894308c3'),
                      bytes.fromhex('b863000000894308c3')]
        starts = [ENTRY + len(root), ENTRY + len(root) + len(bodies[0])]
        result.append(dict(id=arch + '_decoded_region', architecture=arch,
            code_hex=(root + b''.join(bodies)).hex(), case_starts=starts,
            jump_offset=len(root) - (4 if arch == 'arm64' else 2),
            case_bytes=[b.hex() for b in bodies], undefined_flags=[], stubs=[],
            local_functions=[], missing_targets=[EXTERNAL], boundary_kind=1))
    return result


def save(path, value):
    with path.open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def self_test():
    cases = fixtures()
    for f in cases:
        data = bytes.fromhex(f['code_hex'])
        for pc, code in zip(f['case_starts'], f['case_bytes']):
            offset = pc - ENTRY
            if data[offset:offset + len(bytes.fromhex(code))] != bytes.fromhex(code):
                raise RuntimeError('fixture case boundary mismatch')
        if f['undefined_flags'] or f['missing_targets'] != [EXTERNAL]:
            raise RuntimeError('fixture exclusions/boundary drift')
    print(json.dumps(dict(pure_python=True, fixtures=3, exact_case_boundaries=6,
                          exclusions=0, Graph_CPU_LLVM_native_executions=0)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--case', action='append', dest='cases')
    parser.add_argument('--trials', type=int, default=8)
    parser.add_argument('--without-region', action='store_true')
    parser.add_argument('--control', choices=('result', 'memory'))
    parser.add_argument('--expect-control', action='store_true')
    for name, default in (('opt', 'opt-21'), ('llc', 'llc-21'), ('cc', 'gcc')):
        parser.add_argument('--' + name, default=default)
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.out or args.trials < 8 or bool(args.control) != args.expect_control:
        parser.error('fresh --out, at least eight trials and matching control flags required')
    if args.control and args.without_region:
        parser.error('run controls on region coverage; ordinary incomplete runs stay separate')
    chosen = [f for f in fixtures() if not args.cases or f['id'] in args.cases]
    if not chosen or args.cases and {f['id'] for f in chosen} != set(args.cases):
        parser.error('unknown case')
    out = args.out.expanduser().resolve()
    if out.exists():
        parser.error('--out must not exist; previous failures are retained')
    import static_entry_diff as study
    from control_flow_diff import Overlay, libraries, digest
    from static_region_replay import graph_snapshot, canonical_sha
    d, base = study.base.dragon, study.base
    out.mkdir(parents=True)
    pins = libraries(d)
    overlay = Overlay(study, True)
    original_lift = d.LLVMLifter.lift
    active = None
    graphs, rows = [], []

    def lift(lifter, function, graph):
        if active is None or function.start_address != ENTRY:
            raise RuntimeError('unexpected lifted function')
        before = graph_snapshot(graph)
        decoded = {i['address']: i for i in before['instructions']}
        leaders = []
        for pc, code in zip(active['case_starts'], active['case_bytes']):
            # Derive eligibility from actual decoded instructions, with exact
            # first-instruction bytes checked against the independent fixture.
            if pc not in decoded or not decoded[pc]['size'] or not decoded[pc]['bytes'] or not code.startswith(decoded[pc]['bytes']):
                raise RuntimeError('case start was not decoded at its real machine boundary')
            leaders.append(pc)
        jump_pc = ENTRY + active['jump_offset']
        if jump_pc not in decoded or not decoded[jump_pc]['flags']['is_jump'] or decoded[jump_pc]['branches']:
            raise RuntimeError('unknown Guest table unexpectedly acquired static target hints')
        save(overlay.work / 'region-graph-before.json', before)
        try:
            return (original_lift(lifter, function, graph) if args.without_region else
                    lifter.lift_static_region(function, graph, leaders, rebuild_decoded_blocks=True))
        finally:
            after = graph_snapshot(graph)
            save(overlay.work / 'region-graph-after.json', after)
            unchanged = canonical_sha(before) == canonical_sha(after)
            graphs.append(dict(work=str(overlay.work), leaders=leaders,
                before_sha256=canonical_sha(before), after_sha256=canonical_sha(after), unchanged=unchanged))
            if not unchanged:
                raise RuntimeError('region lift mutated source Graph')

    overlay.install()
    d.LLVMLifter.lift = lift
    status = None
    try:
        for f in chosen:
            active = f
            adapter = study.StaticAdapter(f['architecture'])
            width = 4 if f['architecture'] == 'x86_32' else 8
            result_width = 8 if f['architecture'] == 'arm64' else 4
            for level in ('O0', 'O2'):
                work = out / f['id'] / level
                work.mkdir(parents=True)
                exe, bad_ir, _ = study.compiler.compile_case(f, ENTRY, level, work, args,
                    architecture=f['architecture'], static_analysis=True,
                    product_pipeline=True, function_entry=True)
                observations = [(ENTRY, target) for target in f['case_starts'] + [EXTERNAL]]
                observations += [(pc, f['case_starts'][0]) for pc in f['case_starts']]
                for trial in range(args.trials):
                    for pc, target in observations:
                        regs, memory = adapter.initial(f, trial, ENTRY)
                        initial_table = f['case_starts'][1] if target == f['case_starts'][0] else f['case_starts'][0]
                        base.put(memory, base.DATA, initial_table, width)
                        base.put(regs, 0 if f['architecture'] == 'arm64' else 56, target)
                        base.put(regs, adapter.pc_offset, pc)
                        rejected = pc != ENTRY
                        incomplete = args.without_region and not rejected and target != EXTERNAL
                        if rejected:
                            expected = (bytes(regs), bytes(memory), collections.Counter(), (5,0,pc,pc,1,0,5))
                        else:
                            reference_fixture = f
                            if incomplete:
                                reference_fixture = copy.deepcopy(f)
                                reference_fixture['missing_targets'] += f['case_starts']
                            expected = adapter.reference(reference_fixture, regs, memory, pc)
                            if base.get(expected[1], base.DATA, width) != target:
                                raise RuntimeError('Unicorn did not execute Guest STORE before the target LOAD/JMP')
                            if not incomplete and target != EXTERNAL:
                                want = 7 if target == f['case_starts'][0] else 99
                                if base.get(expected[0], 0, result_width) != want or base.get(expected[1], base.DATA + 8, result_width) != want:
                                    raise RuntimeError('Unicorn did not execute the independently specified case effect')
                            code = bytes.fromhex(f['code_hex'])
                            if expected[1][ENTRY:ENTRY + len(code)] != code:
                                raise RuntimeError('reference changed fixed machine bytes')
                        actual, error = base.native(exe, regs, memory, work, trial,
                            extra_bytes=64, regs_size=adapter.regs_size)
                        folder = Path(overlay.snapshots[-1]['work'])
                        (folder / 'expected-registers.bin').write_bytes(expected[0])
                        (folder / 'expected-memory.bin').write_bytes(expected[1])
                        save(folder / 'expected-exit.json', list(expected[3]))
                        row = dict(case=f['id'], architecture=f['architecture'], opt=level,
                            trial=trial, pc=pc, target=target, rejected_entry=rejected,
                            initial_table_target=initial_table,
                            observation='rejected-entry' if rejected else 'ordinary-incomplete' if incomplete else 'external-transfer' if target == EXTERNAL else 'root-return',
                            snapshots=str(folder), full_state_comparison=True,
                            complete_root_comparison=not rejected and not incomplete and target != EXTERNAL)
                        if error:
                            row.update(status=error['kind'], detail=error)
                        else:
                            if args.control == 'result':
                                damaged = bytearray(actual[0]);damaged[0] ^= 1
                                actual = (bytes(damaged),) + actual[1:]
                            differences, record = adapter.compare(f, expected, actual, args.control == 'memory')
                            if bad_ir:
                                differences.append(dict(field='ir', detail='unproved unreachable'))
                            row.update(status=('different' if differences else 'incomplete' if incomplete else 'match'),
                                differences=differences, exit=list(record[:7]),
                                register_fields_compared=len(adapter.fields), memory_bytes_compared=base.MEM_SIZE)
                        rows.append(row)
        expected_rows = len(chosen) * 2 * args.trials * 5
        if args.control:
            def accepted(r):
                field = 'memory' if args.control == 'memory' else ('X0' if r['architecture'] == 'arm64' else 'EAX' if r['architecture'] == 'x86_32' else 'RAX')
                return r['status'] == 'different' and len(r.get('differences', [])) == 1 and r['differences'][0]['field'] == field
            ok = len(rows) == expected_rows and all(accepted(r) for r in rows)
        else:
            ok = len(rows) == expected_rows and all(r['status'] == 'match' for r in rows)
        status = 0 if ok else 1
    finally:
        d.LLVMLifter.lift = original_lift
        overlay.close()
        after = libraries(d)
        stable = after['sha256'] == pins['sha256']
        save(out / 'validation-provenance.json', dict(libraries_before=pins, libraries_after=after,
            stable=stable, modules=overlay.modules, native_snapshots=overlay.snapshots, Graphs=graphs,
            fixtures=chosen, source_sha256=digest(Path(__file__)),
            sources={p.name: digest(p) for p in [Path(study.__file__), Path(study.compiler.__file__),
                Path(__file__).with_name('control_flow_diff.py'), Path(__file__).with_name('static_region_replay.py'),
                Path(__file__).with_name('function_guest_diff.py'), Path(__file__).with_name('function_static_guest_diff.py')]}))
        if not stable:
            raise RuntimeError('actual loaded library bytes changed')
    summary = dict(ok=status == 0, without_region=args.without_region, cases=len(chosen),
        modules=len(overlay.modules), results=len(rows), statuses=dict(collections.Counter(r['status'] for r in rows)),
        root_returns=sum(r.get('exit',[0])[0] == 1 for r in rows),
        external_transfers=sum(r['observation'] == 'external-transfer' and r['status'] == 'match' for r in rows),
        rejected_entry_checks=sum(r['rejected_entry'] for r in rows),
        incomplete=sum(r['status'] == 'incomplete' for r in rows),
        exclusions=0, skips=0, normalizations=0, control=args.control,
        scope='Runtime table target and explicit decoded coverage; ordinary incomplete boundary checks are not complete-function semantic matches')
    save(out / 'results.json', dict(summary=summary, results=rows))
    print(json.dumps(summary, indent=2))
    return status


if __name__ == '__main__':
    raise SystemExit(main())
