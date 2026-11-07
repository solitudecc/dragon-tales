"""Legacy refusal contracts and unchanged full-state V2/static Guest oracles.

The two MOVBE fixtures have a real unsupported zero-op memory store. Frozen
old Legacy executes them and returns stale EAX/RAX and stale DATA bytes; that
is a counterexample, retains a nonzero exit, and is never a production pass.
New Legacy must reject them without changing the module. Rejection is counted
per module, never multiplied into fictitious per-trial CPU comparisons.

Legacy's void(mem, regs) ABI cannot report a machine PC/return exit. Its native
outputs are compared against Unicorn stopped BEFORE RET: every modelled field
except PC and all 64 KiB, with SP preserved and no RET-pop normalization. This
explicit Legacy ABI observation scope is separate from V2/static comparisons,
which retain every Guest field, PC/SP, memory and all seven exit fields.
"""
import argparse
import collections
import copy
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path


LEVELS = ('O0', 'O2')
ENTRY = 0x1000
VALUES = [0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0x01020304, 0x89abcdef, 0xff00ff00]


def fixtures():
    cases = []
    # PUSH changes SP and stack bytes before the marker; POP restores it only
    # after the marker. Both the known store and EAX input are real code.
    prefix = bytes.fromhex('52c7034433221189c8')
    suffix = bytes.fromhex('5a8b03c3')
    fences = bytes.fromhex('f3900faef00faee80faef8')
    for architecture in ('x86_32', 'x86_64'):
        for unsupported in (True, False):
            head = prefix if unsupported else fences + prefix
            instruction = bytes.fromhex('0f38f103' if unsupported else '8903')
            code = head + instruction + suffix
            cases.append(dict(id=architecture + ('_movbe_store' if unsupported else '_supported_store_fences'),
                architecture=architecture, expectation='unsupported' if unsupported else 'complete',
                code_hex=code.hex(), unsupported_before=[ENTRY + len(head)] if unsupported else [],
                ret_pc=ENTRY + len(code) - 1, undefined_flags=[], stubs=[],
                set_gprs={('ECX' if architecture == 'x86_32' else 'RCX'): VALUES}))
    return dict(schema=1, entry=ENTRY, cases=cases)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def accepted(fixture, mode, baseline):
    return mode != 'legacy' or baseline or fixture['expectation'] == 'complete'


def check_plan(data, rows, reports, trials, mode, baseline):
    wanted_reports = {(f['id'], level) for f in data['cases'] for level in LEVELS}
    wanted_rows = {(f['id'], level, trial) for f in data['cases']
                   if accepted(f, mode, baseline) for level in LEVELS for trial in range(trials)}
    report_keys = [(r.get('case'), r.get('opt')) for r in reports]
    row_keys = [(r.get('case'), r.get('opt'), r.get('trial')) for r in rows]
    errors = []
    if len(report_keys) != len(set(report_keys)) or set(report_keys) != wanted_reports:
        errors.append('module contracts do not cover the exact fixture/optimization plan')
    if len(row_keys) != len(set(row_keys)) or set(row_keys) != wanted_rows:
        errors.append('CPU rows do not cover the exact accepted fixture/optimization/trial plan')
    if any(type(r.get('trial')) is not int for r in rows):
        errors.append('non-integer or missing trial')
    by_id = {f['id']: f for f in data['cases']}
    for report in reports:
        f = by_id.get(report.get('case'))
        if f is None or type(report.get('accepted')) is not bool or report['accepted'] != accepted(f, mode, baseline):
            errors.append('unexpected lift acceptance/refusal')
        if report.get('error'):
            errors.append('module infrastructure error retained')
        if report.get('unlifted') != report.get('expected_unlifted'):
            errors.append('unlifted instruction set differs from the fixture')
        if report.get('accepted') is False and report.get('module_unchanged') is not True:
            errors.append('refusal changed the module')
    return dict(ok=not errors, errors=errors, expected_module_contracts=len(wanted_reports),
                expected_cpu_comparisons=len(wanted_rows), actual_module_contracts=len(reports),
                actual_cpu_comparisons=len(rows))


def single_control(row, control):
    differences = row.get('differences', [])
    field = 'memory' if control == 'memory' else ('EAX' if row.get('architecture') == 'x86_32' else 'RAX')
    return (row.get('status') == 'different' and len(differences) == 1 and
            differences[0].get('field') == field and
            (control != 'memory' or differences[0].get('count') == 1))


