#!/usr/bin/env python3
"""Independent original-source, manifest, fallback and precision gates."""
import argparse
import hashlib
import json
from pathlib import Path
import re

NAMES=('plain','overwrite','partial','call','loop','reg_alias','descriptor_alias','exit_alias','recursive','base_reload')
FAULTS=('constant','drop_store','drop_call','bounds','entry')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('old',type=Path);p.add_argument('new',type=Path);p.add_argument('--require-gain',action='store_true');a=p.parse_args()
def function(text,name):
    matches=re.findall(r'^define[^\n]*@'+re.escape(name)+r'\([^\n]*\)[^\n]*\{\n(.*?)^}',text,re.M|re.S)
    assert len(matches)==1,('missing or duplicate function',name)
    return matches[0]
identical={};gains={};structural={}
for index,name in enumerate(NAMES):
    filename=name+'-original.ll';left=(a.old/filename).read_bytes();right=(a.new/filename).read_bytes();assert left==right,filename
    identical[filename]=hashlib.sha256(left).hexdigest();pc=0x1000+index*0x100
    root=f'dragon_static_entry_x86_64_{pc:x}';view=root+'.guarded.oracle'
    manifest=json.loads((a.new/(name+'-manifest.json')).read_text())
    for k,v in {'guarded':True,'check_kind':'runtime-byte-equality','entry':hex(pc),'root_symbol':root,'view_symbol':view,'context_id':'oracle',
                'minimum_register_bytes':'32','minimum_guest_bytes':'264','registers':[{'offset':'0x18','bytes':'0001000000000000'}],
                'memory':[{'offset':'0x100','bytes':'0e00000000000000'}]}.items():assert manifest[k]==v,(name,k)
    assert re.fullmatch('[0-9a-f]{64}',manifest['source_sha256'])
    assert manifest['provenance']=='independent frozen synthetic LLVM facts, never ELF initialization'
    raw=(a.new/(name+'-guarded.ll')).read_text();optimized=(a.new/(name+'-optimized.ll')).read_text()
    body=function(raw,view);ordinary=function(raw,root);opt=function(optimized,view)
    first=re.search(r'^define[^\n]*@([^ (]+)',raw,re.M);assert first and first[1]==view
    assert 'call void @'+root+'(' in body,'missing ordinary fallback'
    assert ('call void @'+view+'(') not in body,'context recursively reapplied'
    blocks={n:b for n,b in re.findall(r'^([\w.]+):[^\n]*\n(.*?)(?=^[\w.]+:|\Z)',body,re.M|re.S)}
    assert 'guarded.bounds' in blocks and 'guarded.fallback' in blocks and 'guarded.accept' in blocks
    assert 'ret void' in blocks['guarded.fallback'] and 'call void @'+root+'(' in blocks['guarded.fallback']
    incoming=[n for n,b in blocks.items() if 'label %guarded.bounds' in b]
    assert incoming==['entry'],(name,'guard facts have extra incoming',incoming)
    if name=='recursive':assert body.count('call void @'+root+'(')==2 and ordinary.count('call void @'+root+'(')==1
    if name=='loop':
        assert 'label %oracle.machine.entry' in blocks['oracle.latch']
        assert '%guarded.accept' in blocks['oracle.machine.entry'] and '%oracle.latch' in blocks['oracle.machine.entry']
    raw_target=sum('load ' in line and '!oracle.target' in line for line in body.splitlines())
    opt_target=sum('load ' in line and '!oracle.target' in line for line in opt.splitlines())
    assert raw_target==1,(name,raw_target)
    gains[name]={'raw_actual_target_loads':raw_target,'optimized_actual_target_loads':opt_target,'target_load_removed':opt_target==0}
    structural[name]={'separate_symbols':True,'entry_only_checks':True,'fallback_kept':True,'manifest_complete':True}
for fault in FAULTS:
    filename='control_'+fault+'.ll';left=(a.old/filename).read_bytes();right=(a.new/filename).read_bytes();assert left==right,filename
    identical[filename]=hashlib.sha256(left).hexdigest()
result={'ok':True,'same_original_and_fault_ir':identical,'structural':structural,'precision':gains,
        'plain_gain':gains['plain']['target_load_removed'],'native_executions':0,
        'note':'A removed target LOAD is guarded specialization; entry fact LOAD/check and ordinary fallback remain.'}
if a.require_gain and not result['plain_gain']:result['ok']=False
print(json.dumps(result,sort_keys=True))
raise SystemExit(0 if result['ok'] else 1)
