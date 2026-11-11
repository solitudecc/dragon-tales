#!/usr/bin/env python3
"""Independent standalone-prefix oracle; emit is preparation, run requires VM.

This synthetic four-pointer ABI tests prefix copying, not createGuardedEntryView.
The existing canonical ten-mode guarded-entry model is deliberately untouched.
"""
import argparse
import ctypes
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

MODES = ('entry_phi', 'diamond_no_dead_latch', 'loop', 'fresh')
VALUES = (0, 1, 1 << 63, (1 << 64) - 1)
INPUTS = tuple((m, x, n, hit, short) for m in MODES for x in VALUES
               for n in range(4) for hit in (False, True) for short in (False, True))
FAULTS = ('wrong_phi', 'drop_store', 'stale_load', 'fixed_frame')
MASK = (1 << 64) - 1
ARENA = 0x12000
REGS, GUEST, EXIT, CONFIG = 0x100, 0x1000, 0x400, 0x500


def ir(mode, fault=None):
    """No expected result is derived from this IR generator."""
    looping = mode in ('loop', 'fresh')
    seed = '7' if fault == 'wrong_phi' else '%seed'
    fresh = ('  %fresh = add i64 %old, 0' if fault == 'stale_load' else
             '  %loaded = load i64, ptr %address, align 1\n  %fresh = freeze i64 %loaded')
    store = '' if fault == 'drop_store' else '  store i64 %written, ptr %address, align 1\n'
    frame = '%index' if fault == 'fixed_frame' else '%nextframe'
    nextwrite = '''  %nextaddress = getelementptr i8, ptr %guest, i64 %nextframe
  %nextwritten = xor i64 %seed, %next
  store i64 %nextwritten, ptr %nextaddress, align 1
''' if mode == 'fresh' else ''
    branch = '''  %odd = and i64 %seed, 1
  %choose = icmp ne i64 %odd, 0
  br i1 %choose, label %left, label %right
left:
  %plus = add i64 %fresh, 1
  store i64 %plus, ptr %address, align 1
  br label %join
right:
  %minus = sub i64 %fresh, 1
  store i64 %minus, ptr %address, align 1
  br label %join
join:
  %selected = phi i64 [ %plus, %left ], [ %minus, %right ]
  br label %done
''' if mode == 'diamond_no_dead_latch' else '''  br i1 %again, label %latch, label %done
'''
    donepred = 'join' if mode == 'diamond_no_dead_latch' else 'read'
    selected = '%selected' if mode == 'diamond_no_dead_latch' else '%fresh'
    limit = '%limit' if looping else '0'
    text = f'''source_filename = "prefix-independent-{mode}"
target datalayout = "e-p:64:64"
define i64 @oracle(ptr %regs, ptr %guest, ptr %exit, ptr %config) {{
entry:
  %xp = getelementptr i8, ptr %regs, i64 8
  %x = load i64, ptr %xp, align 1
  %seed = freeze i64 %x
  %np = getelementptr i8, ptr %regs, i64 16
  %n = load i64, ptr %np, align 1
  %limit = and i64 %n, 3
  %ip = getelementptr i8, ptr %regs, i64 24
  %index = load i64, ptr %ip, align 1
  %size = load i64, ptr %config, align 1
  %room = icmp uge i64 %size, 264
  br i1 %room, label %guard, label %fallback
guard:
  %gp = getelementptr i8, ptr %guest, i64 256
  %g = load i64, ptr %gp, align 1
  %match = icmp eq i64 %g, 14
  br i1 %match, label %guarded.accept, label %fallback
guarded.accept:
  br label %header
fallback:
  br label %header
header:
  %iteration = phi i64 [ 0, %guarded.accept ], [ 0, %fallback ], [ %next, %latch ]
  %frame = phi i64 [ %index, %guarded.accept ], [ %index, %fallback ], [ {frame}, %latch ]
  %old = phi i64 [ {seed}, %guarded.accept ], [ %seed, %fallback ], [ %fresh, %latch ]
  %enough = icmp uge i64 %size, 8
  %end = sub i64 %size, 8
  %within = icmp ule i64 %frame, %end
  %fits = and i1 %enough, %within
  br i1 %fits, label %read, label %failure
read:
  %address = getelementptr i8, ptr %guest, i64 %frame
{fresh}
  %again = icmp ult i64 %iteration, {limit}
{branch}latch:
  %next = add i64 %iteration, 1
  %sum = add i64 %fresh, %seed
  %written = add i64 %sum, %next
{store}  %nextframe = xor i64 %frame, 768
{nextwrite}  br label %header
done:
  %value = phi i64 [ {selected}, %{donepred} ]
  %outold = phi i64 [ %old, %{donepred} ]
  %outframe = phi i64 [ %frame, %{donepred} ]
  %outiteration = phi i64 [ %iteration, %{donepred} ]
  %a = xor i64 %value, %outold
  %result = xor i64 %a, %seed
  br label %finish
failure:
  %bad = xor i64 %frame, 13474738735092580352
  br label %finish
finish:
  %answer = phi i64 [ %result, %done ], [ %bad, %failure ]
  %finalold = phi i64 [ %outold, %done ], [ %old, %failure ]
  %finalframe = phi i64 [ %outframe, %done ], [ %frame, %failure ]
  %finaliteration = phi i64 [ %outiteration, %done ], [ %iteration, %failure ]
  %finalfresh = phi i64 [ %value, %done ], [ 0, %failure ]
  %status = phi i64 [ 1, %done ], [ 5, %failure ]
  %valid = phi i64 [ 1, %done ], [ 0, %failure ]
  %detail = phi i64 [ 0, %done ], [ 3, %failure ]
  store i64 %answer, ptr %regs, align 1
  store i64 %finaliteration, ptr %np, align 1
  store i64 %finalframe, ptr %ip, align 1
  %oldp = getelementptr i8, ptr %regs, i64 40
  store i64 %finalold, ptr %oldp, align 1
  %freshp = getelementptr i8, ptr %regs, i64 48
  store i64 %finalfresh, ptr %freshp, align 1
  store i64 %status, ptr %exit, align 1
  %e1 = getelementptr i8, ptr %exit, i64 8
  store i64 0, ptr %e1, align 1
  %e2 = getelementptr i8, ptr %exit, i64 16
  store i64 4096, ptr %e2, align 1
  %e3 = getelementptr i8, ptr %exit, i64 24
  store i64 57005, ptr %e3, align 1
  %e4 = getelementptr i8, ptr %exit, i64 32
  store i64 %valid, ptr %e4, align 1
  %e5 = getelementptr i8, ptr %exit, i64 40
  store i64 0, ptr %e5, align 1
  %e6 = getelementptr i8, ptr %exit, i64 48
  store i64 %detail, ptr %e6, align 1
  ret i64 %answer
}}
'''
    if mode == 'diamond_no_dead_latch':
        # Separately named derivative of the preserved, rejected diamond.
        # The removed latch was unreachable, and these PHIs select identical
        # entry values on both remaining real incoming edges.
        text = re.sub(r'(?ms)^latch:\n.*?(?=^done:)', '', text)
        for name, incoming in (('iteration', '0'), ('frame', '%index'), ('old', '%seed')):
            text = re.sub(r'(?m)^  %' + name + r' = phi[^\n]*\n', '', text)
            text = re.sub(r'%' + name + r'\b', incoming, text)
    return text


