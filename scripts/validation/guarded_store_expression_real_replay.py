"""Replay frozen B ELFs against separately pinned STORE-expression IR.

No producer, disassembly, lifting or product-pass call is made. A plan binds
exact archived raw/driver bytes to each ELF and to the new parsed-before/after
products. Existing initial states, Unicorn oracle, full comparator and native
protocol are reused unchanged. Reference return/out16 are also checked against
the independent Python specification; this is not a new native C comparison.
Run only under the outer storage/time watchdog used by the owner.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile

import practical_holdout_diff as A

study, base = A.study, A.study.base
ARCHIVE_SHA = '9c623d69bf90ded9c486a70861224c85966bb9698a97f3a9ba791e0cd3f77359'
PAYLOAD_SHA = 'e34e8a645f1a685967bb7fad894c35ef5f0db27747ff56d4a23c0b2c8867d1b8'
FROZEN_SHA = '0ec963329f95b987f93cc0de8f6700b8497068f76b88fdf7e7dbd49101d9f0db'
ARCHIVE_ROOT = 'practical-region-holdouts-20261113'
LIMITS = dict(guest_steps=1000000, subprocess_seconds=180,
              start_free=512 * 1024 * 1024, running_free=64 * 1024 * 1024)
IDS = {'manual-O0', 'manual-O2', 'flatten-switch-O0', 'flatten-switch-O2'}


def require(value, message):
    A.require(value, message)


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def pinned(record):
    path = Path(record['path'])
    require(path.is_absolute() and path.is_file() and not path.is_symlink(), 'unsafe pinned input')
    require(sha_file(path) == record['sha256'], 'pinned input hash mismatch: ' + str(path))
    return path


def write_json(path, value):
    A.write_json(path, value)


def run(command, work, tag):
    A.space(work, LIMITS['running_free'])
    study.run(command, work, tag, timeout=LIMITS['subprocess_seconds'])


def members(archive, requested, payload):
    found = {}
    for name in requested:
        pure = PurePosixPath(name)
        require(not pure.is_absolute() and '..' not in pure.parts and
                str(pure).startswith(ARCHIVE_ROOT + '/'), 'unsafe archive member')
        item = archive.getmember(name)
        require(item.isfile() and not item.issym() and not item.islnk(), 'archive member is not regular')
        data = archive.extractfile(item).read()
        relative = str(pure.relative_to(ARCHIVE_ROOT))
        row = payload['files'][relative]
        require(len(data) == row['size'] and sha_bytes(data) == row['sha256'], 'archive payload pin differs')
        found[name] = data
    return found


def mutate_control(adapter, expected, actual, field):
    regs, memory, events, extra = actual
    if field == 'RAX':
        regs = bytearray(regs); regs[0] ^= 1; regs = bytes(regs)
    elif field == 'memory':
        memory = bytearray(memory); memory[base.DATA] ^= 1; memory = bytes(memory)
    else:
        extra = bytearray(extra); extra[0] ^= 1; extra = bytes(extra)
    changed = (regs, memory, events.copy(), extra)
    differences, _ = adapter.compare(dict(undefined_flags=[]), expected, changed, False)
    require([row['field'] for row in differences] == [field], 'comparator control did not detect exact field')
    return dict(field=field, differences=differences, scope='comparator-only snapshot corruption; no extra native run')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    plan_path = args.plan.resolve(); plan_bytes = plan_path.read_bytes(); plan = json.loads(plan_bytes)
    require(plan['schema'] == 'dragon-store-expression-real-replay-plan-v1', 'wrong replay schema')
    require(plan.get('ready') is True, 'plan is not sealed after binder build')
    require(plan['limits'] == LIMITS, 'execution/resource limits changed')
    require({row['id'] for row in plan['cases']} == IDS and len(plan['cases']) == 4, 'require exact four cases')
    out = args.out.resolve()
    require(not out.exists(), 'output namespace must not exist')
    require(not plan_path.is_relative_to(out), 'output would contain the input plan')
    initial_free = A.space(out, LIMITS['start_free'])
    archive_path = pinned(plan['archive'])
    require(plan['archive']['sha256'] == ARCHIVE_SHA, 'require frozen B archive')
    frozen_root = Path(plan['frozen_root']).resolve()
    require(not out.is_relative_to(frozen_root) and not frozen_root.is_relative_to(out),
            'output and original B frozen namespace must be disjoint')
    frozen_path = frozen_root / 'FROZEN.json'
    require(sha_file(frozen_path) == FROZEN_SHA, 'frozen B manifest changed')
    frozen = json.loads(frozen_path.read_bytes())
    require(frozen['schema'] == 'dragon-practical-holdouts-region-inputs-v1' and
            frozen['seed'] == A.SEED and frozen['inputs'] == list(A.heldout.INPUTS) == study.INPUTS and
            len(frozen['inputs']) == 16 and frozen['control'] is None and
            frozen['guest_steps'] == LIMITS['guest_steps'], 'frozen execution/input contract changed')
    require(frozen['base_A_manifest_sha256'] ==
            '4ae488b0b0c0a236f077ce2bbb7d4ec7cdba40fca719ac2b0713bee3ca0cb0f7', 'B lineage changed')
    for relative in frozen['files']:
        A.frozen_file(frozen_root, relative, frozen)
    tools = {name: pinned(plan['tools'][name]) for name in ('opt', 'llc', 'cc', 'binder')}
    source_before, runtime_before = A.source_pins(), A.runtime_pins()
    # In addition to recording this new adapter's dependency closure, require
    # every original imported source/header pin from B to remain byte exact.
    for path, digest in frozen['source_pins'].items():
        require(Path(path).is_file() and sha_file(path) == digest, 'original B oracle dependency changed: ' + path)
    out.mkdir(parents=True)
    write_json(out / 'INVOCATION.json', dict(plan=plan, plan_sha256=sha_bytes(plan_bytes),
               argv=sys.argv, initial_free=initial_free, source_pins=source_before,
               runtime_pins=runtime_before, mathematical_reference='unchanged heldout Python specification',
               producer_runs=0, lift_runs=0, product_pass_runs=0, new_native_C_comparisons=0))
    rows, references, controls, binding = [], [], [], []
    original_subprocess_run = subprocess.run
    def limited_run(*a, **kw):
        kw['timeout'] = LIMITS['subprocess_seconds']
        return original_subprocess_run(*a, **kw)
    subprocess.run = limited_run
    try:
        with tarfile.open(archive_path) as archive:
            payload_name = ARCHIVE_ROOT + '/B-PAYLOAD-MANIFEST-v1.json'
            payload_bytes = archive.extractfile(payload_name).read()
            require(sha_bytes(payload_bytes) == PAYLOAD_SHA, 'frozen B payload manifest changed')
            payload = json.loads(payload_bytes)
            for case in plan['cases']:
                family, c_level = case['family'], case['c_level']
                require(case['id'] == family + '-' + c_level and family in ('manual', 'flatten-switch')
                        and c_level in ('O0', 'O2'), 'case identity inconsistent')
                image = frozen['families'][family]['images'][c_level]
                original_raw = pinned(case['raw_input'])
                require(str(original_raw) == case['raw_parse_identifier'] and
                        case['raw_input']['sha256'] == case['raw']['sha256'], 'original probe raw identity changed')
                elf_path = A.frozen_file(frozen_root, image['elf'], frozen)
                require(sha_file(elf_path) == image['sha256'] == case['elf_sha256'], 'ELF image pin changed')
                raw_member, driver_member = case['raw']['member'], case['driver']['member']
                raw_parent = PurePosixPath(raw_member).parent
                require(PurePosixPath(driver_member).parent == raw_parent and
                        PurePosixPath(raw_member).name == 'raw.ll' and
                        PurePosixPath(driver_member).name == 'driver.c', 'driver and raw belong to different modules')
                frozen_member = str(raw_parent.parent / 'frozen-input.json')
                data = members(archive, (raw_member, driver_member, frozen_member), payload)
                require(sha_bytes(data[raw_member]) == case['raw']['sha256'] and
                        sha_bytes(data[driver_member]) == case['driver']['sha256'], 'explicit raw/driver pin changed')
                identity = json.loads(data[frozen_member])
                require(identity['sha256'] == image['sha256'] and identity['elf'] == image['elf'], 'B module belongs to another ELF')
                work = out / case['id']; work.mkdir()
                for filename, content in [('raw.ll', data[raw_member]), ('driver.c', data[driver_member]),
                                           ('fixture.elf', elf_path.read_bytes())]:
                    (work / filename).write_bytes(content)
                    if filename == 'fixture.elf':
                        require(sha_file(work / filename) == image['sha256'], 'ELF copy differs')
                for name in ('hints', 'parsed_before', 'after'):
                    source = pinned(case[name]); destination = work / (name + source.suffix)
                    shutil.copyfile(source, destination)
                    require(sha_file(destination) == case[name]['sha256'], 'copied input differs')
                raw, driver = work / 'raw.ll', work / 'driver.c'
                hints = work / ('hints' + Path(case['hints']['path']).suffix)
                before = work / ('parsed_before' + Path(case['parsed_before']['path']).suffix)
                after = work / ('after' + Path(case['after']['path']).suffix)
                require(case['raw_parse_identifier'] and '\x00' not in case['raw_parse_identifier'], 'missing original parser identifier')
                run([tools['binder'], raw, hints, before, case['raw_parse_identifier']], work, 'bind-parser-hints')
                after_text = after.read_text()
                declarations = set(re.findall(r'void (dragon_static_entry_x86_64_[0-9a-f]+)\(S\*,M\*,C\*,E\*\);', driver.read_text()))
                defined = set(re.findall(r'^define void @(dragon_static_entry_x86_64_[0-9a-f]+)\(', after_text, re.M))
                require(declarations and declarations == defined, 'archived driver and after function symbols differ')
                _, regions, symbols = A.elf_parts(elf_path.read_bytes())
                require(symbols == image['symbols'], 'ELF symbols differ from B freeze')
                adapter = study.guest.Adapter('x86_64'); expected_trials = []
                for trial, value in enumerate(frozen['inputs']):
                    A.space(out, LIMITS['running_free'])
                    regs, memory = study.initial(adapter, regions, trial)
                    saved_input = str(raw_parent.parent / f'reference-input-{trial}.bin')
                    input_pin = payload['files'][str(PurePosixPath(saved_input).relative_to(ARCHIVE_ROOT))]
                    require(len(regs + memory) == input_pin['size'] and sha_bytes(regs + memory) == input_pin['sha256'],
                            'initial state differs from the exact original B input')
                    diagnostic = {}
                    # Preserve study.unicorn's diagnostic wrapper, substituting
                    # only the already-frozen 1M instruction limit from recipe B.
                    original_reference = adapter.reference
                    adapter.reference = lambda fixture, r, m, entry: original_reference(
                        dict(fixture, max_instructions=LIMITS['guest_steps']), r, m, entry)
                    try:
                        expected = study.unicorn(adapter, regs, memory, diagnostic)
                    finally:
                        adapter.reference = original_reference
                    require(expected[3][0] == 1 and expected[3][1] == 3 and
                            expected[3][3] == base.SENTINEL and expected[3][4] == 1,
                            'Unicorn did not reach root RET')
                    observed, specified = study.observables(expected), A.heldout.specification(A.SEED, value)
                    require(observed == specified, 'Unicorn/Python specification mismatch')
                    expected_trials.append((trial, regs, memory, expected))
                    expected_stream = (b'DTFLOW1\0' + struct.pack('<QQ', expected[2][0x2000], expected[2][0x3000]) +
                        bytes(expected[0]) + bytes(expected[1]) + struct.pack('<8Q', *expected[3], 0))
                    (work / f'expected-{trial}.bin').write_bytes(expected_stream)
                    references.append(dict(case=case['id'], trial=trial, value=value, input_sha256=input_pin['sha256'],
                         status='match', expected_snapshot_sha256=sha_bytes(expected_stream), exit=expected[3],
                         scope='Unicorn versus Python specification; no new native C comparison'))
                binding.append(dict(case=case['id'], raw_member=raw_member, raw_sha256=case['raw']['sha256'],
                    driver_member=driver_member, driver_sha256=case['driver']['sha256'], elf_sha256=image['sha256'],
                    parsed_before_sha256=case['parsed_before']['sha256'], after_sha256=case['after']['sha256'],
                    exact_parser_hint_binding=True))
                triple = {'aarch64': 'aarch64-linux-gnu', 'x86_64': 'x86_64-linux-gnu'}.get(platform.machine())
                require(triple, 'native replay requires Linux aarch64/x64')
                for level in ('O0', 'O2'):
                    native_work = work / level; native_work.mkdir()
                    shutil.copyfile(driver, native_work / 'driver.c')
                    selected = after
                    run([tools['opt'], '-passes=verify', '-disable-output', after], native_work, 'verify-raw')
                    if level == 'O2':
                        selected = native_work / 'optimized.ll'
                        run([tools['opt'], '-S', '-passes=default<O2>', after, '-o', selected], native_work, 'optimize')
                    run([tools['opt'], '-passes=verify', '-disable-output', selected], native_work, 'verify-final')
                    run([tools['llc'], '-O0', '-filetype=obj', '-mtriple=' + triple, selected, '-o', 'module.o'], native_work, 'llc')
                    run([tools['cc'], '-O0', '-I' + str(base.ROOT / 'include'), '-I' + str(base.ROOT / 'c_shim'),
                         'driver.c', 'module.o', '-o', 'native'], native_work, 'native-link')
                    bad_ir = 'unreachable' in selected.read_text()
                    for trial, regs, memory, expected in expected_trials:
                        A.space(out, LIMITS['running_free'])
                        actual, error = base.native(native_work / 'native', regs, memory, native_work, trial, extra_bytes=64)
                        row = dict(case=case['id'], host_llvm_level=level, trial=trial)
                        if error:
                            row.update(status=error['kind'], detail=error)
                        else:
                            differences, record = adapter.compare(dict(undefined_flags=[]), expected, actual, False)
                            root_return = record[0] == 1 and record[1] == 3 and record[3] == base.SENTINEL and record[4] == 1
                            if not root_return:
                                differences.append(dict(field='root_return', detail='explicit boundary is not a complete-function comparison'))
                            if record[7] != 0:
                                differences.append(dict(field='budget', got=record[7], expected=0))
                            if bad_ir:
                                differences.append(dict(field='ir', detail='unexpected unreachable'))
                            row.update(status='different' if differences else 'match', differences=differences,
                                exit=record[:7], budget_left=record[7], root_return=root_return,
                                register_fields_compared=len(adapter.fields), memory_bytes_compared=base.MEM_SIZE)
                            if not differences:
                                for field in ('RAX', 'memory', 'exit'):
                                    controls.append(dict(case=case['id'], host_llvm_level=level, trial=trial,
                                        **mutate_control(adapter, expected, actual, field)))
                        rows.append(row)
                        write_json(out / 'PROGRESS.json', dict(binding=binding, references=references, results=rows, controls=controls))
        require(sha_file(plan_path) == sha_bytes(plan_bytes), 'plan changed during replay')
        require(sha_file(archive_path) == ARCHIVE_SHA and sha_file(frozen_path) == FROZEN_SHA, 'archive/freeze changed')
        for relative in frozen['files']:
            A.frozen_file(frozen_root, relative, frozen)
        for record in plan['tools'].values():
            pinned(record)
        for case in plan['cases']:
            pinned(case['raw_input'])
            for name in ('hints', 'parsed_before', 'after'):
                pinned(case[name])
        require(A.source_pins() == source_before and A.runtime_pins() == runtime_before, 'source/runtime changed during replay')
        complete = len(rows) == 128 and len(references) == 64 and len(controls) == 384
        correct = complete and all(row['status'] == 'match' and row['root_return'] for row in rows)
        write_json(out / 'RESULTS.json', dict(plan_sha256=sha_bytes(plan_bytes), binding=binding,
             planned_native_comparisons=128, native_comparisons=len(rows), statuses=dict(collections.Counter(r['status'] for r in rows)),
             root_returns=sum(r.get('root_return', False) for r in rows), unicorn_python_spec_checks=len(references),
             new_native_C_comparisons=0, comparator_controls=len(controls), skipped=0, undefined_exclusions=0,
             normalizations=0, producer_runs=0, lift_runs=0, product_pass_runs=0,
             complete=complete, correct=correct, references=references, results=rows, controls=controls))
        return 0 if correct else 1
    except BaseException as error:
        write_json(out / 'FAILURE.json', dict(type=type(error).__name__, detail=str(error),
            native_comparisons=len(rows), references=len(references), controls=len(controls)))
        raise
    finally:
        subprocess.run = original_subprocess_run


if __name__ == '__main__':
    raise SystemExit(main())
