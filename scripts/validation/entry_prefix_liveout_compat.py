#!/usr/bin/env python3
"""Byte-exact compatibility on the previously frozen successful products.

The old cwd and input argument spelling are retained, including ModuleID.
No original oracle directory is written and no native execution is counted.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--prior', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / 'references').mkdir()
    env = dict(os.environ, LD_LIBRARY_PATH=str(args.runtime.resolve()))
    cases = []
    for mode in ('entry_phi', 'diamond_no_dead_latch', 'loop', 'fresh'):
        for blocks, instructions in ((6, 256), (32, 2048)):
            cases.append((f'{mode}-{blocks}-{instructions}', f'before/{mode}.ll',
                          f'after-{blocks}-{instructions}/{mode}.ll', 'oracle', blocks, instructions))
    cases.append(('canonical-loop', 'reference-snapshot/loop-guarded.ll', 'canonical-loop-prefix.ll',
                  'dragon_static_entry_x86_64_1400.guarded.oracle', 32, 2048))
    for mode in ('terminal_default', 'terminal_duplicate'):
        for blocks, instructions in ((6, 256), (32, 2048)):
            cases.append((f'{mode}-{blocks}-{instructions}', f'terminal/before/{mode}.ll',
                          f'terminal/after-{blocks}-{instructions}/{mode}.ll', 'oracle', blocks, instructions))
    report = {'compatible': [], 'controls': [], 'native_executions_added': 0, 'failures': []}

    def call(name, input_name, symbol, blocks, instructions):
        source = (args.prior / input_name).read_bytes()
        (args.out / 'references' / (name + '-input.ll')).write_bytes(source)
        output = (args.out / (name + '.ll')).resolve()
        argv = [str(args.driver.resolve()), input_name, str(output), symbol, str(blocks), str(instructions)]
        result = subprocess.run(argv, cwd=args.prior, env=env, capture_output=True, timeout=180)
        (args.out / (name + '.stdout')).write_bytes(result.stdout)
        (args.out / (name + '.stderr')).write_bytes(result.stderr)
        assert (args.prior / input_name).read_bytes() == source
        return {'name': name, 'argv': argv, 'cwd': str(args.prior), 'exit': result.returncode,
                'source_sha256': sha(source), 'output_exists': output.exists()}, output

    try:
        for name, source, expected_name, symbol, blocks, instructions in cases:
            item, output = call(name, source, symbol, blocks, instructions)
            expected = (args.prior / expected_name).read_bytes()
            (args.out / 'references' / (name + '-expected-after.ll')).write_bytes(expected)
            actual = output.read_bytes() if output.exists() else b''
            result_ok = item['exit'] == 0 and actual == expected
            item.update({'expected_sha256': sha(expected), 'actual_sha256': sha(actual),
                         'byte_equal': result_ok})
            report['compatible'].append(item)
            if not result_ok:
                (args.out / (name + '.diff')).write_text(''.join(difflib.unified_diff(
                    expected.decode().splitlines(True), actual.decode().splitlines(True),
                    fromfile='frozen-old', tofile='candidate')))
                report['failures'].append(name)
        controls = (
            ('old-historical', 'before/reject_historical.ll', 32, 2048, False),
            ('old-shared', 'terminal/before/reject_shared_leaf.ll', 32, 2048, False),
            ('old-budget', 'terminal/before/terminal_default.ll', 1, 2048, False),
            ('old-direct-liveout-now-positive', 'before/reject_liveout.ll', 32, 2048, True),
        )
        for name, source, blocks, instructions, changed in controls:
            item, output = call(name, source, 'oracle', blocks, instructions)
            stats = json.loads((args.out / (name + '.stderr')).read_text())
            item.update({'expected_changed': changed, 'stats': stats})
            report['controls'].append(item)
            assert item['exit'] == (0 if changed else 3) and output.exists() == changed
            assert stats['changed'] == changed and stats['verified']
            if not changed:
                assert stats['rejectedUnchanged'] and not stats['complete']
        assert len(report['compatible']) == 13 and not report['failures']
    except Exception as error:
        report['failures'].append(repr(error))
        raise
    finally:
        (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
