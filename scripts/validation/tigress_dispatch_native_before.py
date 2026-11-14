#!/usr/bin/env python3
"""Portable frozen-input LLVM before calibration. No lift/pass/C/UC oracle run.

A sealed plan binds all paths explicitly. --local-checks uses only stdlib and
unchanged pure comparator bodies from this checkout; it executes no target.
"""
import argparse
import ast
import collections
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import signal
import struct
import subprocess
import sys
import time
import traceback
import types

sys.dont_write_bytecode = True
LIMITS = dict(guest_steps=1000000, subprocess_seconds=180,
              start_free=536870912, running_free=67108864)
TRANSFER_SHA = '719ca007dcbf1c790bfe14104a71fb8a7ab648d04a848d6db50bc5c45fc6fc07'
RAW_SHA = '4315ddcd08fd7e26861197be9a550ca7a1eee685be4206cc56f8c828d07abc05'
MODULE_ID = "; ModuleID = 'lifted'"
ROOT_EXIT = (1, 3, 4111, 61440, 1, 0, 0, 0)
HELPERS = ('function_llvm_diff.py', 'function_dispatch_diff.py',
           'function_guest_diff.py', 'guarded_store_expression_real_replay.py',
           'dispatch_corridor_real_replay.py')
SOURCE_FILES = {'python/dragon.py', 'c_shim/dragon_c.h',
                'include/dragon/lifters/llvm/Execution.h',
                *('scripts/validation/' + name for name in HELPERS)}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as source:
        for data in iter(lambda: source.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def pinned(record, pins, symlink=False):
    path = Path(record['path'])
    require(path.is_absolute() and path.is_file() and (symlink or not path.is_symlink()),
            'unsafe pinned path: ' + str(path))
    require(sha(path) == record['sha256'], 'changed pin: ' + str(path))
    if 'size' in record:
        require(path.stat().st_size == record['size'], 'changed size: ' + str(path))
    pins[str(path)] = dict(sha256=record['sha256'], resolved=str(path.resolve()))
    return path


def recheck(pins):
    return all(Path(path).is_file() and sha(path) == row['sha256']
               and str(Path(path).resolve()) == row['resolved'] for path, row in pins.items())


def pure_function(path, name, namespace):
    """Reuse the exact original AST body without importing its legacy main."""
    text = Path(path).read_text()
    node = next(n for n in ast.parse(text).body if isinstance(n, ast.FunctionDef) and n.name == name)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(path), 'exec'), namespace)
    return namespace[name]


def comparison_helpers(source, base):
    folder = source / 'scripts/validation'
    expected = pure_function(folder / 'dispatch_corridor_real_replay.py', 'expected',
                             dict(require=require, collections=collections, struct=struct))
    mutate = pure_function(folder / 'guarded_store_expression_real_replay.py', 'mutate_control',
                           dict(require=require, base=base))
    return expected, mutate


def decode_actual(blob):
    require(len(blob) == 66136 and blob[:8] == b'DTFLOW1\0', 'invalid snapshot length/magic/EOF')
    a, b = struct.unpack_from('<QQ', blob, 8)
    return (blob[24:536], blob[536:66072],
            collections.Counter({0x2000: a, 0x3000: b}), blob[-64:])


def complete_compare(adapter, expected, actual, actual_bytes, expected_bytes):
    differences, record = adapter.compare(dict(undefined_flags=[]), expected, actual, False)
    root = record == ROOT_EXIT
    if not root:
        differences.append(dict(field='root_return', got=list(record)))
    if actual_bytes != expected_bytes:
        differences.append(dict(field='raw_snapshot', detail='complete66136byte mismatch'))
    return differences, record, root


def transfer_inputs(plan, pins):
    manifest = pinned(plan['transfer'], pins)
    require(sha(manifest) == TRANSFER_SHA, 'wrong frozen selected transfer')
    data = json.loads(manifest.read_text())
    root = Path(plan['transfer']['root'])
    require(root.is_absolute() and root.is_dir(), 'explicit selected root required')
    require(data['schema'] == 'dragon-progressive-flatten-rock5b-input-transfer-v1', 'wrong transfer schema')
    rows = {}
    for row in data['files']:
        rel = PurePosixPath(row['destination_relative'])
        require(not rel.is_absolute() and '..' not in rel.parts and str(rel) not in rows,
                'unsafe/duplicate selected member')
        path = root / str(rel)
        require(path.resolve().is_relative_to(root.resolve()), 'selected member escapes root')
        pinned(dict(path=str(path), sha256=row['sha256'], size=row['size']), pins)
        rows[str(rel)] = path
    require(len(rows) == data['payload_files'] == 50, 'selected50set changed')
    require(sum(path.stat().st_size for path in rows.values()) == data['logical_bytes'], 'selected bytes changed')
    return rows


