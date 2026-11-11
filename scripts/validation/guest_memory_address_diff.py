#!/usr/bin/env python3
"""Full-state oracle for Guest address widths, alias writes and memory barriers.

The producer tests the existing detail API, not machine lowering. Native inputs
never overlap registers with Guest data, and all accessed bytes are valid host
storage. Poison/flags/high-distance UB contracts belong to structural/sanitizer
tests, not fixed-value execution. Output directories are never overwritten.
"""
import argparse
import ctypes
import functools
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

NAMES = ('wrap8', 'signed8', 'scaled16', 'wrap32', 'shift8', 'wrap64', 'lshr8',
         'wide_trunc', 'alias_word', 'alias_partial', 'alias_byte', 'alias_gep',
         'noalias', 'private_alloca', 'captured_alloca', 'two_bases_byte',
         'unknown_call', 'atomic_add', 'cmpxchg', 'fence', 'volatile',
         'loop_word', 'loop_byte', 'sub_wrap8', 'mul_wrap8')
WIDTHS = NAMES[:8] + ('sub_wrap8', 'mul_wrap8')
PRECISION = WIDTHS + ('noalias', 'private_alloca')
UNSAFE = ('alias_word', 'alias_partial', 'alias_byte', 'alias_gep', 'captured_alloca',
          'two_bases_byte', 'unknown_call', 'atomic_add', 'cmpxchg', 'fence', 'volatile')
VALUES = (0, 1, (1 << 64) - 1, 1 << 63, 0x7f, 0x80, 0xff, 0x100)
ALIASES = ('alias_word', 'alias_partial', 'alias_byte', 'alias_gep', 'loop_word', 'loop_byte')
LOOPS = ('loop_word', 'loop_byte')
A, B, MASK = 0x1122334455667788, 0x8877665544332211, (1 << 64) - 1
REG_BYTES, MEM_BYTES, SIDE_BYTES, SNAPSHOT_BYTES = 512, 65536, 64, 66184
EXIT_FIELDS = ('reason', 'transfer_kind', 'source_pc', 'next_pc',
               'next_pc_valid', 'return_pc', 'detail')
GROUPS = (('result', 0, 8), ('calls', 8, 16)) + tuple(
    (name, 16 + index * 8, 24 + index * 8) for index, name in enumerate(EXIT_FIELDS)) + (
    ('registers', 72, 584), ('memory', 584, 66120), ('side', 66120, SNAPSHOT_BYTES))
INPUTS = tuple((name, alias, rounds, x) for name in NAMES
               for alias in (range(3) if name in ALIASES else (2,) if name == 'noalias' else (0,))
               for rounds in ((0, 1, 2, 7) if name in LOOPS else (0,))
               for x in VALUES)
HELPER = r'''
#include <stdint.h>
#include <string.h>
uint64_t address_calls;
void address_unknown_writer(unsigned char *regs, unsigned char *data, uint64_t value) {
  ++address_calls;
  regs[40] ^= (unsigned char)value;
  data[0x700] ^= (unsigned char)(value >> 8);
  memcpy(data + 8, &value, sizeof value);
}
'''


def digest(path):
    sha = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            sha.update(chunk)
    return sha.hexdigest()


@functools.lru_cache(maxsize=len(VALUES))
def initial(x):
    return (bytes((index * 13 + (x & 255)) & 255 for index in range(REG_BYTES)),
            bytes((index * 37 + ((x >> 8) ^ x)) & 255 for index in range(MEM_BYTES)),
            bytes((index * 19 + (x & 255)) & 255 for index in range(SIDE_BYTES)))


def put(target, offset, value, width=8):
    target[offset:offset + width] = (value & ((1 << (width * 8)) - 1)).to_bytes(width, 'little')


def get(target, offset, width=8):
    return int.from_bytes(target[offset:offset + width], 'little')


def alias_write(memory, side, choice, value, width=8):
    put(side if choice == 2 else memory, 16 if choice == 2 else 11 if choice else 8, value, width)


