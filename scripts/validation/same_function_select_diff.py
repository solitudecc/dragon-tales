#!/usr/bin/env python3
"""Native before/after oracle for same-function conditional state threading.

Uses an existing library without configuring/building or modifying its repo.
16 fixtures x 2 conditions x 2 outer guards x 4 boundary values x before/after
x native O0/O2 = 1024 normal observations. Four specified single-result-bit
controls add 1024 control observations. Poison/undef contracts are unit-only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

NAMES = tuple(prefix + suffix for suffix in ('_phi', '_load') for prefix in (
    'select_freeze', 'select_noundef', 'select_default', 'select_same', 'select_guard')) + (
    'gap_compute', 'gap_load', 'gap_readonly', 'call_then_store', 'store_then_call', 'select_loop')
INPUTS = (0, 1, 1 << 63, (1 << 64) - 1)
MASK = (1 << 64) - 1
FIELDS = ('result', 'state0', 'state1', 'side0', 'side1', 'calls')
CONTROL = ('select_freeze_phi', 0, 0, 0)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            value.update(block)
    return value.hexdigest()


def expected(name, condition, guard, x):
    selected = 1 if condition else 2
    result = x + (11 if condition else 22)
    state0, state1, side0, side1, calls = 9, 7, x ^ 0x8080, 0x12345678, 0
    if name.endswith('_load'):
        state0 = selected
    if name.startswith('select_default') and condition:
        result = x + 33
        if name.endswith('_load'):
            state0 = 3
    elif name.startswith('select_same'):
        result = x + 11
    elif name.startswith('select_guard') and not guard:
        result = x + 44
    elif name == 'gap_compute':
        state0, result = 1, x + 7
    elif name == 'gap_load':
        state0, result = 1, (1 if guard else side0) ^ x
    elif name == 'gap_readonly':
        state0, result = 1, 1 ^ x ^ 0xa5
    elif name in ('call_then_store', 'store_then_call'):
        state0 = selected if name == 'call_then_store' else 3
        state1 ^= 0x55
        side0 ^= x
        calls = 1
        result = x + {1: 11, 2: 22, 3: 33}[state0]
    elif name == 'select_loop':
        state0, state1 = 2, max(1, x & 3)
        result = state1
    return dict(zip(FIELDS, (result & MASK, state0, state1, side0, side1, calls)))


EXPECTED = {(name, condition, guard, x): expected(name, condition, guard, x)
            for name in NAMES for condition in (0, 1) for guard in (0, 1) for x in INPUTS}


def parse_native(result, control):
    records, differences = {}, []
    lines = result.stdout.decode().splitlines()
    for line in lines[:-1]:
        name, *pairs = line.split()
        if len(pairs) != 11:
            raise ValueError('wrong native field count')
        row = {key: int(value) for key, value in (pair.split('=') for pair in pairs)}
        if set(row) != {'c', 'g', 'x', *FIELDS, 'expected', 'match'}:
            raise ValueError('wrong native fields')
        key = (name, row['c'], row['g'], row['x'])
        if key in records or key not in EXPECTED:
            raise ValueError(f'unexpected/duplicate native input: {key}')
        actual = {field: row[field] for field in FIELDS}
        wanted = EXPECTED[key]
        if row['expected'] != wanted['result']:
            raise ValueError(f'wrong driver expected result: {key}')
        mismatch = [field for field in FIELDS if actual[field] != wanted[field]]
        if row['match'] != int(not mismatch):
            raise ValueError(f'wrong driver match flag: {key}')
        records[key] = actual
        if mismatch:
            differences.append(dict(key=list(key), fields=mismatch, actual=actual, expected=wanted))
    if set(records) != set(EXPECTED):
        raise ValueError('missing native input coverage')
    wanted_summary = f'comparisons=256 mismatches={len(differences)} skips=0 control={int(control)}'
    if lines[-1] != wanted_summary or result.returncode != int(bool(differences)):
        raise ValueError('wrong native summary or exit code')
    return records, differences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--clang', default='clang-21')
    parser.add_argument('--llvm-config', default='llvm-config-21')
    args = parser.parse_args()
    repo, library, work = (path.expanduser().resolve() for path in (args.repo, args.library, args.work))
    if work.exists():
        parser.error('--work must not exist; outputs are never overwritten')
    work.mkdir(parents=True)
    env = dict(os.environ, LD_LIBRARY_PATH=str(library.parent))
    report = dict(repo=str(repo), library=str(library), commands=[], runs=[], errors=[],
                  normal_comparisons=0, normal_mismatches=0, control_comparisons=0,
                  control_mismatches=0, specified_controls=0, controls_detected=0,
                  skips=0, exclusions=0, normalizations=0)

    def run(name, command, check=True):
        try:
            result = subprocess.run(command, capture_output=True, env=env, timeout=120)
        except subprocess.TimeoutExpired as error:
            (work / (name + '.stdout')).write_bytes(error.stdout or b'')
            (work / (name + '.stderr')).write_bytes(error.stderr or b'')
            report['commands'].append(dict(name=name, argv=command, timeout=True))
            raise RuntimeError(f'{name} timed out; retained partial output') from error
        (work / (name + '.stdout')).write_bytes(result.stdout)
        (work / (name + '.stderr')).write_bytes(result.stderr)
        report['commands'].append(dict(name=name, argv=command, returncode=result.returncode))
        if check and result.returncode:
            raise RuntimeError(f'{name} failed with rc={result.returncode}')
        return result

    try:
        pin = digest(library)
        report['library_sha256'] = pin
        report['repo_head'] = run('repo-head', ['git', '-C', str(repo), 'rev-parse', 'HEAD']).stdout.decode().strip()
        run('repo-status', ['git', '-C', str(repo), 'status', '--short'])
        report['production_source_sha256'] = digest(repo / 'lib/LLVMDeflattening.cpp')
        sources = work / 'sources'
        sources.mkdir()
        names = ('same_function_select_diff.py', 'same_function_select_native.cpp',
                 'same_function_select_driver.c', 'same_function_select_fixtures.hpp')
        for name in names:
            shutil.copyfile(Path(__file__).resolve().parent / name, sources / name)
        report['source_sha256'] = {name: digest(sources / name) for name in names}
        report['input_sha256'] = hashlib.sha256(json.dumps(
            [list(key) for key in EXPECTED], sort_keys=True).encode()).hexdigest()
        run('compiler-version', [args.cxx, '--version'])
        run('clang-version', [args.clang, '--version'])
        includedir = run('llvm-include', [args.llvm_config, '--includedir']).stdout.decode().strip()
        generator = work / 'generate'
        run('compile-generator', [args.cxx, '-std=c++20', '-O0', '-I' + str(repo / 'include'),
                                 '-I' + includedir, str(sources / 'same_function_select_native.cpp'),
                                 str(library), '-Wl,-rpath,' + str(library.parent), '-o', str(generator)])
        generated = run('generate', [str(generator), str(work)])
        report['pass_stats'] = dict((key, int(value)) for key, value in (
            field.split('=') for field in generated.stdout.decode().split()))
        if report['pass_stats']['invalid'] or digest(library) != pin:
            raise RuntimeError('IR verification failed or library changed')
        report['ir_sha256'] = {stage: digest(work / (stage + '.ll')) for stage in ('before', 'after')}
        for level in ('O0', 'O2'):
            for stage in ('before', 'after'):
                name = stage + '-' + level
                executable = work / name
                run('compile-' + name, [args.clang, '-' + level, '-Wno-override-module',
                    str(work / (stage + '.ll')), str(sources / 'same_function_select_driver.c'),
                    '-o', str(executable)])
                actual, mismatches = parse_native(run(name, [str(executable)], False), False)
                controlled, control_mismatches = parse_native(
                    run(name + '-control', [str(executable), 'control'], False), True)
                changes = [(key, field) for key in actual for field in FIELDS
                           if actual[key][field] != controlled[key][field]]
                control_ok = (changes == [(CONTROL, 'result')] and
                              controlled[CONTROL]['result'] == (actual[CONTROL]['result'] ^ 1) and
                              any(tuple(row['key']) == CONTROL and row['fields'] == ['result']
                                  for row in control_mismatches))
                report['normal_comparisons'] += len(actual)
                report['normal_mismatches'] += len(mismatches)
                report['control_comparisons'] += len(controlled)
                report['control_mismatches'] += len(control_mismatches)
                report['specified_controls'] += 1
                report['controls_detected'] += int(control_ok)
                report['runs'].append(dict(stage=stage, level=level, differences=mismatches,
                    control_differences=control_mismatches, specified_control_detected=control_ok))
        report['ok'] = (report['normal_comparisons'] == report['control_comparisons'] == 1024 and
                        report['normal_mismatches'] == 0 and
                        report['specified_controls'] == report['controls_detected'] == 4)
        if digest(library) != pin:
            raise RuntimeError('library changed during native replay')
    except Exception as error:
        report['errors'].append(f'{type(error).__name__}: {error}')
        report['ok'] = False
    report['artifact_sha256'] = {str(path.relative_to(work)): digest(path)
        for path in sorted(work.rglob('*')) if path.is_file()}
    (work / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(json.dumps({key: report[key] for key in (
        'normal_comparisons', 'normal_mismatches', 'control_comparisons', 'control_mismatches',
        'specified_controls', 'controls_detected', 'errors', 'ok')}))
    print(work / 'report.json')
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
