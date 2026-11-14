"""Independent unsigned arithmetic oracle for the production predicate pass.

The probe produces synthetic IR; it is not a machine lifter oracle. Every native
run compares the complete 66112-byte arena, including the opaque call's effects.
All inputs/expected/actual frames are retained in an xz stream for independent
audit. No masking, normalization or exclusion is permitted.
"""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import struct
import subprocess
import time

FRAME = 512 + 65536 + 64
CONTROL_OFFSETS = (0, 16, 32, 64, 96, 112, 192, 200, 256, 512, 8704, 66048, FRAME - 1)
WRAPPER = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
void probe(unsigned char *);
void observer(unsigned char *frame, uint64_t seed) {
  uint64_t counter, cell;
  memcpy(&counter, frame + 256, 8); memcpy(&cell, frame + 8704, 8);
  counter += 1; cell ^= seed;
  memcpy(frame + 256, &counter, 8); memcpy(frame + 8704, &cell, 8);
}
int main(void) {
  unsigned char frame[66112]; size_t count;
  while ((count = fread(frame, 1, sizeof frame, stdin)) != 0) {
    if (count != sizeof frame) return 2;
    probe(frame);
    if (fwrite(frame, 1, sizeof frame, stdout) != sizeof frame) return 3;
  }
  return ferror(stdin) || fflush(stdout) != 0;
}
'''


def put(frame, offset, value, size):
    frame[offset:offset + size] = value.to_bytes(size, 'little')


def initial(width, trial):
    mask = (1 << width) - 1
    edges = (0, 1, mask, 1 << (width - 1), (1 << (width - 1)) - 1,
             (1 << (width - 1)) + 1, 2, 3, mask ^ 1, mask // 3, mask * 2 // 3)
    counts = (0, 1, width - 1, width, width + 1, 2 * width - 1, 2 * width, mask)
    frame = bytearray(hashlib.shake_256(('predicate-boundary/' + str(trial)).encode()).digest(FRAME))
    x = edges[trial] if trial < len(edges) else int.from_bytes(
        hashlib.sha256(('x/' + str(trial)).encode()).digest(), 'little') & mask
    other = edges[(trial * 3 + 2) % len(edges)] ^ (trial * 0x12345679 & mask)
    put(frame, 0, x, width // 8)
    put(frame, 16, counts[trial % len(counts)], width // 8)
    put(frame, 32, other, width // 8)
    return bytes(frame)


def expected(source, width, direction, mode):
    result = bytearray(source)
    size, mask, mask64 = width // 8, (1 << width) - 1, (1 << 64) - 1
    get = lambda offset, n=size: int.from_bytes(source[offset:offset + n], 'little')
    history, count, other = get(0), get(16) % width, get(32)

    def rotate(value):
        if count == 0:
            return value
        if direction == 'left':
            return ((value << count) | (value >> (width - count))) & mask
        return ((value >> count) | (value << (width - count))) & mask

    # The opaque observer is specified independently of its C implementation.
    counter, cell = get(256, 8), get(8704, 8)
    counter = (counter + 1) & mask64
    cell ^= history & mask64
    accumulator, observed_conditions = 0, []
    for _ in range(3):
        mixed = rotate(history) ^ history
        first = mixed if mode == 'varying' else mixed | 1
        second = (rotate(other) ^ other) | 1 if mode == 'distinct' else first
        condition = ((first * second) & 7) == 1
        observed_conditions.append(condition)
        accumulator = (accumulator + (7 if condition else 98)) & mask
        history = (history + 1) & mask
    counter = (counter + 1) & mask64
    cell ^= history & mask64
    put(result, 64, accumulator, size)
    put(result, 96, history, size)
    put(result, 112, mixed, size)
    put(result, 192, int(condition), 1)
    put(result, 200, 3, 4)
    put(result, 256, counter, 8)
    put(result, 8704, cell, 8)
    put(result, 66048, 0xdecafbad12345678, 8)
    return bytes(result), observed_conditions


def compare(reference, actual):
    if len(reference) != FRAME or len(actual) != FRAME:
        raise ValueError('incomplete native frame')
    return reference == actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--no-z3', action='store_true')
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--llc', default='llc-21')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    wrapper = args.out / 'wrapper.c'
    wrapper.write_text(WRAPPER)
    commands, modules, rows = [], [], []

    def run(argv, data=None):
        start = time.monotonic()
        value = subprocess.run([str(a) for a in argv], input=data, capture_output=True, timeout=180)
        commands.append(dict(argv=[str(a) for a in argv], returncode=value.returncode,
                             seconds=time.monotonic() - start,
                             stderr=value.stderr.decode(errors='replace')))
        if value.returncode:
            raise RuntimeError(commands[-1])
        return value.stdout

    success = False
    try:
        with lzma.open(args.out / 'frames.xz', 'xb', preset=6) as stream:
            for width in (32, 64, 128):
                for direction in ('left', 'right'):
                    for mode in ('positive', 'varying', 'distinct'):
                        name = f'{width}-{direction}-{mode}'
                        work = args.out / name
                        work.mkdir()
                        before, after, stats_file = [work / n for n in ('before.ll', 'after.ll', 'stats.json')]
                        run([args.probe, width, direction, mode, before, after, stats_file])
                        stats = json.loads(stats_file.read_text())
                        wanted = 0 if args.baseline or args.no_z3 or mode != 'positive' else 1
                        if stats['available'] == args.no_z3 or stats['folded'] != wanted or stats['unknown']:
                            raise RuntimeError(('unexpected production proof result', name, stats, wanted))
                        modules.append(dict(name=name, **stats))
                        sources = [initial(width, trial) for trial in range(32)]
                        references, conditions = zip(*(expected(s, width, direction, mode) for s in sources))
                        values = {v for case in conditions for v in case}
                        if values != ({True} if mode == 'positive' else {False, True}):
                            raise RuntimeError(('oracle failed both-arm coverage', name, sorted(values)))
                        for level in ('O0', 'O2'):
                            obj, exe = work / (level + '.o'), work / (level + '.native')
                            run([args.llc, '-filetype=obj', '-' + level, after, '-o', obj])
                            run([args.cc, '-' + level, wrapper, obj, '-o', exe])
                            native = run([exe], b''.join(sources))
                            if len(native) != FRAME * len(sources):
                                raise RuntimeError(('wrong native stream length', name, level, len(native)))
                            for trial, (source, reference) in enumerate(zip(sources, references)):
                                actual = native[FRAME * trial:FRAME * (trial + 1)]
                                match = compare(reference, actual)
                                controls = 0
                                for offset in CONTROL_OFFSETS:
                                    faulty = bytearray(actual)
                                    faulty[offset] ^= 1
                                    controls += not compare(reference, bytes(faulty))
                                row = dict(name=name, opt=level, trial=trial, match=match,
                                           compared_bytes=FRAME, controls=controls,
                                           frame_sha256={key: hashlib.sha256(value).hexdigest()
                                             for key, value in (('input', source), ('expected', reference), ('actual', actual))})
                                encoded = json.dumps(row, sort_keys=True).encode()
                                stream.write(struct.pack('<I', len(encoded)) + encoded + source + reference + actual)
                                rows.append(row)
                                if not match or controls != len(CONTROL_OFFSETS):
                                    raise RuntimeError(('native mismatch or missed control', row))
                        print(name, '64 complete native matches', flush=True)
        success = len(rows) == 18 * 2 * 32
    finally:
        summary = dict(established=success, native_matches=sum(r['match'] for r in rows),
                       planned=18 * 2 * 32, modules=len(modules),
                       control_detections=sum(r['controls'] for r in rows),
                       controls_per_frame=len(CONTROL_OFFSETS), control_offsets=CONTROL_OFFSETS,
                       baseline=args.baseline, no_z3=args.no_z3,
                       skips=0, exclusions=0, normalizations=0, commands=commands,
                       producer_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                       harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
        (args.out / 'results.json').write_text(json.dumps(dict(summary=summary, modules=modules, results=rows), indent=2) + '\n')
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
