"""Prepare metadata/pins for the separately authorized real-B mechanical emission.

This command compiles or executes nothing. ready=true applies only to the
actual closed B input accepted by the emission tool, never to Guest replay.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode=True
import loop_real_after_common as C

LEDGER_SHA='8674465843ef192335054e7220914e980874f1d4fa659baaecc60631fd032480'
ADAPTER_SHA='1b7bfbbe06a045ded628b922a5c7d07f5f859468c507d495d6fa8ce73394d34b'
DRIVER_EMITTER_SHA='fcdc72442d77499a8ce7870df063f462370d41bc4a874dc8045215d0a1943f50'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--transfer-root',type=Path,required=True)
    ap.add_argument('--synthetic-root',type=Path,required=True)
    ap.add_argument('--transfer-certificate',type=Path,required=True)
    ap.add_argument('--source-root',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();out=a.out.resolve();pins={}
    C.require(not out.exists() and not out.is_relative_to(a.transfer_root.resolve()) and
              not out.is_relative_to(a.synthetic_root.resolve()),'new nonoverlapping output root required')
    transfer,roles=C.transfer(a.transfer_root,LEDGER_SHA,pins)
    C.pinned(dict(path=str(a.transfer_certificate.resolve()),
        sha256='9f3b53fcad139e4d48cef5636eef8cf4c8c043f2ab4860ac185eba2ac4a18545'),pins)
    old=a.synthetic_root.resolve();audit=json.loads((old/'SAVED-AUDIT.json').read_text())
    summary=json.loads((old/'SUMMARY.json').read_text())
    C.require(audit['correct'] and audit['assertions']==36 and audit['source_adapter_sha256']==ADAPTER_SHA and
              summary['standalone_compile_succeeded'] and summary['standalone_readyfalse_exit']==1,
              'synthetic-tested tool provenance failed')
    executable=old/'standalone-adapter'
    C.require(C.sha(executable)==audit['symbols']['standalone']['executable_sha256'],'standalone executable differs')
    for path in [old/'SAVED-AUDIT.json',old/'SUMMARY.json',executable,old/'source-v1/loop_real_native_adapter.cpp']:
        C.pinned(C.pin(path),pins)
    sdk=Path('/home/ubuntu/vmc-runs/loop-image-effect-index-sdk-20261115-v1')
    library=sdk/'lib/libdragon.so';solver=Path('/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13')
    C.pinned(dict(path=str(library),sha256='ef35b0c796a2cf2a79bdc161458f823768fe232427054ac134b8eb61b9f11e06'),pins)
    C.pinned(dict(path=str(sdk/'SDK-FROZEN.json'),sha256='cf614167bc24697341de5807317f48d8b018f7f2b9db20a3cfab08a07136659a'),pins)
    C.pinned(dict(path=str(solver),sha256='4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a'),pins)
    headers=json.loads((old/'HEADER-MANIFEST.json').read_text())
    C.pinned(C.pin(old/'HEADER-MANIFEST.json'),pins)
    for path,record in headers.items():
        C.pinned(dict(path=path,size=record['size'],sha256=record['sha256']),pins,True)
    driver_source=a.source_root.resolve()/'loop_real_native_driver.py'
    C.require(C.sha(driver_source)==DRIVER_EMITTER_SHA,'driver emitter source differs')
    C.require(C.sha(a.source_root.resolve()/'loop_real_native_adapter.cpp')==ADAPTER_SHA,'adapter source differs')
    for path in [driver_source,a.source_root.resolve()/'loop_real_native_adapter.cpp',Path(__file__).resolve(),Path(C.__file__).resolve()]:
        C.pinned(C.pin(path),pins)
    python=Path('/usr/bin/python3.10');C.pinned(C.pin(python),pins)
    identity=json.loads(roles['main_identity_proof'].read_text())
    view_names={x['function'] for item in identity['view_identity']['imports'] for x in item['users']}
    C.require(len(view_names)==1,'view role is ambiguous')
    view=next(iter(view_names))
    with roles['main_after'].open() as stream:first_line=stream.readline().rstrip('\n')
    C.require(first_line=="; ModuleID = 'guarded-source'",'actual production first line differs')
    actual_roles=dict(schema='loop-real-native-adapter-roles-v1',ready=True,eligible_actual_closed_result=True,
       production_ir_sha256=C.AFTER_SHA,production_first_line=first_line,closure_evidence_sha256=LEDGER_SHA,
       wrapper='dragon_static_entry_x86_64_1000',ordinary_kernel='dragon_static_entry_x86_64_1010',view=view,kernel_pc=4112,
       proof_provenance_only=dict(ledger_sha256=LEDGER_SHA,main_report=C.pin(roles['main_report']),
           NoZ3_report=C.pin(roles['NoZ3_report']),main_identity=C.pin(roles['main_identity_proof']),
           NoZ3_identity=C.pin(roles['NoZ3_identity_proof']),global_precision_required=False,
           allowed_case='B-SP-table-RSI',Guest_execution_authorized=False))
    out.mkdir();(out/'stages').mkdir();(out/'emission').mkdir();(out/'source-v1').mkdir()
    C.write(out/'ACTUAL-ROLES.json',actual_roles)
    shutil.copyfile(driver_source,out/'source-v1/loop_real_native_driver.py')
    shutil.copyfile(a.source_root.resolve()/'loop_real_native_adapter.cpp',out/'source-v1/loop_real_native_adapter.cpp')
    C.write(out/'TRANSFER-CHECK.json',dict(schema='real-B-emission-transfer-check-v1',all115files_size_sha_passed=True,
       ledger_sha256=LEDGER_SHA,common_after_sha256=C.AFTER_SHA,actual_case_closed=True,global_precision_required=False,
       before32_and96_referenced_not_recounted=True,Guest_calls=0,Image_calls=0,product_pass_calls=0))
    for path in [out/'ACTUAL-ROLES.json',out/'TRANSFER-CHECK.json',*list((out/'source-v1').iterdir())]:
        C.pinned(C.pin(path),pins)
    plans={}
    commands={
      'adapter':(executable,[str(executable),str(roles['main_after']),str(out/'ACTUAL-ROLES.json'),str(out/'emission/adapter')],True),
      'driver':(python,[str(python),str(out/'source-v1/loop_real_native_driver.py'),'--driver',str(roles['driver']),
                        '--roles',str(out/'ACTUAL-ROLES.json'),'--out',str(out/'emission/driver')],False)}
    (out/'plans').mkdir()
    for name,(exe,argv,maps) in commands.items():
        plan=dict(schema='dragon-tigress-context-guard-plan-v1',limits=dict(seconds=180,start_free=536870912,running_floor=67108864),
          pins={path:value['sha256'] for path,value in pins.items()},executable=str(exe),
          library_path=[str(library.parent),str(solver.parent)],require_maps=maps,
          stages=dict(probe=dict(argv=['/usr/bin/taskset','-c','0-3',*argv])),
          scope=dict(actual_B_IR=True,mechanical_emission_only=True,Guest_calls=0,Image_calls=0,C_or_UC_calls=0,
                     product_pass_calls=0,IR_compiles=0,ef35_supplies_LLVM_symbols_only=True))
        p=out/'plans'/(name+'.json');C.write(p,plan);plans[name]=dict(path=str(p),sha256=C.sha(p))
    C.require(C.recheck(pins),'source/input/tool/runtime/header changed during metadata preparation')
    C.write(out/'EMIT-READY.json',dict(schema='actual-closed-B-mechanical-emission-ready-v1',ready=True,
       roles=C.pin(out/'ACTUAL-ROLES.json'),transfer_root=str(a.transfer_root.resolve()),transfer_ledger_sha256=LEDGER_SHA,
       plans=plans,Guest_execution_ready=False,Guest_calls=0,IR_compiles=0,product_pass_calls=0,header_count=len(headers)))
    print(json.dumps(dict(root=str(out),ready_sha256=C.sha(out/'EMIT-READY.json'),plans=plans),indent=2))

if __name__=='__main__':main()
