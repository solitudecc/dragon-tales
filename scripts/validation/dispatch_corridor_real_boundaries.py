#!/usr/bin/env python3
"""Emit four separate real boundary inputs and an explicit logical-size driver.

This module never compiles, invokes a pass, runs native code or calls Unicorn.
The raw input files retain the old512-register+65536-Guest format. Logical size
is a separate host-driver argument; complete output remains66136bytes. These
scenarios are not C/Unicorn machine-reference comparisons.
"""
import argparse,hashlib,json,re,struct
from pathlib import Path

NOMINAL_PLAN='9bacce5f629b95d9582b299f7f2457d2fc0f67f9a3340e56f3dceedcc2b21458'
DRIVER_SHA='e0fb71e08b117ddb6c929dc60ff66af938073695ced0fe68b6f50aa9c69c75bb'
def sha(b):return hashlib.sha256(b).hexdigest()
def pin(p):
    p=Path(p);b=p.read_bytes();return dict(path=str(p),size=len(b),sha256=sha(b))
def write(p,v):
    with Path(p).open('x') as f:json.dump(v,f,indent=2,sort_keys=True);f.write('\n')
def logical_driver(original):
    """Change only argc parsing and M.size, preserving the snapshot protocol."""
    assert sha(original)==DRIVER_SHA
    text=original.decode();old='  if(argc!=3) return 90;'
    assert text.count(old)==1
    text=text.replace(old,'''  if(argc!=4) return 90;
  uint64_t logical_size=0;
  if(!argv[3][0]) return 94;
  for(const unsigned char *p=(const unsigned char *)argv[3];*p;++p) {
    if(*p<'0'||*p>'9'||logical_size>65536ULL/10ULL) return 94;
    logical_size=logical_size*10ULL+(*p-'0');
    if(logical_size>sizeof(memory)) return 94;
  }''')
    old='M m={memory,sizeof(memory)};';assert text.count(old)==1
    return text.replace(old,'M m={memory,logical_size};').encode()
def forced_sp_accept(text):
    """Deliberately wrong IR: defeat only the actual SP equality predicate."""
    marker='define void @dragon_static_entry_x86_64_1010.guarded.image_table_manual_o2_v1('
    start=text.index(marker);end=text.index('\n}',start)
    body=text[start:end];m=re.search(r'^guarded\.bounds:.*?\n(?P<block>.*?)(?=^[A-Za-z$._][A-Za-z$._0-9-]*:)',body,re.M|re.S)
    if not m:raise ValueError('actual guarded.bounds block is missing')
    block=m['block'];pattern=r'(?m)^(\s*%[-A-Za-z$._0-9]+\s*=\s*)icmp eq i64 %[-A-Za-z$._0-9]+, 51192$'
    changed,count=re.subn(pattern,r'\1icmp eq i64 51192, 51192',block)
    if count!=1:raise ValueError('actual SP check must match exactly once')
    replacement=body[:m.start('block')]+changed+body[m.end('block'):]
    result=text[:start]+replacement+text[end:]
    assert result!=text and result.splitlines()[0]==text.splitlines()[0]
    return result
def wrong_fault_effect(text):
    """Deliberately wrong IR: after logical-size fault, corrupt Guest and Exit.

    The physical backing is still65536bytes, so these explicit validation-only
    stores are host-safe. This is an injected wrong effect, not product output.
    """
    root='dragon_static_entry_x86_64_1000'
    old='define void @'+root+'('
    assert text.count(old)==1 and '@'+root+'.control_original' not in text
    result=text.replace(old,'define void @'+root+'.control_original(',1)
    result+='''
define void @dragon_static_entry_x86_64_1000(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {
entry:
  call void @dragon_static_entry_x86_64_1000.control_original(ptr %state, ptr %memory, ptr %runtime, ptr %exit)
  %guest = load ptr, ptr %memory, align 1
  %out = getelementptr i8, ptr %guest, i64 32768
  %before = load i8, ptr %out, align 1
  %bad = xor i8 %before, 1
  store i8 %bad, ptr %out, align 1
  %exit_before = load i8, ptr %exit, align 1
  %exit_bad = xor i8 %exit_before, 1
  store i8 %exit_bad, ptr %exit, align 1
  ret void
}
'''
    assert result.splitlines()[0]==text.splitlines()[0]
    return result
