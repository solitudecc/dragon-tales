"""Saved-only exact source/fixture/runtime and budget-policy regression audit."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1048576),b''):h.update(block)
    return h.hexdigest()

def require(value,why):
    if not value:raise RuntimeError(why)

def main():
    root=Path(sys.argv[1]).resolve()
    require(not (root/'SAVED-AUDIT.json').exists(),'new audit only')
    ready=json.loads((root/'READY.json').read_text())
    manifests=json.loads((root/'frozen-v1/OVERLAY-MANIFEST.json').read_text())
    expected_failures={
      'b6':['shared literal positive range','same-SSA HC positive range',
            'different candidate immutable H maps not reused','repeated same opaque root fits whole root budget'],
      'df':['repeated same opaque root fits whole root budget'],'opaque':[]}
    expected_assertions={'b6':21,'df':32,'opaque':36}
    evidence={}
    common_fixtures=None
    require(len(ready['payload'])>10,'complete source payload')
    for relative,pin in ready['payload'].items():
        path=root/relative
        require(path.is_file() and not path.is_symlink() and path.stat().st_size==pin['size'] and sha(path)==pin['sha256'],
                'frozen payload differs '+relative)
    for mode in ['b6','df','opaque']:
        plan_path=Path(ready['plans'][mode]['path'])
        require(sha(plan_path)==ready['plans'][mode]['sha256'],'plan pin')
        plan=json.loads(plan_path.read_text())
        for path,wanted in plan['pins'].items():require(sha(path)==wanted,'input/tool/runtime changed '+path)
        reports={}
        for stage in ['compile','probe']:
            out=root/'stages'/('opaque-probe-v2' if mode=='opaque' and stage=='probe' else mode+'-'+stage)
            guard=json.loads((out/'GUARD.json').read_text())
            invocation=json.loads((out/'INVOCATION.json').read_text())
            require(guard['failure'] is None and guard['pins_unchanged'] and
                    guard['limits']==dict(seconds=180,start_free=536870912,running_floor=67108864),'guard failure')
            require(guard['plan_sha256']==sha(plan_path)==invocation['plan_sha256'],'invocation plan')
            require(invocation['argv']==plan['stages'][stage]['argv'],'actual command differs')
            require(guard['returncode']==(0 if stage=='compile' else ready['plans'][mode]['expected_probe_exit']),
                    'raw stage exit differs')
            reports[stage]=dict(guard_sha256=sha(out/'GUARD.json'),invocation_sha256=sha(out/'INVOCATION.json'),
                                elapsed_seconds=guard['elapsed_seconds'],returncode=guard['returncode'])
        out=root/'stages'/('opaque-probe-v2' if mode=='opaque' else mode+'-probe')
        result=json.loads((out/'stdout').read_text())
        require(result['source_variant']==mode and result['source_inclusion'] and
                not result['installed_Image_execution'] and result['guest_native']==result['C_or_UC']==0,'scope differs')
        require(result['failures']==expected_failures[mode] and result['assertions']==expected_assertions[mode],
                'actual assertion/domain mismatch '+mode)
        cases={x['case']:x for x in result['cases']}
        require(len(cases)==len(result['cases']),'duplicate reported case')
        for name in ['198-work-reject','known-depth33-reject','distinct-HC-keys-reject',
                     'nonknown-reuse-reject','nested-store-no-reset','distinct-opaque-roots-reject']:
            require(not cases[name]['present'] and cases[name]['budget_exhausted'],'negative budget control '+name)
        require(cases['known-depth33-reject']['work']==2,'paid leaf still charged depth failure')
        require(cases['nested-store-no-reset']['nodes']>128,'nested debt was not preserved')
        if mode!='b6':
            for name,low,high,nodes,bits in [
              ('literal-first',40,80,121,32),('literal-second',40,80,121,32),('literal-write-root',40,80,122,64),
              ('HC-first',40,80,121,32),('HC-second',40,80,121,32),('HC-write-root',40,80,122,64),
              ('independent-HC-candidate',280,360,121,32)]:
                c=cases[name]
                require(c['present'] and not c['budget_exhausted'] and
                        (c['low'],c['high'],c['nodes'],c['bits'])==(low,high,nodes,bits),'known-leaf exact accounting '+name)
            require(cases['198-work-reject']['work']==198,'node credit must not spare visit Work')
        if mode=='opaque':
            for name,nodes,bits in [('same-opaque-first',67,32),('same-opaque-second',67,32),('same-opaque-write-root',68,64)]:
                c=cases[name]
                require(c['present'] and not c['budget_exhausted'] and
                        (c['low'],c['high'],c['nodes'],c['bits'])==(0,140,nodes,bits),'opaque exact accounting '+name)
        else:require(not cases['same-opaque-first']['present'],'old opaque positive must really fail')
        fixture_root=root/('fixtures-'+mode)
        fixtures={p.name:dict(size=p.stat().st_size,sha256=sha(p)) for p in fixture_root.iterdir() if p.is_file()}
        require(len(fixtures)==9 and all(p.suffix=='.ll' and not p.is_symlink() for p in fixture_root.iterdir()),'complete nine raw fixtures')
        if common_fixtures is None:common_fixtures=fixtures
        else:require(fixtures==common_fixtures,'old/new actual fixture bytes differ')
        maps=(out/'process.maps').read_text()
        paths={line.split()[-1] for line in maps.splitlines() if '/' in line}
        dragons={p for p in paths if p.endswith('/libdragon.so')}
        solvers={p for p in paths if '/libz3.so' in p}
        require(dragons=={plan['library_path'][0]+'/libdragon.so'} and
                solvers=={plan['library_path'][1]+'/libz3.so.4.13'},'actual mapping mismatch')
        debug=(out/'stderr').read_text()
        inits=set(re.findall(r'calling init:\s*(\S+)',debug))
        require(plan['library_path'][0]+'/libdragon.so' in inits and
                plan['library_path'][1]+'/libz3.so.4.13' in inits,'actual LD init missing')
        exe=Path(plan['executable'])
        symbols=subprocess.run(['/usr/bin/nm','-C',str(exe)],capture_output=True,text=True,check=True).stdout
        dynamic=subprocess.run(['/usr/bin/readelf','-d',str(exe)],capture_output=True,text=True,check=True).stdout
        require(' T dragon::llvmlifter::detail::budgetProbeRunLoopImageTableContext(' in symbols,
                'actual Image implementation must be defined by source inclusion')
        for helper in ['OpaqueIntegerAdmission::getPhiRegions(', 'PhiConstantGraph::get(',
                       'replacePhiLowTruncs(', 'threadDispatchCorridorBatch(']:
            require(any(' U ' in line and helper in line for line in symbols.splitlines()),'helper must remain imported '+helper)
        require(plan['library_path'][0] in dynamic and 'libdragon.so' in dynamic,'explicit runtime dynamic binding')
        (root/(mode+'-symbols.txt')).write_text(symbols)
        (root/(mode+'-dynamic.txt')).write_text(dynamic)
        evidence[mode]=dict(assertions=result['assertions'],failed_assertions=len(result['failures']),
                           failures=result['failures'],cases=result['cases'],stages=reports,
                           executable_sha256=sha(exe),source_sha256=ready['sources'][mode],
                           fixture_set=fixtures,actual_libs=sorted(dragons|solvers),
                           results_sha256=sha(out/'stdout'))
    prep=json.loads((root/'PREPARATION-FAILURE-v1.json').read_text())
    require(prep['unit_started'] is False and prep['API_calls']==0 and prep['directory_members']==[],
            'zero-unit preflight failure must remain distinct')
    require(list((root/'stages/opaque-probe').iterdir())==[], 'failed preflight namespace was overwritten')
    audit=dict(schema='original-LoopRanges-budget-policy-saved-audit-v1',correct=True,
               source_inclusion_only=True,installed_Image_execution=False,guest_native=0,C_or_UC=0,
               original_sources_and_all_input_pins_unchanged=True,all_nine_fixture_bytes_equal=True,
               results=evidence,ready_sha256=sha(root/'READY.json'),
               preparation_failure=dict(path=str(root/'PREPARATION-FAILURE-v1.json'),
                   sha256=sha(root/'PREPARATION-FAILURE-v1.json'),unit_started=False,API_calls=0,
                   classified_as='controller dependency preflight failure, not product failure or skip'),
               note='Old source failures are precision controls, not wrong guest execution. Accounting model changes only; actual source/IR safety controls remain. No claim on later430 source or full real closure.')
    path=root/'SAVED-AUDIT.json';path.write_text(json.dumps(audit,indent=2,sort_keys=True)+'\n')
    print(json.dumps(dict(path=str(path),sha256=sha(path),correct=True),indent=2))

if __name__=='__main__':main()