def self_test():
    data = fixtures()
    reports = [dict(case=f['id'], opt=level, accepted=accepted(f, 'legacy', False),
                    unlifted=f['unsupported_before'], expected_unlifted=f['unsupported_before'],
                    module_unchanged=True) for f in data['cases'] for level in LEVELS]
    rows = [dict(case=f['id'], architecture=f['architecture'], opt=level, trial=trial, status='match')
            for f in data['cases'] if accepted(f, 'legacy', False) for level in LEVELS for trial in range(2)]
    checks = dict(valid_plan=check_plan(data, rows, reports, 2, 'legacy', False)['ok'])
    checks['missing_whole_case_rejected'] = not check_plan(data, rows[:-4], reports, 2, 'legacy', False)['ok']
    damaged = copy.deepcopy(rows); damaged[1] = copy.deepcopy(damaged[0])
    checks['duplicate_trial_rejected'] = not check_plan(data, damaged, reports, 2, 'legacy', False)['ok']
    checks['missing_module_refusal_rejected'] = not check_plan(data, rows, reports[1:], 2, 'legacy', False)['ok']
    damaged = copy.deepcopy(reports); damaged[0]['accepted'] = True
    checks['unsupported_acceptance_rejected'] = not check_plan(data, rows, damaged, 2, 'legacy', False)['ok']
    damaged = copy.deepcopy(reports); damaged[0]['module_unchanged'] = False
    checks['refusal_mutation_rejected'] = not check_plan(data, rows, damaged, 2, 'legacy', False)['ok']
    damaged = copy.deepcopy(reports); damaged[0]['unlifted'] = []
    checks['lost_marker_rejected'] = not check_plan(data, rows, damaged, 2, 'legacy', False)['ok']
    row = dict(status='different', architecture='x86_64', differences=[dict(field='memory', count=1)])
    checks['single_byte_control_accepted'] = single_control(row, 'memory')
    row['differences'][0]['count'] = 2
    checks['multiple_byte_control_rejected'] = not single_control(row, 'memory')
    return dict(ok=all(checks.values()), checks=checks, passed=sum(checks.values()), total=len(checks))


def libraries(dragon, wanted):
    maps = [line for line in Path('/proc/self/maps').read_text().splitlines() if '/libdragon' in line]
    assert maps and not any('(deleted)' in line for line in maps)
    paths = {str(Path(line.split()[-1]).resolve()) for line in maps}
    assert str(Path(dragon._LIB).resolve()) in paths
    actual = {path: digest(path) for path in sorted(paths)}
    assert len(actual) == 2 and {Path(p).name for p in actual} == set(wanted)
    assert {Path(p).name: value for p, value in actual.items()} == wanted, 'actual mapped DSO hash mismatch'
    return dict(libraries=actual, maps=maps)


def native_dependencies(executable, expected, work, mode):
    run = subprocess.run(['ldd', str(executable)], capture_output=True, text=True)
    (work / 'native-ldd.stdout').write_text(run.stdout)
    (work / 'native-ldd.stderr').write_text(run.stderr)
    assert run.returncode == 0, 'native ldd failed'
    paths = set()
    for line in run.stdout.splitlines():
        for field in line.split():
            if field.startswith('/') and Path(field).name in ('libdragon.so', 'libdragon_c.so'):
                paths.add(str(Path(field).resolve()))
    actual = {path: digest(path) for path in sorted(paths)}
    assert actual == (expected if mode == 'v2' else {}), 'native DSO dependencies differ from mapped build'
    return dict(libraries=actual, returncode=run.returncode, stdout_sha256=digest(work / 'native-ldd.stdout'))