def emit(a):
    pb=a.nominal_plan.read_bytes();assert sha(pb)==NOMINAL_PLAN
    nominal=json.loads(pb);assert nominal['case']=='manual-O2'
    source=a.base_input.read_bytes();original=a.driver.read_bytes();manifest=a.guard_manifest.read_bytes()
    assert len(source)==66048 and sha(source)==nominal['inputs'][0]['input']['sha256']
    assert sha(original)==nominal['bindings']['old_driver']['sha256']==DRIVER_SHA
    assert sha(manifest)==nominal['bindings']['guarded_entry_manifest']['sha256']
    m=json.loads(manifest);assert int.from_bytes(source[32:40],'little')==0xc808
    assert source[512+0xc808:512+0xc810]==struct.pack('<Q',0xf000)
    sp=bytearray(source);sp[32:40]=struct.pack('<Q',0xc818)
    sp[512+0xc818:512+0xc820]=struct.pack('<Q',0xf000)
    table=bytearray(source);target_changes=[]
    for check in m['memory']:
        offset=int(check['offset'],0);data=bytes.fromhex(check['bytes']);assert len(data)%8==0
        assert source[512+offset:512+offset+len(data)]==data
        for i in range(0,len(data),8):
            at=512+offset+i;old=int.from_bytes(table[at:at+8],'little');new=old^(1<<63)
            table[at:at+8]=struct.pack('<Q',new);target_changes.append(dict(address=offset+i,old=old,new=new))
    assert len(target_changes)==6
    scenarios=[('sp-miss',bytes(sp),65536),('table-miss',bytes(table),65536),
               ('size-zero',source,0),('size-late-ret',source,0xc808)]
    assert not a.out.exists();a.out.mkdir(parents=True)
    (a.out/'logical-size-driver.c').write_bytes(logical_driver(original))
    rows=[]
    for name,data,size in scenarios:
        p=a.out/(name+'.bin');p.write_bytes(data)
        input_pin=pin(p);input_pin['path']=p.name
        rows.append(dict(name=name,trial=0,input=input_pin,logical_Guest_size=size,
            physical_Guest_bytes=65536,scope='Original-LLVM baseline required; not a Unicorn logical-size model.'))
    driver_pin=pin(a.out/'logical-size-driver.c');driver_pin['path']='logical-size-driver.c'
    plan=dict(schema='ordered-real-boundary-inputs-v1',inputs_frozen=True,execution_ready=False,
        emitter=pin(__file__),nominal_plan=pin(a.nominal_plan),base_input=pin(a.base_input),old_driver=pin(a.driver),
        manifest=pin(a.guard_manifest),logical_driver=driver_pin,scenarios=rows,
        table_mutations=target_changes,original_SP='0xc808',miss_SP='0xc818',root_sentinel='0xf000',
        domain=dict(logical_scenarios=4,physical_initial_frames=3,host_llvm_levels=['O0','O2']),
        counts=dict(new_original_LLVM_calibration_once=8,new_actual_bound_per_backend=8,
                    planned_two_backend_normal_matches=24,new_C=0,new_Unicorn=0),
        limits=nominal['limits'],comparison=nominal['comparison'],
        requirements=['Actual after must come from each new actual-library producer.',
          'Original-LLVM8 calibration and source pins must all pass before actual bound replay.',
          'Compare every66136byte; preserve non-RET/fault Exit without normalization.',
          'Require early/late logical-size faults to be observed; never infer them from names.',
          'Before refusing a fixture or changing a gate, preserve every original failure and ask root.'],
        deliberate_IR_controls=[
          dict(name='force-SP-guard-accept',input='sp-miss',mutation='forced_sp_accept',
               require_RSP_difference=True,require_Guest_difference=True,whole_state_match_must_be=False),
          dict(name='wrong-fault-effect',input='size-zero',mutation='wrong_fault_effect',
               require_Guest_0x8000_xor1=True,require_Exit_byte0_xor1=True,whole_state_match_must_be=False)],
        actual_after_pin_pending=True,no_target_executions=True)
    write(a.out/'INPUTS.json',plan)
    print('frozen four scenarios',sha((a.out/'INPUTS.json').read_bytes()))
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--nominal-plan',type=Path,required=True);p.add_argument('--base-input',type=Path,required=True)
    p.add_argument('--driver',type=Path,required=True);p.add_argument('--guard-manifest',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);emit(p.parse_args())
if __name__=='__main__':main()