def put(a, offset, value):
    struct.pack_into('<Q', a, offset, value & MASK)


def get(a, offset):
    return struct.unpack_from('<Q', a, offset)[0]


def initial(key):
    mode, x, n, hit, short = key
    a = bytearray(bytes((i * 37 + (x & 255)) & 255 for i in range(256)) * (ARENA // 256))
    put(a, REGS + 8, x)
    put(a, REGS + 16, n)
    put(a, REGS + 24, 256)
    put(a, CONFIG, 263 if short else 65536)
    put(a, GUEST + 256, 14 if hit else 34)
    put(a, GUEST + 512, 51)
    return a


def model(key, fault=None):
    mode, x, n, hit, short = key
    a = initial(key)
    frame, iteration, old = 256, 0, 7 if fault == 'wrong_phi' and hit and not short else x
    limit = n if mode in ('loop', 'fresh') else 0
    while True:
        if short:
            value, result, ok = 0, frame ^ 13474738735092580352, False
            break
        value = old if fault == 'stale_load' else get(a, GUEST + frame)
        if mode == 'diamond_no_dead_latch':
            value = (value + (1 if x & 1 else -1)) & MASK
            put(a, GUEST + frame, value)
        if iteration == limit:
            result, ok = value ^ old ^ x, True
            break
        iteration += 1
        if fault != 'drop_store':
            put(a, GUEST + frame, value + x + iteration)
        old = value
        frame = 256 if fault == 'fixed_frame' else frame ^ 768
        if mode == 'fresh':
            put(a, GUEST + frame, x ^ iteration)
    for offset, value in ((0, result), (16, iteration), (24, frame), (40, old), (48, value)):
        put(a, REGS + offset, value)
    for i, value in enumerate((1 if ok else 5, 0, 4096, 57005, int(ok), 0, 0 if ok else 3)):
        put(a, EXIT + 8 * i, value)
    return struct.pack('<QQ', result & MASK, 0) + a


def same(actual, expected, label):
    if actual != expected:
        first = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), None)
        raise AssertionError(f'{label}: first_difference={first} lengths={len(actual)}/{len(expected)}')