def model(key):
    """Execute source-level byte operations independently of transformed IR."""
    name, choice, rounds, x = key
    regs, memory, side = map(bytearray, initial(x))
    initial_reg = get(regs, 32)
    value_b, auxiliary, calls = x ^ B, 0, 0
    count = (rounds & 3) + 1 if name in LOOPS else 1
    if name in WIDTHS:
        # GEP indices are signed-extended to the target index width after the
        # source integer operation wraps at its own declared bit width.
        if name == 'wrap8':
            index, anchor, scale, distractor = (200 + 100) & 255, 0, 1, 300
        elif name == 'signed8':
            index, anchor, scale, distractor = ((127 + 1) & 255) - 256, 512, 1, 640
        elif name == 'scaled16':
            index, anchor, scale, distractor = ((127 + 1) & 255) - 256, 512, 2, 768
        elif name == 'wrap32':
            index, anchor, scale, distractor = (0xffffffff + 2) & 0xffffffff, 512, 1, 641
        elif name == 'shift8':
            index, anchor, scale, distractor = (128 << 1) & 255, 512, 1, 768
        elif name == 'wrap64':
            index, anchor, scale, distractor = (MASK + 2) & MASK, 512, 1, 641
        elif name == 'lshr8':
            index, anchor, scale, distractor = 255 >> 4, 512, 1, 641
        elif name == 'sub_wrap8':
            index, anchor, scale, distractor = (1 - 200) & 255, 512, 1, 313
        elif name == 'mul_wrap8':
            index, anchor, scale, distractor = ((20 * 20) & 255) - 256, 512, 1, 912
        else:
            wide = (MASK + 2) & ((1 << 128) - 1)
            index, anchor, scale, distractor = wide & MASK, 512, 1, 641
        offset = anchor + scale * index
        put(memory, offset, A)
        put(memory, offset if name == 'wrap32' else distractor, value_b)
        observed = get(memory, offset)
    elif name == 'captured_alloca':
        # A generic LLVM detail-pass fixture temporarily obtains its Guest base
        # through a captured local object, then restores the descriptor. This
        # is not a claim about V2's stable-mapping machine execution contract.
        local = bytearray(16)
        put(local, 0, A)
        put(local, 0, value_b)
        observed = get(local, 0)
    elif name in LOOPS:
        put(memory, 8, A)
        observed = 0
        for iteration in range(count):
            observed = (observed + get(memory, 11 if name == 'loop_byte' else 8,
                                       2 if name == 'loop_byte' else 8)) & MASK
            if iteration + 1 < count:
                alias_write(memory, side, choice, value_b)
    else:
        put(memory, 8, A)
        if name in ('alias_word', 'alias_byte', 'alias_gep', 'noalias'):
            alias_write(memory, side, choice, value_b)
        elif name == 'alias_partial':
            alias_write(memory, side, choice, value_b, 2)
        elif name == 'private_alloca':
            auxiliary = value_b
        elif name == 'two_bases_byte':
            put(memory, 8, value_b)
        elif name == 'unknown_call':
            calls = 1
            regs[40] ^= value_b & 255
            memory[0x700] ^= (value_b >> 8) & 255
            put(memory, 8, value_b)
        elif name == 'atomic_add':
            put(memory, 8, (A + 1) & MASK)
        elif name == 'cmpxchg':
            put(memory, 8, value_b)
        observed = get(memory, 11 if name in ('alias_byte', 'two_bases_byte') else 8,
                       2 if name in ('alias_byte', 'two_bases_byte') else 8)
    result = (observed ^ x ^ auxiliary) & MASK
    put(regs, 0, result)
    put(regs, 8, observed)
    put(regs, 16, count)
    put(regs, 32, initial_reg ^ result)
    put(memory, 0x600, result)
    put(memory, 0x608, observed)
    fields = (1, 0, 0x8000 + NAMES.index(name) * 16,
              (result + 0x1000) & MASK, 1, x ^ 0xdeadbeef, count)
    return struct.pack('<QQ7Q', result, calls, *fields) + regs + memory + side


def differences(actual, expected):
    if len(actual) != SNAPSHOT_BYTES or len(expected) != SNAPSHOT_BYTES:
        raise RuntimeError('truncated or extended full-state snapshot')
    return [name for name, begin, end in GROUPS if actual[begin:end] != expected[begin:end]]


def self_test():
    controls = 0
    for key in INPUTS:
        wanted = model(key)
        if differences(wanted, wanted):
            raise RuntimeError('identity comparison failed')
        for field, begin, end in GROUPS:
            corrupt = bytearray(wanted)
            corrupt[begin] ^= 1
            if differences(corrupt, wanted) != [field]:
                raise RuntimeError('single-field control escaped comparison')
            controls += 1
    for bad in (b'', bytes(SNAPSHOT_BYTES - 1), bytes(SNAPSHOT_BYTES + 1)):
        try:
            differences(bad, model(INPUTS[0]))
        except RuntimeError:
            controls += 1
        else:
            raise RuntimeError('malformed snapshot accepted')
    return controls


class Memory(ctypes.Structure):
    _fields_ = [('data', ctypes.c_void_p), ('size', ctypes.c_uint64)]


