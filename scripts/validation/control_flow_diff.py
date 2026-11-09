"""Append the opt-in control-flow pass to the unchanged static-entry oracle.

All static_entry_diff arguments remain available, including --case, --control
and --expect-control. Before/default/after IR and the actual pass report are
retained for each module. This validates Guest effects and entry contracts;
it does not assert that a flattening dispatcher was eliminated.

--v2-budget-only instead checks the existing three one-block *_ret fixtures
with budgets 0/1/2/3. The V2 dispatcher and Guest block each consume one unit:
0/1 stop before Guest effects, 2/3 return with precisely 0/1 units remaining.
The complete original field/memory/Exit comparison remains in use. This mode
also accepts --control budget --expect-control, in addition to result/memory.
--without-control-flow is an explicit default-pipeline comparison run.
--self-test exercises only the small metadata/budget checker, without Dragon,
Unicorn, native compilation, or execution.
"""
import argparse
import collections
import copy
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path


def write_json(path, value):
    with path.open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def budget_difference(extra, expected):
    if len(extra) != 64:
        raise ValueError('invalid original V2 Exit/budget protocol')
    actual = struct.unpack('<8Q', extra)[7]
    return ([dict(field='budget_left', got=actual, expected=expected)]
            if actual != expected else [])


def check_report(report):
    runs = report.get('runs', [])
    if (report.get('ok') is not True or report.get('budget_exhausted') is not False
            or len(runs) != 1 or runs[0].get('name') != 'control-flow'
            or type(runs[0].get('status')) is not int or runs[0]['status'] != 0):
        raise RuntimeError('control-flow pass was not executed successfully')


def self_test():
    good = dict(ok=True, budget_exhausted=False,
                runs=[dict(name='control-flow', status=0)])
    check_report(good)
    rejected = 0
    for change in (dict(ok=False), dict(budget_exhausted=True), dict(runs=[]),
                   dict(runs=[dict(name='dce', status=0)]),
                   dict(runs=[dict(name='control-flow', status=True)]),
                   dict(runs=[dict(name='control-flow', status=2)])):
        try:
            check_report({**good, **change})
        except RuntimeError:
            rejected += 1
        else:
            raise RuntimeError('bad pass report was accepted')
    for budget in (0, 1, 2, 3):
        expected = max(0, budget - 2)
        if budget_difference(struct.pack('<8Q', *([0] * 7), expected), expected):
            raise RuntimeError('correct budget rejected')
        differences = budget_difference(
            struct.pack('<8Q', *([0] * 7), expected ^ 1), expected)
        if len(differences) != 1 or differences[0]['field'] != 'budget_left':
            raise RuntimeError('single-budget-field control was missed')
    for bad in (b'', bytes(63), bytes(65)):
        try:
            budget_difference(bad, 0)
        except ValueError:
            rejected += 1
        else:
            raise RuntimeError('malformed snapshot was accepted')
    print(json.dumps(dict(self_test=True, pass_report_positive=1,
                          budget_positive=4, single_budget_controls=4,
                          malformed_or_failed_report_rejections=rejected,
                          Dragon_Unicorn_LLVM_native_executions=0)))
    return 0


def libraries(dragon):
    lines = [line for line in Path('/proc/self/maps').read_text().splitlines()
             if '/libdragon.so' in line or '/libdragon_c.so' in line]
    if not lines or any('(deleted)' in line for line in lines):
        raise RuntimeError('actual loaded Dragon maps are unavailable/deleted')
    paths = {Path(line.split()[-1]).resolve() for line in lines}
    if {p.name for p in paths} != {'libdragon.so', 'libdragon_c.so'} or len(paths) != 2:
        raise RuntimeError('expected one actual shim/backend pair')
    if Path(dragon._LIB).resolve() not in paths:
        raise RuntimeError('Python shim identity differs from actual maps')
    return dict(maps=lines, sha256={str(p): digest(p) for p in sorted(paths)})


