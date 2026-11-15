"""Pure saved-evidence audit. Never starts a compiler, Dragon, guest or helper."""
from pathlib import Path
import hashlib
import json
import re
import sys
sys.dont_write_bytecode=True

LIMITS=dict(seconds=180,start_free=536870912,running_floor=67108864)
EXPECTED_SDK='/home/ubuntu/vmc-runs/loop-image-phi-regions-sdk-20261115-v1/lib/libdragon.so'
EXPECTED_Z3='/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13'

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()

def load(path):return json.loads(Path(path).read_text())
def require(value,message):
    if not value:raise RuntimeError(message)

def check_mode(root,mode,expected_exit,expected_assertions):
    plan_path=root/'plans'/(mode+'.json');plan=load(plan_path)
    results={}
    for stage in ['compile','run']:
        directory=root/(stage+'-'+mode)
        guard=load(directory/'GUARD.json')
        require(guard['returncode']==(0 if stage=='compile' else expected_exit),mode+' stage exit differs')
        require(guard['failure'] is None and guard['pins_unchanged'],mode+' guard/pins failed')
        require(guard['limits']==LIMITS and guard['elapsed_seconds']<=180,mode+' limits differ')
        require(guard['plan_sha256']==sha(plan_path),mode+' plan receipt differs')
        invocation=load(directory/'INVOCATION.json')
        require(invocation['argv']==plan['stages']['compile' if stage=='compile' else 'probe']['argv'],mode+' argv differs')
        require(invocation['library_path']==plan['library_path'] and invocation['limits']==LIMITS,mode+' environment differs')
        for filename,wanted in invocation['pins'].items():
            require(sha(filename)==wanted,'live frozen pin changed: '+filename)
        results[stage]=guard
    raw=load(root/('run-'+mode)/'stdout')
    require(raw['schema']=='actual-D-cache-transaction-tests-v1',mode+' schema differs')
    require(raw['source_inclusion'] is True and raw['guest_native']==0 and raw['installed_Image_execution'] is False,mode+' scope differs')
    require(raw['assertions']==expected_assertions,mode+' assertion count differs')
    trace=(root/('run-'+mode)/'stderr').read_text()
    init=[s.split('calling init:',1)[1].strip() for s in trace.splitlines() if 'calling init:' in s]
    require([x for x in init if x.endswith('/libdragon.so')]==[EXPECTED_SDK],mode+' SDK loader not unique')
    require([x for x in init if '/libz3.so' in x]==[EXPECTED_Z3],mode+' solver loader not unique')
    require('runtime error:' not in trace and 'AddressSanitizer:' not in trace,mode+' sanitizer diagnostic')
    results['executable_sha256']=sha(root/('cache-tests-'+mode))
    results['report']=raw
    return results

