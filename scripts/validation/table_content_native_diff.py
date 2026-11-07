#!/usr/bin/env python3
"""Full-state Guest oracle for table-content contracts and memory barriers.

Reuses function_guest_diff and its V2/static compilers without changing their
comparison/normalization rules. Core fixtures remain in table_content_diff;
additional fixed x64 variants cover alias, CALL, partial cells and a join with
an uninitialized incoming path. Empty Graph publication is an explicit target
boundary, not complete deobfuscation. Trusted callbacks are caller assertions
at the LOAD, never automatically inferred image contents.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

import table_content_diff as core

ROOT = core.ROOT
EXPECTED_CORE_SHA256 = '3159a79960dd2c19ac86b0a68b3936f69d5f560406602ef7dfba49657397d75d'


def all_fixtures():
    result = core.fixtures()
    initializer = bytes.fromhex('b94010000048890c2580100000b95010000048890c2588100000')
    def barrier(name, setup, *, extra=None, table0=0x1060, rdi=None):
        tail = bytearray.fromhex('833e0177008b06ff24c580100000')
        tail[4] = 0x40 - len(setup) - 5
        f = core.make_fixture(name, 'x86_64', (setup + tail).hex(),
                              core.BASE + len(setup) + 7, 8,
                              (0x1040, 0x1050), table0)
        f.update(barrier_case=True, expected_zero_target=table0)
        if rdi is not None: f['initial_rdi'] = rdi
        if extra:
            code = bytearray.fromhex(f['code_hex'])
            for offset, data in extra.items(): code[offset:offset+len(data)] = data
            f['code_hex'] = code.hex()
        return f
    result.append(barrier('x64-unknown-alias', initializer +
                          bytes.fromhex('b96010000048890f'), rdi=core.TABLE))
    call_after = core.BASE + len(initializer) + 5
    call = b'\xe8' + (0x1070-call_after).to_bytes(4, 'little', signed=True)
    callee = bytes.fromhex('b96010000048890c2580100000c3')
    f = barrier('x64-call-barrier', initializer + call, extra={0x70:callee})
    # The compiler receives the actual callee body; no call-effect summary or
    # external model is injected into Graph analysis or the native program.
    f['local_functions'] = [dict(address=0x1070, code_hex=callee.hex())]
    f['expected_stack_writes'] = [dict(address=core.STACK-8, value=call_after, width=8)]
    result.append(f)
    result.append(barrier('x64-partial-cell', bytes.fromhex(
        'c704258010000060100000b95010000048890c2588100000')))
    paths = bytes.fromhex('b95010000048890c258810000085ff740d'
                          'b96010000048890c2580100000')
    result.append(barrier('x64-path-store', paths, rdi=1))
    result.append(barrier('x64-path-no-store', paths, rdi=0, table0=0x1040))
    return result



def restore_fixture_code(fixture, memory, entry):
    # Base Guest fixtures install dummy bytes at observation targets. Here
    # those targets already contain real RETs inside the immutable128B image.
    # Restore the planned initial image before either CPU or native execution;
    # do not change the comparison or normalize any executed state.
    code = bytes.fromhex(fixture['code_hex'])
    memory[entry:entry+len(code)] = code
    assert bytes(memory[entry:entry+len(code)]) == code

def expected_publication(f, content):
    if f.get('barrier_case'): return set()
    if f['pure_stores']: return {0x1050, 0x1060}
    if content == 'default': return set()
    if f['load_before_store']:
        return {0x1040, 0x1050} if content == 'trusted-load' else set()
    return {0x1050, 0x1060}


def expected_target(f, index):
    return (f.get('expected_zero_target', 0x1040 if f['load_before_store'] else 0x1060)
            if index == 0 else 0x1050 if index == 1 else 0x1040)


def expected_final_table(f, index):
    # This oracle independently preserves initial bytes on the early guard.
    if f['load_before_store'] and index >= 2:
        return bytes.fromhex(f['initial_table_hex'])
    return bytes.fromhex(f['final_table_hex'])


def as_guest_fixture(f, content, mode='v2'):
    if mode not in ('v2','static','product'): raise ValueError('unknown Guest mode')
    width = f['entry_size']; table = bytes.fromhex(f['initial_table_hex'])
    target0 = expected_target(f, 0)
    # V2 may dispatch an unknown JMP to1040 already in the root. Static IR
    #always emits Transfer for an unpublished indirect edge, even when its
    #runtime value happens to equal that known direct-guard block address.
    runtime_known = {0x1040} if mode == 'v2' else set()
    missing = sorted({target0, 0x1050} - runtime_known - expected_publication(f, content))
    set_gprs = ({'X1' if f['id'] == 'a64-absolute' else 'X2': core.DATA}
                if f['architecture'] == 'arm64' else {'RSI': core.DATA})
    if 'initial_rdi' in f: set_gprs['RDI'] = f['initial_rdi']
    return dict(id=f['id'], architecture=f['architecture'], code_hex=f['code_hex'],
                stubs=[], undefined_flags=[], set_gprs=set_gprs,
                local_functions=f.get('local_functions', []),
                missing_targets=missing, boundary_kind=1, max_instructions=100,
                memory_values=[dict(address=core.TABLE+i, width=width,
                                    value=int.from_bytes(table[i:i+width], 'little'))
                               for i in range(0, len(table), width)])



def reference_fixture_for_index(fixture, index):
    # A direct guard branch to1040 executes the local RET in every mode.
    # Only the actual indirect path is an external observation boundary.
    selected = dict(fixture)
    if index >= 2:
        selected['missing_targets'] = []
    return selected

def check_plan(rows, reports, ids, trials, summary):
    expected = {(case, opt, trial) for case in ids for opt in ('O0', 'O2') for trial in range(trials)}
    expected_reports = {(case, opt) for case in ids for opt in ('O0', 'O2')}
    keys = [(r.get('case'), r.get('opt'), r.get('trial')) for r in rows]
    rkeys = [(r.get('case'), r.get('opt')) for r in reports]
    failures = []
    if any(type(r.get('trial')) is not int for r in rows):
        failures.append('native trial keys must be strict integers')
    if len(keys) != len(set(keys)) or set(keys) != expected:
        failures.append('native row keys differ from independently requested plan')
    if len(rkeys) != len(set(rkeys)) or set(rkeys) != expected_reports:
        failures.append('Graph report keys differ from independently requested plan')
    if (any(type(summary.get(k)) is not int for k in ('cases','results','trials_per_opt'))
            or summary.get('cases') != len(ids) or summary.get('results') != len(rows) or summary.get('trials_per_opt') != trials):
        failures.append('Guest summary counts disagree with requested plan')
    if summary.get('statuses') != dict(collections.Counter(r.get('status') for r in rows)):
        failures.append('Guest summary statuses disagree with rows')
    for name in ('register_fields_compared', 'memory_bytes_compared', 'undefined_flag_fields'):
        if summary.get(name) != sum(r.get(name, 0) for r in rows):
            failures.append('Guest summary '+name+' disagrees with rows')
    return dict(ok=not failures, failures=failures, expected_rows=len(expected),
                expected_reports=len(expected_reports))


def control_exact(row, control):
    diffs = row.get('differences', [])
    field = 'memory' if control == 'memory' else ('X0' if row['architecture'] == 'arm64' else 'RAX')
    return (row.get('status') == 'different' and len(diffs) == 1 and diffs[0].get('field') == field
            and (control != 'memory' or diffs[0].get('count') == 1))


def self_test():
    import copy
    fs = all_fixtures(); ids = [f['id'] for f in fs]
    rows = [dict(case=case, architecture='x86_64', opt=opt, trial=trial, status='match')
            for case in ids for opt in ('O0', 'O2') for trial in range(5)]
    reports = [dict(case=case, opt=opt) for case in ids for opt in ('O0', 'O2')]
    def summary(rs):
        return dict(cases=len(ids), results=len(rs), trials_per_opt=5,
                    statuses=dict(collections.Counter(r['status'] for r in rs)),
                    register_fields_compared=0, memory_bytes_compared=0, undefined_flag_fields=0)
    checks = dict(unique11=len(ids) == len(set(ids)) == 11,
                  fixed128=all(len(bytes.fromhex(f['code_hex'])) == 128 for f in fs),
                  four_original_byte_hashes=fs[:4] == core.fixtures()[:4],
                  full_stores_published=expected_publication(fs[4], 'default') == {0x1050,0x1060},
                  LOAD_time_not_JMP_time=expected_publication(fs[5], 'trusted-load') == {0x1040,0x1050},
                  all_barriers_unknown=all(not expected_publication(f, 'default') for f in fs[6:]),
                  valid_plan=check_plan(rows, reports, ids, 5, summary(rows))['ok'])
    missing = [r for r in rows if r['case'] != ids[-1]]
    checks['missing_case_rejected'] = not check_plan(missing, reports[:-2], ids, 5, summary(missing))['ok']
    dup = copy.deepcopy(rows); dup[1] = dict(dup[0])
    checks['duplicate_trial_rejected'] = not check_plan(dup, reports, ids, 5, summary(dup))['ok']
    dup = copy.deepcopy(reports); dup[1] = dict(dup[0])
    checks['duplicate_report_rejected'] = not check_plan(rows, dup, ids, 5, summary(rows))['ok']
    bad = copy.deepcopy(rows); bad[0]['trial'] = False
    checks['boolean_trial_rejected'] = not check_plan(bad, reports, ids, 5, summary(bad))['ok']
    checks['immutable_core_source'] = core.file_sha(core.__file__) == EXPECTED_CORE_SHA256
    timing = fs[5]
    checks['guarded_timing_table_unchanged'] = all(expected_final_table(timing, i) == bytes.fromhex(timing['initial_table_hex']) for i in core.INDICES[2:])
    checks['admitted_timing_STORE_checked'] = all(expected_final_table(timing, i) == bytes.fromhex(timing['final_table_hex']) for i in core.INDICES[:2])
    checks['other_STORES_required_on_guard'] = all(expected_final_table(f, i) == bytes.fromhex(f['final_table_hex']) for f in fs[:5] for i in core.INDICES)
    restores=[];outside=[];patched=[]
    for model in fs:
        fixture=as_guest_fixture(model,'default');code=bytes.fromhex(fixture['code_hex'])
        memory=bytearray(b'\xa5'*65536);memory[core.BASE:core.BASE+len(code)]=code
        for target in fixture['missing_targets']:memory[target]=0 if fixture['architecture']=='arm64' else 0x90
        if fixture['missing_targets']:patched.append(bytes(memory[core.BASE:core.BASE+len(code)])!=code)
        untouched=(bytes(memory[:core.BASE]),bytes(memory[core.BASE+len(code):]))
        restore_fixture_code(fixture,memory,core.BASE)
        restores.append(bytes(memory[core.BASE:core.BASE+len(code)])==code)
        outside.append(untouched==(bytes(memory[:core.BASE]),bytes(memory[core.BASE+len(code):])))
    checks['super_dummy_patch_was_real']=all(patched) and len(patched)==10
    checks['planned_code_restored_all11']=all(restores)
    checks['outside_code_not_modified']=all(outside)
    call=next(f for f in fs if f['id']=='x64-call-barrier');callee=call['local_functions'][0]
    checks['actual_callee_inside_planned_image']=bytes.fromhex(call['code_hex'])[callee['address']-core.BASE:callee['address']-core.BASE+len(bytes.fromhex(callee['code_hex']))]==bytes.fromhex(callee['code_hex'])
    timing=fs[5]
    v2=as_guest_fixture(timing,'default','v2');static=as_guest_fixture(timing,'default','static')
    checks['V2_unknown_JMP_known1040_returns']=0x1040 not in reference_fixture_for_index(v2,0)['missing_targets']
    checks['static_unknown_JMP1040_is_boundary']=0x1040 in reference_fixture_for_index(static,0)['missing_targets']
    checks['direct_guard1040_still_returns']=all(not reference_fixture_for_index(static,i)['missing_targets'] for i in core.INDICES[2:])
    published=as_guest_fixture(timing,'trusted-load','static')
    checks['published1040_still_returns']=0x1040 not in reference_fixture_for_index(published,0)['missing_targets']
    checks['ARM_published_targets_not_relaxed']=all(not as_guest_fixture(f,'trusted-load',mode)['missing_targets'] for f in fs[2:4] for mode in ('v2','static','product'))
    checks['product_uses_static_transfer_contract']=as_guest_fixture(timing,'default','product')['missing_targets']==static['missing_targets']
    single = dict(status='different', architecture='x86_64', differences=[dict(field='memory', count=1)])
    checks['single_byte_control'] = control_exact(single, 'memory')
    single['differences'][0]['count'] = 2
    checks['multi_byte_control_rejected'] = not control_exact(single, 'memory')
    return dict(ok=all(checks.values()), checks=checks, oracle_executions=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mode', choices=('v2','static','product'), default='product')
    parser.add_argument('--strategy', type=int, choices=(0,1), default=0)
    parser.add_argument('--suite', choices=('core','extended','barriers'), default='barriers')
    parser.add_argument('--case', action='append', dest='cases')
    parser.add_argument('--content', choices=('default','trusted-entry1','trusted-load'), default='default')
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--library-pin', action='append', default=[])
    parser.add_argument('--python-sha256')
    parser.add_argument('--control', choices=('result','memory'))
    parser.add_argument('--expect-control', action='store_true')
    parser.add_argument('--opt', default='opt-21'); parser.add_argument('--llc', default='llc-21'); parser.add_argument('--cc', default='gcc')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        result = self_test(); print(json.dumps(result, indent=2)); return 0 if result['ok'] else 1
    if args.out is None or args.trials < 5: parser.error('--out and at least5 trials are required')
    if bool(args.control) != args.expect_control: parser.error('control requires matching --expect-control')
    fs = all_fixtures(); selected = fs[:4] if args.suite == 'core' else fs[:6] if args.suite == 'extended' else fs
    if args.cases:
        if len(args.cases) != len(set(args.cases)) or not set(args.cases) <= {f['id'] for f in selected}:
            parser.error('duplicate or unknown case')
        selected = [f for f in selected if f['id'] in args.cases]
    if args.content != 'default' and any(f.get('barrier_case') for f in selected):
        parser.error('barrier negatives require no trusted contents assertion; choose core/extended')
    pins = {}
    for value in args.library_pin:
        try: path, sha = value.rsplit('=',1)
        except ValueError: parser.error('pin must be PATH=SHA256')
        path = str(Path(path).resolve())
        if path in pins or len(sha) != 64 or any(c not in '0123456789abcdef' for c in sha): parser.error('invalid/duplicate pin')
        pins[path] = sha
    if len(pins) != 2 or {Path(p).name for p in pins} != {'libdragon.so','libdragon_c.so'}:
        parser.error('exact dual library pins are required')
    if not args.python_sha256: parser.error('--python-sha256 is required')
    if core.file_sha(core.__file__) != EXPECTED_CORE_SHA256: raise RuntimeError('immutable core source pin mismatch')
    sys.path.insert(0, os.environ.get('DRAGON_TEST_PYTHON', str(ROOT/'python')))
    import dragon as d
    if core.file_sha(d.__file__) != args.python_sha256: raise RuntimeError('Python source pin mismatch')
    before = core.actual_libraries(d, pins)
    import function_guest_diff as guest
    from function_static_guest_diff import StaticAdapter
    assert guest.base.dragon is d
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    if (out/'results.json').exists(): parser.error('refuse overwriting a completed Guest result')
    source_before = {str(p):core.file_sha(p) for p in [Path(__file__),Path(core.__file__),Path(guest.__file__),Path(guest.compile.__file__),Path(guest.base.__file__),Path(sys.modules[StaticAdapter.__module__].__file__)]}
    by_id = {f['id']:f for f in selected}
    guest_fs = {f['id']:as_guest_fixture(f,args.content,args.mode) for f in selected}
    fixture_path = out/'fixtures.json'
    fixture_path.write_text(json.dumps(dict(entry=core.BASE,cases=list(guest_fs.values())),indent=2)+'\n')
    reports=[]; active={}; original_analyze=d.Disassembler.analyze; original_config=d.Configuration.__init__
    def configure(self,*a,**kw):
        kw['strategy']=args.strategy; original_config(self,*a,**kw)
    def analyze(self,code,address=0,*a,**kw):
        if address != core.BASE: return original_analyze(self,code,address,*a,**kw)
        f=by_id[active['case']]; initial=bytes.fromhex(f['initial_table_hex']); calls=[]
        if bytes(code)!=bytes.fromhex(f['code_hex']): raise RuntimeError('compiler altered planned code bytes')
        def image(at,size):
            return initial[at-core.TABLE:at-core.TABLE+size] if core.TABLE<=at and at+size<=core.TABLE+len(initial) else None
        def known(block,op_index,instruction_pc,at,size):
            ok=core.TABLE<=at and at+size<=core.TABLE+len(initial)
            if args.content=='trusted-entry1': ok=ok and at>=core.TABLE+f['entry_size']
            if f['load_before_store']: ok=ok and instruction_pc==f['load_pc']
            data=initial if f['load_before_store'] else bytes.fromhex(f['final_table_hex'])
            supplied=data[at-core.TABLE:at-core.TABLE+size] if ok else None
            calls.append(dict(block=block,op_index=op_index,instruction_pc=instruction_pc,address=at,size=size,supplied=supplied.hex() if supplied is not None else None))
            return supplied
        options=dict(read_memory=image)
        if args.content!='default': options['read_known_memory']=known
        with d.AnalysisHints(**options) as hints:
            graph=original_analyze(self,code,address,hints=hints)
        instruction=graph.get_instruction(f['dispatch'])
        if instruction is None: raise RuntimeError('planned indirect instruction missing')
        actual=set(instruction.branches); wanted=expected_publication(f,args.content)
        reports.append(dict(case=f['id'],opt=active['opt'],published_targets=sorted(actual),expected_targets=sorted(wanted),
                            exact_publication=actual==wanted,content_callback_calls=calls,
                            graph_unlifted={str(pc):list(graph.get_ignil_block(pc).unlifted()) for pc in graph.block_addresses() if graph.get_ignil_block(pc) and graph.get_ignil_block(pc).unlifted()}))
        return graph
    def compile_case(f,*a,**kw):
        active.update(case=f['id'],opt=a[1]); work=a[2]
        result=guest.compile.compile_case(f,*a,**kw,static_analysis=args.mode!='v2',
            product_pipeline=args.mode=='product',function_entry=args.mode!='v2')
        exe=result[0]
        linkage=guest.compile.subprocess.run(['ldd',str(exe)],capture_output=True,text=True)
        (work/'ldd.txt').write_text(linkage.stdout+linkage.stderr)
        if linkage.returncode: raise RuntimeError('ldd failed')
        import re
        paths={str(Path(m).resolve()) for m in re.findall(r'=>\s+(\S+)',linkage.stdout) if '/libdragon' in m}
        if args.mode=='v2':
            if {p:core.file_sha(p) for p in paths}!=pins: raise RuntimeError('native executable links unexpected dual Dragon libraries')
        elif paths: raise RuntimeError('static executable unexpectedly depends on Dragon shared libraries')
        report=next(r for r in reports if r['case']==f['id'] and r['opt']==a[1])
        report.update(executable=dict(path=str(exe),sha256=core.file_sha(exe)),
                      ldd_path=str(work/'ldd.txt'),ldd_sha256=core.file_sha(work/'ldd.txt'),native_Dragon_paths=sorted(paths))
        return result
    parent=guest.Adapter if args.mode=='v2' else StaticAdapter
    class Adapter(parent):
        def initial(self,f,trial,entry):
            regs,memory=super().initial(f,trial,entry)
            restore_fixture_code(f,memory,entry)
            memory[core.DATA:core.DATA+4]=core.INDICES[trial%len(core.INDICES)].to_bytes(4,'little')
            return regs,memory
        def reference(self,f,regs,memory,entry):
            model=by_id[f['id']]; index=int.from_bytes(memory[core.DATA:core.DATA+4],'little')
            observed=reference_fixture_for_index(f,index)
            value=super().reference(observed,regs,memory,entry)
            target=expected_target(model,index)
            record=value[3]
            if target in f['missing_targets'] and index<2:
                reason=3 if args.mode=='v2' else 2
                wanted=(reason,1,model['dispatch'],target,1,0,0)
            else: wanted=(1,3,target,core.SENTINEL,1,0,0)
            if tuple(record)!=wanted: raise RuntimeError('CPU exit premise differs from independent fixture plan: '+repr((record,wanted)))
            code=bytes.fromhex(model['code_hex'])
            if value[1][core.BASE:core.BASE+len(code)]!=code: raise RuntimeError('Unicorn modified analysed code bytes')
            final_table=expected_final_table(model,index)
            if value[1][core.TABLE:core.TABLE+len(final_table)]!=final_table: raise RuntimeError('CPU table effect differs from planned fixture')
            return value
        def compare(self,f,expected,actual,corrupt):
            if args.control=='result':
                regs=bytearray(actual[0]);regs[0]^=1;actual=(bytes(regs),)+actual[1:]
            return super().compare(f,expected,actual,corrupt)
    d.Disassembler.analyze=analyze;d.Configuration.__init__=configure
    argv=sys.argv
    sys.argv=[str(Path(__file__)),'--out',str(out),'--trials',str(args.trials),'--opt',args.opt,'--llc',args.llc,'--cc',args.cc]
    if args.control=='memory': sys.argv.append('--corrupt-output')
    try: guest_rc=guest.main(fixtures_path=fixture_path,compile_factory=compile_case,adapter_factory=Adapter)
    finally:
        d.Disassembler.analyze=original_analyze;d.Configuration.__init__=original_config;sys.argv=argv
    raw=json.loads((out/'results.json').read_text());rows=raw['results'];summary=raw['summary']
    plan=check_plan(rows,reports,list(by_id),args.trials,summary)
    errors=[r for r in rows if r['status'] not in ('match','different')]
    root=lambda r:len(r.get('exit',[]))==7 and r['exit'][0:2]==[1,3] and r['exit'][3:5]==[core.SENTINEL,1]
    precise=[]
    for row in rows:
        if type(row.get('trial')) is not int: precise.append(False);continue
        f=by_id[row['case']];index=core.INDICES[row['trial']%len(core.INDICES)];target=expected_target(f,index)
        if index<2 and target in guest_fs[f['id']]['missing_targets']:
            wanted=[3 if args.mode=='v2' else 2,1,f['dispatch'],target,1,0,0]
        else: wanted=[1,3,target,core.SENTINEL,1,0,0]
        precise.append(row.get('exit')==wanted)
    controls_ok=all(control_exact(r,args.control) for r in rows) if args.control else None
    ok=(plan['ok'] and not errors and all(precise) and all(r['exact_publication'] for r in reports)
        and (controls_ok if args.control else guest_rc==0 and all(r['status']=='match' for r in rows))
        and summary['skipped']==summary['legacy_normalizations']==summary['undefined_flag_fields']==0)
    after=core.actual_libraries(d,pins)
    if any(core.file_sha(p)!=sha for p,sha in source_before.items()): raise RuntimeError('oracle source changed during run')
    import unicorn,shutil
    uc_maps=[line for line in Path('/proc/self/maps').read_text().splitlines() if '/libunicorn' in line]
    uc_paths={str(Path(line.split()[-1]).resolve()) for line in uc_maps}
    oracle_provenance=dict(interpreter=sys.executable,python=sys.version,unicorn_package=unicorn.__version__,
        unicorn_engine=unicorn.uc_version(),unicorn_binding_sha256=core.file_sha(unicorn.__file__),
        current_CPU_oracle_process_maps=uc_maps,unicorn_libraries={p:core.file_sha(p) for p in uc_paths},
        tools={name:dict(path=shutil.which(name),sha256=core.file_sha(shutil.which(name)),version=summary['tools'][name]) for name in (args.opt,args.llc,args.cc)})
    result=dict(ok=ok,mode=args.mode,strategy=args.strategy,content=args.content,suite=args.suite,
        requested_cases=list(by_id),trials=args.trials,plan=plan,rows=len(rows),Guest_oracle_returncode=guest_rc,
        complete_root_matches=sum(root(r) and r['status']=='match' for r in rows),
        precise_boundary_matches=sum(not root(r) and r['status']=='match' and good for r,good in zip(rows,precise)),
        complete_semantic_differences=sum(root(r) and r['status']=='different' for r in rows),
        different=sum(r['status']=='different' for r in rows),infrastructure_or_reference_errors=len(errors),
        exact_exit_contracts=all(precise),exact_Graph_publication_contracts=all(r['exact_publication'] for r in reports),
        control=args.control,exact_single_field_controls=controls_ok,
        libraries_before=before,libraries_after=after,python_sha256=args.python_sha256,
        source_hashes=source_before,oracle_provenance=oracle_provenance,fixture_sha256=core.file_sha(fixture_path),
        oracle_results_sha256=core.file_sha(out/'results.json'),
        exclusions=0,skips=0,normalizations=0,
        scope='All modeled fields/PC/SP/64KiB/seven exit fields. Default unresolved transfers remain explicit boundaries; trusted contents are test input assertions.')
    (out/'native-validation.json').write_text(json.dumps(dict(summary=result,reports=reports,planned_machine_fixtures=selected),indent=2)+'\n')
    print('TABLE CONTENT NATIVE',json.dumps(result,indent=2))
    return 0 if ok else 1


if __name__=='__main__': sys.exit(main())