def compile_legacy(f, level, work, args, guest, report):
    d = guest.base.dragon
    arch = 1 if f['architecture'] == 'x86_32' else 2
    graph = d.Disassembler(arch, d.Configuration(strategy=args.strategy)).analyze(bytes.fromhex(f['code_hex']), ENTRY)
    function = graph.get_function(ENTRY)
    assert function and function.is_valid, 'invalid Legacy fixture function'
    unlifted = sorted({pc for block in function.block_addresses()
                       for pc in graph.get_ignil_block(block).unlifted()})
    report.update(unlifted=unlifted)
    with d.LLVMLifter(arch) as lifter:
        before = lifter.ir()
        result = lifter.lift(function, graph)
        after = lifter.ir()
    report.update(accepted=result, module_unchanged=after == before)
    (work / 'before.ll').write_text(before)
    (work / 'raw.ll').write_text(after)
    if not result:
        return None
    name = f'sub_{ENTRY:x}'
    header = re.search(r'define void @' + name + r'\(([^)]*)\)', after)
    assert header and len(header[1].split(',')) == 2
    assert all(a.strip().startswith('ptr ') for a in header[1].split(',')), 'unexpected Legacy ABI'
    raw = work / 'raw.ll'
    subprocess.run([args.opt, '-passes=verify', '-disable-output', str(raw)], capture_output=True, check=True)
    source = raw
    if level == 'O2':
        source = work / 'optimized.ll'
        subprocess.run([args.opt, '-S', '-passes=default<O2>', str(raw), '-o', str(source)], capture_output=True, check=True)
    subprocess.run([args.opt, '-passes=verify', '-disable-output', str(source)], capture_output=True, check=True)
    assert 'unreachable' not in source.read_text()
    driver = work / 'driver.c'
    driver.write_text(guest.base.DRIVER.replace('ROOT', name).replace('STUBS', ''))
    triple = {'aarch64': 'aarch64-linux-gnu', 'x86_64': 'x86_64-linux-gnu'}[platform.machine()]
    obj = work / 'module.o'; exe = work / 'native'
    subprocess.run([args.llc, '-O0', '-filetype=obj', '-mtriple=' + triple, str(source), '-o', str(obj)], capture_output=True, check=True)
    subprocess.run([args.cc, '-O0', str(driver), str(obj), '-o', str(exe)], capture_output=True, check=True)
    return exe