def main():
    root=Path(sys.argv[1]).resolve()
    out=Path(sys.argv[2]).resolve()
    require(not out.exists(),'new saved audit output required')
    good=check_mode(root,'good',0,40)
    mutant=check_mode(root,'mutant',1,40)
    ordinary=check_mode(root,'ordinary',0,26)
    require(not good['report']['failures'] and not ordinary['report']['failures'],'normal unit failed')
    require(mutant['report']['failures'],'deliberate faulty rollback escaped')
    require(any('entry exact cache/trials' in x for x in mutant['report']['failures']),
            'mutant must be caught by internal exact-state restoration')
    require(any('conditional child cache did not escape' in x for x in mutant['report']['failures']),
            'mutant must expose retained child, not merely a summary difference')
    expected=['child-success-parent-alias-failure','child-success-parent-budget','complete-overwrite',
              'anchor-phase-hostile-conditional-cache','unmasked-L-plus-one','unanchored-arm-SCC']
    rows=good['report']['cases']
    require([r['case'] for r in rows]==expected,'logical scenario set differs')
    for row in rows[:2]:
        require(row['child_publications']==1 and row['nested'] is True,'child did not truly publish nested')
        require(row['parent_result'] is False,'parent unexpectedly succeeded')
        require(row['work_after_failure']>=row['work_at_child']>row['work_before'],'spent work not retained')
        require(row['nodes_after_failure']>0 and row['points_after_failure']>0,'node/point debt missing')
        require(row['subsequent_work']>row['work_after_failure'],'later independent query did not spend original remaining budget')
    require(rows[1]['work_after_failure']==rows[1]['work_at_child'],'budget must stop at exact spent work, without underflow')
    require(rows[2]['value']==7,'later full overwrite not correctly recognized')
    # This compares actual query results/debts, not IR normalization. The
    # callback's trace-only fields necessarily do not exist when it is off.
    trace_fields={'child_publications','nested','work_at_child'}
    common=[{k:v for k,v in row.items() if k not in trace_fields}
            for row in rows if row['case']!='child-success-parent-budget']
    off=[{k:v for k,v in row.items() if k not in trace_fields} for row in ordinary['report']['cases']]
    require(common==off,'publication hook changed a common query result/debt')
    require(ordinary['report']['ordinary_hook_off'] is True,'ordinary mode differs')
    manifest=load(root/'frozen-v1/OVERLAY-MANIFEST.json')
    require(manifest['original_sha256']=='2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8','D source changed')
    require(sha(root/'frozen-v1/LLVMImageTableContext.original.cpp')==manifest['original_sha256'],'original full TU differs')
    require(manifest['recovered_original_byte_equal'] is True,'overlay recovery not certified')
    for name,record in manifest['overlays'].items():
        p=root/'frozen-v1'/name
        require(p.stat().st_size==record['size'] and sha(p)==record['sha256'],'overlay changed')
    # Verify the one intentionally bad body edit independently of generator metadata.
    original=(root/'frozen-v1/LLVMImageTableContext.overlay.cpp').read_text()
    faulty=(root/'frozen-v1/LLVMImageTableContext.no-parent-rollback.cpp').read_text()
    old='      if (!keep) owner.loads.swap(entryLoads);'
    new='      if (!keep && trial.load->getName() != "parent") owner.loads.swap(entryLoads);'
    require(original.count(old)==1 and faulty==original.replace(old,new,1),'mutant exceeds one exact rollback fault')
    for mode in ['good','mutant','ordinary']:
        argv=load(root/'plans'/(mode+'.json'))['stages']['compile']['argv']
        require([x for x in argv if x.endswith('.cpp')]==[str(root/'source-v2/observation_cache_probe.cpp')],
                'compile must have one source-inclusion TU only')
        require(not any(x.endswith(('.o','.a')) or x.startswith('-lLLVM') for x in argv),
                'explicit helper/static LLVM object was linked')
    symbols=(root/'good.symbols.txt').read_text()
    require('observationCacheRun' in symbols,'renamed source Image symbols absent')
    require(' UND ' in symbols and 'getPhiRegions' in symbols and 'PhiConstantGraph' in symbols,'SDK helper imports absent')
    source=(root/'source-v2/observation_cache_probe.cpp')
    report={'schema':'actual-D-cache-saved-independent-audit-v1','correct':True,
      'scope':{'logical_scenarios':6,'positive_internal_assertions':40,'ordinary_repeat_assertions':26,
        'mutant_assertions':40,'mutant_failed_checks':len(mutant['report']['failures']),
        'guest_native':0,'C_or_UC':0,'installed_Image_execution':False,
        'source_inclusion_original_D_body':True},
      'source_sha256':sha(source),'overlay_manifest_sha256':sha(root/'frozen-v1/OVERLAY-MANIFEST.json'),
      'method_hashes':manifest['methods'],'SDK':EXPECTED_SDK,
      'sdk_sha256':'b3483229d516183c0a323319fa4d7b6c407fa1c33f1fae0b7ca6c79560431291',
      'good':good,'mutant':mutant,'ordinary':ordinary,
      'same_query_hook_off_values_and_debts_equal':True,
      'v1_compile_failure_preserved':'/home/ubuntu/vmc-runs/loop-observation-cache-transactions-20261115-v1',
      'not_claimed':['Guest semantic equivalence','installed Image API execution','full real CFF closure']}
    out.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    print(json.dumps({'report':str(out),'sha256':sha(out),'correct':True,
                      'mutant_failed_checks':len(mutant['report']['failures'])}))

if __name__=='__main__':main()
