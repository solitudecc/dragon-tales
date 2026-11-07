#!/usr/bin/env python3
"""Compare table-dispatch publication with complete, unchanged-code Unicorn runs.

The core four fixtures are the permanent x64/A64 absolute/relative writable
external-table counterexamples. Five unsigned index edges run in both Graph
strategies. Empty publication is an honest analysis boundary, never a complete
function-analysis success. --baseline preserves wrong stale publication and
returns 1; it does not turn the old counterexample into a passing test.

The optional extended suite adds x64 full Guest-STORE initialization and a
LOAD-before-STORE-before-JMP timing case. Trusted contents are a test caller's
explicit per-LOAD input contract, not automatic image immutability proof.
This first layer checks CPU/Graph edges and memory, not native LLVM semantics.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import platform
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE, TABLE, DATA, STACK, SENTINEL = 0x1000, 0x1080, 0x3000, 0xc800, 0xf000
INDICES = (0, 1, 2, 0x80000000, 0xffffffff)
MASK64 = (1 << 64) - 1


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(1048576), b''):
            h.update(data)
    return h.hexdigest()


def make_fixture(name, architecture, prefix, dispatch, width, values, new0,
                 *, relative=False, load=None, load_before_store=False,
                 pure_stores=False):
    code = bytearray(128)
    if architecture == 'x86_64':
        code[:] = b'\xcc' * 128
        prefix = bytes.fromhex(prefix)
        for address in (0x1040, 0x1050, 0x1060):
            code[address - BASE] = 0xc3
    else:
        prefix = b''.join(word.to_bytes(4, 'little') for word in prefix)
        for address in (0x1040, 0x1050, 0x1060):
            code[address - BASE:address - BASE + 4] = bytes.fromhex('c0035fd6')
    assert len(prefix) < 0x40
    code[:len(prefix)] = prefix
    initial = b''.join(value.to_bytes(width, 'little') for value in values)
    final = new0.to_bytes(width, 'little') + initial[width:]
    return dict(id=name, architecture=architecture, code_hex=bytes(code).hex(),
                dispatch=dispatch, entry_size=width, initial_table_hex=initial.hex(),
                final_table_hex=final.hex(), relative=relative,
                load_pc=load, load_before_store=load_before_store,
                pure_stores=pure_stores)


def fixtures():
    # Core x64 bytes are identical to the frozen known-gap-final probe inputs.
    result = [
        make_fixture('x64-absolute', 'x86_64',
            'b96010000048890c2580100000833e01772e8b06ff24c580100000',
            0x1014, 8, (0x1040, 0x1050), 0x1060),
        make_fixture('x64-relative', 'x86_64',
            'c7042580100000e0ffffff833e0177308b06488d1567000000486304824801d0ffe0',
            0x1020, 4, (0xffffffc0, 0xffffffd0), 0xffffffe0, relative=True),
        # The same words as TestTableContents.cpp. Their execution is checked
        # independently on every run, before any counterexample is counted.
        make_fixture('a64-absolute', 'arm64', (
            0xd2821003, 0xd2820c04, 0xf9000064, 0xb9400020,
            0x7100041f, 0x54000168, 0xf8605860, 0xd61f0000),
            0x101c, 8, (0x1040, 0x1050), 0x1060),
        make_fixture('a64-relative', 'arm64', (
            0x10000401, 0x528001c3, 0x79000023, 0xb9400040,
            0x7100041f, 0x54000168, 0x78605820, 0x10000063,
            0x8b20a860, 0xd61f0000),
            0x1024, 2, (6, 10), 14, relative=True),
    ]
    # Entire table contents follow from Guest STOREs, without content callback.
    result.append(make_fixture('x64-full-stores', 'x86_64',
        'b96010000048890c2580100000b95010000048890c2588100000'
        '833e0177218b06ff24c580100000',
        0x1021, 8, (0x1040, 0x1050), 0x1060, pure_stores=True))
    # First LOAD table[IDX], then change table[0], then JMP the loaded value.
    # A content query here sees the initial table even though JMP-time table
    # bytes already contain1060. The trusted callback is indexed by LOAD PC.
    result.append(make_fixture('x64-load-then-store', 'x86_64',
        '833e01773b8b06488b04c580100000b96010000048890c2580100000ffe0',
        0x101c, 8, (0x1040, 0x1050), 0x1060,
        load=0x1007, load_before_store=True))
    return result


def expected_final_table(fixture, index):
    # The timing variant's unsigned guard precedes both its LOAD and STORE.
    # Rejected indices return directly without touching the table at all.
    field = ('initial_table_hex' if fixture['load_before_store'] and index >= 2
             else 'final_table_hex')
    return bytes.fromhex(fixture[field])


def target_status(published, actual):
    if actual is None:
        return 'guarded-return'
    if not published:
        return 'safe-unresolved'
    return 'covered-edge' if actual in published else 'wrong-publication'


def check_keys(rows, reports, selected, strategies, oracle_only):
    expected = {(f['id'], strategy, index)
                for f in selected for strategy in strategies for index in INDICES}
    keys = [(r.get('case'), r.get('strategy'), r.get('index')) for r in rows]
    expected_reports = {(f['id'], strategy) for f in selected for strategy in strategies}
    report_keys = [(r.get('case'), r.get('strategy')) for r in reports]
    return (len(keys) == len(set(keys)) == len(expected) and set(keys) == expected
            and len(report_keys) == len(set(report_keys)) == len(expected_reports)
            and set(report_keys) == expected_reports)


def self_test():
    """Pure Python checks; never load Dragon, Unicorn or compile a program."""
    import copy
    fs = fixtures()
    checks = dict(unique_fixtures=len({f['id'] for f in fs}) == 6,
                  all_128_bytes=all(len(bytes.fromhex(f['code_hex'])) == 128 for f in fs),
                  x64_absolute_frozen_code=digest(bytes.fromhex(fs[0]['code_hex'])) ==
                      '180c04d1934280b16a2df858e00de956780ab9668a2a1d38a86067b778b2513b',
                  x64_relative_frozen_code=digest(bytes.fromhex(fs[1]['code_hex'])) ==
                      '014c15267046de107efc0e72bf43f1f44e510d9730c31454733e024dc6b788f0',
                  stale_target_detected=target_status({0x1040, 0x1050}, 0x1060) == 'wrong-publication',
                  empty_is_boundary=target_status(set(), 0x1060) == 'safe-unresolved',
                  actual_edge_covered=target_status({0x1050, 0x1060}, 0x1060) == 'covered-edge')
    selected = fs[:4]
    rows = [dict(case=f['id'], strategy=s, index=i) for f in selected
            for s in (0, 1) for i in INDICES]
    reports = [dict(case=f['id'], strategy=s) for f in selected for s in (0, 1)]
    checks['valid_plan'] = check_keys(rows, reports, selected, (0, 1), False)
    checks['dropped_case_rejected'] = not check_keys(rows[10:], reports[2:], selected, (0, 1), False)
    dup = copy.deepcopy(rows); dup[1] = dict(dup[0])
    checks['duplicate_input_rejected'] = not check_keys(dup, reports, selected, (0, 1), False)
    dup = copy.deepcopy(reports); dup[1] = dict(dup[0])
    checks['duplicate_report_rejected'] = not check_keys(rows, dup, selected, (0, 1), False)
    timing = fs[5]
    checks['timing_guard_does_not_STORE'] = all(expected_final_table(timing, i) == bytes.fromhex(timing['initial_table_hex']) for i in INDICES[2:])
    checks['timing_admitted_path_does_STORE'] = all(expected_final_table(timing, i) == bytes.fromhex(timing['final_table_hex']) for i in INDICES[:2])
    checks['original_STORE_before_guard_still_required'] = all(expected_final_table(f, i) == bytes.fromhex(f['final_table_hex']) for f in fs[:5] for i in INDICES)
    return dict(ok=all(checks.values()), checks=checks, oracle_executions=0)


def actual_libraries(dragon, expected):
    lines = [line for line in Path('/proc/self/maps').read_text().splitlines()
             if '/libdragon' in line]
    if not lines or any('(deleted)' in line for line in lines):
        raise RuntimeError('missing or deleted Dragon mappings')
    paths = {str(Path(line.split()[-1]).resolve()) for line in lines}
    actual = {path: file_sha(path) for path in sorted(paths)}
    if actual != expected or str(Path(dragon._LIB).resolve()) not in paths:
        raise RuntimeError('actual dual Dragon mappings differ from --library-pin')
    return dict(libraries=actual, maps=lines)


def reference(fixture, index, work):
    import unicorn as U
    from unicorn import x86_const as X, arm64_const as A
    a64 = fixture['architecture'] == 'arm64'
    uc = U.Uc(U.UC_ARCH_ARM64 if a64 else U.UC_ARCH_X86,
              U.UC_MODE_ARM if a64 else U.UC_MODE_64)
    code = bytes.fromhex(fixture['code_hex'])
    table = bytes.fromhex(fixture['initial_table_hex'])
    memory = bytearray(65536)
    memory[BASE:BASE + len(code)] = code
    memory[TABLE:TABLE + len(table)] = table
    memory[DATA:DATA + 4] = index.to_bytes(4, 'little')
    if not a64:
        memory[STACK:STACK + 8] = SENTINEL.to_bytes(8, 'little')
    uc.mem_map(0, len(memory)); uc.mem_write(0, bytes(memory))
    if a64:
        uc.reg_write(A.UC_ARM64_REG_SP, STACK)
        uc.reg_write(A.UC_ARM64_REG_X30, SENTINEL)
        uc.reg_write(A.UC_ARM64_REG_X1 if fixture['id'] == 'a64-absolute'
                     else A.UC_ARM64_REG_X2, DATA)
        pc_reg = A.UC_ARM64_REG_PC
    else:
        uc.reg_write(X.UC_X86_REG_RSP, STACK)
        uc.reg_write(X.UC_X86_REG_RSI, DATA)
        pc_reg = X.UC_X86_REG_RIP
    trace = []
    def hook(machine, pc, size, _):
        trace.append(dict(pc=machine.reg_read(pc_reg), size=size))
    uc.hook_add(U.UC_HOOK_CODE, hook)
    uc.emu_start(BASE, SENTINEL, count=100)
    final = bytes(uc.mem_read(0, len(memory)))
    edges = [b['pc'] for a, b in zip(trace, trace[1:])
             if a['pc'] == fixture['dispatch']]
    target0 = 0x1040 if fixture['load_before_store'] else 0x1060
    expected_target = target0 if index == 0 else 0x1050 if index == 1 else 0x1040
    expected_edges = [expected_target] if index < 2 else []
    final_table = expected_final_table(fixture, index)
    last = trace[-1] if trace else None
    wanted_ret = bytes.fromhex('c0035fd6') if a64 else b'\xc3'
    checks = dict(normal_root_return=uc.reg_read(pc_reg) == SENTINEL,
                  architectural_return_instruction=bool(last) and
                    bytes(uc.mem_read(last['pc'], last['size'])) == wanted_ret,
                  correct_return_site=bool(last) and last['pc'] == expected_target,
                  actual_indirect_edge=edges == expected_edges,
                  code_unchanged=final[BASE:BASE + len(code)] == code,
                  table_entry0_changed_to_expected=final[TABLE:TABLE + len(table)] == final_table,
                  table_entry1_unchanged=final[TABLE + fixture['entry_size']:TABLE + len(table)] == table[fixture['entry_size']:],
                  correct_stack=uc.reg_read(A.UC_ARM64_REG_SP if a64 else X.UC_X86_REG_RSP) == STACK + (0 if a64 else 8))
    # Every byte outside the actual Guest STORE is still identical. Full table
    # stores may rewrite entry1 to the same value, so these bytes also match.
    want_memory = bytearray(memory)
    want_memory[TABLE:TABLE + len(table)] = final_table
    checks['all_64KiB_memory_matches_declared_effects'] = final == want_memory
    work.mkdir(parents=True, exist_ok=False)
    (work/'initial-memory.bin').write_bytes(memory)
    (work/'final-memory.bin').write_bytes(final)
    cpu = dict(index=index, final_pc=uc.reg_read(pc_reg), actual_edges=edges,
               trace=trace, checks=checks, complete_CPU_return=all(checks.values()),
               code_sha256=digest(code), initial_memory_sha256=digest(memory),
               final_memory_sha256=digest(final), final_table_hex=final[TABLE:TABLE + len(table)].hex(),
               memory_bytes_checked=65536)
    (work/'cpu.json').write_text(json.dumps(cpu, indent=2) + '\n')
    if not all(checks.values()):
        raise RuntimeError('Unicorn fixture premise failed: ' + repr(checks))
    return cpu


def analyze(dragon, fixture, strategy, content):
    table = bytes.fromhex(fixture['initial_table_hex'])
    width = fixture['entry_size']
    calls = []
    def image(address, size):
        if TABLE <= address and address + size <= TABLE + len(table):
            return table[address - TABLE:address - TABLE + size]
        return None
    def known(block, op_index, instruction_pc, address, size):
        accepted = TABLE <= address and address + size <= TABLE + len(table)
        if content == 'trusted-entry1':
            accepted = accepted and address >= TABLE + width
        if fixture['load_before_store']:
            accepted = accepted and instruction_pc == fixture['load_pc']
        value = None
        if accepted:
            at_load = (table if fixture['load_before_store'] else
                       bytes.fromhex(fixture['final_table_hex']))
            value = at_load[address - TABLE:address - TABLE + size]
        calls.append(dict(block=block, op_index=op_index, instruction_pc=instruction_pc,
                          address=address, size=size, supplied=value.hex() if value is not None else None))
        return value
    kwargs = dict(read_memory=image)
    if content != 'default':
        kwargs['read_known_memory'] = known
    with dragon.AnalysisHints(**kwargs) as hints:
        with dragon.Disassembler(4 if fixture['architecture'] == 'arm64' else 2,
                                dragon.Configuration(strategy=strategy)) as dis:
            with dis.analyze(bytes.fromhex(fixture['code_hex']), BASE, hints=hints) as graph:
                instruction = graph.get_instruction(fixture['dispatch'])
                if instruction is None:
                    raise RuntimeError('planned indirect instruction missing from Graph')
                published = sorted(instruction.branches)
                unlifted = {str(pc): list(graph.get_ignil_block(pc).unlifted())
                            for pc in graph.block_addresses() if graph.get_ignil_block(pc)
                            and graph.get_ignil_block(pc).unlifted()}
                return dict(published_targets=published, unlifted=unlifted,
                            content_callback_calls=calls,
                            candidate_scope='ordinary default Graph; no finite-dispatch injection')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--suite', choices=('core', 'extended'), default='core')
    parser.add_argument('--case', action='append', dest='cases')
    parser.add_argument('--strategy', action='append', type=int, choices=(0, 1))
    parser.add_argument('--content', choices=('default', 'trusted-entry1', 'trusted-load'), default='default')
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--oracle-only', action='store_true')
    parser.add_argument('--library-pin', action='append', default=[], metavar='PATH=SHA256')
    parser.add_argument('--python-sha256')
    parser.add_argument('--control', choices=('stale-target',))
    parser.add_argument('--expect-control', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        result = self_test(); print(json.dumps(result, indent=2)); return 0 if result['ok'] else 1
    if args.out is None: parser.error('--out is required')
    if args.oracle_only and (args.baseline or args.content != 'default'):
        parser.error('--oracle-only is a CPU premise check, without baseline/content claims')
    if args.baseline and (args.content != 'default' or args.suite != 'core' or args.control):
        parser.error('--baseline requires core/default and no control')
    if bool(args.control) != args.expect_control or args.oracle_only and args.control:
        parser.error('controls require --expect-control and a real Graph run')
    harness_before = file_sha(__file__)
    selected = fixtures()[:4] if args.suite == 'core' else fixtures()
    ids = [f['id'] for f in selected]
    if args.cases:
        if len(args.cases) != len(set(args.cases)) or not set(args.cases) <= set(ids):
            parser.error('unknown or duplicate case')
        selected = [f for f in selected if f['id'] in args.cases]
    strategies = tuple(args.strategy or (0, 1))
    if len(set(strategies)) != len(strategies): parser.error('duplicate strategy')
    expected_pins = {}
    for item in args.library_pin:
        try: name, value = item.rsplit('=', 1)
        except ValueError: parser.error('pin must be PATH=SHA256')
        name = str(Path(name).resolve())
        if name in expected_pins or len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
            parser.error('duplicate or invalid library pin')
        expected_pins[name] = value
    dragon = None; before = None; python_source = None
    if not args.oracle_only:
        if len(expected_pins) != 2 or {Path(p).name for p in expected_pins} != {'libdragon.so', 'libdragon_c.so'}:
            parser.error('exact backend and C-shim --library-pin values are required')
        if not args.python_sha256: parser.error('--python-sha256 is required')
        sys.path.insert(0, os.environ.get('DRAGON_TEST_PYTHON', str(ROOT/'python')))
        import dragon
        python_source = dict(path=str(Path(dragon.__file__).resolve()), sha256=file_sha(dragon.__file__))
        if python_source['sha256'] != args.python_sha256:
            raise RuntimeError('loaded Python wrapper differs from --python-sha256')
        before = actual_libraries(dragon, expected_pins)
    import unicorn
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    if (out/'results.json').exists(): parser.error('refuse to overwrite a completed result')
    rows = []; reports = []
    for fixture in selected:
        for strategy in strategies:
            report = dict(case=fixture['id'], strategy=strategy)
            if args.oracle_only:
                report['oracle_only'] = True
            else:
                try: report.update(analyze(dragon, fixture, strategy, args.content))
                except Exception as error: report['error'] = repr(error)
            reports.append(report)
            for index in INDICES:
                row = dict(case=fixture['id'], strategy=strategy, index=index)
                work = out/fixture['id']/('s'+str(strategy))/('%08x' % index)
                try:
                    cpu = reference(fixture, index, work)
                    row.update(cpu_result=str(work/'cpu.json'), complete_CPU_return=True)
                    actual = cpu['actual_edges'][0] if cpu['actual_edges'] else None
                    if args.oracle_only:
                        row['status'] = 'oracle-only'
                    elif 'error' in report:
                        row.update(status='infrastructure-error', error=report['error'])
                    else:
                        published = set(report['published_targets'])
                        checked = ({0x1040, 0x1050} if args.control else published)
                        row.update(status=target_status(checked, actual), actual_indirect_target=actual,
                                   published_targets=sorted(published), checked_targets=sorted(checked))
                except Exception as error:
                    row.update(status='reference-error', error=repr(error), complete_CPU_return=False)
                rows.append(row)
    after = actual_libraries(dragon, expected_pins) if dragon else None
    contracts = []
    for report in reports:
        if args.oracle_only: continue
        f = next(f for f in selected if f['id'] == report['case'])
        if args.baseline: wanted = {0x1040, 0x1050}
        elif args.content != 'default' and not (f['load_before_store'] and args.content == 'trusted-entry1'):
            wanted = {0x1040 if f['load_before_store'] else 0x1060, 0x1050}
        else: wanted = {0x1050, 0x1060} if f['pure_stores'] else set()
        contracts.append(dict(case=f['id'], strategy=report['strategy'], expected_targets=sorted(wanted),
                              ok='error' not in report and set(report['published_targets']) == wanted))
    counts = dict(collections.Counter(row['status'] for row in rows))
    plan_ok = check_keys(rows, reports, selected, strategies, args.oracle_only)
    errors = sum(row['status'] in ('reference-error', 'infrastructure-error') for row in rows)
    wrong = counts.get('wrong-publication', 0)
    expected_control = sum(not f['load_before_store'] for f in selected) * len(strategies)
    controls_ok = (wrong == expected_control and errors == 0) if args.control else None
    success = plan_ok and not errors and all(c['ok'] for c in contracts) and (
        controls_ok if args.control else not wrong)
    oracle_maps = [line for line in Path('/proc/self/maps').read_text().splitlines() if '/libunicorn' in line]
    oracle_paths = {str(Path(line.split()[-1]).resolve()) for line in oracle_maps}
    if file_sha(__file__) != harness_before:
        raise RuntimeError('harness source changed during execution')
    source_names = ['lib/analysis/TableContents.cpp', 'lib/disassemblers/x86/x86Analysis.cpp',
                    'lib/disassemblers/arm/armAnalysis.cpp', 'tests/disassemblers/TestTableContents.cpp']
    source_hashes = {name: file_sha(ROOT/name) for name in source_names if (ROOT/name).is_file()}
    summary = dict(ok=success, baseline=args.baseline, oracle_only=args.oracle_only,
        content=args.content, cases=len(selected), strategies=list(strategies), indices=list(INDICES),
        CPU_executions=len(rows), complete_CPU_returns=sum(r.get('complete_CPU_return', False) for r in rows),
        statuses=counts, wrong_publications=wrong, plan_ok=plan_ok,
        graph_publication_contracts=len(contracts), graph_contract_failures=sum(not c['ok'] for c in contracts),
        safe_unresolved_is_complete_analysis=False, full_native_Guest_comparisons=0,
        control=args.control, control_detected_exact_expected_count=controls_ok,
        skip=0, exclusions=0, normalizations=0,
        source_SHA_observation=source_hashes, source_observation_is_build_attestation=False,
        harness_sha256=harness_before, fixture_sha256=digest(json.dumps(selected, sort_keys=True).encode()),
        python_wrapper=python_source, libraries_before=before, libraries_after=after,
        tools=dict(python=sys.version, executable=sys.executable, host=platform.machine(),
                   unicorn_package=unicorn.__version__, unicorn_engine=unicorn.uc_version(),
                   unicorn_module_sha256=file_sha(unicorn.__file__),
                   unicorn_maps=oracle_maps, unicorn_libraries={p: file_sha(p) for p in oracle_paths}),
        scope='CPU+Graph edge/content checks only; no native LLVM/full-state equivalence claim')
    result = dict(summary=summary, planned_fixtures=selected, reports=reports, rows=rows, contracts=contracts)
    (out/'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
