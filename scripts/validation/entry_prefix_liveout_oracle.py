#!/usr/bin/env python3
"""Independent fixtures for direct-exit ordinary SSA live-out bridges.

Preparation only until root authorizes VM compilation/execution. These fixtures
are separate from every previously frozen entry-prefix model and input set.
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

MODES = ('direct', 'diamond_local', 'loop_local', 'unknown_call', 'volatile', 'duplicate_exit')
VALUES = (0, 1, 1 << 63, (1 << 64) - 1)
INPUTS = tuple((m, x, n, hit, short) for m in MODES for x in VALUES
               for n in range(4) for hit in (False, True) for short in (False, True))
FAULTS = {'wrong_snapshot': 'loop_local', 'stale_load': 'loop_local',
          'drop_store': 'loop_local', 'wrong_history': 'loop_local',
          'drop_call': 'unknown_call', 'drop_volatile_store': 'volatile', 'wrong_exit': 'direct'}
MASK, ARENA = (1 << 64) - 1, 0x12000
REGS, GUEST, EXIT, CONFIG = 0x100, 0x1000, 0x400, 0x500
HELPER = r'''
#include <stdint.h>
#include <string.h>
uint64_t entry_liveout_calls;
void entry_liveout_unknown(unsigned char *regs,unsigned char *guest,uint64_t frame,uint64_t x){
  uint64_t value,side,r;
  ++entry_liveout_calls;
  memcpy(&r,regs+56,8);r^=x;memcpy(regs+56,&r,8);
  memcpy(&side,guest+768,8);side+=(x|UINT64_C(1));memcpy(guest+768,&side,8);
  memcpy(&value,guest+frame,8);value^=x^UINT64_C(0x9e3779b97f4a7c15);memcpy(guest+frame,&value,8);
}
'''


def finish(label, answer, frame, iteration, history, fresh, after, selector, failure=False, wrong_exit=False):
    """Only operands explicitly supplied by the local predecessor are used."""
    p = lambda name: '%' + label + '.' + name
    text = label + ':\n'
    for offset, value in ((0, answer), (16, iteration), (24, frame), (40, history), (48, fresh), (64, after)):
        text += f'  {p("r"+str(offset))} = getelementptr i8, ptr %regs, i64 {offset}\n'
        text += f'  store i64 {value}, ptr {p("r"+str(offset))}, align 1\n'
    text += f'  {p("pc")} = xor i64 {selector}, 4096\n'
    source = p('pc')
    if wrong_exit:
        text += f'  {p("wrongpc")} = add i64 {source}, 1\n'
        source = p('wrongpc')
    fields = ('5' if failure else '1', '0', source, '4096' if failure else '57005',
              '0' if failure else '1', '0', '3' if failure else '0')
    for i, value in enumerate(fields):
        text += f'  {p("e"+str(i))} = getelementptr i8, ptr %exit, i64 {8*i}\n'
        text += f'  store i64 {value}, ptr {p("e"+str(i))}, align 1\n'
    return text + f'  ret i64 {answer}\n'


def ir(mode, fault=None):
    assert mode in MODES
    fresh = '%history' if fault == 'stale_load' else '%loaded'
    snapshot = '%history' if fault == 'wrong_snapshot' else '%fresh'
    history_next = 'xor i64 %snapshot_fresh, 1' if fault == 'wrong_history' else 'add i64 %snapshot_fresh, 0'
    text = '''source_filename = "entry-prefix-liveout-independent"
target datalayout = "e-p:64:64"
declare void @entry_liveout_unknown(ptr, ptr, i64, i64)
define i64 @oracle(ptr %regs, ptr %guest, ptr %exit, ptr %config) {
entry:
  %xp = getelementptr i8, ptr %regs, i64 8
  %x = load i64, ptr %xp, align 1
  %seed = freeze i64 %x
  %np = getelementptr i8, ptr %regs, i64 16
  %n = load i64, ptr %np, align 1
  %limit = and i64 %n, 3
  %size = load i64, ptr %config, align 1
  %room = icmp uge i64 %size, 904
  br i1 %room, label %guard, label %boundsfail
guard:
  %gp = getelementptr i8, ptr %guest, i64 256
  %g = load i64, ptr %gp, align 1
  %match = icmp eq i64 %g, 14
  br i1 %match, label %guarded.accept, label %fallback
guarded.accept:
  br label %definition
fallback:
  br label %definition
definition:
  %iteration = phi i64 [ 0, %guarded.accept ], [ 0, %fallback ], [ %next_iteration, %latch ]
  %frame = phi i64 [ 256, %guarded.accept ], [ 256, %fallback ], [ %next_frame, %latch ]
  %history = phi i64 [ %seed, %guarded.accept ], [ %seed, %fallback ], [ %history_next, %latch ]
  %address = getelementptr i8, ptr %guest, i64 %frame
  %loaded = load i64, ptr %address, align 1
'''
    text += f'  %fresh = freeze i64 {fresh}\n  %selector = xor i64 %fresh, %seed\n'
    # A switch boundary keeps Out original. The duplicate mode deliberately has
    # three successor slots to the same predecessor block, not three sources.
    text += '  switch i64 %selector, label %out [\n'
    if mode == 'duplicate_exit':
        text += '    i64 14, label %out\n    i64 15, label %out\n'
    text += '  ]\nout:\n'
    text += f'  %snapshot_fresh = add i64 {snapshot}, 0\n'
    text += '''  %snapshot_frame = add i64 %frame, 0
  %snapshot_iteration = add i64 %iteration, 0
  %snapshot_history = add i64 %history, 0
  %snapshot_selector = add i64 %selector, 0
  %local_address = getelementptr i8, ptr %guest, i64 %snapshot_frame
  %next_iteration = add i64 %snapshot_iteration, 1
  %next_frame = xor i64 %snapshot_frame, 768
  %store_sum = add i64 %snapshot_fresh, %seed
  %stored = add i64 %store_sum, %next_iteration
'''
    text += f'  %history_next = {history_next}\n'
    if mode == 'unknown_call':
        if fault != 'drop_call':
            text += '  call void @entry_liveout_unknown(ptr %regs, ptr %guest, i64 %snapshot_frame, i64 %seed)\n'
        text += '  %after_effect = load i64, ptr %local_address, align 1\n  %observed = xor i64 %snapshot_fresh, %after_effect\n'
    elif mode == 'volatile':
        text += '''  %counterp = getelementptr i8, ptr %guest, i64 896
  %counter = load volatile i64, ptr %counterp, align 1
  %next_counter = add i64 %counter, 1
'''
        if fault != 'drop_volatile_store':
            text += '  store volatile i64 %next_counter, ptr %counterp, align 1\n'
        text += '''  %volatile_value = add i64 %snapshot_fresh, 17
  store volatile i64 %volatile_value, ptr %local_address, align 1
  %after_effect = load volatile i64, ptr %local_address, align 1
  %observed = xor i64 %snapshot_fresh, %after_effect
'''
    else:
        text += '  %after_effect = add i64 %snapshot_fresh, 0\n  %observed = add i64 %snapshot_fresh, 0\n'
    if mode == 'diamond_local':
        text += '''  %odd = and i64 %seed, 1
  %choose = icmp ne i64 %odd, 0
  br i1 %choose, label %left, label %right
left:
  %plus = add i64 %observed, 1
  %left_memory = getelementptr i8, ptr %guest, i64 800
  store i64 %plus, ptr %left_memory, align 1
  br label %join
right:
  %minus = sub i64 %observed, 1
  %right_memory = getelementptr i8, ptr %guest, i64 800
  store i64 %minus, ptr %right_memory, align 1
  br label %join
join:
  %selected = phi i64 [ %plus, %left ], [ %minus, %right ]
  %answer1 = xor i64 %selected, %snapshot_history
  %answer = xor i64 %answer1, %seed
  br label %finish
'''
        # Keep no unreachable latch using definition-block values: this path
        # has no dynamic loop and its original PHIs are removed below.
    else:
        limit = '%limit' if mode == 'loop_local' else '0'
        text += f'  %again = icmp ult i64 %snapshot_iteration, {limit}\n'
        text += '''  %answer1 = xor i64 %observed, %snapshot_history
  %answer = xor i64 %answer1, %seed
  br i1 %again, label %latch, label %finish
latch:
'''
        if fault != 'drop_store':
            text += '  store i64 %stored, ptr %local_address, align 1\n'
        text += '  br label %definition\n'
    text += finish('finish', '%answer', '%snapshot_frame', '%snapshot_iteration',
                   '%snapshot_history', '%snapshot_fresh', '%after_effect', '%snapshot_selector',
                   wrong_exit=fault == 'wrong_exit')
    text += finish('boundsfail', '13474738735092580608', '256', '0', '%seed', '0', '0', '0', True,
                   wrong_exit=fault == 'wrong_exit')
    text += '}\n'
    if mode == 'diamond_local':
        # Both actual entry incoming values agree; remove the now-absent loop
        # predecessor in this separately designed fixture, before freezing it.
        for name, value in (('iteration', '0'), ('frame', '256'), ('history', '%seed')):
            text = re.sub(r'(?m)^  %' + name + r' = phi[^\n]*\n', '', text)
            text = re.sub(r'%' + name + r'\b', value, text)
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
    put(a, CONFIG, 903 if short else 65536)
    put(a, GUEST + 256, 14 if hit else 34)
    put(a, GUEST + 512, 51)
    put(a, GUEST + 768, x)
    put(a, GUEST + 896, x)
    return a


def model(key, fault=None):
    mode, x, n, hit, short = key
    a = initial(key)
    calls, frame, iteration, history = 0, 256, 0, x
    if short:
        answer, snap_fresh, after, selector, ok = 13474738735092580608, 0, 0, 0, False
    else:
        while True:
            loaded = get(a, GUEST + frame)
            fresh = history if fault == 'stale_load' else loaded
            selector = fresh ^ x
            snap_fresh = history if fault == 'wrong_snapshot' else fresh
            if mode == 'unknown_call':
                if fault != 'drop_call':
                    calls += 1
                    put(a, REGS + 56, get(a, REGS + 56) ^ x)
                    put(a, GUEST + 768, get(a, GUEST + 768) + (x | 1))
                    put(a, GUEST + frame, get(a, GUEST + frame) ^ x ^ 0x9e3779b97f4a7c15)
                after = get(a, GUEST + frame)
                observed = snap_fresh ^ after
            elif mode == 'volatile':
                count = get(a, GUEST + 896)
                if fault != 'drop_volatile_store':
                    put(a, GUEST + 896, count + 1)
                put(a, GUEST + frame, snap_fresh + 17)
                after = get(a, GUEST + frame)
                observed = snap_fresh ^ after
            else:
                after = snap_fresh
                observed = snap_fresh
            if mode == 'diamond_local':
                observed = (observed + (1 if x & 1 else -1)) & MASK
                put(a, GUEST + 800, observed)
            answer = observed ^ history ^ x
            if mode != 'loop_local' or iteration >= n:
                ok = True
                break
            iteration += 1
            if fault != 'drop_store':
                put(a, GUEST + frame, snap_fresh + x + iteration)
            history = snap_fresh ^ (1 if fault == 'wrong_history' else 0)
            frame ^= 768
    for offset, value in ((0, answer), (16, iteration), (24, frame), (40, history), (48, snap_fresh), (64, after)):
        put(a, REGS + offset, value)
    source_pc = (selector ^ 4096) + (1 if fault == 'wrong_exit' else 0)
    for i, value in enumerate((1 if ok else 5, 0, source_pc, 57005 if ok else 4096, int(ok), 0, 0 if ok else 3)):
        put(a, EXIT + 8 * i, value)
    return struct.pack('<QQ', answer & MASK, calls) + a


def same(actual, expected, label):
    if actual != expected:
        first = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), None)
        raise AssertionError((label, first, len(actual), len(expected)))


def structural():
    prefix = '''define i64 @oracle(i64 %x, i1 %accepted) {
entry:
  br i1 %accepted, label %guarded.accept, label %fallback
guarded.accept:
  br label %definition
fallback:
  br label %definition
definition:
'''
    return {
        'reject_remote_use': prefix + '''  %selected = phi i64 [ %x, %guarded.accept ], [ %x, %fallback ]
  %value = add i64 %selected, 1
  switch i64 %x, label %out []
out:
  %local = add i64 %value, 0
  br label %remote
remote:
  %answer = add i64 %value, %local
  ret i64 %answer
}
''',
        'reject_shared_exit': prefix + '''  %value = add i64 %x, 1
  switch i64 %x, label %out [ i64 1, label %via ]
via:
  br label %out
out:
  %answer = add i64 %value, 1
  ret i64 %answer
}
''',
        'reject_direct_history_phi': prefix + '''  %history = phi i64 [ %x, %guarded.accept ], [ %x, %fallback ], [ %fresh, %out ]
  %fresh = add i64 %history, 1
  switch i64 %fresh, label %out []
out:
  %local = add i64 %fresh, 0
  %again = icmp ult i64 %local, 3
  br i1 %again, label %definition, label %done
done:
  ret i64 %local
}
''',
        'struct_freeze_undef': prefix + '''  %fresh = freeze i64 undef
  switch i64 %fresh, label %out []
out:
  %local = add i64 %fresh, 1
  br label %done
done:
  ret i64 %local
}
''',
        'struct_poison_flag': prefix + '''  %selected = phi i64 [ %x, %guarded.accept ], [ poison, %fallback ]
  %fresh = add nsw i64 %selected, 1
  switch i64 %fresh, label %out []
out:
  %local = add i64 %fresh, 1
  br label %done
done:
  ret i64 %local
}
''',
    }


def emit(directory):
    directory.mkdir(parents=True, exist_ok=False)
    for mode in MODES:
        (directory / (mode + '.ll')).write_text(ir(mode))
    for fault, mode in FAULTS.items():
        (directory / ('control_' + fault + '.ll')).write_text(ir(mode, fault))
    for name, text in structural().items():
        (directory / (name + '.ll')).write_text(text)
    (directory / 'helper.c').write_text(HELPER)
    (directory / 'inputs.json').write_text(json.dumps(INPUTS, indent=2) + '\n')
    (directory / 'source.sha256').write_text(hashlib.sha256(Path(__file__).read_bytes()).hexdigest() + '\n')
    (directory / 'contract.json').write_text(json.dumps({
        'gain_required': MODES, 'normal_inputs': len(INPUTS), 'fault_modes': FAULTS,
        'scope': 'One defining block and one original direct unique-predecessor exit per value.',
        'structural_only': {'reject_remote_use': 'reject nonlocal use',
                            'reject_shared_exit': 'reject multiple predecessor blocks',
                            'reject_direct_history_phi': 'reject unsupported historical PHI path',
                            'struct_freeze_undef': 'preserve distinct old/fresh freezes; no deterministic native oracle',
                            'struct_poison_flag': 'preserve poison path and nsw flag; not a native defined-value case'},
        'native_exclusions': [], 'candidate_budget': [32, 2048],
    }, indent=2) + '\n')


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
                            str(path), str(before / 'helper.c'), '-o', str(so)], stdout=log, stderr=log, check=True)
        lib = ctypes.CDLL(str(so.resolve()))
        fn = lib.oracle
        fn.argtypes, fn.restype = [ctypes.c_void_p] * 4, ctypes.c_uint64
        return lib, fn

    def invoke(lib, fn, key):
        ctypes.memmove(address, bytes(initial(key)), ARENA)
        calls = ctypes.c_uint64.in_dll(lib, 'entry_liveout_calls')
        calls.value = 0
        answer = fn(address + REGS, address + GUEST, address + EXIT, address + CONFIG)
        return struct.pack('<QQ', answer, calls.value) + bytes(arena)

    try:
        for mode in MODES:
            text = (after / (mode + '.ll')).read_text()
            assert re.search(r'(?m)^guarded\.accept:[^\n]*\n\s*br label %definition\.entry\.prefix\b', text)
            assert not re.search(r'(?m)^out\.entry\.prefix:', text), 'Out effects must stay original'
            out = re.search(r'(?ms)^out:[^\n]*\n(.*?)(?=^[A-Za-z_.][A-Za-z_0-9.]*:|^})', text)
            assert out and re.search(r'= phi i64 ', out.group(1)), 'ordinary live-out bridge required'
            if mode == 'unknown_call':
                assert len(re.findall(r'\bcall void @entry_liveout_unknown\(', text)) == 1
                assert 'call void @entry_liveout_unknown' in out.group(1)
            if mode == 'volatile':
                assert len(re.findall(r'\b(?:load|store) volatile\b', text)) == 4
                assert len(re.findall(r'\b(?:load|store) volatile\b', out.group(1))) == 4
            if mode == 'duplicate_exit':
                bridges = [line for line in out.group(1).splitlines() if '= phi i64 ' in line]
                assert any(line.count('%definition ]') == 3 and line.count('%definition.entry.prefix ]') == 3 for line in bridges)
            report['precision'][mode] = {'seed_copied': True, 'out_not_copied': True, 'actual_bridge': True}
        for stage, directory in (('before', before), ('after', after)):
            for mode in MODES:
                lib, fn = compile_ir(directory / (mode + '.ll'), stage + '-' + mode)
                with gzip.open(output / f'{stage}-{mode}.snapshots.gz', 'wb') as stream:
                    for key in INPUTS:
                        if key[0] != mode:
                            continue
                        actual = invoke(lib, fn, key)
                        stream.write(actual)
                        expected = model(key)
                        same(actual, expected, (stage, key))
                        report['normal'] += 1
                        for offset in (0, 8, 16 + REGS, 16 + GUEST + 256) + tuple(16 + EXIT + 8 * i for i in range(7)):
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
                    actual, expected, damaged = invoke(lib, fn, key), model(key), model(key, fault)
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
