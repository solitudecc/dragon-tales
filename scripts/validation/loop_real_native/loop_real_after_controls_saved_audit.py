"""Saved-only verification of exactly four real fallback/observer control classes."""
import argparse
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode=True
import loop_real_after_common as C
from loop_real_after_saved_audit import inventory,check_json_storage

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();root=a.root.resolve();out=a.out.resolve();pins={}
    C.require(not out.exists() and not out.is_relative_to(root),'new external audit required')
    before=inventory(root);p=root/'PLAN.json'
    C.require(C.sha(p)=='99decb96ae1032cf24a2ad37e8563a77ca620ff352a632662559ef2fe4e070e4','controls plan differs')
    plan=json.loads(p.read_text());ledger,roles=C.transfer(plan['transfer_root'],plan['transfer_ledger_sha256'],pins)
    support=roles['c_header'].parent.parent.parent/'tigress_dispatch_native_before.py'
    C.require(C.sha(support)==C.BEFORE_SUPPORT_SHA,'pure comparator loader differs')
    helper=C.load_module(support,'frozen_controls_comparator');compat=roles['c_header'].parent.parent
    adapter,base=helper.local_adapter(compat);parse_nominal,mutate=helper.comparison_helpers(compat,base)
    C.require(len(adapter.fields)==76,'field model differs')
    result=json.loads((root/'run/RESULTS.json').read_text());guard=json.loads((root/'guarded-run/GUARD.json').read_text())
    C.require(result['passed'] and result['complete'] and result['pins_unchanged'] and result['failure'] is None and
        guard['returncode']==0 and guard['failure'] is None and guard['pins_unchanged'],'execution/guard did not pass')
    original=roles['initial_frames'][0].read_bytes();nominal_blob=roles['expected_frames'][0].read_bytes()
    nominal_expected=parse_nominal(nominal_blob);checked=[];recomputed=[]
    rows={(r['case'],r['level'],r['role']):r for r in result['results']}
    C.require(len(rows)==len(result['results'])==12,'saved execution domain differs')
    for item in plan['changed_inputs']:
        source=Path(item['input']['path']).read_bytes()
        delta=[dict(offset=i,space='register' if i<512 else 'Guest',space_offset=i if i<512 else i-512,before=b,after=c)
            for i,(b,c) in enumerate(zip(original,source)) if b!=c]
        C.require(len(source)==66048 and delta==item['byte_deltas'],'changed input delta/EOF differs')
        for level in ['O0','O2']:
            work=root/'run'/item['case']/level;peer_work=work/'ordinary-peer';view_work=work/'observer'
            peer_blob=(peer_work/'output-0.bin').read_bytes();peer=helper.decode_actual(peer_blob)
            actual_blob=(view_work/'output-0.bin').read_bytes();actual=helper.decode_actual(actual_blob)
            C.require((peer_work/'input-0.bin').read_bytes()==source==(view_work/'input-0.bin').read_bytes(),'actual changed inputs differ')
            peer_record=struct.unpack('<8Q',peer[3]);expected=(peer[0],peer[1],peer[2],peer_record[:7])
            differences,record=adapter.compare(dict(undefined_flags=[]),expected,actual,False)
            C.require(not differences and peer_blob==actual_blob and record==peer_record,'complete fallback frame differs')
            raw=(view_work/'output-0.counts').read_bytes();C.require(raw==b'DTLOBS1\0'+struct.pack('<2Q',0,1),'fallback sidecar differs')
            for role,blob in [('ordinary-changed-input',peer_blob),('observer-fallback',actual_blob)]:
                row=rows[item['case'],level,role]
                C.require(row['output_sha256']==C.sha((peer_work if role=='ordinary-changed-input' else view_work)/'output-0.bin') and
                    row['exit_and_budget']==list(record),'stored peer/view record differs from raw')
            C.require(rows[item['case'],level,'observer-fallback']['observer_counts']==[0,1],'stored fallback counts differ')
            if item['case']=='used-table-slot48':
                C.require(record==(2,1,4390,57005,1,0,0,0) and int.from_bytes(peer[0][128:136],'little')==0xdead,
                    'actual used-table transfer differs')
            else:C.require(record==C.ROOT_EXIT,'SP-plus-eight root sentinel result differs')
            for field in ['RAX','memory','exit']:
                recomputed.append(dict(case=item['case'],level=level,**mutate(adapter,expected,actual,field)))
            checked.append(dict(case=item['case'],level=level,ordinary_and_fallback_complete_bytes_equal=True,
                observer_counts=[0,1],exit_and_budget=list(record)))
    for item in plan['faults']:
        for level in ['O0','O2']:
            work=root/'run'/item['case']/level/'execution';blob=(work/'output-0.bin').read_bytes();actual=helper.decode_actual(blob)
            C.require((work/'input-0.bin').read_bytes()==original,'fault initial input differs')
            differences,record,is_root=helper.complete_compare(adapter,nominal_expected,actual,blob,nominal_blob)
            C.require(not differences and is_root and blob==nominal_blob,'runtime observer fault changed full state')
            side=(work/'output-0.counts').read_bytes();hit=item['expected_observer']
            C.require(side==b'DTLOBS1\0'+struct.pack('<2Q',*hit) and hit!=[1,0],'specified observer fault not observed')
            row=rows[item['case'],level,'observer-runtime-fault']
            C.require(row['observer_counts']==hit and row['observer_gate_rejected'] and row['specified_detection'] and
                row['nominal_fullframe_match'] and row['output_sha256']==C.sha(work/'output-0.bin'),'stored fault row differs')
            checked.append(dict(case=item['case'],level=level,nominal_complete_bytes_equal=True,actual_observer=hit,
                nominal_observer_gate=[1,0],specified_fault_detected=True))
    stored=json.loads(json.dumps(recomputed));tuple_arrays=check_json_storage(recomputed,stored)
    C.require(tuple_arrays==8,'Exit tuple JSON storage count differs')
    key=lambda r:(r['case'],r['level'],r['field'])
    C.require(sorted(stored,key=key)==sorted(result['controls'],key=key),'full specified comparator controls differ')
    C.require(result['counts']==plan['expected_counts'] and len(result['attempts'])==12 and
        result['skipped']==result['undefined_exclusions']==result['normalizations']==0,'stored counters/domain differ')
    C.require(all(result[k]==0 for k in ['new_C_runs','new_UC_runs','nominal64_recounted']),'reference counts inflated')
    C.require(before==inventory(root) and C.recheck(pins),'saved/input file set/stat/hash changed during audit')
    out.mkdir();C.write(out/'SOURCE-FILESET.json',dict(root=str(root),files=before,count=len(before),logical_bytes=sum(x['size'] for x in before.values())))
    C.write(out/'REPORT.json',dict(schema='loop-real-four-controls-saved-audit-v1',passed=True,source_results=C.pin(root/'run/RESULTS.json'),
        counts=result['counts'],raw_bytes_per_output=66136,register_fields=76,Guest_bytes=65536,Exit_bytes=56,budget_bytes=8,
        checked=checked,comparator_JSON_tuple_arrays=tuple_arrays,all_other_JSON_types_values_order_unchanged=True,
        Guest_data_normalizations=0,new_Guest_runs=0,new_C_runs=0,new_UC_runs=0,SDK_loads=0,
        source_identity_hash_unchanged=True,checker=C.pin(Path(__file__))))
    print('REPORT',C.sha(out/'REPORT.json'));print('SOURCE-FILESET',C.sha(out/'SOURCE-FILESET.json'))

if __name__=='__main__':main()