def mapped(names):
    found = {name: set() for name in names}
    for line in Path('/proc/self/maps').read_text().splitlines():
        parts = line.split(maxsplit=5)
        if len(parts) != 6 or not parts[5].startswith('/'):
            continue
        for name in names:
            if Path(parts[5]).name.startswith(name):
                found[name].add(str(Path(parts[5]).resolve()))
    return {name: sorted(paths) for name, paths in found.items()}


def verify_maps(runtime, solver, reference):
    maps = mapped(('libdragon.so', 'libdragon_c.so', 'libz3.so', 'libunicorn.so'))
    for name, key in [('libdragon.so', 'backend'), ('libdragon_c.so', 'shim')]:
        require(maps[name] == [str(Path(runtime[key]['path']).resolve())], 'wrong mapped ' + name)
    want_z3 = [str(Path(solver['library']['path']).resolve())] if solver['enabled'] else []
    require(maps['libz3.so'] == want_z3, 'wrong/mixed worker Z3 mapping')
    require(maps['libunicorn.so'] == [str(Path(reference['UC_native']['path']).resolve())],
            'wrong/mixed preserved Unicorn reference mapping')
    return maps


def command(argv, work, tag):
    with (work / (tag + '.stdout')).open('wb') as stdout, (work / (tag + '.stderr')).open('wb') as stderr:
        subprocess.run([str(x) for x in argv], cwd=work, check=True, stdout=stdout, stderr=stderr,
                       timeout=LIMITS['subprocess_seconds'])


