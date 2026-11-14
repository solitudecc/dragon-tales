"""Read saved synthetic LLVM predicate frames; never load or execute code.

The independent model uses bit-string permutations and low-three-bit arithmetic.
Its scope is pure integer LLVM-pass fixtures plus a specified opaque observer,
not machine instruction lifting, Unicorn, or real-program de-flattening.
"""
import argparse
import collections
import hashlib
import json
import lzma
from pathlib import Path
import struct
import traceback

FRAME = 66112
OFFSETS = (0, 16, 32, 64, 96, 112, 192, 200, 256, 512, 8704, 66048, 66111)
NAMES = [f'{w}-{d}-{m}' for w in (32, 64, 128)
         for d in ('left', 'right') for m in ('positive', 'varying', 'distinct')]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def initial(width, trial):
    modulus = 2 ** width
    edges = [0, 1, modulus - 1, modulus // 2, modulus // 2 - 1,
             modulus // 2 + 1, 2, 3, modulus - 2, (modulus - 1) // 3,
             2 * (modulus - 1) // 3]
    counts = [0, 1, width - 1, width, width + 1, 2 * width - 1,
              2 * width, modulus - 1]
    raw = bytearray(hashlib.shake_256(f'predicate-boundary/{trial}'.encode()).digest(FRAME))
    x = edges[trial] if trial < 11 else int.from_bytes(
        hashlib.sha256(f'x/{trial}'.encode()).digest(), 'little') % modulus
    other = edges[(3 * trial + 2) % 11] ^ ((trial * 0x12345679) % modulus)
    for offset, value in ((0, x), (16, counts[trial % 8]), (32, other)):
        raw[offset:offset + width // 8] = value.to_bytes(width // 8, 'little')
    return bytes(raw)


def model(source, width, direction, mode):
    size, modulus = width // 8, 2 ** width
    read = lambda offset, n=size: int.from_bytes(source[offset:offset + n], 'little')
    first, raw_count, other = read(0), read(16), read(32)
    count = raw_count % width

    def permute(word):
        bits = format(word, f'0{width}b')
        moved = bits[count:] + bits[:count] if direction == 'left' else bits[-count:] + bits[:-count]
        return int(moved, 2)

    observations = [((first + turn) % modulus) for turn in range(3)]
    mixed = [permute(word) ^ word for word in observations]
    lhs = [value if mode == 'varying' else value | 1 for value in mixed]
    rhs = [((permute(other) ^ other) | 1)] * 3 if mode == 'distinct' else lhs
    conditions = [((left % 8) * (right % 8)) % 8 == 1 for left, right in zip(lhs, rhs)]
    result = bytearray(source)
    stores = [(64, sum(7 if flag else 98 for flag in conditions) % modulus, size),
              (96, (first + 3) % modulus, size), (112, mixed[-1], size),
              (192, int(conditions[-1]), 1), (200, 3, 4),
              (256, (read(256, 8) + 2) % (2 ** 64), 8),
              (8704, read(8704, 8) ^ (first % (2 ** 64)) ^ (((first + 3) % modulus) % (2 ** 64)), 8),
              (66048, 0xdecafbad12345678, 8)]
    for offset, value, count_bytes in stores:
        result[offset:offset + count_bytes] = value.to_bytes(count_bytes, 'little')
    return bytes(result), conditions, (first, raw_count, other)


def validate_xz(path):
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    total = 0
    with path.open('rb') as stream:
        while not decoder.eof:
            chunk = stream.read(1024 * 1024) if decoder.needs_input else b''
            if decoder.needs_input and not chunk:
                raise AssertionError('truncated XZ stream')
            total += len(decoder.decompress(chunk, max_length=1024 * 1024))
        assert not decoder.unused_data and not stream.read(1), 'trailing XZ bytes/streams'
    return total


def main(args):
    root, out = args.root.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = dict(schema='predicate-boundary-saved-independent-v1', passed=False,
                  new_product_or_native_executions=0, machine_lifter_or_Unicorn_checks=0,
                  normalizations=0, skips=0, exclusions=0, failures=[])
    try:
        result_path, stream_path = root / 'results.json', root / 'frames.xz'
        saved = json.loads(result_path.read_bytes())
        summary = saved['summary']
        assert summary['established'] is True
        assert summary['baseline'] == (args.mode == 'baseline')
        assert summary['no_z3'] == (args.mode == 'noz3')
        assert summary['planned'] == summary['native_matches'] == 1152
        assert summary['modules'] == 18 and summary['controls_per_frame'] == 13
        assert summary['control_detections'] == 14976 and tuple(summary['control_offsets']) == OFFSETS
        assert all(summary[k] == 0 for k in ('skips', 'exclusions', 'normalizations'))
        assert sha(args.harness) == summary['harness_sha256']
        assert sha(args.probe) == summary['producer_sha256']
        wanted_files = {name + suffix for name in NAMES for suffix in ('/before.ll', '/after.ll', '/stats.json')}
        paths = {root / file for file in wanted_files} | {result_path, stream_path, root / 'wrapper.c', args.harness, args.probe, Path(__file__)}
        if args.producer_source:
            paths.add(args.producer_source)
        pins = {str(p): sha(p) for p in sorted(paths)}
        modules = {row['name']: row for row in saved['modules']}
        assert len(saved['modules']) == len(modules) == 18 and set(modules) == set(NAMES)
        for name in NAMES:
            width, direction, mode = name.split('-'); row = modules[name]
            assert (row['width'], row['direction'], row['mode']) == (int(width), direction, mode)
            assert row['available'] == (args.mode != 'noz3') and row['unknown'] == 0
            assert row['folded'] == (1 if args.mode == 'main' and mode == 'positive' else 0)
            assert {k: v for k, v in row.items() if k != 'name'} == json.loads((root / name / 'stats.json').read_bytes())
            before, after = root / name / 'before.ll', root / name / 'after.ll'
            if row['folded'] == 0:
                assert before.read_bytes() == after.read_bytes(), name + ': no-fold IR changed'
            else:
                assert before.read_bytes() != after.read_bytes(), name + ': folded IR unchanged'
        keys = [(name, level, trial) for name in NAMES for level in ('O0', 'O2') for trial in range(32)]
        rows = saved['results']; assert len(rows) == len(keys) == 1152
        census = {name: dict(inputs=set(), triples=set(), raw_counts=set(), effective_counts=set(), traces={}, fresh_varying_trials=[]) for name in NAMES}
        count, byte_count, controls = 0, 0, 0
        with lzma.open(stream_path, 'rb') as stream:
            for wanted, listed in zip(keys, rows):
                prefix = stream.read(4); assert len(prefix) == 4
                length, = struct.unpack('<I', prefix); assert 0 < length <= 65536
                header = stream.read(length); assert len(header) == length
                row = json.loads(header); assert row == listed
                assert header == json.dumps(row, sort_keys=True).encode(), 'noncanonical/duplicate-key frame header'
                name, level, trial = wanted
                assert (row['name'], row['opt'], row['trial']) == wanted
                assert row['match'] is True and row['compared_bytes'] == FRAME and row['controls'] == 13
                source, expected, actual = [stream.read(FRAME) for _ in range(3)]
                assert len(source) == len(expected) == len(actual) == FRAME
                assert row['frame_sha256'] == {k: hashlib.sha256(v).hexdigest() for k, v in [('input',source),('expected',expected),('actual',actual)]}
                width, direction, mode = name.split('-'); width = int(width)
                assert source == initial(width, trial)
                independent, flags, triple = model(source, width, direction, mode)
                assert independent == expected == actual, wanted
                for offset in OFFSETS:
                    corrupted = bytearray(actual); corrupted[offset] ^= 1
                    assert expected != corrupted
                    assert corrupted[:offset] == actual[:offset] and corrupted[offset + 1:] == actual[offset + 1:]
                    assert corrupted[offset] == (actual[offset] ^ 1)
                    controls += 1
                if level == 'O0':
                    c = census[name]; c['inputs'].add(hashlib.sha256(source).hexdigest()); c['triples'].add(triple)
                    c['raw_counts'].add(triple[1]); c['effective_counts'].add(triple[1] % width)
                    c['traces'][str(trial)] = flags
                    if len(set(flags)) == 2: c['fresh_varying_trials'].append(trial)
                count += 1; byte_count += 4 + length + 3 * FRAME
            assert stream.read(1) == b'', 'extra saved frames/data'
        assert validate_xz(stream_path) == byte_count
        coverage = {}
        for name, c in census.items():
            arms = collections.Counter(flag for trace in c['traces'].values() for flag in trace)
            assert len(c['inputs']) == len(c['triples']) == 32
            assert set(arms) == ({True} if name.endswith('-positive') else {False, True})
            coverage[name] = dict(unique_initial_frames=32, unique_arithmetic_tuples=32,
                raw_counts=[hex(x) for x in sorted(c['raw_counts'])], effective_counts=sorted(c['effective_counts']),
                condition_observations={str(k): v for k, v in arms.items()},
                within_three_iteration_varying_trials=c['fresh_varying_trials'])
        assert pins == {str(p): sha(p) for p in sorted(paths)}, 'read input changed'
        unique_inputs=set().union(*(c['inputs'] for c in census.values()))
        assert len(unique_inputs)==96
        report.update(passed=True, frames=count, complete_bytes_each=FRAME,
            comparator_controls_reconstructed=controls, exact_control_offsets=OFFSETS,
            xz_crc_and_eof=True, exact_header_domain_order=True, files_pre_post_sha_unchanged=True,
            unique_module_input_domain=18 * 32, unique_initial_frames_global=len(unique_inputs),
            host_repetitions_do_not_expand_domain=True, before_native_calibration=(args.mode == 'baseline'),
            before_calibration_reason='Baseline no-fold modules are full-byte identical before/after; their actual saved native frames match the independent model.' if args.mode == 'baseline' else None,
            coverage=coverage, input_sha256=pins,
            scope='Saved-only full66112 arena audit: synthetic integer LLVM pass + specified opaque observer. Comparator controls only; no new native, machine-lifter, C or Unicorn executions. Does not prove real CFF removal.')
    except BaseException as error:
        report['failures'].append(dict(type=type(error).__name__,detail=str(error),traceback=traceback.format_exc()))
        raise
    finally:
        target=out/'REPORT.json';target.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
        print('REPORT_SHA '+sha(target),flush=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--mode',choices=('baseline','main','noz3'),required=True)
    parser.add_argument('--harness',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--producer-source',type=Path)
    main(parser.parse_args())
