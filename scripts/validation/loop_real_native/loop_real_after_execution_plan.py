"""Add the fixed outer guard and explicit nominal argv inventory; execute nothing."""
import argparse
import json
from pathlib import Path
import sys
sys.dont_write_bytecode=True
import loop_real_after_common as C

GUARD_SHA='d0452de8980c041803db571d1555fbdb63c8d2234ebaa81875f5f2125dbc7cad'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--plan-sha256',required=True)
    ap.add_argument('--guard',type=Path,required=True)
    a=ap.parse_args();root=a.root.resolve();plan_path=root/'PLAN.json'
    C.require(C.sha(plan_path)==a.plan_sha256,'nominal plan changed')
    plan=json.loads(plan_path.read_text());ready_path=root/'READY.json';ready=json.loads(ready_path.read_text())
    C.require(plan['ready'] and plan['execution_requires_parent_GO'] and plan['limits']==C.LIMITS,'nominal plan not ready')
    C.require(C.sha(a.guard)==GUARD_SHA,'fixed guard source changed')
    C.require(not (root/'EXECUTION.json').exists(),'new execution record required')
    pins=dict(ready['source_preflight_pins'])
    for p in [plan_path,ready_path,a.guard,Path(__file__),Path('/usr/bin/python3.10'),Path('/usr/bin/taskset')]:
        pins[str(p)]=C.sha(p)
    reference=json.loads(Path(plan['reference_environment']['path']).read_text())
    pins[reference['UC_native']['path']]=reference['UC_native']['sha256']
    certificate=Path('/home/ubuntu/vmc-runs/loop-image-native-public-input-verification-20261115-v1/DUAL-VERIFIED.json')
    C.require(C.sha(certificate)=='9f3b53fcad139e4d48cef5636eef8cf4c8c043f2ab4860ac185eba2ac4a18545','transfer dual certificate changed')
    pins[str(certificate)]=C.sha(certificate)
    C.require(all(C.sha(path)==value for path,value in pins.items()),'execution preparation pin changed')
    guard_plan=dict(schema='dragon-tigress-context-guard-plan-v1',
        limits=dict(seconds=180,start_free=536870912,running_floor=67108864),pins=pins,
        stages=dict(probe=dict(argv=ready['actual_command'])),library_path=plan['runtime']['library_path'],
        executable=str(Path(plan['python']['path']).resolve()),require_maps=True)
    C.write(root/'GUARD-PLAN.json',guard_plan)
    guard_argv=['/usr/bin/python3.10',str(a.guard),'--plan',str(root/'GUARD-PLAN.json'),
        '--sha256',C.sha(root/'GUARD-PLAN.json'),'--stage','probe','--out',str(root/'guarded-run')]
    commands=[];run=root/'run';bindings=plan['bindings'];tools=plan['tools']
    compat=Path(plan['transfer_root'])/'before/source/compat'
    for group in plan['groups']:
        module=bindings[group+'_IR']['path']
        for level in plan['host_llvm_levels']:
            work=run/group/level;chosen=module
            compile_commands=[[tools['opt']['path'],'-passes=verify','-disable-output',module]]
            if level=='O2':
                chosen=str(work/'optimized.ll')
                compile_commands.append([tools['opt']['path'],'-S','-passes=default<O2>',module,'-o',chosen])
            compile_commands.extend([
                [tools['opt']['path'],'-passes=verify','-disable-output',chosen],
                [tools['llc']['path'],'-O0','-filetype=obj','-mtriple='+plan['target_triple'],chosen,'-o','module.o']])
            link=[tools['cc']['path'],'-O0','-I'+str(compat/'include'),'-I'+str(compat/'c_shim'),'driver.c','module.o']
            if group=='observer':link.append(bindings['observer_helper']['path'])
            compile_commands.extend([link+['-o','native'],[tools['readelf']['path'],'-d',str(work/'native')]])
            commands.append(dict(group=group,host_llvm_level=level,cwd=str(work),compile_commands=compile_commands,
                native_commands=[[str(work/'native'),str(work/('input-%d.bin'%i)),str(work/('output-%d.bin'%i))] for i in range(16)],
                sidecars=[str(work/('output-%d.counts'%i)) for i in range(16)] if group=='observer' else []))
    C.write(root/'COMMANDS.json',dict(groups=commands,all_subprocess_seconds=180,
        commands_are_planned_not_executed=True,expected_group_matches=dict(after=32,observer=32),
        expected_comparator_controls=dict(after=96,observer=96),observer_each=[1,0]))
    C.write(root/'EXECUTION.json',dict(schema='loop-real-after-nominal-execution-v1',
        plan=C.pin(plan_path),guard_plan=C.pin(root/'GUARD-PLAN.json'),commands=C.pin(root/'COMMANDS.json'),
        guard=C.pin(a.guard),generator=C.pin(Path(__file__)),argv=guard_argv,
        environment=ready['environment'],requires_parent_GO=True,executed=False,
        layout='24 header +512 raw registers +65536 Guest +56 Exit(7*u64) +8 budget =66136 bytes',
        original_C16_UC16_and_before32_are_references_not_new_counts=True))
    print(json.dumps(dict(root=str(root),guard_plan_sha256=C.sha(root/'GUARD-PLAN.json'),
        execution_sha256=C.sha(root/'EXECUTION.json'),commands_sha256=C.sha(root/'COMMANDS.json'),argv=guard_argv),indent=2))

if __name__=='__main__':main()
