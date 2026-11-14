"""Portable one-ELF decoded-region IR baseline; no native/C/UC/checked passes.

Invocation: python3 THIS --plan PLAN --plan-sha256 SHA --out NEW_DIRECTORY
PLAN is a newly frozen portable binding, not an edited legacy replay plan:
  schema: dragon-portable-decoded-region-plan-v1; ready: true
  script/build_manifest/coverage_manifest/python: {path, sha256}
  transfer: {path, sha256, root}  # the selected TRANSFER-MANIFEST, unchanged
  source: {root, files: {relative_path: sha256}}
  runtime: {backend: {path, sha256}, shim: {path, sha256},
            library_path: [explicit absolute directories]}
  solver: {enabled: true, library: {path, sha256}}  # false + null for no-Z3
  tools: {opt: {path, sha256}}
  case: {family: flatten-switch, c_level: O2, entry_symbol: oracle_entry,
         kernel_symbol: kernel}
No path is inferred by replacing a legacy HOME prefix.  All hashes stream and
work on Python 3.10.  The imported region helper's file_digest function is never
called. --local-checks OUT runs only stdlib/local saved-IR controls, no Dragon.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
START, FLOOR, SECONDS = 512 * 1024**2, 64 * 1024**2, 180
CFG_SOURCE_SHA = '3c399e7e327883836f341a41001974819990d39aa6f77462bc3f4304c15729ba'
SOURCE_FILES = {'python/dragon.py', 'c_shim/dragon_c.h',
                'include/dragon/lifters/llvm/Execution.h',
                'scripts/validation/static_region_replay.py',
                'scripts/validation/progressive_deobfuscation_baseline.py'}
CASE = dict(family='flatten-switch', c_level='O2', entry_symbol='oracle_entry',
            kernel_symbol='kernel')


def require(value, message):
    if not value:
        raise RuntimeError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def read(path):
    return json.loads(Path(path).read_text())


def pinned(row, pins, allow_symlink=False):
    p = Path(row['path'])
    require(p.is_absolute() and p.is_file() and (allow_symlink or not p.is_symlink()),
            'unsafe pin: ' + str(p))
    require(sha(p) == row['sha256'], 'pin hash differs: ' + str(p))
    pins[str(p)] = row['sha256']
    return p


def module_at(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None, 'module import unavailable')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def transfer_files(plan, pins):
    """Verify only the explicitly selected set; legacy full manifest is data."""
    transfer_path = pinned(plan['transfer'], pins)
    transfer, root = read(transfer_path), Path(plan['transfer']['root'])
    require(root.is_absolute(), 'input root must be absolute')
    require(transfer['schema'] == 'dragon-progressive-flatten-rock5b-input-transfer-v1',
            'wrong selected input schema')
    rows = {}
    for row in transfer['files']:
        rel = PurePosixPath(row['destination_relative'])
        require(not rel.is_absolute() and '..' not in rel.parts and str(rel) not in rows,
                'unsafe/duplicate selected member')
        path = root / str(rel)
        require(path.resolve().is_relative_to(root.resolve()), 'input escapes explicit root')
        pinned(dict(path=str(path), sha256=row['sha256']), pins)
        require(path.stat().st_size == row['size'], 'selected input size differs')
        rows[str(rel)] = dict(path=path, **row)
    require(len(rows) == transfer['payload_files'] == 50, 'selected input set differs')
    require(sum(r['size'] for r in rows.values()) == transfer['logical_bytes'],
            'selected logical bytes differ')
    return transfer, rows


def attributes(text, symbol):
    header = re.search(r'^define void @' + re.escape(symbol) + r'\([^\n]*\) #(\d+)', text, re.M)
    require(header is not None, 'accepted region function absent')
    group = re.search(r'^attributes #' + header[1] + r' = (\{[^\n]*\})', text, re.M)
    require(group is not None, 'region attribute group absent')
    values = dict(re.findall(r'"(dragon\.[^"]+)"="([^"]*)"', group[1]))
    require(values.get('dragon.static-coverage') == 'decoded-region'
            and values.get('dragon.coverage-source') == 'decoded-machine'
            and values.get('dragon.static-entry-policy') == 'function-start',
            'region contract attributes differ')
    covered = {int(x, 16) for x in values.get('dragon.coverage-blocks', '').split(',') if x}
    return dict(group=int(header[1]), text=group[1], values=values,
                coverage_blocks=sorted(covered))


def load_switches(text, symbol):
    """Exact raw SSA LOAD-driven switch cases; does not infer table contents."""
    active, block, collecting, operand, result = False, None, None, None, []
    loads = {}
    for line in text.splitlines():
        if line.startswith('define '):
            active = bool(re.match(r'define void @' + re.escape(symbol) + r'\(', line))
        if not active:
            continue
        if line == '}':
            require(collecting is None, 'unterminated coverage switch')
            active = False
            continue
        label = re.match(r'^([-a-zA-Z$._0-9]+):', line)
        if label:
            block = label[1]
        load = re.match(r'\s*(%[-a-zA-Z$._0-9]+) = load i(\d+)\b', line)
        if load:
            loads[load[1]] = dict(width=int(load[2]), instruction=line.strip())
        switch = re.match(r'\s*switch i(\d+) (%[-a-zA-Z$._0-9]+), label %[^ ]+ \[\s*$', line)
        if switch:
            require(collecting is None, 'nested switch parse')
            operand = switch[2]
            collecting = dict(block=block, operand=operand, width=int(switch[1]),
                              cases=[], source_load=loads.get(operand))
        elif collecting is not None:
            if line.strip().startswith(']'):
                result.append(collecting)
                collecting = None
            else:
                case = re.fullmatch(r'\s*i\d+ (-?\d+), label %[-a-zA-Z$._0-9]+\s*', line)
                require(case is not None, 'unsupported coverage case syntax')
                collecting['cases'].append(int(case[1]) % (1 << collecting['width']))
    return result


def coverage(text, symbol, attr, graph, oracle):
    targets = set(oracle['table']['targets'])
    decoded = {r['address']: r for r in graph['instructions']}
    require(len(targets) == oracle['table']['unique_targets']
            and len(oracle['table']['targets']) == oracle['table']['slots'],
            'coverage oracle counts differ')
    jump = oracle['jump']
    require(jump['address'] in decoded and decoded[jump['address']]['bytes'] == jump['bytes'],
            'coverage jump bytes differ')
    switches = [r for r in load_switches(text, symbol) if r['source_load'] is not None]
    qualifying = [r for r in switches if targets.issubset(set(r['cases']))]
    missing_decoded = sorted(targets - set(decoded))
    missing_region = sorted(targets - set(attr['coverage_blocks']))
    complete = not missing_decoded and not missing_region and len(qualifying) == 1
    return dict(expected_table_slots=oracle['table']['slots'],
                expected_unique_targets=len(targets), missing_decoded=missing_decoded,
                missing_actual_region_targets=missing_region,
                load_driven_switches=switches, qualifying_dispatchers=qualifying,
                entry_case_region_coverage_gate=complete,
                scope='raw decoded-region case coverage only; no immutability/equivalence proof')


def runtime_maps(backend, shim):
    paths = set()
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and '/libdragon' in fields[5]:
            require(not fields[5].endswith(' (deleted)'), 'deleted Dragon mapping')
            paths.add(str(Path(fields[5]).resolve()))
    require(paths == {str(backend.resolve()), str(shim.resolve())}, 'actual SDK/backend mappings differ')
    return sorted(paths)


def validate_solver_paths(paths, enabled, expected):
    require(type(enabled) is bool, 'solver enabled flag must be boolean')
    wanted = {str(Path(expected).resolve())} if enabled else set()
    require(set(paths) == wanted, 'worker Z3 mapping differs from the frozen solver')
    return sorted(paths)


def solver_maps(enabled, library):
    paths = set()
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and re.fullmatch(r'libz3\.so(?:\..*)?',
                Path(fields[5].removesuffix(' (deleted)')).name):
            require(not fields[5].endswith(' (deleted)'), 'deleted Z3 mapping')
            paths.add(str(Path(fields[5]).resolve()))
    return validate_solver_paths(paths, enabled, library)


@contextlib.contextmanager
def frozen_graph(graph, regions, out, report):
    before = regions.graph_snapshot(graph)
    save(out / 'graph.before.json', before)
    try:
        yield before
    finally:
        after = regions.graph_snapshot(graph)
        save(out / 'graph.after.json', after)
        report['source_graph_exact'] = after == before
        require(after == before, 'source Graph changed')


def verify(opt, path, out, tag):
    command = [str(opt), '-passes=verify', '-disable-output', str(path)]
    with (out / (tag + '.verify.stdout')).open('wb') as stdout, \
            (out / (tag + '.verify.stderr')).open('wb') as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=SECONDS)
    require(result.returncode == 0, 'verify failed: ' + tag)


def worker(args):
    out, pins = args.out, {}
    report = dict(schema='dragon-portable-decoded-region-result-v1', ok=False,
                  semantic_validation=False, precision_complete=False,
                  native_runs=0, C_runs=0, Unicorn_runs=0, checked_component_calls=0,
                  failures=[], calls=dict(disassembly=0, region_lifts=0, pipelines=0, passes=0))
    try:
        plan_path = pinned(dict(path=str(args.plan), sha256=args.plan_sha256), pins)
        plan = read(plan_path)
        require(plan['schema'] == 'dragon-portable-decoded-region-plan-v1' and plan['ready'] is True,
                'portable plan is not frozen/ready')
        require(plan['case'] == CASE, 'one-sample case selection differs')
        require(pinned(plan['script'], pins).resolve() == Path(__file__).resolve(), 'runner identity differs')
        python = pinned(plan['python'], pins, allow_symlink=True)
        require(str(python) == sys.executable, 'explicit Python interpreter path differs')
        report['python'] = dict(path=sys.executable, resolved_path=str(python.resolve()),
                                sha256=sha(python), version=sys.version)
        pinned(plan['build_manifest'], pins)
        transfer, files = transfer_files(plan, pins)
        oracle_path = pinned(plan['coverage_manifest'], pins)
        oracle = read(oracle_path)
        source = Path(plan['source']['root'])
        require(source.is_absolute() and set(plan['source']['files']) == SOURCE_FILES,
                'explicit source binding set differs')
        for rel, digest in plan['source']['files'].items():
            pinned(dict(path=str(source / rel), sha256=digest), pins)
        cfg_path = source / 'scripts/validation/progressive_deobfuscation_baseline.py'
        require(sha(cfg_path) == CFG_SOURCE_SHA, 'frozen ten-pass/CFG helper differs')
        cfg = module_at(cfg_path, 'portable_frozen_ordinary_helpers')
        regions = module_at(source / 'scripts/validation/static_region_replay.py', 'portable_static_regions')
        backend, shim = (pinned(plan['runtime'][name], pins, allow_symlink=True)
                         for name in ('backend', 'shim'))
        opt = pinned(plan['tools']['opt'], pins, allow_symlink=True)
        solver = plan['solver']
        require(type(solver['enabled']) is bool, 'explicit solver configuration required')
        solver_library = pinned(solver['library'], pins, allow_symlink=True) if solver['enabled'] else None
        require(solver['enabled'] or solver.get('library') is None, 'no-Z3 plan has a solver library')
        library_path = plan['runtime'].get('library_path', [str(backend.parent)])
        require(isinstance(library_path, list) and library_path
                and all(isinstance(p, str) and Path(p).is_absolute() for p in library_path)
                and str(backend.parent) in library_path, 'invalid explicit library path')
        require(os.environ.get('DRAGON_C_LIB') == str(shim)
                and os.environ.get('LD_LIBRARY_PATH') == os.pathsep.join(library_path)
                and not os.environ.get('LD_PRELOAD'), 'explicit actual runtime environment differs')
        resolutions = {p: str(Path(p).resolve()) for p in pins}
        report['source_resolved_paths'] = resolutions
        report['library_path'] = library_path
        sys.path.insert(0, str(source / 'python'))
        import dragon as d
        from elftools.elf.elffile import ELFFile
        require(Path(d.__file__).resolve() == (source / 'python/dragon.py').resolve(),
                'unexpected Python wrapper')
        report.update(source_pins=pins, runtime_maps=runtime_maps(backend, shim),
                      worker_Z3_maps=solver_maps(solver['enabled'], solver_library),
                      opt_solver_context='separate verifier process; its LD_DEBUG is retained; no shared Z3 context assumed',
                      recipe=dict(max_blocks=4096, max_dispatch_cases=65536,
                                  rebuild_decoded_blocks=True, pipeline=cfg.PIPELINE,
                                  max_iterations=1, max_pass_runs=16),
                      known_memory_assertions=0, input_table_replacements=0,
                      graph_finite_passes=0, dynamic_Guest_semantics='unchanged')
        frozen = read(files['provenance/B-FROZEN.json']['path'])
        image_record = frozen['families'][CASE['family']]['images'][CASE['c_level']]
        elf_path = files['inputs/flatten-switch-O2.elf']['path']
        data = elf_path.read_bytes()
        require(sha(elf_path) == image_record['sha256'] == oracle['elf']['sha256'],
                'ELF lineage/coverage oracle differs')
        symbols = regions.function_symbols(data, ELFFile)
        named = {r['name']: pc for pc, rows in symbols.items() for r in rows}
        require(named[CASE['entry_symbol']] == image_record['symbols'][CASE['entry_symbol']]
                and named[CASE['kernel_symbol']] == image_record['symbols'][CASE['kernel_symbol']],
                'ELF symbolic entry binding differs')
        kernel = named[CASE['kernel_symbol']]
        require(kernel == oracle['kernel']['entry'], 'coverage kernel entry differs')
        selected = []
        with d.LoadedImage.load_bytes(data) as image:
            report['calls']['disassembly'] += 1
            with image.disassemble(d.Configuration(strategy=frozen['strategy'])) as graph, \
                    frozen_graph(graph, regions, out, report) as before_graph:
                report['source_Graph_unlifted'] = [dict(block=b['address'], instruction=pc)
                    for b in before_graph['blocks'] if b['ignil'] is not None
                    for pc in b['ignil']['unlifted']]
                report['private_region_unlifted_status'] = 'not separately exposed by this public API; Graph markers and full raw IR retained'
                entries = sorted(r['address'] for r in before_graph['functions'] if r['valid'])
                require(set(image_record['symbols'].values()).issubset(entries), 'symbol entry coverage failed')
                with d.LLVMLifter(d.Architecture.X86_64, 'lifted', execution_mode=2,
                                 static_entry_policy=1) as lifter:
                    try:
                        for entry in entries:
                            ranges = {(r['start'], r['end']) for r in symbols.get(entry, [])}
                            require(len(ranges) == 1, 'no unique positive executable STT_FUNC extent')
                            bounds = next(iter(ranges))
                            selection = regions.select_region(bounds, before_graph)
                            with graph.get_function(entry) as function:
                                original = sorted(function.block_addresses())
                                extra = sorted(set(selection['selected_starts']) - set(original))
                                row = dict(entry=entry, bounds=list(bounds), original_blocks=original,
                                           additional_blocks=extra, **selection)
                                selected.append(row)
                                save(out / 'region-selection.json', selected)
                                report['calls']['region_lifts'] += 1
                                ok = lifter.lift_static_region(function, graph, extra, max_blocks=4096,
                                    max_dispatch_cases=65536, rebuild_decoded_blocks=True)
                            row['accepted'] = bool(ok)
                            require(ok, 'decoded-region lift rejected ' + hex(entry))
                            row['actual_attributes'] = attributes(lifter.ir(), 'dragon_static_entry_x86_64_' + format(entry, 'x'))
                            save(out / 'region-selection.json', selected)
                    finally:
                        (out / 'before.ll').write_text(lifter.ir())
                    raw = (out / 'before.ll').read_text()
                    report['raw_ModuleID_literal'] = raw.splitlines()[0]
                    verify(opt, out / 'before.ll', out, 'before')
                    save(out / 'before.cfg.json', cfg.ir_cfg(raw))
                    row = next(r for r in selected if r['entry'] == kernel)
                    symbol = 'dragon_static_entry_x86_64_' + format(kernel, 'x')
                    measured = coverage(raw, symbol, row['actual_attributes'], before_graph, oracle)
                    save(out / 'raw-coverage.json', measured)
                    report['raw_coverage'] = measured
                    require(measured['entry_case_region_coverage_gate'], 'private region case/decoded coverage incomplete')
                    try:
                        with d.PassPipeline(cfg.PIPELINE) as pipeline:
                            report['calls']['pipelines'] += 1
                            result = pipeline.run(lifter, max_iterations=1, max_pass_runs=16, verify=True)
                    except d.PipelineError as error:
                        report['pass_report'] = error.report
                        report['calls']['passes'] = len(error.report['runs'])
                        save(out / 'pass-report.json', error.report)
                        raise
                    finally:
                        (out / 'after.ll').write_text(lifter.ir())
                    report['pass_report'] = result
                    report['calls']['passes'] = len(result['runs'])
                    save(out / 'pass-report.json', result)
                    require(result['ok'] and [r['name'] for r in result['runs']] == [n for n, _ in cfg.PIPELINE],
                            'pipeline failed or sequence changed')
                    verify(opt, out / 'after.ll', out, 'after')
                    save(out / 'after.cfg.json', cfg.ir_cfg((out / 'after.ll').read_text()))
                    report['after_ModuleID_literal'] = (out / 'after.ll').read_text().splitlines()[0]
        require(all(sha(Path(p)) == h for p, h in pins.items()), 'source/input/tool pin changed')
        require(all(str(Path(p).resolve()) == real for p, real in resolutions.items()),
                'a source/tool/runtime symlink resolution changed')
        report['worker_Z3_final_maps'] = solver_maps(solver['enabled'], solver_library)
        report.update(ok=True, verified=True, source_graph_exact=True,
                      region_coverage_complete=True, precision_complete=False,
                      semantic_scope='IR/decoded-region baseline only; full equivalence and business-loop removal pending')
        return 0
    except BaseException as error:
        report['failures'].append(dict(type=type(error).__name__, message=str(error)))
        report['pins_unchanged'] = all(Path(p).is_file() and sha(p) == h for p, h in pins.items())
        raise
    finally:
        report['source_pins'] = pins
        save(out / 'REPORT.json', report)


def local_checks(out):
    """Preparation controls only. No import of Dragon or ELF parser."""
    require(not out.exists(), 'new local control receipt required')
    text = '''define void @test(ptr %memory) #0 {
entry:
  br label %dispatch
dispatch:
  %target = load i64, ptr %memory, align 1
  switch i64 %target, label %miss [
    i64 32, label %business
    i64 48, label %business
  ]
business:
  ret void
miss:
  ret void
}
attributes #0 = { "dragon.static-coverage"="decoded-region" "dragon.coverage-source"="decoded-machine" "dragon.static-entry-policy"="function-start" "dragon.coverage-blocks"="10,20,30" }
'''
    oracle = dict(jump=dict(address=16, bytes='ff'),
                  table=dict(targets=[32, 48], unique_targets=2, slots=2))
    graph = dict(instructions=[dict(address=x, bytes='ff' if x == 16 else '90')
                               for x in (16, 32, 48)])
    attr = attributes(text, 'test')
    require(coverage(text, 'test', attr, graph, oracle)['entry_case_region_coverage_gate'],
            'valid synthetic raw coverage control failed')
    rejected = []
    for name, changed_text, changed_attr, changed_graph in (
        ('missing-switch-case', text.replace('    i64 48, label %business\n', ''), attr, graph),
        ('missing-actual-region-target', text, dict(coverage_blocks=[16, 32]), graph),
        ('missing-decoded-target', text, attr, dict(instructions=graph['instructions'][:2])),
        ('selector-not-actual-LOAD', text.replace('%target = load i64, ptr %memory, align 1',
                                               '%target = add i64 32, 0'), attr, graph)):
        require(not coverage(changed_text, 'test', changed_attr, changed_graph, oracle)
                ['entry_case_region_coverage_gate'], name + ' accepted')
        rejected.append(name)
    for name, changed in (
        ('wrong-jump-bytes', dict(instructions=[dict(address=16, bytes='00'),
                                               *graph['instructions'][1:]])),
        ('missing-region-contract', None)):
        try:
            if changed is None:
                attributes(text.replace('"decoded-region"', '"ordinary"'), 'test')
            else:
                coverage(text, 'test', attr, changed, oracle)
        except RuntimeError:
            rejected.append(name)
        else:
            raise RuntimeError(name + ' accepted')
    validate_solver_paths(['/frozen/wheel/libz3.so'], True, '/frozen/wheel/libz3.so')
    validate_solver_paths([], False, None)
    for name, paths, enabled, expected in (
        ('wrong-system-Z3', ['/system/libz3.so.4'], True, '/frozen/wheel/libz3.so'),
        ('mixed-worker-Z3', ['/system/libz3.so.4', '/frozen/wheel/libz3.so'], True, '/frozen/wheel/libz3.so'),
        ('unexpected-Z3-in-noz3', ['/system/libz3.so.4'], False, None)):
        try:
            validate_solver_paths(paths, enabled, expected)
        except RuntimeError:
            rejected.append(name)
        else:
            raise RuntimeError(name + ' accepted')
    save(out, dict(schema='dragon-portable-decoded-region-local-controls-v1', passed=True,
                   source_sha256=sha(__file__), synthetic_coverage_control=True,
                   negative_controls=rejected, negative_detections=len(rejected),
                   new_Dragon_calls=0, native_calls=0, C_calls=0, Unicorn_calls=0,
                   product_precision_claim=False))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--plan', type=Path)
    p.add_argument('--plan-sha256')
    p.add_argument('--out', type=Path)
    p.add_argument('--local-checks', type=Path)
    p.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = p.parse_args()
    if args.local_checks:
        require(not args.plan and not args.plan_sha256 and not args.out and not args.worker,
                'local checks cannot be mixed with execution')
        return local_checks(args.local_checks)
    require(args.plan is not None and args.plan_sha256 and args.out is not None,
            'execution requires plan, explicit SHA and output')
    require(args.plan.is_absolute() and args.out.is_absolute(), 'absolute plan/output required')
    if args.worker:
        return worker(args)
    require(sha(args.plan) == args.plan_sha256, 'plan hash differs before guard')
    require(not args.out.exists() and args.out.parent.is_dir(), 'new output with existing parent required')
    require(shutil.disk_usage(args.out.parent).free >= START, '512 MiB startup gate failed')
    args.out.mkdir()
    plan = read(args.plan)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', LD_DEBUG='libs')
    env['DRAGON_C_LIB'] = plan['runtime']['shim']['path']
    env['LD_LIBRARY_PATH'] = os.pathsep.join(plan['runtime'].get('library_path',
        [str(Path(plan['runtime']['backend']['path']).parent)]))
    require(not env.get('LD_PRELOAD'), 'LD_PRELOAD must be empty')
    command = [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--worker']
    start, failure = time.monotonic(), None
    with (args.out / 'worker.stdout').open('wb') as stdout, (args.out / 'loader.stderr').open('wb') as stderr:
        proc = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=env, start_new_session=True)
        while proc.poll() is None:
            if time.monotonic() - start > SECONDS:
                failure = '180 second deadline'
            elif shutil.disk_usage(args.out).free < FLOOR:
                failure = '64 MiB running floor'
            if failure:
                os.killpg(proc.pid, signal.SIGKILL)
                break
            time.sleep(0.1)
        code = proc.wait()
    save(args.out / 'GUARD.json', dict(argv=command, returncode=code, failure=failure,
         elapsed_seconds=time.monotonic() - start, source_plan_sha256=args.plan_sha256,
         limits=dict(start_bytes=START, floor_bytes=FLOOR, seconds=SECONDS),
         free_after=shutil.disk_usage(args.out).free))
    return 1 if failure else code


if __name__ == '__main__':
    raise SystemExit(main())