def emit(directory):
    directory.mkdir(parents=True, exist_ok=False)
    for mode in MODES:
        (directory / f'{mode}.ll').write_text(ir(mode))
    for fault in FAULTS:
        (directory / f'control_{fault}.ll').write_text(ir('loop', fault))
    (directory / 'reject_historical.ll').write_text('''define i64 @oracle(i64 %x) {
entry:
  br label %history
history:
  %old = phi i64 [ %x, %entry ], [ %next, %guarded.accept ]
  %next = add i64 %old, 1
  %again = icmp ult i64 %next, 3
  br i1 %again, label %guarded.accept, label %done
guarded.accept:
  br label %history
done:
  ret i64 %next
}
''')
    (directory / 'reject_liveout.ll').write_text('''declare void @opaque()
define i64 @oracle(i64 %x) {
entry:
  %condition = icmp eq i64 %x, 0
  br i1 %condition, label %guarded.accept, label %fallback
guarded.accept:
  br label %header
fallback:
  br label %header
header:
  %value = phi i64 [ %x, %guarded.accept ], [ %x, %fallback ]
  %result = add i64 %value, 1
  br label %outside
outside:
  call void @opaque()
  ret i64 %result
}
''')
    (directory / 'inputs.json').write_text(json.dumps(INPUTS, indent=2) + '\n')
    (directory / 'contract.json').write_text(json.dumps({
        'abi': 'synthetic prefix-only four pointers; i64 return',
        'inputs': len(INPUTS), 'arena_bytes': ARENA, 'snapshot_bytes': ARENA + 16,
        'model_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'faults': FAULTS, 'exclusions': [], 'default_budget_gain_required': False,
        'full_region_gain_required': MODES,
    }, indent=2) + '\n')


