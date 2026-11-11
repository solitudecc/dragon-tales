#!/usr/bin/env python3
"""Independent terminal-RET leaf semantics for the standalone v3 prefix tool."""
import argparse
import ctypes
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

MODES = ('terminal_default', 'terminal_duplicate')
VALUES = (0, 1, 1 << 63, (1 << 64) - 1)
INPUTS = tuple((m, x, hit, short) for m in MODES for x in VALUES
               for hit in (False, True) for short in (False, True))
FAULTS = {'wrong_default': 'terminal_default', 'drop_store': 'terminal_default',
          'wrong_phi': 'terminal_duplicate'}
MASK, ARENA = (1 << 64) - 1, 0x12000
REGS, GUEST, EXIT, CONFIG = 0x100, 0x1000, 0x400, 0x500


def ir(mode, fault=None, shared=False):
    duplicate = mode == 'terminal_duplicate'
    effect = '' if fault == 'drop_store' else '  store i64 %changed, ptr %address, align 1\n'
    text = '''source_filename = "terminal-leaf-independent"
target datalayout = "e-p:64:64"
define i64 @oracle(ptr %regs, ptr %guest, ptr %exit, ptr %config) {
entry:
  %xp = getelementptr i8, ptr %regs, i64 8
  %x = load i64, ptr %xp, align 1
  %size = load i64, ptr %config, align 1
  %room = icmp uge i64 %size, 776
  br i1 %room, label %guard, label %boundsfail
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
  %seed = phi i64 [ %x, %guarded.accept ], [ %x, %fallback ]
  %address = getelementptr i8, ptr %guest, i64 256
  %prior = load i64, ptr %address, align 1
  %changed = xor i64 %prior, 85
'''
    text += effect + '''  %loaded = load i64, ptr %address, align 1
  %fresh = freeze i64 %loaded
  %selector = add i64 %fresh, %seed
  switch i64 %selector, label %default [
    i64 91, label %hit
'''
    if duplicate:
        text += '    i64 92, label %hit\n'
    text += '  ]\n'
    if shared:
        # Both paths have already executed header; its ordinary selector use
        # in shared default remains valid but cannot enter a unique-source leaf.
        text += 'hit:\n  br label %default\n'
    else:
        observed = '7' if fault == 'wrong_phi' else '%selector'
        hitprefix = ('  %chosen = phi i64 [ ' + observed + ', %header ], [ ' + observed + ', %header ]\n') if duplicate else '  %chosen = add i64 %selector, 0\n'
        text += leaf('hit', '%chosen', '%fresh', '%x', hitprefix, False)
    defaultprefix = ('  %defaultvalue = add i64 %selector, 1\n' if fault == 'wrong_default'
                     else '  %defaultvalue = add i64 %selector, 0\n')
    text += leaf('default', '%defaultvalue', '%fresh', '%x', defaultprefix, True)
    text += leaf('boundsfail', '256', '0', '%x', '', True, bounds=True)
    return text + '}\n'


def leaf(name, observed, fresh, x, prefix, error, bounds=False):
    def v(s):
        return '%' + name + '.' + s
    text = name + ':\n' + prefix
    text += f'  {v("result")} = xor i64 {observed}, {x}\n'
    text += f'  store i64 {v("result")}, ptr %regs, align 1\n'
    for offset, value in ((16, '0'), (24, '256'), (40, fresh), (48, observed)):
        text += f'  {v("r" + str(offset))} = getelementptr i8, ptr %regs, i64 {offset}\n'
        text += f'  store i64 {value}, ptr {v("r" + str(offset))}, align 1\n'
    if not bounds:
        text += f'  {v("mem")} = getelementptr i8, ptr %guest, i64 768\n'
        text += f'  {v("effect")} = xor i64 {observed}, {fresh}\n'
        text += f'  store i64 {v("effect")}, ptr {v("mem")}, align 1\n'
    values = ('5' if error else '1', '0', observed, observed if error else '57005',
              '0' if error else '1', '0', '3' if bounds else '6' if error else '0')
    for i, value in enumerate(values):
        text += f'  {v("e" + str(i))} = getelementptr i8, ptr %exit, i64 {8 * i}\n'
        text += f'  store i64 {value}, ptr {v("e" + str(i))}, align 1\n'
    text += f'  ret i64 {v("result")}\n'
    return text


def put(a, offset, value):
    struct.pack_into('<Q', a, offset, value & MASK)


def get(a, offset):
    return struct.unpack_from('<Q', a, offset)[0]


def initial(key):
    mode, x, hit, short = key
    a = bytearray(bytes((i * 37 + (x & 255)) & 255 for i in range(256)) * (ARENA // 256))
    put(a, REGS + 8, x)
    put(a, CONFIG, 775 if short else 65536)
    put(a, GUEST + 256, 14 if hit else 34)
    return a


def model(key, fault=None):
    mode, x, hit, short = key
    a = initial(key)
    if short:
        observed, fresh, error, detail = 256, 0, True, 3
    else:
        prior = get(a, GUEST + 256)
        if fault != 'drop_store':
            put(a, GUEST + 256, prior ^ 85)
        fresh = get(a, GUEST + 256)
        selector = (fresh + x) & MASK
        error = selector not in ((91, 92) if mode == 'terminal_duplicate' else (91,))
        observed = (selector + 1) & MASK if error and fault == 'wrong_default' else 7 if not error and fault == 'wrong_phi' else selector
        detail = 6 if error else 0
        put(a, GUEST + 768, observed ^ fresh)
    answer = observed ^ x
    for offset, value in ((0, answer), (16, 0), (24, 256), (40, fresh), (48, observed)):
        put(a, REGS + offset, value)
    fields = (5 if error else 1, 0, observed, observed if error else 57005,
              0 if error else 1, 0, detail)
    for i, value in enumerate(fields):
        put(a, EXIT + 8 * i, value)
    return struct.pack('<QQ', answer & MASK, 0) + a


def same(actual, expected, label):
    if actual != expected:
        first = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), None)
        raise AssertionError((label, first, len(actual), len(expected)))