def invoke(library, key, override=None):
    name, choice, rounds, x = key
    regs, memory, side = initial(x)
    reg_array = (ctypes.c_ubyte * REG_BYTES).from_buffer_copy(regs)
    mem_array = (ctypes.c_uint64 * (MEM_BYTES // 8)).from_buffer_copy(memory)
    side_array = (ctypes.c_uint64 * (SIDE_BYTES // 8)).from_buffer_copy(side)
    exit_array = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + index for index in range(7)])
    base, side_base = ctypes.addressof(mem_array), ctypes.addressof(side_array)
    if base % 8:
        raise RuntimeError('native atomic fixture Guest buffer is not aligned')
    alias = side_base + 16 if choice == 2 else base + (11 if choice else 8)
    if name == 'alias_gep':
        alias -= 8
    descriptor = Memory(base, MEM_BYTES)
    function = getattr(library, override or ('address_' + name))
    function.argtypes = [ctypes.c_void_p, ctypes.POINTER(Memory), ctypes.c_void_p,
                        ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32]
    function.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(library, 'address_calls')
    calls.value = 0
    result = function(reg_array, ctypes.byref(descriptor), exit_array, x, alias, rounds)
    if descriptor.data != base or descriptor.size != MEM_BYTES:
        raise RuntimeError('native function failed to restore Guest descriptor')
    return struct.pack('<QQ7Q', result, calls.value, *exit_array) + bytes(reg_array) + bytes(mem_array) + bytes(side_array)


def required_old_bug(key):
    name, choice, rounds, x = key
    if name in ('wrap8', 'signed8', 'scaled16', 'wrap32', 'shift8', 'sub_wrap8', 'mul_wrap8',
                'captured_alloca', 'two_bases_byte', 'atomic_add', 'cmpxchg'):
        return True
    if name in ALIASES[:4] and choice != 2:
        return True
    return name == 'loop_word' and choice != 2 and (rounds & 3) > 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--repo', type=Path)
    parser.add_argument('--library', type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--expect-old-bugs', action='store_true')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--clang', default='clang-21')
    parser.add_argument('--llvm-config', default='llvm-config-21')
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(dict(ok=True, inputs=len(INPUTS), single_field_and_length_controls=self_test())))
        return 0
    if args.repo is None or args.library is None or args.work is None:
        parser.error('--repo, --library and --work required')
    repo, library, work = (value.expanduser().resolve() for value in (args.repo, args.library, args.work))
    if work.exists():
        parser.error('--work must be a new path; evidence is never overwritten')
    work.mkdir(parents=True)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent))
    report = dict(library=str(library), inputs=len(INPUTS), commands=[], rows=[], errors=[],
                  normal_comparisons=0, normal_mismatches=0, control_comparisons=0,
                  control_mismatches=0, controls_required=0, controls_detected=0,
                  excluded=0, skipped=0, normalized=0, semantic_ok=False,
                  baseline_control=bool(args.expect_old_bugs), checks_ok=False)

    def run(name, command):
        try:
            result = subprocess.run(command, capture_output=True, env=env, timeout=180)
        except subprocess.TimeoutExpired as error:
            (work / (name + '.stdout')).write_bytes(error.stdout or b'')
            (work / (name + '.stderr')).write_bytes(error.stderr or b'')
            report['commands'].append(dict(name=name, argv=command, timeout=True))
            raise RuntimeError(f'{name} timed out') from error
        (work / (name + '.stdout')).write_bytes(result.stdout)
        (work / (name + '.stderr')).write_bytes(result.stderr)
        report['commands'].append(dict(name=name, argv=command, returncode=result.returncode))
        if result.returncode:
            raise RuntimeError(f'{name} failed with exit {result.returncode}')
        return result.stdout

    try:
        pin = report['library_sha256'] = digest(library)
        report['repo_head'] = run('repo-head', ['git', '-C', str(repo), 'rev-parse', 'HEAD']).decode().strip()
        run('repo-status', ['git', '-C', str(repo), 'status', '--short'])
        sources = work / 'sources'
        sources.mkdir()
        for name in ('guest_memory_address_native.cpp', 'guest_memory_address_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {path.name: digest(path) for path in sources.iterdir()}
        include = run('llvm-include', [args.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [args.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'),
            '-I' + include, str(sources / 'guest_memory_address_native.cpp'), str(library),
            '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        records = run('generate', [str(generator), str(work)]).decode().splitlines()
        stats = {}
        for line in records:
            fields = dict(field.split('=') for field in line.split())
            name = fields.pop('case')
            if name not in NAMES or name in stats:
                raise RuntimeError('unexpected or duplicate pass fixture')
            stats[name] = {key: int(value) for key, value in fields.items()}
        if set(stats) != set(NAMES) or any(row['invalid'] or not row['available'] for row in stats.values()):
            raise RuntimeError('missing or invalid pass fixtures')
        report['pass_stats'] = stats
        if not args.expect_old_bugs:
            if any(stats[name]['forwarded'] < 1 or stats[name]['original_load'] for name in PRECISION):
                raise RuntimeError('a required exact-width or proven-local precision control did not forward')
            if any(stats[name]['forwarded'] or not stats[name]['original_load'] for name in UNSAFE):
                raise RuntimeError('an unknown write or synchronization barrier forwarded a stale value')
        report['self_controls'] = self_test()
        (work / 'helper.c').write_text(HELPER)
        with gzip.open(work / 'expected.bin.gz', 'wb', compresslevel=1) as stream:
            for key in INPUTS:
                stream.write(model(key))
        required = {key for key in INPUTS if required_old_bug(key)}
        observed_after = {}
        for level in ('O0', 'O2'):
            for stage in ('before', 'after'):
                label = stage + '-' + level
                output = work / (label + '.so')
                run('compile-' + label, [args.clang, '-' + level, '-shared', '-fPIC', '-Wno-override-module']
                    + [str(work / (name + '-' + stage + '.ll')) for name in NAMES]
                    + [str(work / 'helper.c'), '-o', str(output)])
                native = ctypes.CDLL(str(output))
                found = set()
                with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as stream:
                    for key in INPUTS:
                        actual = invoke(native, key)
                        stream.write(actual)
                        mismatch = differences(actual, model(key))
                        report['normal_comparisons'] += 1
                        report['normal_mismatches'] += bool(mismatch)
                        if mismatch:
                            found.add(key)
                        report['rows'].append(dict(level=level, stage=stage, key=key,
                            fields=mismatch, snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                if stage == 'before' and found:
                    raise RuntimeError('untransformed reference does not match independent byte model')
                if stage == 'after':
                    observed_after[level] = found
            for control, mode in (('width', 'wrap8'), ('alias', 'alias_word')):
                label = 'control-' + control + '-' + level
                output = work / (label + '.so')
                run('compile-' + label, [args.clang, '-' + level, '-shared', '-fPIC',
                    '-Wno-override-module', str(work / ('control_' + control + '.ll')),
                    str(work / 'helper.c'), '-o', str(output)])
                native = ctypes.CDLL(str(output))
                wanted = {key for key in INPUTS if key[0] == mode and (control == 'width' or key[1] != 2)}
                found = set()
                with gzip.open(work / (label + '.bin.gz'), 'wb', compresslevel=1) as stream:
                    for key in (key for key in INPUTS if key[0] == mode):
                        actual = invoke(native, key, 'control_' + control)
                        stream.write(actual)
                        mismatch = differences(actual, model(key))
                        report['control_comparisons'] += 1
                        report['control_mismatches'] += bool(mismatch)
                        if mismatch:
                            found.add(key)
                        report['rows'].append(dict(level=level, control=control, key=key,
                            fields=mismatch, snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                report['controls_required'] += len(wanted)
                report['controls_detected'] += len(wanted & found)
                if found != wanted:
                    raise RuntimeError('faulty IR does not fail exactly its declared inputs')
        if digest(library) != pin:
            raise RuntimeError('runtime changed during validation')
        report['semantic_ok'] = report['normal_mismatches'] == 0
        report['old_required_bug_inputs'] = len(required)
        report['old_observed_bug_inputs'] = {key: len(value) for key, value in observed_after.items()}
        control_ok = all(required <= value for value in observed_after.values())
        coverage_ok = (report['normal_comparisons'] == len(INPUTS) * 4
                       and report['controls_detected'] == report['controls_required'] == 48)
        report['baseline_control_ok'] = bool(args.expect_old_bugs and control_ok and coverage_ok)
        report['ok'] = bool(coverage_ok and report['semantic_ok'])
        report['checks_ok'] = report['baseline_control_ok'] if args.expect_old_bugs else report['ok']
    except Exception as error:
        report['errors'].append(f'{type(error).__name__}: {error}')
        report['ok'] = report['checks_ok'] = False
    report['artifact_sha256'] = {str(path.relative_to(work)): digest(path)
        for path in sorted(work.rglob('*')) if path.is_file()}
    (work / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({key: report.get(key) for key in ('inputs', 'normal_comparisons',
        'normal_mismatches', 'control_comparisons', 'control_mismatches', 'controls_required',
        'controls_detected', 'semantic_ok', 'baseline_control', 'baseline_control_ok',
        'errors', 'ok', 'checks_ok')}))
    return 0 if report['checks_ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
