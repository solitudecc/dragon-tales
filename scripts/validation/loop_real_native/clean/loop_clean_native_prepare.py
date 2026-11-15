"""Freeze binding-only cleanup replay inputs and guards; execute no target."""
import argparse
import json
from pathlib import Path
import sys
sys.dont_write_bytecode=True
import loop_real_after_common as C

CLEAN_SHA='8418fd314005fa8b771473ea56e907a352416b459246e4f0e39d796c55ee1d2e'
REPORT_SHA='6ed3489244c19e6c28b039b455810e06e19beda59aaf33dfbf31d3f2321c6c31'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True);ap.add_argument('--source-root',type=Path,required=True)
    ap.add_argument('--cleanup-root',type=Path,required=True);ap.add_argument('--stage',choices=['binding','native'],required=True)
    a=ap.parse_args();root=a.root.resolve();source=a.source_root.resolve();cleanup=a.cleanup_root.resolve();pins={}
    nominal=Path('/home/ubuntu/vmc-runs/loop-real-native-nominal-plan-20261115-v1/PLAN.json')
    C.require(C.sha(nominal)=='8d31e3abcafb78909d312d432c8c04e4bfee0892588a2cd8513ed32facfaa206','original nominal plan differs')
    baseline=json.loads(nominal.read_text());ledger,roles=C.transfer(baseline['transfer_root'],baseline['transfer_ledger_sha256'],pins)
    report=cleanup/'REPORT.json';after=cleanup/'after.ll';C.require(C.sha(report)==REPORT_SHA and C.sha(after)==CLEAN_SHA,'cleanup artifact differs')
    d=json.loads(report.read_text())
    C.require(d['schema']=='dragon-accepted-view-cleanup-report-v1' and d['ok'] and d['cleanup_emitted'] and
        d['source_and_all_pins_unchanged'] and d['ready_and_closed_and_sealed_gates_passed'] and
        d['passes_attempted']==6 and d['recipe']=='existing-control-flow-six-v1' and d['source_sha256']==C.AFTER_SHA and
        d['closed_report_sha256']==C.sha(roles['main_report']) and d['after_sha256']==CLEAN_SHA and
        d['Guest_native_runs']==d['Unicorn_runs']==0,'cleanup provenance/recipe/closure gates failed')
    C.require(len(d['stages'])==7 and all(s['whole_verified'] for s in d['stages']) and
        all(s['outside_text_and_identity_unchanged'] for s in d['stages'][1:]),'cleanup identity/verification gates failed')
    C.require((cleanup/'00-source.ll').read_bytes()==roles['main_after'].read_bytes(),'cleanup source is not original closed B')
    for p in [nominal,report,after,cleanup/'00-source.ll',cleanup/'00-parsed.ll']:
        C.pinned(C.pin(p),pins)
    for p in sorted(source.iterdir()):
        C.require(p.is_file() and not p.is_symlink(),'regular source-only root required');C.pinned(C.pin(p),pins)
    original=source/'loop_real_native_adapter.cpp'
    C.require(C.sha(original)=='1b7bfbbe06a045ded628b922a5c7d07f5f859468c507d495d6fa8ce73394d34b','included original binding body differs')
    guard=Path('/home/ubuntu/vmc-runs/loop-real-native-emission-source-20261115-v1/guard.py');C.require(C.sha(guard)=='d0452de8980c041803db571d1555fbdb63c8d2234ebaa81875f5f2125dbc7cad','fixed guard differs');C.pinned(C.pin(guard),pins)
    sdk=Path('/home/ubuntu/vmc-runs/loop-image-effect-index-sdk-20261115-v1');library=sdk/'lib/libdragon.so'
    C.require(C.sha(library)=='ef35b0c796a2cf2a79bdc161458f823768fe232427054ac134b8eb61b9f11e06','LLVM symbol-provider SDK differs')
    for p in [library,sdk/'SDK-FROZEN.json']:C.pinned(C.pin(p),pins)
    compiler=Path('/usr/bin/clang++-21');C.require(C.sha(compiler)=='378ecf70007376686987e9587a08fe49ec848ba9625bf5f3969e00203291ddba','binding compiler differs')
    C.pinned(C.pin(compiler),pins,True);C.pinned(C.pin(compiler.resolve()),pins)
    z3=Path(baseline['solver']['library']['path']);C.pinned(baseline['solver']['library'],pins)
    header_file=Path('/home/ubuntu/vmc-runs/loop-native-adapter-synthetic-20261115-v1/HEADER-MANIFEST.json')
    C.require(C.sha(header_file)=='37678f0b857113385b69130570d3ea841e2ac3360b59ff0e04e41ad5178e3b5b','compiled original header file set differs')
    C.pinned(C.pin(header_file),pins)
    for path,row in json.loads(header_file.read_text()).items():C.pinned(dict(path=path,**row),pins,True)
    executable=root/'clean-binding';limits=dict(seconds=180,start_free=536870912,running_floor=67108864)
    def guarded(name,argv,paths,exe,maps):
        path=root/(name+'.json');C.require(not path.exists(),'new stage plan required')
        C.write(path,dict(schema='dragon-tigress-context-guard-plan-v1',limits=limits,
            pins={k:v['sha256'] for k,v in paths.items()},library_path=[str(library.parent),str(z3.parent)] if name!='NATIVE-GUARD' else baseline['runtime']['library_path'],
            executable=str(exe),require_maps=maps,stages=dict(compile=dict(argv=argv)) if name=='COMPILE' else dict(probe=dict(argv=argv))))
        return dict(path=str(path),sha256=C.sha(path))
    if a.stage=='binding':
        C.require(not root.exists(),'new binding namespace required');root.mkdir()
        config=dict(schema='loop-clean-native-binding-roles-v1',ready=True,eligible_actual_closed_cleanup=True,
            wrapper='dragon_static_entry_x86_64_1000',ordinary_kernel=d['ordinary_root_symbol'],view=d['view_symbol'],kernel_pc=4112,
            production_ir_sha256=CLEAN_SHA,production_first_line=after.read_text().splitlines()[0],
            cleanup_report_sha256=REPORT_SHA,closed_source_sha256=C.AFTER_SHA,
            external_closure_provenance_only=True,Guest_runs=0,observer_calls=0)
        C.write(root/'ROLES.json',config);C.pinned(C.pin(root/'ROLES.json'),pins)
        argv=['/usr/bin/taskset','-c','0-3',str(compiler),'-O1','-g0','-DNDEBUG','-std=c++20','-I/usr/lib/llvm-21/include',
            '-DDRAGON_LOOP_NATIVE_ADAPTER_SOURCE="'+str(original)+'"',str(source/'loop_clean_native_adapter.cpp'),str(library),
            '-Wl,-rpath,'+str(library.parent),'-Wl,-rpath-link,'+str(z3.parent),'-pthread','-ldl','-o',str(executable)]
        plans=dict(compile=guarded('COMPILE',argv,pins,executable,False),
            unit=guarded('UNIT',['/usr/bin/taskset','-c','0-3',str(executable),'--synthetic-binding-unit'],pins,executable,False),
            emit=guarded('EMIT',['/usr/bin/taskset','-c','0-3',str(executable),str(after),str(root/'ROLES.json'),str(root/'binding')],pins,executable,True))
        C.require(C.recheck(pins),'binding preparation pins changed');C.write(root/'BINDING-READY.json',dict(plans=plans,source_pins=pins,
            cleanup=C.pin(report),source=C.pin(after),headers=C.pin(header_file),header_count=3034,
            source_parse_and_bind_only=True,observer_calls=0,Guest_runs=0))
        print(json.dumps(plans,indent=2));return
    for name in ['compile','unit','emit']:
        g=json.loads((root/('guard-'+name)/'GUARD.json').read_text());C.require(g['returncode']==0 and g['failure'] is None and g['pins_unchanged'],'binding stage failed')
    unit=json.loads((root/'guard-unit/stdout').read_text());C.require(unit['passed'] and unit['checks']==9 and unit['observer_calls']==unit['Guest_calls']==0,'synthetic binding-only unit failed')
    emitted=root/'binding/REPORT.json';e=json.loads(emitted.read_text())
    C.require(e['binding_only'] and e['observer_calls']==0 and e['input_sha256']==CLEAN_SHA and e['whole_module_verified'] and
        e['production_input_unchanged'] and e['binding']['only_one_callee_operand_changed'] and e['binding']['retargeted_direct_calls']==1 and
        e['binding']['ordinary_kernel_unchanged'] and e['binding']['view_unchanged'],'clean mechanical binding failed')
    module=root/'binding/native-adapter.ll';C.require(C.sha(module)==e['native_adapter_sha256'],'bound clean IR differs')
    for item in [baseline['python'],baseline['reference_environment'],baseline['runtime']['backend'],baseline['runtime']['shim'],
                 baseline['solver']['library'],*baseline['tools'].values(),*baseline['bindings'].values()]:
        if isinstance(item,dict):C.pinned(item,pins,True)
    for p in [executable,emitted,module,root/'ROLES.json',root/'BINDING-READY.json']:C.pinned(C.pin(p),pins)
    native=dict(schema='loop-clean-native-plan-v1',ready=True,limits=C.LIMITS,allowed_cpus=[0,1,2,3],host_llvm_levels=['O0','O2'],
        transfer_root=baseline['transfer_root'],transfer_ledger_sha256=baseline['transfer_ledger_sha256'],
        sources=[C.pin(p) for p in sorted(source.iterdir())],python=baseline['python'],reference_environment=baseline['reference_environment'],
        runtime=baseline['runtime'],solver=baseline['solver'],tools=baseline['tools'],inputs=baseline['inputs'],target_triple=baseline['target_triple'],
        module=C.pin(module),module_first_line=module.read_text().splitlines()[0],binding_report=C.pin(emitted),cleanup_report=C.pin(report),
        driver=baseline['bindings']['driver'],expected_native=32,expected_comparator_controls=96,new_observer_evidence=False,
        no_prior76_replayed=True,new_C_runs=0,new_UC_runs=0)
    C.require(C.recheck(pins),'native preparation pins changed');C.write(root/'NATIVE-PLAN.json',native);C.pinned(C.pin(root/'NATIVE-PLAN.json'),pins)
    argv=['/usr/bin/taskset','-c','0-3',baseline['python']['path'],str(source/'loop_clean_native_replay.py'),
        '--plan',str(root/'NATIVE-PLAN.json'),'--plan-sha256',C.sha(root/'NATIVE-PLAN.json'),'--out',str(root/'native')]
    p=guarded('NATIVE-GUARD',argv,pins,Path(baseline['python']['path']).resolve(),True)
    print('NATIVE-PLAN',C.sha(root/'NATIVE-PLAN.json'));print('NATIVE-GUARD',p['sha256'])

if __name__=='__main__':main()