def run(before, after, output, clang, opt):
    output.mkdir(parents=True, exist_ok=False)
    report = {'normal': 0, 'fault_observations': 0, 'required_faults': 0,
              'detected_faults': 0, 'comparator_controls': 0, 'faults': {},
              'precision': {}, 'opt': opt, 'failures': []}
    arena = (ctypes.c_ubyte * ARENA)()
    address = ctypes.addressof(arena)

    def compile_ir(path, stem):
        so = output / (stem + '.so')
        with (output / (stem + '.compile.log')).open('wb') as log:
            subprocess.run([clang, '-shared', '-fPIC', f'-O{opt}', '-Wno-override-module',
                            str(path), '-o', str(so)], stdout=log, stderr=log, check=True)
        lib = ctypes.CDLL(str(so.resolve()))
        fn = lib.oracle
        fn.argtypes, fn.restype = [ctypes.c_void_p] * 4, ctypes.c_uint64
        return lib, fn

    def invoke(fn, key):
        state = initial(key)
        ctypes.memmove(address, bytes(state), ARENA)
        result = fn(address + REGS, address + GUEST, address + EXIT, address + CONFIG)
        return struct.pack('<QQ', result, 0) + bytes(arena)

    try:
        for mode in MODES:
            text = (after / f'{mode}.ll').read_text()
            accepted = re.search(r'(?m)^guarded\.accept:[^\n]*\n\s*br label %header\.entry\.prefix\b', text)
            if not accepted:
                raise AssertionError(f'{mode}: actual seed redirect gate failed')
            if mode in ('loop', 'fresh'):
                latch = re.search(r'(?ms)^latch\.entry\.prefix:[^\n]*\n(.*?)(?=^[A-Za-z_.][A-Za-z_0-9.]*:|^})', text)
                if not latch or not re.search(r'br label %header\s*(?:,|$)', latch.group(1), re.M):
                    raise AssertionError(f'{mode}: cloned latch must reenter original header')
            report['precision'][mode] = {'seed_redirect': True, 'loop_returns_original': mode in ('loop', 'fresh')}
        for stage, directory in (('before', before), ('after', after)):
            for mode in MODES:
                lib, fn = compile_ir(directory / f'{mode}.ll', stage + '-' + mode)
                with gzip.open(output / f'{stage}-{mode}.snapshots.gz', 'wb') as stream:
                    for key in INPUTS:
                        if key[0] != mode:
                            continue
                        actual, expected = invoke(fn, key), model(key)
                        stream.write(actual)
                        same(actual, expected, str((stage, key)))
                        report['normal'] += 1
                        for offset in (0, 8, 16 + REGS, 16 + GUEST + 256, 16 + EXIT + 48):
                            damaged = bytearray(actual)
                            damaged[offset] ^= 1
                            try:
                                same(damaged, expected, 'comparator control')
                            except AssertionError:
                                report['comparator_controls'] += 1
                            else:
                                raise AssertionError('comparator control escaped')
        for fault in FAULTS:
            required = detected = 0
            lib, fn = compile_ir(before / f'control_{fault}.ll', 'control-' + fault)
            with gzip.open(output / f'control-{fault}.snapshots.gz', 'wb') as stream:
                for key in INPUTS:
                    if key[0] != 'loop':
                        continue
                    actual, expected, damaged = invoke(fn, key), model(key), model(key, fault)
                    stream.write(actual)
                    same(actual, damaged, str((fault, key, 'fault model')))
                    report['fault_observations'] += 1
                    if damaged != expected:
                        report['required_faults'] += 1
                        required += 1
                        try:
                            same(actual, expected, str((fault, key)))
                        except AssertionError:
                            report['detected_faults'] += 1
                            detected += 1
                        else:
                            raise AssertionError('fault escaped')
            report['faults'][fault] = {'required': required, 'detected': detected}
            if not required or detected != required:
                raise AssertionError(f'{fault}: ineffective fault control')
    except Exception as error:
        report['failures'].append(repr(error))
        raise
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    create = sub.add_parser('emit')
    create.add_argument('directory', type=Path)
    execute = sub.add_parser('run')
    execute.add_argument('--before', type=Path, required=True)
    execute.add_argument('--after', type=Path, required=True)
    execute.add_argument('--output', type=Path, required=True)
    execute.add_argument('--clang', default='clang-21')
    execute.add_argument('--opt', type=int, choices=(0, 2), default=2)
    args = parser.parse_args()
    if args.command == 'emit':
        emit(args.directory)
    else:
        run(args.before, args.after, args.output, args.clang, args.opt)