def worker(args):
    plan_bytes = args.plan.read_bytes()
    require(hashlib.sha256(plan_bytes).hexdigest() == args.plan_sha256, 'plan SHA differs')
    plan = json.loads(plan_bytes)
    out, pins, rows, controls, failure = args.out, {}, [], [], None
    original_run = subprocess.run
    report = dict(schema='tigress-dispatch-native-before-result-v1', phase='before',
                  skipped=0, undefined_exclusions=0, normalizations=0,
                  new_native_C_comparisons=0, new_Unicorn_comparisons=0,
                  old_C_matches_recounted=0, old_Unicorn_matches_recounted=0,
                  producer_runs=0, product_pass_runs=0, lift_runs=0)
    try:
        require(plan['schema'] == 'tigress-dispatch-native-before-plan-v1' and plan['ready'] is True,
                'plan not ready')
        require(plan['limits'] == LIMITS and plan['host_llvm_levels'] == ['O0', 'O2'], 'limits/levels changed')
        require(plan['case'] == 'flatten-switch-O2' and plan['raw_ModuleID_literal'] == MODULE_ID,
                'wrong case/ModuleID')
        require(pinned(plan['script'], pins).resolve() == Path(__file__).resolve(), 'runner identity differs')
        python = pinned(plan['python'], pins, True)
        require(str(python) == sys.executable, 'explicit interpreter differs')
        reference = json.loads(pinned(plan['reference_environment'], pins).read_text())
        require(reference['ready'] is True and reference['interpreter'] == str(python),
                'explicit correct reference interpreter binding differs')
        pinned(reference['UC_native'], pins, True)
        require(sorted(os.sched_getaffinity(0)) == plan['allowed_cpus'], 'CPU affinity differs from plan')
        pinned(plan['build_manifest'], pins)
        selected = transfer_inputs(plan, pins)
        source = Path(plan['source']['root'])
        require(source.is_absolute() and set(plan['source']['files']) == SOURCE_FILES, 'source binding set differs')
        for rel, digest in plan['source']['files'].items():
            pinned(dict(path=str(source / rel), sha256=digest), pins)
        # Old B helper/header bytes, not a rewritten expectation/specification.
        lineage = json.loads(selected['provenance/B-FROZEN.json'].read_text())['source_pins']
        for rel in ('python/dragon.py', 'c_shim/dragon_c.h',
                    'include/dragon/lifters/llvm/Execution.h',
                    'scripts/validation/function_guest_diff.py',
                    'scripts/validation/function_dispatch_diff.py',
                    'scripts/validation/function_llvm_diff.py'):
            old = [digest for path, digest in lineage.items() if path.endswith('/' + rel)]
            require(old == [plan['source']['files'][rel]], 'old oracle source compatibility differs: ' + rel)
        module = pinned(plan['raw'], pins)
        require(module.stat().st_size == 747549 and sha(module) == RAW_SHA,
                'wrong actual decoded-region raw')
        require(module.read_bytes() == selected['reference/canonical-before.ll'].read_bytes(), 'raw/canonical differ')
        require(module.read_text().splitlines()[0] == MODULE_ID, 'raw ModuleID differs')
        require(plan['driver']['path'] == str(selected['sources/frozen-driver.c']), 'driver binding differs')
        driver = pinned(plan['driver'], pins)
        tools = {name: pinned(plan['tools'][name], pins, True) for name in ('cc', 'opt', 'llc')}
        for key in ('backend', 'shim'):
            pinned(plan['runtime'][key], pins, True)
        solver = plan['solver']
        require(type(solver['enabled']) is bool, 'explicit solver configuration required')
        if solver['enabled']:
            pinned(solver['library'], pins, True)
        else:
            require(solver.get('library') is None, 'noZ3 plan pins a solver')
        require(os.environ.get('DRAGON_C_LIB') == plan['runtime']['shim']['path'] and
                os.environ.get('LD_LIBRARY_PATH') == os.pathsep.join(plan['runtime']['library_path']) and
                not os.environ.get('LD_PRELOAD') and not os.environ.get('LIBUNICORN_PATH'), 'runtime environment differs')
        sys.path.insert(0, str(source / 'scripts/validation'))
        import function_guest_diff as guest
        base = guest.base
        require(Path(guest.__file__).resolve() == (source / 'scripts/validation/function_guest_diff.py').resolve()
                and Path(base.dragon.__file__).resolve() == (source / 'python/dragon.py').resolve(), 'unexpected helper/wrapper')
        initial_maps = verify_maps(plan['runtime'], solver, reference)
        adapter = guest.Adapter('x86_64')
        require(len(adapter.fields) == 76 and base.MEM_SIZE == 65536, 'comparison ABI differs')
        parse_expected, mutate = comparison_helpers(source, base)
        trials = []
        require([row['trial'] for row in plan['inputs']] == list(range(16)), 'original16trial order differs')
        frozen_values = json.loads(selected['provenance/B-FROZEN.json'].read_text())['inputs']
        require([row['value'] for row in plan['inputs']] == frozen_values, 'frozen16values differ')
        for row in plan['inputs']:
            i = row['trial']
            require(row['input']['path'] == str(selected[f'inputs/input-{i}.bin']) and
                    row['expected']['path'] == str(selected[f'reference/expected-{i}.bin']), 'trial source binding differs')
            ib = pinned(row['input'], pins).read_bytes()
            eb = pinned(row['expected'], pins).read_bytes()
            require(len(ib) == 66048, 'input EOF/size differs')
            trials.append((i, ib[:512], ib[512:], parse_expected(eb), eb))
        report.update(plan_sha256=args.plan_sha256, argv=sys.argv, source_pins=pins,
                      Python=dict(path=sys.executable, resolved=str(python.resolve()), version=sys.version),
                      worker_initial_maps=initial_maps, raw_ModuleID_literal=MODULE_ID,
                      comparison_fields=[list(row) for row in adapter.fields], input_domain=frozen_values,
                      old_reference_scope='saved original C16/UC16,not executed or recounted')
        save(out / 'INVOCATION.json', report)
        sequence = [0]
        def limited_run(*a, **kw):
            # Matches the old real replay's effective per-subprocess180s guard.
            kw['timeout'] = LIMITS['subprocess_seconds']
            start = time.monotonic()
            result = original_run(*a, **kw)
            sequence[0] += 1
            tag = out / ('subprocess-%03d' % sequence[0])
            save(str(tag) + '.json', dict(argv=[str(x) for x in a[0]], returncode=result.returncode,
                                        seconds=time.monotonic() - start))
            for name in ('stdout', 'stderr'):
                data = getattr(result, name)
                if data is not None:
                    (Path(str(tag) + '.' + name)).write_bytes(data if isinstance(data, bytes) else data.encode())
            return result
        subprocess.run = limited_run
        triple = {'aarch64': 'aarch64-linux-gnu', 'x86_64': 'x86_64-linux-gnu'}.get(platform.machine())
        require(triple is not None, 'unsupported native host')
        for level in ('O0', 'O2'):
            work = out / level
            work.mkdir()
            shutil.copyfile(driver, work / 'driver.c')
            require(sha(work / 'driver.c') == plan['driver']['sha256'], 'driver copy differs')
            chosen = module
            command([tools['opt'], '-passes=verify', '-disable-output', module], work, 'verify-raw')
            if level == 'O2':
                chosen = work / 'optimized.ll'
                command([tools['opt'], '-S', '-passes=default<O2>', module, '-o', chosen], work, 'optimize')
                require(chosen.read_text().splitlines()[0] == "; ModuleID = '" + str(module) + "'", 'opt ModuleID differs')
            command([tools['opt'], '-passes=verify', '-disable-output', chosen], work, 'verify-final')
            command([tools['llc'], '-O0', '-filetype=obj', '-mtriple=' + triple, chosen, '-o', 'module.o'], work, 'llc')
            command([tools['cc'], '-O0', '-I' + str(source / 'include'), '-I' + str(source / 'c_shim'),
                     'driver.c', 'module.o', '-o', 'native'], work, 'native-link')
            bad_ir = 'unreachable' in chosen.read_text()
            for trial, regs, memory, want, eb in trials:
                require(shutil.disk_usage(out).free >= LIMITS['running_free'], 'running floor')
                actual, error = base.native(work / 'native', regs, memory, work, trial, extra_bytes=64)
                row = dict(case=plan['case'], phase='before', host_llvm_level=level, trial=trial)
                if error:
                    row.update(status=error['kind'], detail=error)
                else:
                    raw = (work / f'output-{trial}.bin').read_bytes()
                    decode_actual(raw)  # exact length/magic/EOF, including unused bytes
                    differences, record, root = complete_compare(adapter, want, actual, raw, eb)
                    if bad_ir:
                        differences.append(dict(field='ir', detail='unexpected unreachable'))
                    row.update(status='different' if differences else 'match', differences=differences,
                               exit=list(record[:7]), budget_left=record[7], root_return=root,
                               raw_snapshot_equal=raw == eb, register_fields_compared=76,
                               memory_bytes_compared=65536, output_sha256=sha(work / f'output-{trial}.bin'),
                               input_sha256=sha(work / f'input-{trial}.bin'))
                    require((work / f'input-{trial}.bin').read_bytes() == regs + memory, 'native input copy differs')
                    if not differences:
                        for field in ('RAX', 'memory', 'exit'):
                            controls.append(dict(host_llvm_level=level, trial=trial,
                                                 **mutate(adapter, want, actual, field)))
                rows.append(row)
                save(out / 'PROGRESS.json', dict(results=rows, controls=controls))
                require(row['status'] == 'match', 'before full-state calibration mismatch; raw retained')
        require(recheck(pins) and args.plan.read_bytes() == plan_bytes, 'pin changed during calibration')
        report['worker_final_maps'] = verify_maps(plan['runtime'], solver, reference)
    except BaseException as error:
        failure = dict(type=type(error).__name__, detail=str(error), traceback=traceback.format_exc())
        save(out / 'FAILURE.json', failure)
    finally:
        subprocess.run = original_run
        complete = len(rows) == 32 and len(controls) == 96
        report.update(complete=complete, correct=complete and failure is None and
                      all(row['status'] == 'match' for row in rows), results=rows, controls=controls,
                      native_comparisons=len(rows), comparator_controls=len(controls), failure=failure,
                      statuses=dict(collections.Counter(row['status'] for row in rows)), pins_unchanged=recheck(pins))
        save(out / 'RESULTS.json', report)
    return 0 if report['correct'] else 1


