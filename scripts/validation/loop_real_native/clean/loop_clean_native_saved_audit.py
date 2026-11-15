"""Pure saved32 clean frames and96 exact comparator controls; no SDK/Guest calls."""
import argparse
import json
from pathlib import Path
import sys
sys.dont_write_bytecode=True
import loop_real_after_common as C
from loop_real_after_saved_audit import inventory,check_json_storage

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--root',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();root=a.root.resolve();out=a.out.resolve();pins={};C.require(not out.exists() and not out.is_relative_to(root),'new external audit required')
    before=inventory(root);plan_file=root/'NATIVE-PLAN.json';C.require(C.sha(plan_file)=='518e649948a345b84ebbf3672460d7665c724b9ff2ac573f95797ebbc6368c67','clean plan differs')
    plan=json.loads(plan_file.read_text());ledger,roles=C.transfer(plan['transfer_root'],plan['transfer_ledger_sha256'],pins)
    support=roles['c_header'].parent.parent.parent/'tigress_dispatch_native_before.py';C.require(C.sha(support)==C.BEFORE_SUPPORT_SHA,'pure reference loader differs')
    helper=C.load_module(support,'saved_clean_reference');compat=roles['c_header'].parent.parent
    adapter,base=helper.local_adapter(compat);expected,mutate=helper.comparison_helpers(compat,base);C.require(len(adapter.fields)==76,'field model differs')
    results=json.loads((root/'native/RESULTS.json').read_text());C.require(results['correct'] and results['complete'] and
        results['failure'] is None and results['pins_unchanged'] and results['native_attempts']==results['native_comparisons']==32 and
        results['comparator_controls']==96 and not results['new_observer_evidence'],'actual clean execution did not pass')
    for stage in ['compile','unit','emit','native']:
        guard=json.loads((root/('guard-'+stage)/'GUARD.json').read_text());C.require(guard['returncode']==0 and guard['failure'] is None and guard['pins_unchanged'],'stage guard failed')
    unit=json.loads((root/'guard-unit/stdout').read_text());C.require(unit['checks']==9 and unit['passed'] and unit['synthetic_only'] and unit['observer_calls']==0,'synthetic binder check failed')
    emission=json.loads((root/'binding/REPORT.json').read_text());C.require(emission['binding_only'] and emission['observer_calls']==0 and
        emission['whole_module_verified'] and emission['production_input_unchanged'],'binding-only emission failed')
    parsed=(root/'binding/production.parsed-before.ll').read_text();bound=(root/'binding/native-adapter.ll').read_text()
    old=emission['binding']['call_before'];new=emission['binding']['call_after']
    C.require(parsed.count(old)==1 and parsed.replace(old,new)==bound,'complete bound module is not exact one-call textual delta')
    C.require(C.sha(root/'binding/native-adapter.ll')==plan['module']['sha256']==emission['native_adapter_sha256'],'bound module SHA differs')
    C.require('@dragon_loop_native_accepted' not in bound and '@dragon_loop_native_fallback' not in bound and
        not list((root/'native').rglob('*.counts')) and not(root/'binding/native-observer.ll').exists(),'unexpected clean observer evidence')
    required={(level,i) for level in ['O0','O2'] for i in range(16)}
    indexed={(r['level'],r['trial']):r for r in results['results']};C.require(len(indexed)==len(results['results'])==32 and set(indexed)==required,'saved domain differs')
    recomputed=[];checked=[]
    for level,trial in sorted(required):
        work=root/'native'/level;blob=(work/('output-%d.bin'%trial)).read_bytes();actual=helper.decode_actual(blob)
        eb=roles['expected_frames'][trial].read_bytes();ib=roles['initial_frames'][trial].read_bytes();want=expected(eb)
        C.require((work/('input-%d.bin'%trial)).read_bytes()==ib,'saved input differs')
        differences,record,is_root=helper.complete_compare(adapter,want,actual,blob,eb)
        C.require(not differences and is_root and blob==eb,'complete saved clean frame differs')
        row=indexed[level,trial];C.require(row['status']=='match' and row['differences']==[] and row['raw_snapshot_equal'] and row['root_return'] and
            row['exit_and_budget']==list(record) and row['output_sha256']==C.sha(work/('output-%d.bin'%trial)) and
            row['input_sha256']==C.sha(work/('input-%d.bin'%trial)) and row['register_fields']==76,'stored result differs from raw')
        for field in ['RAX','memory','exit']:recomputed.append(dict(level=level,trial=trial,**mutate(adapter,want,actual,field)))
        checked.append(dict(level=level,trial=trial,full_frame_equal=True,output_sha256=C.sha(work/('output-%d.bin'%trial))))
    serialized=json.loads(json.dumps(recomputed));tuples=check_json_storage(recomputed,serialized);C.require(tuples==64,'Exit tuple JSON shape differs')
    key=lambda r:(r['level'],r['trial'],r['field']);C.require(sorted(serialized,key=key)==sorted(results['controls'],key=key),'full saved comparator controls differ')
    C.require(all(results[k]==0 for k in ['new_C_runs','new_UC_runs','previous76_recounted','skipped','undefined_exclusions','normalizations']),'scope/count inflation')
    C.require(before==inventory(root) and C.recheck(pins),'source/reference identity or hash changed')
    out.mkdir();C.write(out/'SOURCE-FILESET.json',dict(root=str(root),files=before,count=len(before),logical_bytes=sum(x['size'] for x in before.values())))
    C.write(out/'REPORT.json',dict(schema='loop-clean-native-saved-audit-v1',passed=True,source_results=C.pin(root/'native/RESULTS.json'),
        complete_saved_inputs_checked=32,complete_saved_outputs_checked=32,comparator_controls_recomputed=96,raw_bytes=66136,register_fields=76,
        register_raw_bytes=512,Guest_bytes=65536,Exit_bytes=56,budget_bytes=8,unique_inputs=16,new_observer_evidence=False,
        new_Guest_runs=0,new_C_runs=0,new_UC_runs=0,SDK_loads=0,previous76_recounted=0,Guest_normalizations=0,
        only_JSON_storage_type_changes_are_Exit_tuple_arrays=tuples,source_identity_hash_unchanged=True,
        exact_complete_module_single_callee_delta=True,checked=checked,checker=C.pin(Path(__file__))))
    print('REPORT',C.sha(out/'REPORT.json'));print('SOURCE-FILESET',C.sha(out/'SOURCE-FILESET.json'))

if __name__=='__main__':main()
