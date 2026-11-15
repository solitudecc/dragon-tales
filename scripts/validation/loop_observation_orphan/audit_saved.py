"""Audit saved isolated orphan/control evidence only; no target execution."""
from pathlib import Path
import hashlib,json,sys
sys.dont_write_bytecode=True
LIMITS=dict(seconds=180,start_free=536870912,running_floor=67108864)
SDK='/home/ubuntu/vmc-runs/loop-image-phi-regions-sdk-20261115-v1/lib/libdragon.so'
Z3='/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13'
def sha(p):
    h=hashlib.sha256()
    with Path(p).open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()
def load(p):return json.loads(Path(p).read_text())
def need(ok,why):
    if not ok:raise RuntimeError(why)
def main():
    root=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve();need(not out.exists(),'new report required')
    results={}
    for mode,expected in [('good',0),('mutant',1)]:
        plan_path=root/'plans'/(mode+'-v2.json');plan=load(plan_path)
        stages={}
        for stage in ['compile','run']:
            folder=root/(stage+'-'+mode);receipt=load(folder/'GUARD.json');inv=load(folder/'INVOCATION.json')
            need(receipt['returncode']==(0 if stage=='compile' else expected),'unexpected target exit')
            need(receipt['failure'] is None and receipt['pins_unchanged'],'guard/pin failure')
            need(receipt['limits']==LIMITS and receipt['elapsed_seconds']<=180,'guard limits differ')
            need(receipt['plan_sha256']==sha(plan_path),'plan receipt differs')
            need(inv['argv']==plan['stages']['compile' if stage=='compile' else 'probe']['argv'],'argv differs')
            need(inv['argv'][:3]==['/usr/bin/taskset','-c','4-7'],'affinity differs')
            for name,h in inv['pins'].items():need(sha(name)==h,'source/tool/runtime pin changed: '+name)
            stages[stage]=receipt
        raw=load(root/('run-'+mode)/'stdout')
        need(raw['schema']=='actual-D-isolated-orphan-coverage-v1' and raw['assertions']==8,'scope/count differs')
        need(raw['guest_native']==0 and raw['installed_Image_execution'] is False and raw['source_inclusion'] is True,'scope misreported')
        need(raw['entry_missing_is_not_this_control'] is True,'orphan control not isolated')
        need(not raw['failures'] if mode=='good' else len(raw['failures'])==2,'failure count differs')
        if mode=='mutant':
            need(set(raw['failures'])=={'unsupported anchor obligation refused','refusal restores exact numeric/phase state'},
                 'fault must reach acceptance/cache publication after skipping only all-node coverage')
        trace=(root/('run-'+mode)/'stderr').read_text()
        init=[line.split('calling init:',1)[1].strip() for line in trace.splitlines() if 'calling init:' in line]
        need([p for p in init if p.endswith('/libdragon.so')]==[SDK],'SDK binding differs')
        need([p for p in init if '/libz3.so' in p]==[Z3],'solver binding differs')
        need('runtime error:' not in trace and 'AddressSanitizer:' not in trace,'sanitizer diagnostic')
        results[mode]=dict(stages=stages,report=raw,executable_sha256=sha(root/('orphan-tests-'+mode)))
    good=(root/'overlay/LLVMImageTableContext.overlay.cpp').read_text()
    bad=(root/'overlay/LLVMImageTableContext.no-all-node-coverage.cpp').read_text()
    original='      if (!work.spend() || !covered) return {};'
    faulty='      if (!work.spend()) return {}; // TEST FAULT: ignore unanchored collected node'
    need(good.count(original)==1 and bad==good.replace(original,faulty,1),'fault changes more than all-node coverage')
    need(sha(root/'overlay/LLVMImageTableContext.overlay.cpp')=='7ad0f4675eb9b678337b49de37785459eb81117cbdaedaff2523f19adfc86af3','actual D overlay changed')
    report=dict(schema='isolated-orphan-saved-audit-v1',correct=True,results=results,
      original_D_source_sha256='2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8',
      source_sha256=sha(root/'source-v1/orphan_coverage_probe.cpp'),
      scope=dict(logical_scenarios=1,good_assertions=8,mutant_assertions=8,mutant_failed_checks=2,
        guest_native=0,C=0,Unicorn=0,installed_Image=False,previous_40_not_recounted=True,
        purpose='complete actual-CFG coverage policy; unreachable orphan is not an old wrong-execution counterexample'),
      preserved_zero_target_plan_v1=True,
      plan_v2_change='clarifies permitted disjoint unit4-7/mainCTest0-3 concurrency only; all source/argv/pins/guards unchanged')
    out.write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    print(json.dumps(dict(report=str(out),sha256=sha(out),correct=True)))
if __name__=='__main__':main()
