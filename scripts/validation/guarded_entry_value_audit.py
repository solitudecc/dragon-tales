#!/usr/bin/env python3
"""Prove the saved plain fast path uses the checked 14 at its register sink.
This inspects SSA/control/data edges, independently of oracle metadata tags.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
S=r'%[\w.]+'
ROOT='dragon_static_entry_x86_64_1000'
def extract(text):
    m=re.findall(r'^define[^\n]*@'+re.escape(ROOT+'.guarded.oracle')+r'\([^\n]*\)[^\n]*\{\n(.*?)^}',text,re.M|re.S)
    assert len(m)==1;return m[0]
def prove(text):
    body=extract(text)
    blocks={n:b for n,b in re.findall(r'^([\w.]+):[^\n]*\n(.*?)(?=^[\w.]+:|\Z)',body,re.M|re.S)}
    defs={};owner={}
    for name,block in blocks.items():
        for value,rhs in re.findall(r'^  ('+S+r') = (.*)$',block,re.M):defs[value]=rhs;owner[value]=name
    def sole(values):assert len(values)==1,values;return values[0]
    def find(rhs):return sole([v for v,d in defs.items() if d==rhs])
    def gep(base,offset):return find(f'getelementptr i8, ptr {base}, i64 {offset}')
    def load(kind,pointer):return find(f'load {kind}, ptr {pointer}, align 1')
    regs=load('ptr',gep('%state',24));guest=load('ptr','%memory')
    payload=load('i64',gep(regs,8));index=load('i64',gep(regs,24));pc=load('i64',gep(regs,128))
    table=load('i64',gep(guest,256));equal=find(f'icmp eq i64 {table}, 14');guard=owner[table]
    branch=re.findall(r'br i1 '+re.escape(equal)+r', label %([\w.]+), label %([\w.]+)',blocks[guard]);hit,fallback=sole(branch)
    assert 'call void @'+ROOT+'(' in blocks[fallback]
    preds={name:set() for name in blocks}
    for name,block in blocks.items():
        for successor in re.findall(r'label %([\w.]+)',block):preds[successor].add(name)
    assert preds[hit]=={guard} and preds[guard]=={'guarded.bounds'}
    assert preds['guarded.bounds']=={'entry'}
    assert ' phi ' not in blocks[hit] and ' phi ' not in blocks[guard]
    for name in ('entry','guarded.bounds',guard):
        assert not re.search(r'\b(store|call|invoke|atomicrmw|cmpxchg|fence)\b',blocks[name]),name
    # Require real PC and bound/register/non-null predicates on the route to
    # the table read; no tag or instruction name substitutes for these values.
    entrycmp=find(f'icmp eq i64 {pc}, 4096')
    assert re.search(r'br i1 '+re.escape(entrycmp)+r', label %guarded.bounds, label %entry.unsupported',blocks['entry'])
    size=load('i64',gep('%memory',8));regsize=load('i64',gep('%state',16))
    required={f'icmp ugt i64 {size}, 263',f'icmp ugt i64 {regsize}, 31',
              f'icmp ne ptr {guest}, null',f'icmp ne ptr {regs}, null',f'icmp eq i64 {index}, 256'}
    boundbranch=sole(re.findall(r'br i1 ('+S+r'), label %'+re.escape(guard)+r', label %'+re.escape(fallback),blocks['guarded.bounds']))
    def conjunction(value,active=None):
        active=set() if active is None else set(active);assert value not in active;active.add(value);rhs=defs[value]
        m=re.fullmatch(r'and i1 ('+S+r'), ('+S+r')',rhs)
        if m:return conjunction(m[1],active)|conjunction(m[2],active)
        m=re.fullmatch(r'select i1 ('+S+r'), i1 ('+S+r'), i1 false',rhs)
        if m:return conjunction(m[1],active)|conjunction(m[2],active)
        assert rhs.startswith('icmp '),rhs;return {rhs}
    assert conjunction(boundbranch)==required
    stores=re.findall(r'^  store i64 ('+S+r'), ptr '+re.escape(regs)+r', align 1$',blocks[hit],re.M)
    result=sole(stores);assert owner[result]==hit
    expected={f'xor i64 {payload}, 14',f'xor i64 14, {payload}'};assert defs[result] in expected
    assert not re.search(r'\b(load|call|invoke|phi)\b',blocks[hit]),'fresh unproved dependency on fast path'
    return {'guard_block':guard,'success_block':hit,'fallback_block':fallback,'result_ssa':result,'result_expression':defs[result],
            'register_base':regs,'payload_ssa':payload,'target_load_ssa':table,'equality_ssa':equal,'bounds':sorted(required),
            'guarded_body_sha256':hashlib.sha256(body.encode()).hexdigest(),'equality_expression':defs[equal]}
p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);a=p.parse_args()
optimized=(a.directory/'plain-optimized.ll').read_text();raw=(a.directory/'plain-guarded.ll').read_text();proof=prove(optimized)
controls={
 'metadata_only':re.sub(r', !oracle.target !\d+','',raw),
 'wrong_result':optimized.replace(proof['result_expression'],proof['result_expression'].replace('14','15'),1),
 'wrong_checked_value':optimized.replace(proof['equality_expression'],proof['equality_expression'].replace('14','15'),1),
 'lost_fallback':optimized.replace('br i1 '+proof['equality_ssa']+', label %'+proof['success_block']+', label %'+proof['fallback_block'],
                                    'br i1 '+proof['equality_ssa']+', label %'+proof['success_block']+', label %'+proof['success_block'],1),
 'wrong_bound':optimized.replace('263','255',1)}
detected=[]
for name,damaged in controls.items():
    assert damaged!=optimized
    try:prove(damaged)
    except (AssertionError,KeyError):detected.append(name)
    else:raise AssertionError('precision control escaped: '+name)
print(json.dumps({'ok':True,'plain_runtime_checked_value':14,'proof':proof,'controls_detected':detected,'native_executions':0,
                  'optimized_sha256':hashlib.sha256(optimized.encode()).hexdigest(),'raw_sha256':hashlib.sha256(raw.encode()).hexdigest()},sort_keys=True))
