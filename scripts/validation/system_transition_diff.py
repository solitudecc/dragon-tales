"""Exact pre-system-transition boundaries, using the unchanged Guest oracle.

Run one v2/static/product mode and strategy per fresh output directory. Every
fixture executes a register copy and Guest STORE before the transition. A
register write, another STORE and RET after it must never execute. Unicorn's
existing unsupported_before hook stops BEFORE the transition; this is an
Unsupported-boundary contract, not an emulation of kernel/privilege effects.

The legacy_unlifted_diff compiler/reference/comparator are reused unchanged.
This wrapper replaces its fixtures, plan audit, single-control predicate and
pure self-test, and records reference snapshots/observed hook PCs. Legacy mode
is deliberately outside this harness. All successful normal observations are
incomplete Guest boundaries, never complete-function/deobfuscation successes.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import legacy_unlifted_diff as study


ENTRY = study.ENTRY
_base_check_plan = study.check_plan
VALUES64 = [0, 1, 0xffffffffffffffff, 0x8000000000000000,
            0xffffffff, 0x7fffffffffffffff, 0x100000000, 0x80000000]


def boundary(fixture):
    pc = fixture['unsupported_before'][0]
    return [5, 0, pc, pc, 1, 0, 2]


def fixtures():
    definitions = (
        ('x86_32_int80', 'x86_32', 'cd80'),
        ('x86_32_sysenter', 'x86_32', '0f34'),
        ('x86_64_syscall', 'x86_64', '0f05'),
        ('x86_64_hlt', 'x86_64', 'f4'),
        ('arm64_svc', 'arm64', '010000d4'),
        ('arm64_hvc', 'arm64', '020000d4'),
        ('arm64_smc', 'arm64', '030000d4'),
        ('arm64_eret', 'arm64', 'e0039fd6'),
    )
    result = []
    for name, arch, instruction in definitions:
        a64 = arch == 'arm64'
        # MOV X0,X1 / STR X0,[X19], or MOV EAX,ECX / MOV [E/RBX],EAX.
        prefix = bytes.fromhex('e00301aa600200f9' if a64 else '89c88903')
        suffix = bytes.fromhex('600c80d2600600f9c0035fd6' if a64
                               else 'b863000000894304c3')
        bad = bytes.fromhex(instruction)
        code = prefix + bad + suffix
        width = 8 if a64 else 4
        values = study.VALUES if arch == 'x86_32' else VALUES64
        register = 'X1' if a64 else 'ECX' if arch == 'x86_32' else 'RCX'
        f = dict(id=name, architecture=arch, expectation='unsupported',
                 code_hex=code.hex(), unsupported_before=[ENTRY + len(prefix)],
                 system_opcode_hex=instruction, prefix_hex=prefix.hex(),
                 suffix_hex=suffix.hex(), prefix_store_width=width,
                 ret_pc=ENTRY + len(code) - (4 if a64 else 1),
                 undefined_flags=[], stubs=[], set_gprs={register: list(values)},
                 memory_values=[dict(address=0x8000 + width, width=width,
                                     value=0x8877665544332211 & ((1 << (8 * width)) - 1))])
        assert code[len(prefix):len(prefix) + len(bad)] == bad
        if a64:
            assert len(code) % 4 == 0
        result.append(f)
    return dict(schema=1, entry=ENTRY, cases=result)


def single_control(row, control):
    arch = row.get('architecture')
    if arch not in ('x86_32', 'x86_64', 'arm64') or control not in ('result', 'memory'):
        return False
    differences = row.get('differences', [])
    if row.get('status') != 'different' or not isinstance(differences, list) or len(differences) != 1:
        return False
    field = 'memory' if control == 'memory' else {
        'x86_32': 'EAX', 'x86_64': 'RAX', 'arm64': 'X0'}[arch]
    return (type(differences[0]) is dict and differences[0].get('field') == field and
            (control != 'memory' or (type(differences[0].get('count')) is int and
                                     differences[0]['count'] == 1)))


def check_plan(data, rows, reports, trials, mode, baseline):
    result = _base_check_plan(data, rows, reports, trials, mode, baseline)
    errors = list(result['errors'])
    by_id = {f['id']: f for f in data['cases']}
    if len(by_id) != len(data['cases']) or mode not in ('v2', 'static', 'product') or baseline:
        errors.append('invalid fixture identity or unsupported observation mode')
    for report in reports:
        f = by_id.get(report.get('case'))
        if (f is None or report.get('expected_unlifted') != f['unsupported_before'] or
                report.get('unlifted') != f['unsupported_before'] or
                any(type(pc) is not int for pc in report.get('unlifted', []))):
            errors.append('module markers do not equal the fixture transition PC')
    for row in rows:
        f = by_id.get(row.get('case'))
        record = row.get('exit')
        if (f is None or row.get('architecture') != f['architecture'] or
                type(record) is not list or len(record) != 7 or
                any(type(value) is not int for value in record) or record != boundary(f)):
            errors.append('not the exact seven-field pre-transition Unsupported exit')
        if row.get('status') not in ('match', 'different'):
            errors.append('execution/reference error is not a semantic observation')
    return dict(result, ok=not errors, errors=errors)


def self_test():
    data = fixtures()
    reports = [dict(case=f['id'], opt=level, accepted=True,
                    unlifted=f['unsupported_before'], expected_unlifted=f['unsupported_before'])
               for f in data['cases'] for level in study.LEVELS]
    rows = [dict(case=f['id'], architecture=f['architecture'], opt=level,
                 trial=trial, status='match', exit=boundary(f))
            for f in data['cases'] for level in study.LEVELS for trial in range(2)]
    checks = {'exact_plan_accepted': check_plan(data, rows, reports, 2, 'v2', False)['ok']}
    def reject(name, changed_rows=None, changed_reports=None, changed_data=None):
        checks[name] = not check_plan(changed_data or data,
            rows if changed_rows is None else changed_rows,
            reports if changed_reports is None else changed_reports, 2, 'v2', False)['ok']
    reject('missing_whole_fixture', rows[:-4])
    reject('missing_trial', rows[:-1])
    changed = copy.deepcopy(rows); changed[1] = copy.deepcopy(changed[0]); reject('duplicate_trial', changed)
    for value, name in ((True, 'boolean_trial'), (0.0, 'float_trial')):
        changed = copy.deepcopy(rows); changed[0]['trial'] = value; reject(name, changed)
    reject('missing_module', changed_reports=reports[1:])
    changed = copy.deepcopy(reports); changed[1] = copy.deepcopy(changed[0]); reject('duplicate_module', changed_reports=changed)
    changed = copy.deepcopy(reports); changed[0]['unlifted'] = []; reject('missing_marker', changed_reports=changed)
    changed = copy.deepcopy(reports); changed[0]['unlifted'] = []; changed[0]['expected_unlifted'] = []
    reject('changed_actual_and_expected_marker', changed_reports=changed)
    changed = copy.deepcopy(reports); changed[0]['accepted'] = False; reject('unexpected_refusal', changed_reports=changed)
    changed = copy.deepcopy(reports); changed[0]['error'] = 'injected'; reject('module_error', changed_reports=changed)
    for index in range(7):
        changed = copy.deepcopy(rows); changed[0]['exit'][index] ^= 1
        reject('incorrect_exit_field_' + str(index), changed)
    for value, name in ((None, 'missing_exit'), ([5] * 8, 'extra_exit_field'),
                        ([True, 0, 4100, 4100, 1, 0, 2], 'boolean_exit')):
        changed = copy.deepcopy(rows); changed[0]['exit'] = value; reject(name, changed)
    changed = copy.deepcopy(rows); changed[0]['architecture'] = 'arm64'; reject('wrong_architecture', changed)
    for arch, field in (('x86_32', 'EAX'), ('x86_64', 'RAX'), ('arm64', 'X0')):
        row = dict(architecture=arch, status='different', differences=[dict(field=field)])
        checks[arch + '_result_control'] = single_control(row, 'result')
    row = dict(architecture='arm64', status='different', differences=[dict(field='RAX')])
    checks['ARM_RAX_is_not_X0'] = not single_control(row, 'result')
    row['differences'] = [dict(field='memory', count=1)]
    checks['single_memory_byte'] = single_control(row, 'memory')
    for count in (2, True):
        row['differences'][0]['count'] = count
        checks['bad_memory_count_' + str(count)] = not single_control(row, 'memory')
    row['differences'] = [dict(field='X0'), dict(field='exit')]
    checks['extra_difference_rejected'] = not single_control(row, 'result')
    return dict(ok=all(checks.values()), passed=sum(checks.values()), total=len(checks),
                checks=checks, Dragon_Unicorn_LLVM_native_executions=0)


def main():
    if sys.flags.optimize:
        raise RuntimeError('run without Python -O/PYTHONOPTIMIZE; validation assertions are required')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('v2', 'static', 'product'), default='product')
    parser.add_argument('--strategy', type=int, choices=(0, 1), default=0)
    parser.add_argument('--static-entry', choices=('function', 'resumable'), default='function')
    parser.add_argument('--control', choices=('result', 'memory'))
    parser.add_argument('--expect-control', action='store_true')
    parser.add_argument('--expect-backend-sha256'); parser.add_argument('--expect-shim-sha256')
    parser.add_argument('--trials', type=int, default=8); parser.add_argument('--out', type=Path)
    parser.add_argument('--opt', default='opt-21'); parser.add_argument('--llc', default='llc-21')
    parser.add_argument('--cc', default='gcc'); parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        report = self_test(); print(json.dumps(report, sort_keys=True)); return 0 if report['ok'] else 1
    if args.out is None or args.trials < 8 or bool(args.control) != args.expect_control:
        parser.error('fresh --out, eight edge trials, and matching control flags are required')
    if os.environ.get('DRAGON_TEST_PYTHON'):
        sys.path.insert(0, os.environ['DRAGON_TEST_PYTHON']); __import__('dragon')
    import function_guest_diff as guest
    assert guest.base.dragon.IGNILOpcode.UNSUPPORTED == 72
    assert guest.base.dragon.IGNILOpcode.name(72) == 'UNSUPPORTED'
    from function_static_guest_diff import StaticAdapter  # noqa: F401: same original adapter
    pending, captures, checked_objects = [], [], {}
    original_reference, original_native = guest.Adapter.reference, guest.base.native
    old = {name: getattr(study, name) for name in ('fixtures', 'single_control', 'check_plan', 'self_test')}

    def reference(adapter, f, regs, memory, entry):
        assert not pending, 'reference/native calls must be sequential'
        visited = []; original_uc = guest.Uc
        def observed_uc(*a, **kw):
            uc = original_uc(*a, **kw)
            def observe(u, pc, size, _):
                visited.append(dict(pc=int(u.reg_read(adapter.pc_reg)), size=size,
                                    bytes=bytes(u.mem_read(pc, size)).hex()))
            uc.hook_add(guest.UC_HOOK_CODE, observe)
            return uc
        guest.Uc = observed_uc
        try:
            expected = original_reference(adapter, f, regs, memory, entry)
        finally:
            guest.Uc = original_uc
        bad_pc = f['unsupported_before'][0]; width = f['prefix_store_width']
        assert expected[3] == tuple(boundary(f)) and visited[-1]['pc'] == bad_pc
        assert visited[-1]['bytes'] == f['system_opcode_hex']
        assert [v['pc'] for v in visited] == [entry, entry + (4 if adapter.a64 else 2), bad_pc]
        source_width = 8 if f['architecture'] == 'arm64' else 4
        value = guest.base.get(regs, 8, source_width)
        assert guest.base.get(expected[0], 0, 8 if f['architecture'] != 'x86_32' else 4) == value
        assert guest.base.get(expected[1], guest.base.DATA, width) == value
        assert expected[1][guest.base.DATA + width:guest.base.DATA + 2 * width] == memory[guest.base.DATA + width:guest.base.DATA + 2 * width]
        code = bytes.fromhex(f['code_hex'])
        assert expected[1][entry:entry + len(code)] == code
        pending.append(dict(fixture=f, expected=expected, visited=visited))
        return expected

    def native(exe, regs, memory, work, trial, **kw):
        assert len(pending) == 1
        current = pending.pop(); folder = Path(work) / ('system-boundary-' + str(trial))
        folder.mkdir()
        expected = current['expected']
        (folder / 'expected-registers.bin').write_bytes(expected[0])
        (folder / 'expected-memory.bin').write_bytes(expected[1])
        study_json = dict(case=current['fixture']['id'], trial=trial,
            expected_exit=list(expected[3]), hook_visits=current['visited'],
            system_instruction_executed_by_Unicorn=False,
            scope='Existing unsupported_before code hook stopped before transition; no kernel model')
        (folder / 'reference.json').write_text(json.dumps(study_json, indent=2) + '\n')
        if str(exe) not in checked_objects:
            result = subprocess.run(['llvm-objdump-21', '--no-show-raw-insn', '-d',
                                     str(Path(work) / 'module.o')], capture_output=True, check=True)
            (Path(work) / 'host-object-disassembly.stdout').write_bytes(result.stdout)
            (Path(work) / 'host-object-disassembly.stderr').write_bytes(result.stderr)
            names = re.findall(r'^\s*[0-9a-f]+:\s+([.a-z][a-z0-9.]*)\b', result.stdout.decode(), re.M)
            forbidden = {'svc', 'hvc', 'smc', 'syscall', 'sysenter', 'sysret', 'sysretq',
                         'sysexit', 'sysexitl', 'sysexitq', 'hlt', 'iret', 'iretw', 'iretd', 'iretq',
                         'eret', 'eretaa', 'eretab', 'drps'}
            assert names and not set(names) & forbidden, 'host module contains a system transition'
            checked_objects[str(exe)] = dict(object_sha256=study.digest(Path(work) / 'module.o'),
                disassembly_sha256=study.digest(Path(work) / 'host-object-disassembly.stdout'))
        actual, error = original_native(exe, regs, memory, work, trial, **kw)
        captures.append(dict(case=current['fixture']['id'], trial=trial,
            path=str(folder), native_error=error, expected_registers_sha256=study.digest(folder / 'expected-registers.bin'),
            expected_memory_sha256=study.digest(folder / 'expected-memory.bin')))
        return actual, error

    for name, value in (('fixtures', fixtures), ('single_control', single_control),
                        ('check_plan', check_plan), ('self_test', self_test)):
        setattr(study, name, value)
    guest.Adapter.reference, guest.base.native = reference, native
    try:
        result = study.main()
    finally:
        guest.Adapter.reference, guest.base.native = original_reference, original_native
        for name, value in old.items():
            setattr(study, name, value)
    path = args.out.resolve() / 'results.json'; data = json.loads(path.read_text())
    summary = data['summary']; capture_ok = not pending and len(captures) == summary['cpu_comparisons']
    summary.update(system_transition_wrapper_sha256=study.digest(Path(__file__)),
        wrapper_replaced_functions=['fixtures', 'single_control', 'check_plan', 'self_test'],
        original_Guest_reference_and_comparator_unchanged=True,
        all_observations_are_pretransition_boundaries=True,
        actual_root_returns_observed=sum(r.get('exit', [0])[0] == 1 for r in data['results']),
        expected_complete_root_successes=0, expected_incomplete_boundary_checks=summary['plan_audit']['expected_cpu_comparisons'],
        boundary_reference_snapshots=captures, reference_capture_complete=capture_ok,
        host_objects_without_system_transition=checked_objects,
        syscall_or_kernel_effects_reference_executed=False,
        complete_deobfuscation=False,
        semantics='Full modeled Guest fields, PC/SP,64KiB and7 Exit fields at the exact pre-system Unsupported boundary; no complete-function or kernel/OS semantics claim')
    summary['source_sha256'][str(Path(__file__))] = study.digest(Path(__file__))
    summary['ok'] = summary['ok'] and capture_ok
    path.write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps(summary, sort_keys=True))
    return 0 if result == 0 and summary['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