def emit(directory):
    directory.mkdir(parents=True, exist_ok=False)
    for mode in MODES:
        (directory / (mode + '.ll')).write_text(ir(mode))
    for fault, mode in FAULTS.items():
        (directory / ('control_' + fault + '.ll')).write_text(ir(mode, fault))
    (directory / 'reject_shared_leaf.ll').write_text(ir('terminal_default', shared=True))
    (directory / 'inputs.json').write_text(json.dumps(INPUTS, indent=2) + '\n')
    (directory / 'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest() + '\n')


def run(before, after, output, opt):
    output.mkdir(parents=True, exist_ok=False)
    report = {'normal': 0, 'fault_observations': 0, 'faults': {},
              'comparator_controls': 0, 'precision': {}, 'opt': opt, 'failures': []}
    arena = (ctypes.c_ubyte * ARENA)()
    address = ctypes.addressof(arena)

    def compile_ir(path, name):
        so = output / (name + '.so')
        with (output / (name + '.compile.log')).open('wb') as log:
            subprocess.run(['clang-21', '-shared', '-fPIC', f'-O{opt}', '-Wno-override-module',
                            str(path), '-o', str(so)], stdout=log, stderr=log, check=True)
        lib = ctypes.CDLL(str(so.resolve()))
        fn = lib.oracle
        fn.argtypes, fn.restype = [ctypes.c_void_p] * 4, ctypes.c_uint64
        return lib, fn

    def invoke(fn, key):
        ctypes.memmove(address, bytes(initial(key)), ARENA)
        answer = fn(address + REGS, address + GUEST, address + EXIT, address + CONFIG)
        return struct.pack('<QQ', answer, 0) + bytes(arena)

    try:
        for mode in MODES:
            text = (after / (mode + '.ll')).read_text()
            assert re.search(r'(?m)^guarded\.accept:[^\n]*\n\s*br label %header\.entry\.prefix\b', text)
            assert re.search(r'(?m)^default\.entry\.prefix:', text)
            assert re.search(r'(?m)^hit\.entry\.prefix:', text)
            switch = re.search(r'(?ms)^header\.entry\.prefix:[^\n]*\n(.*?\n  ])', text)
            assert switch and 'label %default.entry.prefix' in switch.group(1)
            assert switch.group(1).count('label %hit.entry.prefix') == (2 if mode == 'terminal_duplicate' else 1)
            report['precision'][mode] = {'fresh_selector_and_terminal_leaves_copied': True}
        for stage, directory in (('before', before), ('after', after)):
            for mode in MODES:
                lib, fn = compile_ir(directory / (mode + '.ll'), stage + '-' + mode)
                with gzip.open(output / f'{stage}-{mode}.snapshots.gz', 'wb') as stream:
                    for key in INPUTS:
                        if key[0] != mode:
                            continue
                        actual = invoke(fn, key)
                        stream.write(actual)
                        expected = model(key)
                        same(actual, expected, (stage, key))
                        report['normal'] += 1
                        for offset in (0, 16 + REGS, 16 + GUEST + 768) + tuple(16 + EXIT + 8 * i for i in range(7)):
                            damaged = bytearray(actual)
                            damaged[offset] ^= 1
                            try:
                                same(damaged, expected, 'comparator control')
                            except AssertionError:
                                report['comparator_controls'] += 1
                            else:
                                raise AssertionError('comparator escaped')
        for fault, mode in FAULTS.items():
            lib, fn = compile_ir(before / ('control_' + fault + '.ll'), 'control-' + fault)
            required = caught = 0
            with gzip.open(output / f'control-{fault}.snapshots.gz', 'wb') as stream:
                for key in INPUTS:
                    if key[0] != mode:
                        continue
                    actual, expected, damaged = invoke(fn, key), model(key), model(key, fault)
                    stream.write(actual)
                    same(actual, damaged, (fault, key, 'fault model'))
                    report['fault_observations'] += 1
                    if damaged != expected:
                        required += 1
                        try:
                            same(actual, expected, (fault, key))
                        except AssertionError:
                            caught += 1
                        else:
                            raise AssertionError('fault escaped')
            report['faults'][fault] = {'required': required, 'caught': caught}
            assert required and caught == required
    except Exception as error:
        report['failures'].append(repr(error))
        raise
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('emit')
    p.add_argument('directory', type=Path)
    p = sub.add_parser('run')
    p.add_argument('--before', required=True, type=Path)
    p.add_argument('--after', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--opt', type=int, choices=(0, 2), required=True)
    args = parser.parse_args()
    if args.command == 'emit':
        emit(args.directory)
    else:
        run(args.before, args.after, args.output, args.opt)