def local_adapter(source):
    """Execute unchanged pure class methods with unused UC register IDs stubbed.

    This loader is exclusively a local comparator control, never a reference
    execution. The actual worker imports the complete original module normally.
    """
    folder = source / 'scripts/validation'
    text = (folder / 'function_llvm_diff.py').read_text()
    nodes = []
    names = {'MEM_SIZE', 'REGS_SIZE', 'DATA', 'STACK', 'SENTINEL', 'GPRS', 'FLAGS', 'FIELDS'}
    for node in ast.parse(text).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in names for target in node.targets for t in ast.walk(target)):
            nodes.append(node)
        elif isinstance(node, ast.AugAssign) and isinstance(node.target, ast.Name) and node.target.id == 'FIELDS':
            nodes.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name == 'get':
            nodes.append(node)
    scope = {}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(folder / 'function_llvm_diff.py'), 'exec'), scope)
    base = types.SimpleNamespace(**scope)
    class UnusedIDs:
        def __getattr__(self, name):
            return 0
    tree = ast.parse((folder / 'function_guest_diff.py').read_text())
    cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'Adapter')
    cls.body = [n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name in ('__init__', 'compare')]
    scope = dict(base=base, X=UnusedIDs(), A=UnusedIDs(), UC_ARCH_X86=0,
                 UC_MODE_32=0, UC_MODE_64=0, UC_ARCH_ARM64=0, UC_MODE_ARM=0, struct=struct)
    exec(compile(ast.Module(body=[cls], type_ignores=[]), str(folder / 'function_guest_diff.py'), 'exec'), scope)
    return scope['Adapter']('x86_64'), base


