#!/usr/bin/env python3
"""Frozen sequential oracle for actual checked cyclic-corridor production runs."""
import argparse
import ctypes
import gzip
import hashlib
import itertools
import json
from pathlib import Path
import struct
import subprocess

MODES = ('loop', 'table_before', 'table_after', 'history', 'duplicate',
         'liveout', 'sidecall', 'call_inside')
MASK = (1 << 64) - 1
ARENA, REGS, GUEST, EXIT, DESC, CONFIG = 0x12000, 0x100, 0x1000, 0x400, 0x500, 0x600
TARGETS = (43, 44, 45, 99999)
REPLACEMENTS = (43, 44, 777, 99999)
INPUTS = []
for mode in MODES:
    for i, (n, h, state, target) in enumerate(itertools.product(
            (0, 1, 2, 3, 7), (0, 1, 1 << 63, MASK), (42, 43), TARGETS)):
        INPUTS.append((mode, n, h, state, target, 65536 if i == 159 else 1024,
                       (i // 4) % 2, (0, 1, 2, 6)[(i // 8) % 4],
                       REPLACEMENTS[(i // 16) % 4]))
    for alias, size, target in itertools.product((0, 1), (0, 855, 856, 864), TARGETS):
        INPUTS.append((mode, 0 if size == 0 else 2, MASK, 43 if alias else 42,
                       target, size, alias, 1, REPLACEMENTS[TARGETS.index(target)]))
INPUTS = tuple(INPUTS)
assert len(INPUTS) == len(set(INPUTS)) == 1536
FAULTS = {
    'double_prefix': 'loop', 'omit_prefix': 'loop', 'frozen_state': 'loop',
    'stale_history': 'history', 'stale_observation': 'history', 'reload_selector': 'table_after',
    'drop_table_store': 'table_after', 'wrong_root_phi': 'duplicate',
    'wrong_liveout': 'liveout', 'drop_case45': 'loop', 'omit_sidecall': 'sidecall',
}
HELPER = r'''#include <stdint.h>
#include <string.h>
uint64_t business_calls;
static uint64_t rd(unsigned char*p){uint64_t v;memcpy(&v,p,8);return v;}
static void wr(unsigned char*p,uint64_t v){memcpy(p,&v,8);}
void prefix_unknown(unsigned char*r,unsigned char*g,unsigned char*c){
 uint64_t target=rd(c),h=rd(c+24),state=rd(c+32);++business_calls;
 wr(r,h+0x31);wr(r+8,h^0x55);wr(r+16,state);wr(r+24,state&1);
 wr(r+48,0x8888);wr(r+56,rd(r+56)+h+3);wr(g+80,0x9abc);
 wr(g+128,0);wr(g+160,state&1);wr(g+848,target);wr(g+856,target);
}
void side_unknown(unsigned char*r,unsigned char*g,unsigned char*c,uint64_t i){
 uint64_t replacement=rd(c+8);++business_calls;
 wr(r,rd(r)+i+11);wr(r+16,rd(r+16)^1);wr(r+48,0xbeef+i);
 wr(g+80,0xcafe + i);wr(g+128,rd(g+128)+i+3);
 wr(g+848,replacement);wr(g+856,replacement);wr(r+56,rd(r+56)+i+0x33);
}
'''


def put(arena, offset, value):
    struct.pack_into('<Q', arena, offset, value & MASK)


def get(arena, offset):
    return struct.unpack_from('<Q', arena, offset)[0]


def regoff(key):
    return GUEST + 32 if key[6] else REGS


def initial(key, guest):
    _, n, h, state, target, size, alias, trigger, replacement = key
    arena = bytearray((i * 41 + 19) & 255 for i in range(ARENA))
    r = regoff(key)
    for offset, value in ((0, h), (8, h ^ 0x91), (16, state), (24, 0),
                          (32, 0x1234), (40, n), (48, 0x1111), (56, 0x2222)):
        put(arena, r + offset, value)
    put(arena, GUEST + 128, 0xabc)
    put(arena, GUEST + 160, state & 1)
    put(arena, GUEST + 848, 777)
    put(arena, GUEST + 856, 777)
    put(arena, DESC, guest)
    put(arena, DESC + 8, size)
    for offset, value in ((0, target), (8, replacement), (16, trigger), (24, h), (32, state)):
        put(arena, CONFIG + offset, value)
    return arena


def finish(label, kind, remaining, iteration, state, history, acc, last, tag, extra='0'):
    values = (acc, history, state, last, iteration, '%x', str(tag), extra)
    result = label + ':\n'
    previous = values[0]
    for i, value in enumerate(values[1:]):
        current = f'%{label}.answer{i}'
        result += f'  {current} = xor i64 {previous}, {value}\n'
        previous = current
    for offset, value in ((0, previous), (8, history), (16, state), (24, iteration),
                          (32, acc), (40, remaining), (48, last)):
        result += f'  %{label}.r{offset} = getelementptr i8, ptr %regs, i64 {offset}\n'
        result += f'  store i64 {value}, ptr %{label}.r{offset}, align 1\n'
    for i, value in enumerate((str(kind), '0', str(0xd000 + tag), last,
                               '1' if kind == 1 else '0', '%x', str(tag))):
        result += f'  %{label}.e{i} = getelementptr i8, ptr %exit, i64 {i*8}\n'
        result += f'  store i64 {value}, ptr %{label}.e{i}, align 1\n'
    return result + f'  ret i64 {previous}\n'


def ir(mode, fault=None):
    assert mode in MODES
    duplicate = mode == 'duplicate'
    history = mode == 'history'
    seed_count = 2 if duplicate else 1
    terminal_count = 2 if duplicate else 1
    source = '''source_filename = "guarded-checked-corridors-independent"
target datalayout = "e-p:64:64"
declare void @prefix_unknown(ptr,ptr,ptr)
declare void @side_unknown(ptr,ptr,ptr,i64)
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %sizep = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizep, align 1
  %x = load i64, ptr %regs, align 1
  %hp = getelementptr i8, ptr %regs, i64 8
  %initial_history = load i64, ptr %hp, align 1
  %np = getelementptr i8, ptr %regs, i64 40
  %initial_n = load i64, ptr %np, align 1
  %statep = getelementptr i8, ptr %regs, i64 16
  %sidep = getelementptr i8, ptr %regs, i64 48
  %ledgerp = getelementptr i8, ptr %guest, i64 128
  %seedp = getelementptr i8, ptr %guest, i64 160
'''
    call = '  call void @prefix_unknown(ptr %regs, ptr %guest, ptr %config)\n'
    if fault != 'omit_prefix':
        source += call * (2 if fault == 'double_prefix' else 1)
    source += '''  %post_call_state = load i64, ptr %statep, align 1
  %statebit = and i64 %post_call_state, 1
  %statebool = icmp ne i64 %statebit, 0
  %seed_state = select i1 %statebool, i64 43, i64 42
'''
    if history:
        source += '  %initial_observation = load i64, ptr %ledgerp, align 1\n'
    source += '''
  %target = load i64, ptr %config, align 1
  %replacementp = getelementptr i8, ptr %config, i64 8
  %replacement = load i64, ptr %replacementp, align 1
  %triggerp = getelementptr i8, ptr %config, i64 16
  %trigger = load i64, ptr %triggerp, align 1
'''
    source += '  br label %' + ('head' if history else 'seed') + '\nseed:\n'
    if duplicate:
        source += '''  %seed_selector = load i64, ptr %seedp, align 1
  switch i64 %seed_selector, label %head [
    i64 0, label %head
  ], !dragon.guarded-transitions.generated !0
'''
    else:
        source += '  br label %head, !dragon.guarded-transitions.generated !0\n'
    source += 'head:\n'
    phis = {
        'remaining': ('%remaining' if history else '%initial_n', '%nnext', '%side.n', '%callee.n', '%initial_n'),
        'iteration': ('%iteration' if history else '0', '%inext', '%side.i', '%callee.i', '0'),
        'state': ('%seed_state', '%snext', '%side.s', '%callee.s', '%seed_state'),
        'history': ('%observe' if history else '%initial_history', '%accnext' if fault == 'wrong_root_phi' else '%hnext', '%side.hnext', '%callee.h', '%initial_history'),
        'acc': ('%acc' if history else '%x', '%accnext', '%side.anext', '%callee.a', '%x'),
        'last': ('%last' if history else '0', '%loaded', '%side.last', '%callee.last', '0'),
        'phase': ('1', '1', '1', '1', '0'),
    }
    for name, (seed_value, terminal_value, side_value, callee_value, entry_value) in phis.items():
        inputs = [(seed_value, 'seed')] * seed_count + [(terminal_value, 'dispatch')] * terminal_count
        if mode == 'sidecall':
            inputs.append((side_value, 'side.outside'))
        if fault != 'drop_case45':
            inputs.append((callee_value, 'callee.outside'))
        if history:
            inputs.insert(0, (entry_value, 'entry'))
        source += f'  %{name} = phi i64 ' + ', '.join(f'[ {value}, %{pred} ]' for value, pred in inputs) + '\n'
    source += '  %state.value = add i64 ' + ('%seed_state' if fault == 'frozen_state' else '%state') + ', 0\n'
    base = '%initial_history' if fault == 'stale_history' else '%history'
    if history:
        source += '  %observe.base = xor i64 ' + base + ', %state.value\n'
        source += '  %ledger = load i64, ptr %ledgerp, align 1\n'
        source += '  %fresh_observation = freeze i64 ' + ('%initial_observation' if fault == 'stale_observation' else '%ledger') + '\n'
        source += '  %observe = xor i64 %observe.base, %fresh_observation\n'
    else:
        source += '  %observe = xor i64 ' + base + ', %state.value\n'
        source += '  %ledger = load i64, ptr %ledgerp, align 1\n'
    source += '''
  %ledger_next = add i64 %ledger, %observe
  store i64 %ledger_next, ptr %ledgerp, align 1
  %bootstrap = icmp eq i64 %phase, 0
  br i1 %bootstrap, label %seed, label %stop.check
stop.check:
  %done = icmp eq i64 %remaining, 0
  br i1 %done, label %finish, label %body
body:
  %side = load i64, ptr %sidep, align 1
  %running0 = add i64 %acc, %observe
  %running1 = add i64 %running0, %side
  %running = add i64 %running1, %iteration
  %nnext = sub i64 %remaining, 1
  %inext = add i64 %iteration, 1
  %snext = xor i64 %state.value, 1
'''
    if mode == 'sidecall':
        source += '  %leave = icmp eq i64 %iteration, %trigger\n  br i1 %leave, label %side.outside, label %range\n'
    else:
        source += '  br label %range\n'
    source += '''range:
  %state_ok = icmp ult i64 %state.value, 64
  br i1 %state_ok, label %index, label %state_failure
index:
  %scaled = shl i64 %state.value, 3
  %address = add i64 %scaled, 512
  %tableptr = getelementptr i8, ptr %guest, i64 %address
  %end = add i64 %address, 8
  %room = icmp ule i64 %end, %size
  br i1 %room, label %dispatch, label %table_failure
dispatch:
  %change = icmp uge i64 %iteration, %trigger
  %stored = select i1 %change, i64 %replacement, i64 %target
'''
    if mode == 'call_inside':
        source += '  call void @side_unknown(ptr %regs, ptr %guest, ptr %config, i64 %iteration)\n'
    if mode == 'table_before':
        source += '  store i64 %stored, ptr %tableptr, align 1\n'
    source += '  %loaded = load i64, ptr %tableptr, align 1\n'
    if mode == 'table_after' and fault != 'drop_table_store':
        source += '  store i64 %stored, ptr %tableptr, align 1\n'
    selector = '%loaded'
    if fault == 'reload_selector':
        source += '  %reloaded = load i64, ptr %tableptr, align 1\n'
        selector = '%reloaded'
    source += '''  %hnext = xor i64 %observe, %loaded
  %accnext = add i64 %running, %loaded
'''
    source += '  switch i64 ' + selector + ', label %default_exit [\n    i64 43, label %head\n'
    source += '    i64 44, label %' + ('head' if duplicate else 'business') + '\n'
    if fault != 'drop_case45':
        source += '    i64 45, label %callee.outside\n'
    source += '    i64 777, label %alternate\n  ]\n'
    if mode == 'sidecall':
        source += '''side.outside:
  %side.n = phi i64 [ %nnext, %body ]
  %side.i = phi i64 [ %inext, %body ]
  %side.s = phi i64 [ %snext, %body ]
  %side.oldh = phi i64 [ %observe, %body ]
  %side.olda = phi i64 [ %running, %body ]
  %side.last = phi i64 [ %last, %body ]
  %side.calliter = phi i64 [ %iteration, %body ]
  %side.hnext = xor i64 %side.oldh, 777
  %side.anext = add i64 %side.olda, 777
'''
        if fault != 'omit_sidecall':
            source += '  call void @side_unknown(ptr %regs, ptr %guest, ptr %config, i64 %side.calliter)\n'
        source += '  br label %head\n'
    if fault != 'drop_case45':
        source += 'callee.outside:\n'
        for name, value in (('n', '%nnext'), ('i', '%inext'), ('s', '%snext'), ('h', '%hnext'),
                            ('a', '%accnext'), ('last', '%loaded'), ('calliter', '%iteration')):
            source += f'  %callee.{name} = phi i64 [ {value}, %dispatch ]\n'
        source += '  call void @side_unknown(ptr %regs, ptr %guest, ptr %config, i64 %callee.calliter)\n  br label %head\n'
    if not duplicate:
        if mode == 'liveout':
            source += 'business:\n  %business_value = phi i64 [ ' + ('0' if fault == 'wrong_liveout' else '%accnext') + ', %dispatch ]\n  %live_mix = xor i64 %running, %observe\n  br label %business_finish\n'
            source += finish('business_finish', 1, '%nnext', '%inext', '%snext', '%hnext', '%business_value', '%loaded', 2, '%live_mix')
        else:
            source += finish('business', 1, '%nnext', '%inext', '%snext', '%hnext', '%accnext', '%loaded', 2)
    source += finish('alternate', 1, '%nnext', '%inext', '%snext', '%hnext', '%accnext', '%loaded', 3)
    source += finish('default_exit', 5, '%nnext', '%inext', '%snext', '%hnext', '%accnext', '%loaded', 13)
    source += finish('state_failure', 5, '%nnext', '%inext', '%snext', '%history', '%running', '%state.value', 11)
    source += finish('table_failure', 5, '%nnext', '%inext', '%snext', '%history', '%running', '%address', 12)
    source += finish('finish', 1, '%remaining', '%iteration', '%state.value', '%history', '%acc', '%last', 1)
    return source + '}\n!0 = !{}\n'


def model(key, guest, fault=None):
    mode, n, x, start, target, size, alias, trigger, replacement = key
    arena = initial(key, guest)
    r = regoff(key)
    initial_history = get(arena, r + 8)
    calls = 0
    def prefix():
        nonlocal calls
        calls += 1
        for offset, value in ((0, x + 0x31), (8, x ^ 0x55), (16, start), (24, start & 1), (48, 0x8888)):
            put(arena, r + offset, value)
        put(arena, r + 56, get(arena, r + 56) + x + 3)
        for offset, value in ((80, 0x9abc), (128, 0), (160, start & 1), (848, target), (856, target)):
            put(arena, GUEST + offset, value)
    def external(iteration):
        nonlocal calls
        calls += 1
        put(arena, r, get(arena, r) + iteration + 11)
        put(arena, r + 16, get(arena, r + 16) ^ 1)
        put(arena, r + 48, 0xbeef + iteration)
        put(arena, GUEST + 80, 0xcafe + iteration)
        put(arena, GUEST + 128, get(arena, GUEST + 128) + iteration + 3)
        put(arena, GUEST + 848, replacement)
        put(arena, GUEST + 856, replacement)
        put(arena, r + 56, get(arena, r + 56) + iteration + 0x33)
    if fault != 'omit_prefix':
        prefix()
    if fault == 'double_prefix':
        prefix()
    seed_state = 43 if get(arena, r + 16) & 1 else 42
    initial_observation = get(arena, GUEST + 128)
    remaining, iteration, state, history, acc, last = n, 0, seed_state, initial_history, x, 0
    if mode == 'history':
        observe = (initial_history if fault == 'stale_history' else history) ^ state ^ get(arena, GUEST + 128)
        put(arena, GUEST + 128, get(arena, GUEST + 128) + observe)
        history = observe
    for visits in range(100):
        current = seed_state if fault == 'frozen_state' else state
        observe = (initial_history if fault == 'stale_history' else history) ^ current
        if mode == 'history':
            observe ^= initial_observation if fault == 'stale_observation' else get(arena, GUEST + 128)
        put(arena, GUEST + 128, get(arena, GUEST + 128) + observe)
        if remaining == 0:
            kind, tag, state = 1, 1, current
            break
        running = (acc + observe + get(arena, r + 48) + iteration) & MASK
        next_n, next_i, next_state = remaining - 1, iteration + 1, current ^ 1
        if mode == 'sidecall' and iteration == trigger:
            if fault != 'omit_sidecall':
                external(iteration)
            remaining, iteration, state, history, acc = next_n, next_i, next_state, observe ^ 777, (running + 777) & MASK
            continue
        if current >= 64:
            kind, tag, last, acc = 5, 11, current, running
            remaining, iteration, state = next_n, next_i, next_state
            break
        address = 512 + current * 8
        if address + 8 > size:
            kind, tag, last, acc = 5, 12, address, running
            remaining, iteration, state = next_n, next_i, next_state
            break
        if mode == 'call_inside':
            external(iteration)
        stored = replacement if iteration >= trigger else target
        if mode == 'table_before':
            put(arena, GUEST + address, stored)
        loaded = get(arena, GUEST + address)
        if mode == 'table_after' and fault != 'drop_table_store':
            put(arena, GUEST + address, stored)
        selected = get(arena, GUEST + address) if fault == 'reload_selector' else loaded
        next_history, next_acc = observe ^ loaded, (running + loaded) & MASK
        if selected == 43 or (selected == 44 and mode == 'duplicate'):
            remaining, iteration, state, history, acc, last = next_n, next_i, next_state, (next_acc if fault == 'wrong_root_phi' else next_history), next_acc, loaded
            continue
        if selected == 45 and fault != 'drop_case45':
            external(iteration)
            remaining, iteration, state, history, acc, last = next_n, next_i, next_state, next_history, next_acc, loaded
            continue
        remaining, iteration, state, history, acc, last = next_n, next_i, next_state, next_history, next_acc, loaded
        if selected == 44:
            kind, tag = 1, 2
            if mode == 'liveout':
                extra = running ^ observe
                if fault == 'wrong_liveout':
                    acc = 0
            else:
                extra = 0
        elif selected == 777:
            kind, tag, extra = 1, 3, 0
        else:
            kind, tag, extra = 5, 13, 0
        break
    else:
        raise AssertionError('sequential model failed its independently bounded fixture count')
    if tag != 2 or mode != 'liveout':
        extra = 0
    answer = (acc ^ history ^ state ^ last ^ iteration ^ x ^ tag ^ extra) & MASK
    for offset, value in ((0, answer), (8, history), (16, state), (24, iteration),
                          (32, acc), (40, remaining), (48, last)):
        put(arena, r + offset, value)
    for i, value in enumerate((kind, 0, 0xd000 + tag, last, int(kind == 1), x, tag)):
        put(arena, EXIT + i*8, value)
    return struct.pack('<QQ', answer, calls) + arena


def same(actual, expected, label):
    if actual != expected:
        offset = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), None)
        raise AssertionError((label, offset, len(actual), len(expected)))


def emit(directory):
    directory.mkdir(parents=True, exist_ok=False)
    for mode in MODES:
        (directory / (mode + '.ll')).write_text(ir(mode))
    for fault, mode in FAULTS.items():
        (directory / ('control_' + fault + '.ll')).write_text(ir(mode, fault))
    (directory / 'helper.c').write_text(HELPER)
    (directory / 'inputs.json').write_text(json.dumps(INPUTS, indent=2) + '\n')
    (directory / 'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest() + '\n')


def run(before, after, wrong, out, opt, before_only=False):
    out.mkdir(parents=True, exist_ok=False)
    arena = (ctypes.c_ubyte * ARENA)()
    address = ctypes.addressof(arena)
    guest = address + GUEST
    report = dict(opt=opt, guest_address=guest, normal=0, fault_observations=0,
                  faults={}, comparator_controls=0, errors=[], before_only=before_only)
    def compile_ir(path, name):
        with (out / (name + '.compile.log')).open('wb') as log:
            subprocess.run(['opt-21', '-passes=verify', '-disable-output', str(path)], stdout=log, stderr=log, check=True, timeout=180)
            so = out / (name + '.so')
            subprocess.run(['clang-21', '-shared', '-fPIC', f'-O{opt}', '-Wno-override-module', str(path), str(before / 'helper.c'), '-o', str(so)], stdout=log, stderr=log, check=True, timeout=180)
        lib = ctypes.CDLL(str(so.resolve()))
        fn = lib.oracle
        fn.argtypes = [ctypes.c_void_p] * 4
        fn.restype = ctypes.c_uint64
        return lib, fn
    def invoke(lib, fn, key):
        ctypes.memmove(address, bytes(initial(key, guest)), ARENA)
        calls = ctypes.c_uint64.in_dll(lib, 'business_calls')
        calls.value = 0
        result = fn(address + regoff(key), address + DESC, address + EXIT, address + CONFIG)
        return struct.pack('<QQ', result, calls.value) + bytes(arena)
    try:
        stages = [('before', before, MODES)]
        if not before_only:
            stages += [('after', after, MODES), ('wrong', wrong, ('loop',))]
        for stage, directory, modes in stages:
            for mode in modes:
                lib, fn = compile_ir(directory / (mode + '.ll'), stage + '-' + mode)
                with gzip.open(out / (stage + '-' + mode + '.snapshots.gz'), 'wb') as stream:
                    for key in INPUTS:
                        if key[0] != mode:
                            continue
                        actual = invoke(lib, fn, key)
                        stream.write(actual)
                        expected = model(key, guest)
                        same(actual, expected, (stage, key))
                        report['normal'] += 1
                        for offset in (0, 8, 16 + regoff(key), 16 + GUEST + 128) + tuple(16 + EXIT + i*8 for i in range(7)):
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
            with gzip.open(out / ('control-' + fault + '.snapshots.gz'), 'wb') as stream:
                for key in INPUTS:
                    if key[0] != mode:
                        continue
                    actual = invoke(lib, fn, key)
                    stream.write(actual)
                    damaged, expected = model(key, guest, fault), model(key, guest)
                    same(actual, damaged, ('fault model', fault, key))
                    report['fault_observations'] += 1
                    if damaged != expected:
                        required += 1
                        try:
                            same(actual, expected, ('fault', fault, key))
                        except AssertionError:
                            caught += 1
                        else:
                            raise AssertionError('fault escaped')
            assert required and caught == required
            report['faults'][fault] = dict(required=required, caught=caught)
    except Exception as error:
        report['errors'].append(repr(error))
        raise
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='action', required=True)
    e = sub.add_parser('emit')
    e.add_argument('directory', type=Path)
    r = sub.add_parser('run')
    for name in ('before', 'after', 'wrong', 'output'):
        r.add_argument(name, type=Path)
    r.add_argument('--opt', type=int, choices=(0, 2), required=True)
    r.add_argument('--before-only', action='store_true')
    args = parser.parse_args()
    if args.action == 'emit':
        emit(args.directory)
    else:
        run(args.before, args.after, args.wrong, args.output, args.opt, args.before_only)