class Overlay:
    """Only orchestration hooks; existing lifter, compiler and compare stay intact."""
    def __init__(self, study, enabled):
        self.study, self.enabled = study, enabled
        self.dragon = study.base.dragon
        self.compile = study.compiler.compile_case
        self.optimize = self.dragon.LLVMLifter.optimize
        self.native = study.base.native
        self.work = None
        self.modules, self.snapshots = [], []

    def install(self):
        owner = self

        def compile_case(*args, **kwargs):
            if owner.work is not None:
                raise RuntimeError('nested compiler invocation')
            owner.work = Path(args[3])
            before_count = len(owner.modules)
            try:
                result = owner.compile(*args, **kwargs)
                if len(owner.modules) != before_count + 1:
                    raise RuntimeError('product optimization hook did not run once')
                if (owner.work / 'raw.ll').read_bytes() != (owner.work / 'control-flow-after.ll').read_bytes():
                    raise RuntimeError('compiled raw IR differs from audited post-pass IR')
                linked = subprocess.run(['ldd', str(result[0])], capture_output=True, check=True)
                (owner.work / 'control-flow-ldd.txt').write_bytes(linked.stdout)
                return result
            finally:
                owner.work = None

        def optimize(lifter, pipeline=None):
            if pipeline is not None:
                return owner.optimize(lifter, pipeline)
            if owner.work is None:
                raise RuntimeError('optimization has no current fixture module')
            work = owner.work
            (work / 'control-flow-before-default.ll').write_text(lifter.ir())
            owner.optimize(lifter)  # unchanged compatible six-step pipeline
            (work / 'control-flow-before.ll').write_text(lifter.ir())
            if owner.enabled:
                with owner.dragon.PassPipeline(['control-flow']) as steps:
                    report = steps.run(lifter, max_iterations=1, max_pass_runs=1)
                check_report(report)
            else:
                report = dict(control_flow_applied=False,
                              scope='explicit unchanged-default comparison run')
            (work / 'control-flow-after.ll').write_text(lifter.ir())
            write_json(work / 'control-flow-report.json', report)
            owner.modules.append(dict(work=str(work), applied=owner.enabled,
                before_default_sha256=digest(work / 'control-flow-before-default.ll'),
                before_sha256=digest(work / 'control-flow-before.ll'),
                after_sha256=digest(work / 'control-flow-after.ll'), report=report))

        def native(exe, regs, memory, work, trial, **kwargs):
            # The old static oracle reuses trial filenames for different PCs.
            # Keep every original protocol snapshot in a distinct directory.
            folder = Path(work) / 'control-flow-snapshots' / str(len(owner.snapshots)).zfill(6)
            folder.mkdir(parents=True)
            result = owner.native(exe, regs, memory, folder, trial, **kwargs)
            owner.snapshots.append(dict(work=str(folder), exe=str(exe), trial=trial,
                input_sha256=digest(folder / f'input-{trial}.bin'),
                output_sha256=(digest(folder / f'output-{trial}.bin')
                               if (folder / f'output-{trial}.bin').exists() else None)))
            return result

        self.study.compiler.compile_case = compile_case
        self.dragon.LLVMLifter.optimize = optimize
        self.study.base.native = native

    def close(self):
        self.study.compiler.compile_case = self.compile
        self.dragon.LLVMLifter.optimize = self.optimize
        self.study.base.native = self.native