def local_checks(path):
    require(not path.exists(), 'local receipt already exists')
    source = Path(__file__).resolve().parents[2]
    adapter, base = local_adapter(source)
    parse_expected, mutate = comparison_helpers(source, base)
    blob = b'DTFLOW1\0' + bytes(16 + 512 + 65536) + struct.pack('<8Q', *ROOT_EXIT)
    want, actual = parse_expected(blob), decode_actual(blob)
    require(len(adapter.fields) == 76 and not complete_compare(adapter, want, actual, blob, blob)[0], 'positive control failed')
    controls = [mutate(adapter, want, actual, field) for field in ('RAX', 'memory', 'exit')]
    rejects = []
    for name, bad in [('truncated', blob[:-1]), ('trailing-byte', blob + b'\0'), ('bad-magic', b'X' + blob[1:])]:
        try:
            decode_actual(bad)
        except RuntimeError:
            rejects.append(name)
        else:
            raise RuntimeError('bad protocol accepted: ' + name)
    for name, offset in [('unmapped-register-padding', 24 + 399), ('budget', len(blob) - 8)]:
        damaged = bytearray(blob)
        damaged[offset] ^= 1
        differences, _, _ = complete_compare(adapter, want, decode_actual(bytes(damaged)), bytes(damaged), blob)
        require(any(row['field'] == 'raw_snapshot' for row in differences), 'raw/budget control missed')
        rejects.append(name)
    save(path, dict(schema='tigress-dispatch-native-local-controls-v1', passed=True,
                    source_sha256=sha(__file__), positive_controls=1, original_comparator_controls=controls,
                    protocol_raw_controls=rejects, new_target_runs=0, Unicorn_runs=0, Dragon_calls=0,
                    note='original AST method bodies;unusedUCregisterIDs stubbed only in this local control'))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan', type=Path)
    ap.add_argument('--plan-sha256')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--local-checks', type=Path)
    ap.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.local_checks:
        require(not args.plan and not args.out and not args.worker, 'mixed local controls/execution')
        return local_checks(args.local_checks)
    require(args.plan and args.plan_sha256 and args.out and args.plan.is_absolute() and args.out.is_absolute(),
            'execution needs absolute plan/output and plan SHA')
    if args.worker:
        return worker(args)
    require(sha(args.plan) == args.plan_sha256, 'plan SHA differs before guard')
    plan = json.loads(args.plan.read_text())
    require(not args.out.exists() and args.out.parent.is_dir(), 'fresh output required')
    require(not args.plan.is_relative_to(args.out), 'output contains plan')
    for protected in (Path(plan['transfer']['root']), Path(plan['source']['root'])):
        require(not args.out.resolve().is_relative_to(protected.resolve()) and
                not protected.resolve().is_relative_to(args.out.resolve()), 'output overlaps frozen input/source root')
    require(shutil.disk_usage(args.out.parent).free >= LIMITS['start_free'], 'startup floor')
    args.out.mkdir()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', LD_DEBUG='libs')
    env['DRAGON_C_LIB'] = plan['runtime']['shim']['path']
    env['LD_LIBRARY_PATH'] = os.pathsep.join(plan['runtime']['library_path'])
    require(not env.get('LD_PRELOAD') and not env.get('LIBUNICORN_PATH'), 'unexpected loader overrides')
    argv = [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--worker']
    start, failure = time.monotonic(), None
    with (args.out / 'worker.stdout').open('wb') as stdout, (args.out / 'loader.stderr').open('wb') as stderr:
        process = subprocess.Popen(argv, env=env, stdout=stdout, stderr=stderr, start_new_session=True)
        while process.poll() is None:
            if time.monotonic() - start > LIMITS['subprocess_seconds']:
                failure = '180 second whole calibration deadline'
            elif shutil.disk_usage(args.out).free < LIMITS['running_free']:
                failure = '64MiB running floor'
            if failure:
                os.killpg(process.pid, signal.SIGKILL)
                break
            time.sleep(.1)
        code = process.wait()
    save(args.out / 'GUARD.json', dict(argv=argv, returncode=code, failure=failure,
         seconds=time.monotonic() - start, plan_sha256=args.plan_sha256, limits=LIMITS))
    return 1 if failure else code


if __name__ == '__main__':
    raise SystemExit(main())
