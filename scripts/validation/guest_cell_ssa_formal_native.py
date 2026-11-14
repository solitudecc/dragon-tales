"""Formal-library after-only replay for the frozen GuestCellSSA native domain.

The original oracle, prototype namespace and d380 archive stay unchanged.
The bridge reuses completed before calibration only after exact source/IR
binding. This file never compiles a producer or links a helper object.
Run every emit/replay command through the existing 512/64 MiB /180 s guard.
"""
import argparse
import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

ORACLE_SHA = 'd75c10ef6812f99df0ab6fd441f158515f851d8bc3c13beb9b819dd637c183c9'
OLD_MANIFEST_SHA = 'bad148348887a7576e7c891638a4005de5925cf8f74ae14f9e6ef9ee8f6ee8e3'
OLD_ARCHIVE_SHA = 'd380f02809a1d93916be05dd7ff89f957caabd712a29c1bef0b3971a4feecf3b'
API = 'dragon::llvmlifter::detail::forwardGuestCellLoad('


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def put(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def pin(path):
    path = Path(path)
    status = path.stat()
    digest = sha(path)
    after = path.stat()
    fields = ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')
    if any(getattr(status, k) != getattr(after, k) for k in fields):
        raise ValueError(('file changed while hashing', str(path)))
    return dict(path=str(path), size=status.st_size, sha256=digest)


def oracle(prototype):
    path = prototype / 'source/scripts/validation/guest_cell_ssa_native.py'
    if sha(path) != ORACLE_SHA:
        raise ValueError('frozen independent oracle source mismatch')
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('frozen_cell_oracle', path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def old_binding(prototype, model):
    frozen = prototype / 'frozen'
    if sha(frozen / 'EMITTED.json') != OLD_MANIFEST_SHA:
        raise ValueError('original frozen manifest mismatch')
    _, source_pins = model.file_pins(frozen)
    if ((frozen / 'wrapper.c').read_bytes() != model.WRAPPER.encode() or
            (frozen / 'reference.c').read_bytes() != model.C_SPEC.encode()):
        raise ValueError('frozen C sources differ from independent oracle')
    calibrations, common = [], None
    required = {(f'{w}-{m}', t, e) for w in model.WIDTHS
                for m in model.MODES for t in range(model.INPUTS)
                for e in ('C', 'LLVM')}
    for opt in ('O0', 'O2'):
        folder = prototype / ('before-' + opt)
        result = json.loads((folder / 'RESULTS.json').read_text())
        if (not result['passed'] or result['phase'] != 'before' or
                result['host_opt'] != opt or result['frozen_sha256'] != OLD_MANIFEST_SHA or
                result['oracle_sha256'] != ORACLE_SHA or result['frames'] != 192 or
                result['matches'] != 192 or result['controls'] != 2496 or
                result['unique_module_inputs'] != 96 or result['unique_initial_arenas'] != 32):
            raise ValueError(('old calibration metadata mismatch', opt))
        keys = [(r['name'], r['trial'], r['engine']) for r in result['rows']]
        if len(keys) != 192 or set(keys) != required:
            raise ValueError('old calibration is not the complete C/LLVM domain')
        domain = {}
        for row in result['rows']:
            key = (row['name'], row['trial'])
            value = (row['sha256']['input'], row['sha256']['expected'])
            if key in domain and domain[key] != value:
                raise ValueError('C/LLVM calibration input/expectation mismatch')
            domain[key] = value
        if common is not None and common != domain:
            raise ValueError('O0/O2 calibration domain differs')
        common = domain
        frames = pin(folder / 'frames.xz')
        if frames['sha256'] != result['frame_stream_sha256']:
            raise ValueError('original saved calibration stream hash mismatch')
        calibrations.append(dict(host_opt=opt, result=pin(folder / 'RESULTS.json'),
                                 frames=frames, old_C_matches=96, old_LLVM_matches=96))
    return dict(prototype=str(prototype), old_archive_sha256=OLD_ARCHIVE_SHA,
                old_manifest=pin(frozen / 'EMITTED.json'), oracle_sha256=ORACLE_SHA,
                calibrations=calibrations, referenced_old_before_matches=384,
                referenced_old_C_matches=192, referenced_old_LLVM_matches=192,
                unique_module_inputs=96, unique_initial_arenas=32,
                original_frozen_pins=source_pins)


class Commands:
    def __init__(self, out):
        self.out, self.rows = out, []

    def run(self, argv, data=None, env=None, stderr_path=None):
        start = time.monotonic()
        value = subprocess.run([str(x) for x in argv], input=data, capture_output=True,
                               timeout=180, env=env)
        if stderr_path is not None:
            Path(stderr_path).write_bytes(value.stderr)
        self.rows.append(dict(argv=[str(x) for x in argv], exit=value.returncode,
                              seconds=time.monotonic() - start,
                              stdout_bytes=len(value.stdout),
                              stdout_sha256=hashlib.sha256(value.stdout).hexdigest(),
                              stderr=value.stderr.decode(errors='replace')))
        (self.out / 'COMMANDS.json').write_text(json.dumps(self.rows, indent=2) + '\n')
        if value.returncode:
            raise RuntimeError(self.rows[-1])
        return value.stdout


def emit(args, model, commands):
    if not all((args.backend, args.producer, args.producer_sha256,
                args.runtime, args.runtime_sha256)):
        raise ValueError('emit requires backend and exact producer/runtime pins')
    if not args.runtime.is_absolute() or not args.producer.is_absolute():
        raise ValueError('producer/runtime paths must be absolute')
    if args.runtime.parent.name != args.backend:
        raise ValueError('backend label does not match the selected runtime directory')
    if sha(args.producer) != args.producer_sha256 or sha(args.runtime) != args.runtime_sha256:
        raise ValueError('actual producer/runtime hash mismatch')
    before_binding = old_binding(args.prototype, model)
    defined = commands.run(['nm', '-C', '--defined-only', args.producer]).decode()
    undefined = commands.run(['nm', '-D', '-C', '--undefined-only', args.producer]).decode()
    exported = commands.run(['nm', '-D', '-C', '--defined-only', args.runtime]).decode()
    if API in defined or API not in undefined or API not in exported:
        raise ValueError('API must be undefined in producer and exported by selected actual so')
    (args.out / 'producer-defined.txt').write_text(defined)
    (args.out / 'producer-undefined.txt').write_text(undefined)
    (args.out / 'runtime-api-export.txt').write_text(
        '\n'.join(line for line in exported.splitlines() if API in line) + '\n')
    dynamic = commands.run(['readelf', '-d', args.producer]).decode()
    if 'Shared library: [libdragon.so]' not in dynamic:
        raise ValueError('producer is not dynamically linked to libdragon.so')
    (args.out / 'producer-dynamic.txt').write_text(dynamic)
    runtime_pin, producer_pin = pin(args.runtime), pin(args.producer)
    original = args.prototype / 'frozen'
    for name in ('wrapper.c', 'reference.c'):
        (args.out / name).write_bytes((original / name).read_bytes())
    modules, comparisons = [], []
    env = dict(os.environ, LD_LIBRARY_PATH=str(args.runtime.parent),
               LD_PRELOAD='', LD_DEBUG='libs,bindings', PYTHONDONTWRITEBYTECODE='1')
    for width in model.WIDTHS:
        for mode in model.MODES:
            name = f'{width}-{mode}'
            work = args.out / name
            work.mkdir()
            paths = [work / n for n in ('before.ll', 'after.ll', 'bad-phi.ll', 'STATS.json')]
            commands.run([args.producer, width, mode, *paths], env=env,
                         stderr_path=work / 'loader.stderr')
            bindings = [line for line in (work / 'loader.stderr').read_text().splitlines()
                        if 'binding file' in line and 'forwardGuestCellLoad' in line]
            if (len(bindings) != 1 or
                    ('binding file ' + str(args.producer) + ' [0]') not in bindings[0] or
                    ('to ' + str(args.runtime) + ' [0]') not in bindings[0]):
                raise ValueError(('API runtime binding was not the selected actual so', name, bindings))
            stats = json.loads(paths[-1].read_text())
            prior = json.loads((original / name / 'STATS.json').read_text())
            if set(stats) != set(prior):
                raise ValueError(('stats schema changed', name))
            # Full raw JSONs remain saved. Only maps contain new provenance;
            # all scalar fields have an explicit exact compatibility gate.
            if any(stats[k] != prior[k] for k in prior if k != 'runtime_maps'):
                raise ValueError(('non-provenance stats changed', name))
            if not stats['runtime_maps'] or any(str(args.runtime) not in line
                                               for line in stats['runtime_maps']):
                raise ValueError(('wrong mapped backend', name, stats['runtime_maps']))
            for filename in ('before.ll', 'after.ll', 'bad-phi.ll'):
                old, new = original / name / filename, work / filename
                if old.exists() != new.exists():
                    raise ValueError(('IR product set changed', name, filename))
                if old.exists():
                    equal = old.read_bytes() == new.read_bytes()
                    comparisons.append(dict(old=pin(old), new=pin(new), byte_equal=equal))
                    if not equal:
                        raise ValueError(('IR compatibility changed', name, filename))
            modules.append(dict(name=name, stats=stats, API_binding=bindings[0]))
    if pin(args.runtime) != runtime_pin or pin(args.producer) != producer_pin:
        raise ValueError('actual tool bytes changed during emit')
    if old_binding(args.prototype, model) != before_binding:
        raise ValueError('old calibration/source pins changed during emit')
    files = [dict(path=str(p.relative_to(args.out)), bytes=p.stat().st_size, sha256=sha(p))
             for p in sorted(args.out.rglob('*')) if p.is_file()]
    put(args.out / 'EMITTED.json', dict(schema='guest-cell-ssa-formal-emitted-v1',
                                      files=files, modules=modules))
    bridge = dict(schema='guest-cell-ssa-formal-calibration-bridge-v1', ready=True,
                  backend=args.backend, producer=producer_pin, runtime=runtime_pin,
                  adapter=pin(__file__), original=before_binding,
                  frozen_manifest_sha256=sha(args.out / 'EMITTED.json'),
                  complete_IR_byte_comparisons=comparisons, original_C_sources_byte_equal=True,
                  producer_defines_API=False, producer_imports_API=True,
                  actual_so_exports_API=True, helper_object_linked=False,
                  new_product_calls=6, new_native_executions=0)
    put(args.out / 'BRIDGE.json', bridge)
    print(json.dumps(dict(ready=True, backend=args.backend,
                         bridge_sha256=sha(args.out / 'BRIDGE.json'),
                         frozen_manifest_sha256=bridge['frozen_manifest_sha256'])))


def formal_binding(args, model):
    if not args.frozen or not args.bridge_sha256:
        raise ValueError('replay/audit requires frozen and exact bridge SHA')
    if sha(args.frozen / 'BRIDGE.json') != args.bridge_sha256:
        raise ValueError('formal bridge hash mismatch')
    bridge = json.loads((args.frozen / 'BRIDGE.json').read_text())
    if (not bridge['ready'] or bridge['adapter']['sha256'] != sha(__file__) or
            bridge['original'] != old_binding(args.prototype, model) or
            bridge['frozen_manifest_sha256'] != sha(args.frozen / 'EMITTED.json')):
        raise ValueError('formal/original calibration bridge mismatch')
    manifest, pins = model.file_pins(args.frozen)
    wanted = {r['path'] for r in manifest['files']} | {'EMITTED.json', 'BRIDGE.json'}
    actual = {str(p.relative_to(args.frozen)) for p in args.frozen.rglob('*') if p.is_file()}
    if actual != wanted:
        raise ValueError('formal frozen file set changed')
    return bridge, pins


def replay(args, model, commands):
    if args.phase not in ('after', 'fault') or args.host_opt not in ('O0', 'O2'):
        raise ValueError('replay requires after/fault and O0/O2')
    bridge, pins_before = formal_binding(args, model)
    rows, failure = [], None
    try:
        with lzma.open(args.out / 'frames.xz', 'xb', preset=1) as stream:
            for width in model.WIDTHS:
                for mode in model.MODES:
                    if args.phase == 'fault' and mode != 'closed':
                        continue
                    name = f'{width}-{mode}'
                    work = args.out / name
                    work.mkdir()
                    obj, target = work / 'target.o', work / 'target.native'
                    filename = 'after.ll' if args.phase == 'after' else 'bad-phi.ll'
                    commands.run([args.llc, '-mtriple=aarch64-unknown-linux-gnu',
                                  '-filetype=obj', '-relocation-model=pic', '-' + args.host_opt,
                                  args.frozen / name / filename, '-o', obj])
                    commands.run([args.cc, '-' + args.host_opt,
                                  args.frozen / 'wrapper.c', obj, '-o', target])
                    sources = [model.initial(width, t) for t in range(model.INPUTS)]
                    native = commands.run([target, width, model.MODES.index(mode)], b''.join(sources))
                    if len(native) != model.FRAME * model.INPUTS:
                        raise ValueError(('native output length', name, len(native)))
                    for trial, source in enumerate(sources):
                        wanted, coverage = model.expected(source, width, mode)
                        actual = native[trial * model.FRAME:(trial + 1) * model.FRAME]
                        match = wanted == actual
                        specified = (actual[2048:2048 + width // 8] != wanted[2048:2048 + width // 8] or
                                     actual[96:128] != wanted[96:128])
                        controls = 0
                        if args.phase != 'fault':
                            for offset in model.CONTROL_OFFSETS:
                                corrupt = bytearray(actual)
                                corrupt[offset] ^= 1
                                controls += bytes(corrupt) != wanted
                        row = dict(name=name, width=width, mode=mode,
                                   kind='refusal' if mode == 'call-after' else 'gain',
                                   phase=args.phase, opt=args.host_opt, engine='LLVM', trial=trial,
                                   match=match, controls=controls,
                                   specified_fault=specified if args.phase == 'fault' else None,
                                   coverage=coverage, sha256={k:hashlib.sha256(v).hexdigest()
                                     for k,v in [('input',source),('expected',wanted),('actual',actual)]})
                        encoded = json.dumps(row, sort_keys=True).encode()
                        stream.write(struct.pack('<I', len(encoded)) + encoded + source + wanted + actual)
                        rows.append(row)
                        if (not all(coverage['branch_visits']) or
                                (args.phase == 'fault' and (match or not specified)) or
                                (args.phase != 'fault' and (not match or controls != len(model.CONTROL_OFFSETS)))):
                            raise ValueError(('native mismatch/missed fault/control', row))
        _, pins_after = formal_binding(args, model)
        if pins_before != pins_after:
            raise ValueError('formal source/IR changed during replay')
    except Exception as error:
        failure = type(error).__name__ + ': ' + str(error)
    result = dict(schema='guest-cell-ssa-formal-native-v1', passed=failure is None,
                  failure=failure, backend=bridge['backend'], phase=args.phase,
                  host_opt=args.host_opt, frozen_root=str(args.frozen),
                  frozen_sha256=sha(args.frozen / 'EMITTED.json'),
                  bridge_sha256=args.bridge_sha256, oracle_sha256=ORACLE_SHA,
                  adapter_sha256=sha(__file__), rows=rows, frames=len(rows),
                  matches=sum(r['match'] for r in rows), controls=sum(r['controls'] for r in rows),
                  specified_faults=sum(r['specified_fault'] is True for r in rows),
                  unique_module_inputs=len({(r['name'],r['trial']) for r in rows}),
                  unique_initial_arenas=len({r['sha256']['input'] for r in rows}),
                  frame_stream_sha256=sha(args.out / 'frames.xz') if (args.out / 'frames.xz').exists() else None,
                  commands=commands.rows, declared_pins_before=pins_before,
                  referenced_old_before_matches=384, newly_executed_C_matches=0,
                  scope='Whole66112 bytes; frozen32/128 domain; old before calibration referenced only; no skips/exclusions/normalization')
    put(args.out / 'RESULTS.json', result)
    print(json.dumps({k:result[k] for k in ('passed','backend','phase','frames','matches','controls','specified_faults')}))
    if failure:
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--emit', action='store_true')
    action.add_argument('--replay', action='store_true')
    action.add_argument('--audit', type=Path)
    parser.add_argument('--prototype', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--backend', choices=('main','noz3'))
    parser.add_argument('--producer', type=Path)
    parser.add_argument('--producer-sha256')
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--runtime-sha256')
    parser.add_argument('--frozen', type=Path)
    parser.add_argument('--bridge-sha256')
    parser.add_argument('--phase', choices=('after','fault'))
    parser.add_argument('--host-opt', choices=('O0','O2'))
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--llc', default='llc-21')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    commands = Commands(args.out)
    try:
        model = oracle(args.prototype)
        if args.emit:
            emit(args, model, commands)
        elif args.replay:
            replay(args, model, commands)
        else:
            bridge, _ = formal_binding(args, model)
            result = json.loads((args.audit / 'RESULTS.json').read_text())
            if (result['backend'] != bridge['backend'] or
                    result['adapter_sha256'] != sha(__file__) or
                    result['bridge_sha256'] != args.bridge_sha256 or
                    result['frozen_sha256'] != sha(args.frozen / 'EMITTED.json')):
                raise ValueError('saved run has a different formal binding')
            model.audit_saved(args.audit, args.out)
            report = json.loads((args.out / 'REPORT.json').read_text())
            if (report['unique_module_inputs'] != result['unique_module_inputs'] or
                    report['unique_initial_arenas'] != result['unique_initial_arenas']):
                raise ValueError('reported unique domain differs from saved complete domain')
            put(args.out / 'FORMAL-BINDING.json', dict(passed=True, backend=bridge['backend'],
                bridge_sha256=args.bridge_sha256, adapter_sha256=sha(__file__),
                saved_audit_sha256=sha(args.out / 'REPORT.json'), new_product_executions=0,
                new_native_executions=0, referenced_old_before_matches=384))
    except BaseException as error:
        put(args.out / 'FAILURE.json', dict(error=repr(error), commands=commands.rows,
                                           adapter_sha256=sha(__file__)))
        raise


if __name__ == '__main__':
    main()