def main():
    if sys.flags.optimize:
        raise RuntimeError('run without Python -O/PYTHONOPTIMIZE; validation assertions are required')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('legacy', 'v2', 'static', 'product'), default='product')
    parser.add_argument('--strategy', choices=(0, 1), type=int, default=0)
    parser.add_argument('--static-entry', choices=('function', 'resumable'), default='function')
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--control', choices=('result', 'memory'))
    parser.add_argument('--expect-control', action='store_true')
    parser.add_argument('--expect-backend-sha256')
    parser.add_argument('--expect-shim-sha256')
    parser.add_argument('--trials', type=int, default=8)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--opt', default='opt-21'); parser.add_argument('--llc', default='llc-21'); parser.add_argument('--cc', default='gcc')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        result = self_test(); print(json.dumps(result, sort_keys=True)); return 0 if result['ok'] else 1
    if args.out is None or args.trials < 8:
        parser.error('--out and at least eight edge trials are required')
    if args.baseline and args.control or bool(args.control) != args.expect_control:
        parser.error('use matching control/expect-control and no baseline controls')
    if any(not re.fullmatch(r'[0-9a-f]{64}', value or '') for value in (args.expect_backend_sha256, args.expect_shim_sha256)):
        parser.error('pin both actual libraries with --expect-backend-sha256 and --expect-shim-sha256')
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    assert not list(out.iterdir()), 'use a fresh output directory'
    if os.environ.get('DRAGON_TEST_PYTHON'):
        sys.path.insert(0, os.environ['DRAGON_TEST_PYTHON']); __import__('dragon')
    import function_guest_diff as guest
    from function_static_guest_diff import StaticAdapter
    d = guest.base.dragon
    expected_libraries = {'libdragon.so': args.expect_backend_sha256, 'libdragon_c.so': args.expect_shim_sha256}
    before = libraries(d, expected_libraries)
    data = fixtures(); fixture_path = out / 'fixtures.json'
    fixture_path.write_text(json.dumps(data, indent=2))
    reports, rows = [], []
    original_config = d.Configuration.__init__
    original_analyze = d.Disassembler.analyze
    active = {}

    def configuration(self, *a, **kw):
        kw['strategy'] = args.strategy; original_config(self, *a, **kw)

    def analyze(self, code, address=0, *a, **kw):
        graph = original_analyze(self, code, address, *a, **kw)
        if address == ENTRY:
            function = graph.get_function(ENTRY)
            assert function and function.is_valid
            active['report']['unlifted'] = sorted({pc for block in function.block_addresses()
                for pc in graph.get_ignil_block(block).unlifted()})
        return graph

    d.Configuration.__init__ = configuration
    d.Disassembler.analyze = analyze
    try:
        for f in data['cases']:
            adapter = (guest.Adapter if args.mode in ('legacy', 'v2') else StaticAdapter)(f['architecture'])
            for level in LEVELS:
                work = out / f['id'] / level; work.mkdir(parents=True)
                report = dict(case=f['id'], opt=level, architecture=f['architecture'], accepted=None,
                              expected_unlifted=f['unsupported_before'])
                reports.append(report); active['report'] = report
                try:
                    if args.mode == 'legacy':
                        exe = compile_legacy(f, level, work, args, guest, report)
                    else:
                        exe, bad_ir, _ = guest.compile.compile_case(f, ENTRY, level, work, args,
                            architecture=f['architecture'], static_analysis=args.mode != 'v2',
                            product_pipeline=args.mode == 'product',
                            function_entry=args.mode != 'v2' and args.static_entry == 'function')
                        assert not bad_ir, 'unreachable in fixture lowering'
                        report['accepted'] = True
                    if exe is None:
                        continue
                    report['native_dependencies'] = native_dependencies(exe, before['libraries'], work, args.mode)
                    report['native_sha256'] = digest(exe)
                except Exception as error:
                    report['error'] = repr(error)
                    if isinstance(error, subprocess.CalledProcessError):
                        report['stdout'] = (error.stdout or b'').decode(errors='replace')
                        report['stderr'] = (error.stderr or b'').decode(errors='replace')
                    continue
                for trial in range(args.trials):
                    row = dict(case=f['id'], architecture=f['architecture'], opt=level, trial=trial)
                    try:
                        regs, memory = adapter.initial(f, trial, ENTRY)
                        reference_fixture = dict(f, unsupported_before=[f['ret_pc']]) if args.mode == 'legacy' else f
                        expected = adapter.reference(reference_fixture, regs, memory, ENTRY)
                        actual, error = guest.base.native(exe, regs, memory, work, trial,
                            extra_bytes=0 if args.mode == 'legacy' else 64, regs_size=adapter.regs_size)
                        if error:
                            row.update(status=error['kind'], detail=error)
                        else:
                            if args.control:
                                slot = 0 if args.control == 'result' else 1
                                damaged = bytearray(actual[slot]); damaged[0 if slot == 0 else guest.base.DATA] ^= 1
                                actual = actual[:slot] + (bytes(damaged),) + actual[slot + 1:]
                            if args.mode == 'legacy':
                                fields = [x for x in adapter.fields if x[0] != 'PC']
                                diffs = [dict(field=n, got=hex(guest.base.get(actual[0], o, w)), expected=hex(guest.base.get(expected[0], o, w)))
                                    for n, o, w in fields if actual[0][o:o+w] != expected[0][o:o+w]]
                                addresses = [i for i, (got, want) in enumerate(zip(actual[1], expected[1])) if got != want]
                                if addresses:
                                    diffs.append(dict(field='memory', count=len(addresses), examples=[dict(address=hex(i), got=actual[1][i], expected=expected[1][i]) for i in addresses[:8]]))
                                assert actual[2] == expected[2], 'unexpected call events'
                                row.update(reference_boundary=list(expected[3]), legacy_observation='before RET; PC outside Legacy ABI')
                            else:
                                fields = adapter.fields
                                diffs, record = adapter.compare(f, expected, actual, False)
                                row['exit'] = list(record[:7])
                            row.update(status='different' if diffs else 'match', differences=diffs,
                                register_fields_compared=len(fields), memory_bytes_compared=guest.base.MEM_SIZE,
                                undefined_flag_fields=0, normalization_fields=0)
                    except Exception as error:
                        row.update(status='execution_or_reference_error', detail=repr(error))
                    rows.append(row)
    finally:
        d.Configuration.__init__ = original_config
        d.Disassembler.analyze = original_analyze
    after = libraries(d, expected_libraries)
    assert before['libraries'] == after['libraries']
    plan = check_plan(data, rows, reports, args.trials, args.mode, args.baseline)
    fixture_map = {f['id']: f for f in data['cases']}
    def boundary(row):
        f = fixture_map[row['case']]
        return row.get('exit') == [5, 0, f['unsupported_before'][0], f['unsupported_before'][0], 1, 0, 2]
    def root(row):
        f = fixture_map[row['case']]
        return row.get('exit') == [1, 3, f['ret_pc'], guest.base.SENTINEL, 1, 0, 0]
    def legacy_wrong(row):
        differences = row.get('differences', [])
        return row.get('status') == 'different' and {x['field'] for x in differences} == {
            'EAX' if row['architecture'] == 'x86_32' else 'RAX', 'memory'} and len(differences) == 2 and next(
                x for x in differences if x['field'] == 'memory')['count'] == 4 and {
                int(x['address'], 16) for x in next(x for x in differences if x['field'] == 'memory')['examples']} == set(range(guest.base.DATA, guest.base.DATA + 4))
    contract_rows = [r for r in rows if fixture_map[r['case']]['expectation'] == 'unsupported']
    positive_rows = [r for r in rows if fixture_map[r['case']]['expectation'] == 'complete']
    errors = [r for r in rows if r.get('status') not in ('match', 'different')]
    module_errors = sum(bool(r.get('error')) for r in reports)
    if args.mode == 'legacy':
        boundaries_ok = all(r.get('reference_boundary') == [5, 0, fixture_map[r['case']]['ret_pc'], fixture_map[r['case']]['ret_pc'], 1, 0, 2] for r in rows)
    else:
        boundaries_ok = all(boundary(r) for r in contract_rows) and all(root(r) for r in positive_rows)
    controls_ok = all(single_control(r, args.control) for r in rows) if args.control else True
    baseline_evidence = (args.mode == 'legacy' and args.baseline and plan['ok'] and not errors and boundaries_ok and
                         all(legacy_wrong(r) for r in contract_rows) and bool(contract_rows) and
                         all(r['status'] == 'match' for r in positive_rows))
    ok = bool(plan['ok'] and not errors and boundaries_ok and controls_ok and
              all(r['status'] == ('different' if args.control else 'match') for r in rows))
    result = dict(ok=ok, baseline_expected_counterexamples_observed=baseline_evidence,
        mode=args.mode, strategy=args.strategy, static_entry=args.static_entry, baseline=args.baseline,
        control=args.control, fixture_sha256=digest(fixture_path), module_contracts=len(reports),
        rejected_module_contracts=sum(r.get('accepted') is False for r in reports),
        cpu_comparisons=len(rows), trials_per_optimization=args.trials,
        statuses=dict(collections.Counter(r.get('status') for r in rows)), errors=len(errors) + module_errors,
        module_errors=module_errors, execution_or_reference_errors=len(errors),
        legacy_actual_counterexamples=sum(legacy_wrong(r) for r in contract_rows) if args.mode == 'legacy' else 0,
        legacy_PC_outside_ABI_observations=len(rows) if args.mode == 'legacy' else 0,
        legacy_normalization_fields=0, undefined_flag_fields=0, skips=0,
        full_Guest_boundary_matches=sum(r['status'] == 'match' and boundary(r) for r in contract_rows) if args.mode != 'legacy' else 0,
        complete_root_matches=sum(r['status'] == 'match' and root(r) for r in positive_rows) if args.mode != 'legacy' else 0,
        single_field_controls=sum(single_control(r, args.control) for r in rows) if args.control else 0,
        control_application='comparison-only; native snapshot files are preserved unchanged' if args.control else None,
        register_fields_compared=sum(r.get('register_fields_compared', 0) for r in rows),
        memory_bytes_compared=sum(r.get('memory_bytes_compared', 0) for r in rows),
        plan_audit=plan, libraries_before=before, libraries_after=after,
        source_sha256={str(p): digest(p) for p in (Path(__file__), Path(guest.__file__), Path(guest.compile.__file__),
            Path(guest.base.__file__), Path(sys.modules['function_static_guest_diff'].__file__), Path(d.__file__))},
        tools={command: subprocess.run([command, '--version'], capture_output=True, text=True, check=True).stdout.splitlines()[0]
               for command in (args.opt, args.llc, args.cc)},
        self_test=self_test(), semantics='Legacy pre-RET ABI observations or honest module refusal; V2/static full Guest roots or exact pre-instruction boundaries; baseline counterexamples never production passes')
    (out / 'results.json').write_text(json.dumps(dict(summary=result, reports=reports, results=rows), indent=2))
    print(json.dumps(result, sort_keys=True))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