def budget_main(study, argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--case', action='append', dest='cases')
    parser.add_argument('--trials', type=int, default=8)
    parser.add_argument('--control', choices=('result', 'memory', 'budget'))
    parser.add_argument('--expect-control', action='store_true')
    for command in ('opt', 'llc', 'cc'):
        parser.add_argument('--' + command, default=command + '-21' if command != 'cc' else 'gcc')
    args = parser.parse_args(argv)
    if args.trials < 1 or bool(args.control) != args.expect_control:
        parser.error('positive trials and matching control flags required')
    fixtures = [f for f in study.fixtures() if f['id'] in
                {'x86_32_ret', 'x86_64_ret', 'arm64_ret'}]
    if args.cases:
        fixtures = [f for f in fixtures if f['id'] in args.cases]
    if not fixtures or args.cases and {f['id'] for f in fixtures} != set(args.cases):
        parser.error('budget mode accepts existing x86_32_ret/x86_64_ret/arm64_ret cases')
    code = {'x86_32': 'c70307000000c3', 'x86_64': 'c70307000000c3',
            'arm64': 'e00080d2c0035fd6'}
    rows = []
    for fixture in fixtures:
        if fixture['code_hex'] != code[fixture['architecture']] or fixture['stubs'] or fixture['undefined_flags']:
            raise RuntimeError('one-block budget fixture premise drift')
        adapter = study.StaticAdapter(fixture['architecture'])
        for budget in (0, 1, 2, 3):
            f = copy.deepcopy(fixture)
            f['context_budget'] = budget
            for level in ('O0', 'O2'):
                work = args.out / f['id'] / ('budget-' + str(budget)) / level
                work.mkdir(parents=True, exist_ok=True)
                exe, bad_ir, _ = study.compiler.compile_case(f, study.ENTRY, level, work, args,
                    architecture=f['architecture'], product_pipeline=True)
                for trial in range(args.trials):
                    regs, memory = adapter.initial(f, trial, study.ENTRY)
                    expected = (adapter.reference(f, regs, memory, study.ENTRY) if budget >= 2
                        else (bytes(regs), bytes(memory), collections.Counter(),
                              (6, 0, study.ENTRY, study.ENTRY, 1, 0, 0)))
                    actual, error = study.base.native(exe, regs, memory, work, trial,
                        extra_bytes=64, regs_size=adapter.regs_size)
                    row = dict(case=f['id'], architecture=f['architecture'], opt=level,
                        budget=budget, trial=trial, reference='Unicorn' if budget >= 2 else 'explicit-no-Guest-execution-contract')
                    if error:
                        row.update(status=error['kind'], detail=error)
                    else:
                        if args.control == 'result':
                            value = bytearray(actual[0]);value[0] ^= 1
                            actual = (bytes(value),) + actual[1:]
                        elif args.control == 'budget':
                            value = bytearray(actual[3]);value[56] ^= 1
                            actual = actual[:3] + (bytes(value),)
                        differences, record = adapter.compare(f, expected, actual, args.control == 'memory')
                        differences += budget_difference(actual[3], max(0, budget - 2))
                        if bad_ir:
                            differences.append(dict(field='ir', detail='unproved unreachable'))
                        row.update(status='different' if differences else 'match', differences=differences,
                            exit=list(record[:7]), budget_left=record[7],
                            register_fields_compared=len(adapter.fields), memory_bytes_compared=study.base.MEM_SIZE)
                    rows.append(row)
    expected_field = ('budget_left' if args.control == 'budget' else 'memory' if args.control == 'memory' else None)
    def accepted(row):
        if not args.control:
            return row['status'] == 'match'
        field = expected_field or ('X0' if row['architecture'] == 'arm64' else 'EAX' if row['architecture'] == 'x86_32' else 'RAX')
        return row['status'] == 'different' and len(row.get('differences', [])) == 1 and row['differences'][0]['field'] == field
    ok = len(rows) == len(fixtures) * 4 * 2 * args.trials and all(accepted(r) for r in rows)
    summary = dict(ok=ok, cases=len(fixtures), results=len(rows), statuses=dict(collections.Counter(r['status'] for r in rows)),
        root_returns=sum(r.get('exit', [0])[0] == 1 for r in rows),
        budget_boundaries=sum(r.get('exit', [0])[0] == 6 for r in rows),
        exclusions=0, skips=0, normalizations=0, control=args.control,
        scope='Exact V2 dispatcher/block budget contract plus unchanged all-field/64KiB/Exit comparison; not a flattening effectiveness test')
    write_json(args.out / 'budget-results.json', dict(summary=summary, results=rows))
    print(json.dumps(summary, indent=2))
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--v2-budget-only', action='store_true')
    parser.add_argument('--without-control-flow', action='store_true')
    mode, remaining = parser.parse_known_args()
    if mode.self_test:
        return self_test()
    import static_entry_diff as study
    if '-h' in remaining or '--help' in remaining:
        print(__doc__)
        if mode.v2_budget_only:
            return budget_main(study, remaining)
        sys.argv = [sys.argv[0], *remaining]
        return study.main()
    out_parser = argparse.ArgumentParser(add_help=False)
    out_parser.add_argument('--out', type=Path, required=True)
    out, _ = out_parser.parse_known_args(remaining)
    out.out.mkdir(parents=True, exist_ok=True)
    before = libraries(study.base.dragon)
    overlay = Overlay(study, not mode.without_control_flow)
    overlay.install()
    result = None
    try:
        if mode.v2_budget_only:
            result = budget_main(study, remaining)
        else:
            sys.argv = [sys.argv[0], *remaining]
            result = study.main()
        return result
    finally:
        overlay.close()
        after = libraries(study.base.dragon)
        if before['sha256'] != after['sha256']:
            raise RuntimeError('actual loaded library bytes changed during validation')
        source = Path(__file__).resolve()
        write_json(out.out / 'control-flow-validation.json', dict(returncode=result,
            mode='V2-exact-budget' if mode.v2_budget_only else 'existing-static-entry-oracle',
            control_flow_applied=overlay.enabled, source_sha256=digest(source),
            libraries_before=before, libraries_after=after, modules=overlay.modules,
            native_snapshots=overlay.snapshots, oracle_and_compare_unchanged=True,
            source_pins={p.name: digest(p) for p in [source,
                source.with_name('static_entry_diff.py'), source.with_name('function_dispatch_diff.py'),
                source.with_name('function_guest_diff.py'), source.with_name('function_static_guest_diff.py'),
                source.with_name('function_llvm_diff.py'), source.parents[2] / 'tests/fixtures/llvm-guest-flow.json']},
            scope='Guest/entry/effect compatibility and exact-budget regression only; pass execution or IR size change is not proof of de-flattening'))


if __name__ == '__main__':
    raise SystemExit(main())
