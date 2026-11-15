"""Seal real nominal replay metadata after successful mechanical emission.

No compiler, LLVM pass, reference or Guest is executed. The resulting plan
still requires the parent's separate execution GO.
"""
import argparse
import json
from pathlib import Path
import platform
import sys

sys.dont_write_bytecode=True
import loop_real_after_common as C

LEDGER_SHA='8674465843ef192335054e7220914e980874f1d4fa659baaecc60631fd032480'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--transfer-root',type=Path,required=True)
    ap.add_argument('--emission-root',type=Path,required=True)
    ap.add_argument('--source-root',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();out=a.out.resolve();pins={}
    C.require(not out.exists(),'new plan namespace required')
    transfer,roles=C.transfer(a.transfer_root,LEDGER_SHA,pins)
    before=json.loads(roles['before_ready_plan'].read_text())
    C.require(before['limits']==C.LIMITS and before['host_llvm_levels']==['O0','O2'],'before limits/levels differ')
    emission=a.emission_root.resolve();record=emission/'emission/adapter/REPORT.json'
    report=json.loads(record.read_text());driver_report=json.loads((emission/'emission/driver/REPORT.json').read_text())
    C.require(report['input_sha256']==C.AFTER_SHA and report['production_input_unchanged'] and report['whole_modules_verified'] and
       report['binding']['retargeted_direct_calls']==1 and report['binding']['only_one_callee_operand_changed'] and
       report['binding']['ordinary_kernel_unchanged'] and report['binding']['view_unchanged'] and
       report['observation']['ordinary_fallback_call_unchanged'] and report['observation']['other_functions_unchanged'],
       'real emission gates incomplete')
    C.require(driver_report['original_driver_sha256']==C.DRIVER_SHA and driver_report['reverse_difference_reproduces_original'] and
              driver_report['ordinary1000_initial_call_unchanged'],'driver emission changed more than allowed routes')
    artifact={
      'emission_report':record,'after_IR':emission/'emission/adapter/native-adapter.ll',
      'observer_IR':emission/'emission/adapter/native-observer.ll','driver':emission/'emission/driver/driver.adapter.c',
      'observer_helper':emission/'emission/driver/observer-sidecar.c'}
    C.require(C.sha(artifact['after_IR'])==report['native_adapter_sha256'] and
              C.sha(artifact['observer_IR'])==report['native_observer_sha256'],'actual emitted IR differs')
    for name in ['driver.adapter.c','observer-sidecar.c']:
        C.require(C.sha(emission/'emission/driver'/name)==driver_report['payload'][name]['sha256'],'actual emitted driver/helper differs')
    for name in ['adapter','driver']:
        guard=json.loads((emission/'stages'/(name+'-emit')/'GUARD.json').read_text())
        C.require(guard['returncode']==0 and guard['failure'] is None and guard['pins_unchanged'],'emission guard failed')
    source=a.source_root.resolve()
    sources={name:C.pin(source/file) for name,file in [('runner','loop_real_after_replay.py'),('common','loop_real_after_common.py')]}
    source_pins=[C.pin(source/'loop_real_after_nominal_plan.py')]
    for path in [emission/'EMIT-READY.json',emission/'ACTUAL-ROLES.json',emission/'TRANSFER-CHECK.json',
                 emission/'emission/driver/REPORT.json',emission/'source-v1/loop_real_native_adapter.cpp',
                 emission/'source-v1/loop_real_native_driver.py']:
        source_pins.append(C.pin(path))
    for name in ['adapter','driver']:
        source_pins.extend(C.pin(emission/'stages'/(name+'-emit')/file) for file in ['GUARD.json','INVOCATION.json'])
    runtime=dict(backend=transfer['external_references']['main_runtime'],shim=transfer['external_references']['main_shim'],
                 library_path=[str(Path(transfer['external_references']['main_runtime']['path']).parent),
                               str(Path(before['solver']['library']['path']).parent)])
    for name in ['SDK_manifest','NoZ3_runtime','NoZ3_shim']:
        item=transfer['external_references'][name]
        source_pins.append(dict(path=item['path'],sha256=item['sha256'],**({'size':item['size']} if 'size' in item else {})))
    python=before['python'];solver=before['solver'];reference=before['reference_environment'];tools=dict(before['tools'])
    tools['readelf']=C.pin(Path('/usr/bin/readelf'))
    inputs=[]
    for i in range(16):
        item=before['inputs'][i]
        C.require(item['trial']==i and item['value']==C.VALUES[i] and
                  item['input']['sha256']==C.sha(roles['initial_frames'][i]) and
                  item['expected']['sha256']==C.sha(roles['expected_frames'][i]),'before/current original input mapping differs')
        inputs.append(dict(trial=i,value=item['value'],input=C.pin(roles['initial_frames'][i]),expected=C.pin(roles['expected_frames'][i])))
    bindings={name:C.pin(path) for name,path in artifact.items()}
    for group in ['after','observer']:
        with artifact[group+'_IR'].open() as stream:bindings[group+'_ModuleID_literal']=stream.readline().rstrip('\n')
    for item in [*sources.values(),*source_pins,python,reference,*tools.values(),runtime['backend'],runtime['shim'],solver['library']]:
        C.pinned(item,pins,item in [python,*tools.values()])
    for item in bindings.values():
        if isinstance(item,dict):C.pinned(item,pins)
    C.require(C.recheck(pins),'metadata source/data/tool/runtime changed')
    triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}.get(platform.machine())
    C.require(triple is not None,'unsupported native host')
    plan=dict(schema='loop-real-after-nominal-plan-v1',ready=True,execution_requires_parent_GO=True,
      transfer_root=str(a.transfer_root.resolve()),transfer_ledger_sha256=LEDGER_SHA,
      source_pins=source_pins,sources=sources,python=python,reference_environment=reference,
      runtime=runtime,solver=solver,tools=tools,inputs=inputs,bindings=bindings,limits=C.LIMITS,
      host_llvm_levels=['O0','O2'],groups=['after','observer'],allowed_cpus=[0,1,2,3],target_triple=triple,
      expected_counts=dict(after_native=32,observer_native=32,comparator_controls_after=96,comparator_controls_observer=96),
      observer_counts_each=[1,0],common_main_NoZ3_IR_replayed_once=True,new_C=0,new_Unicorn=0,old_before32_recounted=0,
      boundary_and_runtime_faults_in_this_plan=False,semantic_validation_already_passed=False)
    out.mkdir();C.write(out/'PLAN.json',plan)
    argv=['/usr/bin/taskset','-c','0-3',python['path'],sources['runner']['path'],
          '--plan',str(out/'PLAN.json'),'--plan-sha256',C.sha(out/'PLAN.json'),'--out',str(out/'run')]
    C.write(out/'READY.json',dict(schema='loop-real-after-nominal-ready-v1',ready=True,
       plan=C.pin(out/'PLAN.json'),actual_command=argv,environment=dict(PYTHONDONTWRITEBYTECODE='1',DRAGON_C_LIB=runtime['shim']['path'],
       LD_LIBRARY_PATH=':'.join(runtime['library_path']),LD_PRELOAD='',LIBUNICORN_PATH=''),
       source_preflight_pins={name:value['sha256'] for name,value in pins.items()},
       compile_calls=0,Guest_calls=0,C_or_UC_calls=0,execution_requires_parent_GO=True,
       scope='Real B emission/reference/tool pins sealed; this readiness is not an execution or semantic-success claim'))
    print(json.dumps(dict(root=str(out),plan_sha256=C.sha(out/'PLAN.json'),ready_sha256=C.sha(out/'READY.json'),actual_command=argv),indent=2))

if __name__=='__main__':main()
