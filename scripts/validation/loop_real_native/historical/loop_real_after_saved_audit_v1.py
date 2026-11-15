"""Saved-only nominal full-frame/observer/comparator audit; no SDK or Guest calls."""
import argparse
import collections
import json
from pathlib import Path
import sys
sys.dont_write_bytecode=True
import loop_real_after_common as C

def inventory(root):
    root=Path(root);out={}
    for p in sorted(root.rglob('*')):
        C.require(not p.is_symlink(),'saved tree symlink')
        if p.is_file():
            s=p.stat();out[str(p.relative_to(root))]=dict(size=s.st_size,sha256=C.sha(p),
                identity=[s.st_dev,s.st_ino,s.st_mode,s.st_nlink,s.st_mtime_ns,s.st_ctime_ns])
    return out

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--plan-sha256',required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();root=a.root.resolve();out=a.out.resolve();pins={}
    C.require(not out.exists() and not out.is_relative_to(root),'new external audit required')
    before=inventory(root);plan_path=root/'PLAN.json'
    C.require(C.sha(plan_path)==a.plan_sha256,'plan differs')
    plan=json.loads(plan_path.read_text());data,roles=C.transfer(plan['transfer_root'],plan['transfer_ledger_sha256'],pins)
    support=roles['c_header'].parent.parent.parent/'tigress_dispatch_native_before.py'
    C.require(C.sha(support)==C.BEFORE_SUPPORT_SHA,'frozen pure comparator loader differs')
    helper=C.load_module(support,'saved_before_comparator')
    adapter,base=helper.local_adapter(roles['c_header'].parent.parent)
    parse,mutate=helper.comparison_helpers(roles['c_header'].parent.parent,base)
    C.require(len(adapter.fields)==76,'field domain differs')
    results=json.loads((root/'run/RESULTS.json').read_text());guard=json.loads((root/'guarded-run/GUARD.json').read_text())
    C.require(guard['returncode']==0 and guard['failure'] is None and guard['pins_unchanged'],'outer guard failed')
    C.require(results['correct'] and results['complete'] and results['failure'] is None and results['pins_unchanged'],
        'normal actual execution did not pass')
    C.require(results['skipped']==results['undefined_exclusions']==results['normalizations']==0,'comparison domain loosened')
    required={(g,o,i) for g in ['after','observer'] for o in ['O0','O2'] for i in range(16)}
    actual=[(r['phase'],r['host_llvm_level'],r['trial']) for r in results['results']]
    C.require(len(actual)==64 and set(actual)==required,'normal saved domain/count differs')
    indexed={(r['phase'],r['host_llvm_level'],r['trial']):r for r in results['results']}
    controls=[];checked=[]
    for group,level,trial in sorted(required):
        work=root/'run'/group/level;row=indexed[group,level,trial]
        source=roles['initial_frames'][trial].read_bytes();expected_blob=roles['expected_frames'][trial].read_bytes()
        input_path=work/('input-%d.bin'%trial);output_path=work/('output-%d.bin'%trial)
        C.require(input_path.read_bytes()==source and len(source)==66048,'saved initial raw frame differs')
        blob=output_path.read_bytes();actual_frame=helper.decode_actual(blob);expected=parse(expected_blob)
        differences,record,is_root=helper.complete_compare(adapter,expected,actual_frame,blob,expected_blob)
        C.require(not differences and is_root and blob==expected_blob and len(blob)==66136,'saved fullframe mismatch')
        C.require(row['status']=='match' and row['differences']==[] and row['raw_snapshot_equal'] and row['root_return'] and
            row['register_fields_compared']==76 and row['memory_bytes_compared']==65536 and
            row['output_sha256']==C.sha(output_path) and row['input_sha256']==C.sha(input_path) and
            row['exit']==list(record[:7]) and row['budget_left']==record[7],'stored row differs from raw output')
        counts=None
        if group=='observer':
            raw=(work/('output-%d.counts'%trial)).read_bytes()
            C.require(len(raw)==24 and raw[:8]==b'DTLOBS1\0','saved sidecar length/magic/EOF differs')
            counts=list(helper.struct.unpack('<2Q',raw[8:]))
            C.require(counts==[1,0] and row['observer_counts']==counts,'actual observer did not hit exactly once')
        else:C.require(row['observer_counts'] is None,'unobserved row has observer data')
        for field in ['RAX','memory','exit']:
            controls.append(dict(phase=group,host_llvm_level=level,trial=trial,**mutate(adapter,expected,actual_frame,field)))
        checked.append(dict(group=group,level=level,trial=trial,output_sha256=C.sha(output_path),observer_counts=counts))
    key=lambda r:(r['phase'],r['host_llvm_level'],r['trial'],r['field'])
    C.require(sorted(controls,key=key)==sorted(results['controls'],key=key),'saved specified comparator controls differ')
    C.require(results['native_comparisons_by_group']==dict(after=32,observer=32) and
        results['native_attempts_by_group']==dict(after=32,observer=32) and
        results['comparator_controls_by_group']==dict(after=96,observer=96),'stored aggregate differs')
    C.require(all(results[k]==0 for k in ['new_C_runs','new_Unicorn_runs','before_matches_recounted','old_UC16_recounted',
        'lift_runs','product_pass_runs','emitter_runs','NoZ3_duplicate_runs']),'old/new execution counts confused')
    after=inventory(root);C.require(before==after and C.recheck(pins),'saved/input set/identity/hash changed during audit')
    out.mkdir();C.write(out/'SOURCE-FILESET.json',dict(root=str(root),files=before,count=len(before),
        logical_bytes=sum(r['size'] for r in before.values())))
    C.write(out/'REPORT.json',dict(schema='loop-real-after-nominal-saved-audit-v1',passed=True,
        source_root=str(root),source_results=C.pin(root/'run/RESULTS.json'),plan=C.pin(plan_path),
        source_fileset=C.pin(out/'SOURCE-FILESET.json'),normal_saved_checked=dict(after=32,observer=32),
        specified_comparator_controls_recomputed=dict(after=96,observer=96),observer_sidecars_checked=32,
        observer_counts_each=[1,0],raw_bytes=66136,raw_register_bytes=512,register_fields=76,Guest_bytes=65536,
        Exit_bytes=56,budget_bytes=8,unique_inputs=16,source_identity_and_hash_unchanged=True,
        new_Guest_runs=0,new_C_runs=0,new_UC_runs=0,SDK_loads=0,checked=checked,checker=C.pin(Path(__file__))))
    print(json.dumps(dict(root=str(out),report_sha256=C.sha(out/'REPORT.json'),source_fileset_sha256=C.sha(out/'SOURCE-FILESET.json'))))

if __name__=='__main__':main()
