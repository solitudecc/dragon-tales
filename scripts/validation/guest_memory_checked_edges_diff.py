#!/usr/bin/env python3
"""Independent full-state oracle for edge-local checked Guest memory forwarding.

Uses an existing libdragon, without building or changing the repository. A small
producer invokes the real pass; native before/after O0/O2 code is compared with
an independent byte model. Two deliberately incorrect IR transformations must
fail on exactly declared inputs. This tests LLVM transformations, not a new
machine-instruction lifter or arbitrary concurrent/poison semantics.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

NAMES = ('plain', 'two_guards', 'dynamic', 'liveout', 'alias', 'partial',
         'unknown_call', 'fence', 'volatile', 'overwrite', 'loop')
POSITIVE = ('plain', 'two_guards', 'dynamic', 'liveout')
REJECTED = ('alias', 'partial', 'unknown_call', 'fence', 'volatile', 'overwrite')
VALUES = (0, 1, 1 << 63, (1 << 64) - 1)
MASK = (1 << 64) - 1
REG_BYTES, MEM_BYTES, SNAPSHOT_BYTES = 512, 65536, 66120
EXIT_FIELDS = ('reason', 'transfer_kind', 'source_pc', 'next_pc',
               'next_pc_valid', 'return_pc', 'detail')
GROUPS = (('result', 0, 8), ('calls', 8, 16)) + tuple(
    (name, 16 + index * 8, 24 + index * 8) for index, name in enumerate(EXIT_FIELDS)) + (
    ('registers', 72, 584), ('memory', 584, SNAPSHOT_BYTES))
INPUTS = tuple((name, path, bound, x) for name in NAMES for path in range(4)
               for bound in range(4 if name == 'loop' else 3) for x in VALUES)
HELPER = r'''
#include <stdint.h>
#include <string.h>
uint64_t edge_calls;
void edge_unknown_writer(unsigned char *regs, unsigned char *data,
                         void *slot, uint64_t x) {
  ++edge_calls;
  regs[48] ^= (unsigned char)x;
  data[0x300] ^= (unsigned char)(x >> 8);
  uint64_t value = x ^ UINT64_C(0xfaceb00c12345678);
  memcpy(slot, &value, sizeof value);
}
'''


def digest(path):
    sha = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            sha.update(chunk)
    return sha.hexdigest()


def parameters(key):
    name, path, bound, x = key
    sp = 256 + (x & 1) * 256
    offset = sp - 128 if path == 0 else sp - 96 + (path - 1) * 32
    size = (MEM_BYTES, offset + 7, offset - 1, sp - 96 + 7)[bound]
    return sp, offset, size, x & 0xffffffff, sp - 128 if x & 1 else 0x600


def initial(key):
    x = key[3]
    regs = bytearray((index * 13 + (x & 255)) & 255 for index in range(REG_BYTES))
    memory = bytearray((index * 37 + ((x >> 8) ^ x)) & 255 for index in range(MEM_BYTES))
    return regs, memory


def put(target, offset, value):
    struct.pack_into('<Q', target, offset, value & MASK)


def model(key):
    """Execute the source contract, without consulting generated or rewritten IR."""
    name, path, bound, x = key
    mode = NAMES.index(name)
    sp, offset, size, rounds, alias = parameters(key)
    regs, memory = initial(key)
    initial16 = struct.unpack_from('<Q', regs, 16)[0]
    put(regs, 0, sp)
    put(regs, 8, x ^ (0x11110000 + mode))
    calls = 0
    if path == 0:
        slot = sp - 128
        put(memory, slot, x ^ 0x1357 if name == 'dynamic' else 50)
        if name == 'alias':
            put(memory, alias, x ^ 0x4567)
        elif name == 'partial':
            memory[slot + 1] = 0xab
        elif name == 'unknown_call':
            calls = 1
            regs[48] ^= x & 255
            memory[0x300] ^= (x >> 8) & 255
            put(memory, slot, x ^ 0xfaceb00c12345678)
    if name == 'liveout' and path == 0 and x & 1:
        tag, result, count = 3, x ^ 0xe411000000000000, 0
        memory[0x510] = 0xe7
    else:
        iteration = 0
        limit = (rounds & 3) + 1
        while True:
            if name == 'overwrite':
                put(memory, offset, x ^ 0x987654321)
            first = offset <= size
            second = first and size - offset >= 8
            if not first or not second:
                tag = 1 if name in ('plain', 'dynamic', 'liveout') or not first else 2
                memory[0x510] = (path + tag) & 255
                result = offset ^ (0xbad0000000000000 + tag)
                count = iteration
                break
            value = struct.unpack_from('<Q', memory, offset)[0]
            put(memory, 0x500, value + x)
            result, count, tag = value ^ x, iteration + 1, 0
            if name != 'loop' or count >= limit:
                break
            iteration += 1
            offset = sp - 96
    put(regs, 0, result)
    put(regs, 8, offset)
    put(regs, 16, initial16 ^ result)
    put(regs, 24, count)
    put(regs, 32, path)
    successful, early = tag == 0, tag == 3
    fields = (1 if successful else 2 if early else 5, int(early),
              0x4000 + mode * 0x100 + tag * 8,
              (result + 0x5000) & MASK if successful else offset,
              int(successful or early), x ^ 0x9000,
              0 if successful else 5 if early else 3)
    return (struct.pack('<QQ7Q', result & MASK, calls, *fields) + regs + memory)


def differences(actual, expected):
    if len(actual) != SNAPSHOT_BYTES or len(expected) != SNAPSHOT_BYTES:
        raise RuntimeError('truncated or extended full-state snapshot')
    return [name for name, begin, end in GROUPS if actual[begin:end] != expected[begin:end]]


def self_test():
    count = 0
    for key in INPUTS:
        expected = model(key)
        if differences(expected, expected):
            raise RuntimeError('identity comparison failed')
        for field, begin, end in GROUPS:
            corrupt = bytearray(expected)
            corrupt[begin] ^= 1
            if differences(corrupt, expected) != [field]:
                raise RuntimeError('single-field control escaped comparison')
            count += 1
    for invalid in (b'', bytes(SNAPSHOT_BYTES - 1), bytes(SNAPSHOT_BYTES + 1)):
        try:
            differences(invalid, model(INPUTS[0]))
        except RuntimeError:
            count += 1
        else:
            raise RuntimeError('malformed snapshot accepted')
    return count


class Memory(ctypes.Structure):
    _fields_ = [('data', ctypes.c_void_p), ('size', ctypes.c_uint64)]


def invoke(library, key):
    regs, memory = initial(key)
    reg_array = (ctypes.c_ubyte * REG_BYTES).from_buffer_copy(regs)
    mem_array = (ctypes.c_ubyte * MEM_BYTES).from_buffer_copy(memory)
    exit_array = (ctypes.c_uint64 * 7)(*[0xcafe000000000000 + index for index in range(7)])
    base = ctypes.addressof(mem_array)
    sp, offset, size, rounds, alias = parameters(key)
    descriptor = Memory(base, size)
    function = getattr(library, 'edge_' + key[0])
    function.argtypes = [ctypes.c_void_p, ctypes.POINTER(Memory), ctypes.c_void_p,
                        ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64,
                        ctypes.c_uint32, ctypes.c_void_p]
    function.restype = ctypes.c_uint64
    calls = ctypes.c_uint64.in_dll(library, 'edge_calls')
    calls.value = 0
    result = function(reg_array, ctypes.byref(descriptor), exit_array,
                      key[1], sp, key[3], rounds, base + alias)
    if descriptor.data != base or descriptor.size != size:
        raise RuntimeError('native function changed Guest descriptor')
    return struct.pack('<QQ7Q', result, calls.value, *exit_array) + bytes(reg_array) + bytes(mem_array)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--repo', type=Path)
    parser.add_argument('--library', type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--require-gains', action='store_true')
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--clang', default='clang-21')
    parser.add_argument('--llvm-config', default='llvm-config-21')
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(dict(ok=True, single_field_and_length_controls=self_test())))
        return 0
    if args.repo is None or args.library is None or args.work is None:
        parser.error('--repo, --library and --work are required')
    if args.baseline and args.require_gains:
        parser.error('--baseline and --require-gains are mutually exclusive')
    repo, library, work = (path.expanduser().resolve() for path in (args.repo, args.library, args.work))
    if work.exists():
        parser.error('--work must not already exist; evidence is never overwritten')
    work.mkdir(parents=True)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent))
    report = dict(library=str(library), repo=str(repo), commands=[], rows=[], errors=[],
                  normal_comparisons=0, normal_mismatches=0, control_comparisons=0,
                  control_mismatches=0, specified_controls=0, controls_detected=0,
                  inputs=len(INPUTS), exclusions=0, skips=0, normalizations=0)

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
            raise RuntimeError(f'{name} failed with rc={result.returncode}')
        return result.stdout

    try:
        pin = report['library_sha256'] = digest(library)
        report['repo_head'] = run('repo-head', ['git', '-C', str(repo), 'rev-parse', 'HEAD']).decode().strip()
        run('repo-status', ['git', '-C', str(repo), 'status', '--short'])
        run('cxx-version', [args.cxx, '--version'])
        run('clang-version', [args.clang, '--version'])
        report['production_source_sha256'] = digest(repo / 'lib/GuestMemorySSA.cpp')
        sources = work / 'sources'
        sources.mkdir()
        for name in ('guest_memory_checked_edges_native.cpp', 'guest_memory_checked_edges_diff.py'):
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {path.name: digest(path) for path in sources.iterdir()}
        includedir = run('llvm-include', [args.llvm_config, '--includedir']).decode().strip()
        generator = work / 'generate'
        run('compile-generator', [args.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'),
            '-I' + includedir, str(sources / 'guest_memory_checked_edges_native.cpp'),
            str(library), '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        run('generator-ldd', ['ldd', str(generator)])
        generated = run('generate', [str(generator), str(work)]).decode().splitlines()
        stats = {}
        for line in generated:
            fields = dict(field.split('=') for field in line.split())
            name = fields.pop('case')
            if name not in NAMES or name in stats:
                raise RuntimeError('unexpected or duplicate pass record')
            stats[name] = {key: int(value) for key, value in fields.items()}
        if set(stats) != set(NAMES) or any(item['invalid'] for item in stats.values()):
            raise RuntimeError('missing or invalid pass fixtures')
        if any(item['unknown_edges'] != 3 for item in stats.values()):
            raise RuntimeError('an unknown incoming edge was redirected')
        if any(item['original_load'] != int(name != 'overwrite')
               for name, item in stats.items()):
            raise RuntimeError('original shared LOAD lifetime contract changed')
        report['pass_stats'] = stats
        if args.require_gains and any(not stats[name]['redirected'] or not stats[name]['forwarded']
                                      for name in POSITIVE):
            raise RuntimeError('a required positive fixture did not recover an edge')
        if args.baseline and any(stats[name]['redirected'] for name in POSITIVE):
            raise RuntimeError('baseline unexpectedly already recovers requested edges')
        if any(stats[name]['redirected'] for name in REJECTED):
            raise RuntimeError('a barrier fixture was incorrectly edge-specialized')
        helper = work / 'helper.c'
        helper.write_text(HELPER)
        with (work / 'expected.bin').open('wb') as stream:
            for key in INPUTS:
                stream.write(model(key))
        report['self_controls'] = self_test()
        for level in ('O0', 'O2'):
            for stage in ('before', 'after'):
                label = stage + '-' + level
                output = work / (label + '.so')
                command = [args.clang, '-' + level, '-shared', '-fPIC', '-Wno-override-module']
                command += [str(work / (name + '-' + stage + '.ll')) for name in NAMES]
                run('compile-' + label, command + [str(helper), '-o', str(output)])
                native = ctypes.CDLL(str(output))
                with (work / (label + '.bin')).open('wb') as snapshots:
                    for key in INPUTS:
                        actual = invoke(native, key)
                        snapshots.write(actual)
                        mismatch = differences(actual, model(key))
                        report['normal_comparisons'] += 1
                        report['normal_mismatches'] += bool(mismatch)
                        report['rows'].append(dict(stage=stage, level=level, key=key,
                            fields=mismatch, snapshot_sha256=hashlib.sha256(actual).hexdigest()))
            for control in ('global', 'guard'):
                label = 'control-' + control + '-' + level
                output = work / (label + '.so')
                run('compile-' + label, [args.clang, '-' + level, '-shared', '-fPIC',
                    '-Wno-override-module', str(work / ('control-' + control + '.ll')),
                    str(helper), '-o', str(output)])
                native = ctypes.CDLL(str(output))
                observed = set()
                required = {key for key in INPUTS if key[0] == 'plain' and
                    (key[1] != 0 and key[2] == 0 if control == 'global' else key[2] != 0)}
                with (work / (label + '.bin')).open('wb') as snapshots:
                    for key in (key for key in INPUTS if key[0] == 'plain'):
                        actual = invoke(native, key)
                        snapshots.write(actual)
                        mismatch = differences(actual, model(key))
                        if mismatch:
                            observed.add(key)
                        report['control_comparisons'] += 1
                        report['control_mismatches'] += bool(mismatch)
                        report['rows'].append(dict(control=control, level=level, key=key,
                            fields=mismatch, snapshot_sha256=hashlib.sha256(actual).hexdigest()))
                report['specified_controls'] += len(required)
                report['controls_detected'] += len(required & observed)
                if observed != required:
                    raise RuntimeError('faulty IR did not fail exactly the specified control inputs')
        if digest(library) != pin:
            raise RuntimeError('library changed during replay')
        report['ok'] = (report['normal_comparisons'] == len(INPUTS) * 4 and
                        report['normal_mismatches'] == 0 and report['control_comparisons'] == 192 and
                        report['controls_detected'] == report['specified_controls'] == 88)
    except Exception as error:
        report['errors'].append(f'{type(error).__name__}: {error}')
        report['ok'] = False
    report['artifact_sha256'] = {str(path.relative_to(work)): digest(path)
        for path in sorted(work.rglob('*')) if path.is_file()}
    (work / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({key: report[key] for key in ('inputs', 'normal_comparisons',
        'normal_mismatches', 'control_comparisons', 'control_mismatches', 'specified_controls',
        'controls_detected', 'errors', 'ok')}))
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
