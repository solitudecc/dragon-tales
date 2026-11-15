"""Prepare exactly two observer faults and two changed-input fallback controls."""
import argparse
import json
from pathlib import Path
import re
import sys
sys.dont_write_bytecode=True
import loop_real_after_common as C

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--nominal-root',type=Path,required=True)
    ap.add_argument('--source-root',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();nominal=a.nominal_root.resolve();out=a.out.resolve();pins={}
    C.require(not out.exists(),'new controls namespace required')
    p=nominal/'PLAN.json';C.require(C.sha(p)=='8d31e3abcafb78909d312d432c8c04e4bfee0892588a2cd8513ed32facfaa206','nominal plan differs')
    source=a.source_root.resolve();plan=json.loads(p.read_text());ledger,roles=C.transfer(plan['transfer_root'],plan['transfer_ledger_sha256'],pins)
    result=json.loads((nominal/'run/RESULTS.json').read_text())
    C.require(result['correct'] and result['complete'] and result['native_comparisons_by_group']==dict(after=32,observer=32),
        'nominal calibration incomplete')
    for path in [p,nominal/'run/RESULTS.json',nominal/'guarded-run/GUARD.json']:
        C.pinned(C.pin(path),pins)
    before_root=Path('/home/ubuntu/vmc-runs/loop-image-real-native-before-20261115-v1')
    before_set=before_root/'FILESET.json'
    C.require(C.sha(before_set)=='f7af3afb930f78f37383508f57b39d5690ad488edd1a925b6e9ab07cc011476c','original before file set differs')
    C.pinned(C.pin(before_set),pins);old_files={r['path']:r for r in json.loads(before_set.read_text())['files']}
    ordinary={};observer={}
    for level in ['O0','O2']:
        rel='run/'+level+'/native';old=old_files[rel]
        ordinary[level]=C.pin(before_root/rel)
        C.require(ordinary[level]['sha256']==old['sha256'] and ordinary[level]['size']==old['size'],'original before executable differs')
        observer[level]=C.pin(nominal/'run/observer'/level/'native')
        C.pinned(ordinary[level],pins);C.pinned(observer[level],pins)
    raw=C.pinned(plan['bindings']['observer_IR'],pins).read_text()
    emission=json.loads(C.pinned(plan['bindings']['emission_report'],pins).read_text())
    accepted=emission['observation']['accepted_after'];C.require(accepted in raw,'accepted observer block differs')
    role_records=[r for r in plan['source_pins'] if Path(r['path']).name=='ACTUAL-ROLES.json']
    C.require(len(role_records)==1,'actual roles source is ambiguous')
    roles_path=C.pinned(role_records[0],pins)
    actual_roles=json.loads(roles_path.read_text());C.pinned(C.pin(roles_path),pins)
    view=actual_roles['view'];kernel=actual_roles['ordinary_kernel']
    old_call='call void @'+view+'(';new_call='call void @'+kernel+'('
    C.require(raw.count(old_call)==1,'unique wrapper direct call absent')
    omitted=raw.replace(old_call,new_call)
    # Verify reversal at the exact replaced byte offset; other ordinary calls remain untouched.
    at=raw.index(old_call)
    C.require(omitted[:at]+old_call+omitted[at+len(new_call):]==raw,'fault changed more than one callee operand')
    old_add='%native.observe.accepted.next = add i64 %native.observe.accepted.old, 1'
    new_add='%native.observe.accepted.next = add i64 %native.observe.accepted.old, 2'
    C.require(raw.count(old_add)==1 and old_add in accepted,'accepted increment is ambiguous')
    doubled=raw.replace(old_add,new_add);C.require(doubled.replace(new_add,old_add)==raw,'counter fault changed extra bytes')
    original=roles['initial_frames'][0].read_bytes();C.require(len(original)==66048,'initial frame length differs')
    memory=original[512:];table_address=8192+48*8;table_old=int.from_bytes(memory[table_address:table_address+8],'little')
    C.require(table_old==0x1741,'slot48 original target differs')
    guard_pattern=r'getelementptr i8, ptr %[-\w.]+, i64 '+str(table_address)+r'\n  (%[-\w.]+) = load i64, ptr %[-\w.]+, align 1\n  %[-\w.]+ = icmp eq i64 \1, '+str(table_old)
    C.require(re.search(guard_pattern,raw),'slot48 is not an actual full-word equality guard')
    table=bytearray(original);table[512+table_address:512+table_address+8]=(0xdead).to_bytes(8,'little')
    sp=int.from_bytes(original[32:40],'little');sentinel=memory[sp:sp+8]
    C.require(sp==0xc808 and int.from_bytes(sentinel,'little')==61440,'wrapper SP/sentinel differs')
    shifted=bytearray(original);shifted[32:40]=(sp+8).to_bytes(8,'little');shifted[512+sp+8:512+sp+16]=sentinel
    C.require(shifted[512+sp:512+sp+8]==sentinel,'original sentinel was removed')
    out.mkdir();(out/'frozen').mkdir()
    faults=[]
    for name,text,counts in [('omitted-wrapper-binding',omitted,[0,0]),('accepted-plus-two',doubled,[2,0])]:
        path=out/'frozen'/(name+'.ll');path.write_text(text)
        C.require(text.splitlines()[0]==raw.splitlines()[0],'fault ModuleID changed')
        faults.append(dict(case=name,IR=C.pin(path),expected_observer=counts,
            nominal_fullframe_must_match=True,observer_1_0_gate_must_reject=True))
    changed=[]
    for name,blob in [('used-table-slot48',bytes(table)),('SP-plus-eight',bytes(shifted))]:
        path=out/'frozen'/(name+'.bin');path.write_bytes(blob)
        delta=[dict(offset=i,space='register' if i<512 else 'Guest',space_offset=i if i<512 else i-512,
                    before=b,after=c) for i,(b,c) in enumerate(zip(original,blob)) if b!=c]
        C.require(delta,'changed input is identical')
        changed.append(dict(case=name,input=C.pin(path),byte_deltas=delta,expected_observer=[0,1],
            reference='same changed input on original before LLVM executable, separately for O0/O2',
            nominal_ROOT_EXIT_required=False))
    source_pins=[C.pin(source/f) for f in ['loop_real_after_controls.py','loop_real_after_common.py','loop_real_after_controls_plan.py']]
    for item in source_pins:C.pinned(item,pins)
    for item in [plan['python'],plan['reference_environment'],plan['runtime']['backend'],plan['runtime']['shim'],
                 plan['solver']['library'],*plan['tools'].values()]:C.pinned(item,pins,True)
    for item in plan['bindings'].values():
        if isinstance(item,dict):C.pinned(item,pins)
    for item in faults:C.pinned(item['IR'],pins)
    for item in changed:C.pinned(item['input'],pins)
    final=dict(schema='loop-real-four-controls-plan-v1',ready=True,limits=C.LIMITS,allowed_cpus=[0,1,2,3],
        host_llvm_levels=['O0','O2'],transfer_root=plan['transfer_root'],transfer_ledger_sha256=plan['transfer_ledger_sha256'],
        nominal_root=str(nominal),nominal_plan=C.pin(p),python=plan['python'],reference_environment=plan['reference_environment'],
        runtime=plan['runtime'],solver=plan['solver'],tools=plan['tools'],bindings=plan['bindings'],sources=source_pins,
        ordinary_executables=ordinary,observer_executables=observer,faults=faults,changed_inputs=changed,
        source_frame=C.pin(roles['initial_frames'][0]),nominal_expected=C.pin(roles['expected_frames'][0]),
        target_triple=plan['target_triple'],new_C_runs=0,new_UC_runs=0,nominal64_recounted=0,
        expected_counts=dict(ordinary_changed_input_references=4,observer_fallback_fullframe_matches=4,
            runtime_observer_fault_observations=4,specified_observer_fault_detections=4,comparator_controls=12),
        SP_delta=dict(register_offset=32,old=sp,new=sp+8,sentinel_source=sp,sentinel_destination=sp+8,
            sentinel_value=61440,old_sentinel_retained=True),table_delta=dict(base=8192,slot=48,width=8,address=table_address,old=table_old,new=0xdead))
    C.require(C.recheck(pins),'preparation pins changed');C.write(out/'PLAN.json',final)
    pins[str(out/'PLAN.json')]=C.sha(out/'PLAN.json')
    guard=Path('/home/ubuntu/vmc-runs/loop-real-native-emission-source-20261115-v1/guard.py')
    C.require(C.sha(guard)=='d0452de8980c041803db571d1555fbdb63c8d2234ebaa81875f5f2125dbc7cad','fixed guard differs')
    pins[str(guard)]=C.sha(guard)
    command=['/usr/bin/taskset','-c','0-3',plan['python']['path'],str(source/'loop_real_after_controls.py'),
        '--plan',str(out/'PLAN.json'),'--plan-sha256',C.sha(out/'PLAN.json'),'--out',str(out/'run')]
    C.write(out/'GUARD-PLAN.json',dict(schema='dragon-tigress-context-guard-plan-v1',
        limits=dict(seconds=180,start_free=536870912,running_floor=67108864),pins={k:v['sha256'] if isinstance(v,dict) else v for k,v in pins.items()},
        stages=dict(probe=dict(argv=command)),library_path=plan['runtime']['library_path'],
        executable=str(Path(plan['python']['path']).resolve()),require_maps=True))
    C.write(out/'EXECUTION.json',dict(plan=C.pin(out/'PLAN.json'),guard_plan=C.pin(out/'GUARD-PLAN.json'),
        argv=['/usr/bin/python3.10',str(guard),'--plan',str(out/'GUARD-PLAN.json'),'--sha256',C.sha(out/'GUARD-PLAN.json'),
            '--stage','probe','--out',str(out/'guarded-run')],environment=dict(PYTHONDONTWRITEBYTECODE='1',
            DRAGON_C_LIB=plan['runtime']['shim']['path'],LD_LIBRARY_PATH=':'.join(plan['runtime']['library_path']),LD_PRELOAD='',LIBUNICORN_PATH=''),
        execution_started=False,metadata_only=True))
    print(json.dumps(dict(root=str(out),plan_sha256=C.sha(out/'PLAN.json'),guard_plan_sha256=C.sha(out/'GUARD-PLAN.json'),execution_sha256=C.sha(out/'EXECUTION.json'))))

if __name__=='__main__':main()
