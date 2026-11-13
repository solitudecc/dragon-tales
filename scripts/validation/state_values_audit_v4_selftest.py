#!/usr/bin/env python3
"""Audit regressions over separately verified SSA; no target/native execution."""
import argparse,copy,gzip,hashlib,json,sys
from pathlib import Path
import guarded_state_values_manifest_audit_v4 as state
import guarded_state_values_set_cycles_v4_audit as cycles


def main():
 parser=argparse.ArgumentParser();parser.add_argument('--fixtures',type=Path,default=Path(__file__).resolve().parents[2]/'tests/fixtures/state-values-audit-v4');parser.add_argument('--output',type=Path);args=parser.parse_args();root=args.fixtures
 manifest=json.loads((root/'MANIFEST.json').read_text())
 for name,pin in manifest['files'].items():
  data=(root/name).read_bytes();assert len(data)==pin['size'] and hashlib.sha256(data).hexdigest()==pin['sha256']
 def load(name):return json.loads(gzip.decompress((root/(name+'.json.gz')).read_bytes()))
 hints=json.loads((root/'hints.json').read_text());positive=0;controls=[]
 def refuse(name,action):
  try:action()
  except (AssertionError,KeyError,ValueError):controls.append(name)
  else:raise AssertionError('accepted fault: '+name)
 historical=load('historical-selector')
 refuse('historical-loop-phi-sets',lambda:cycles.sets(historical,historical))
 blocks,items,defs=state.tables(historical)
 refuse('historical-loop-phi-selector',lambda:state.selected_selector(defs,'%selector','fast','fallback'))
 direct={'%edge':dict(opcode='phi',block='fallback',incoming=[dict(block='fast',value='%fresh'),dict(block='fast',value='%fresh')]),'%fresh':dict(opcode='load',block='fast')}
 assert state.selected_selector(direct,'%edge','fast','fallback')=='%fresh';positive+=1
 refuse('foreign-phi-block',lambda:state.selected_selector(direct,'%edge','fast','other'))
 wrong=copy.deepcopy(direct);wrong['%edge']['incoming'][1]['value']='%other'
 refuse('disagreeing-duplicate-phi-input',lambda:state.selected_selector(wrong,'%edge','fast','fallback'))
 for mode in ('plain','duplicate'):
  before,after=load(mode+'-before'),load(mode+'-after')
  result,witnesses=state.projected(before,after,hints);assert len(witnesses)==2
  assert {w['seed']:w['state_fallback'] for w in witnesses}==dict(seedA='fallbackA',seedB='fallbackB')
  assert all(w['selected_clone_path'] for w in witnesses);positive+=1
  reverse,rev=state.projected(before,after,list(reversed(hints)));assert reverse==result and {w['seed'] for w in rev}=={'seedA','seedB'};positive+=1
  for fault in ('state-reread','missing-freeze','double-freeze','wrong-predicate','wrong-original-direction','skip-selected-root','wrong-load-address','changed-load-alignment','wrong-phi-value','wrong-new-exit-phi-value','missing-phi-slot','omitted-CFG-edge','duplicate-slot-id','missing-hint','duplicate-hint','foreign-function'):
   bad=copy.deepcopy(after);bs,rows,_=state.tables(bad);ds={i['operand']:i for items in rows.values() for i in items if i['operand'].startswith('%')};hs=copy.deepcopy(hints)
   if fault=='state-reread':
    i=ds['%eA'];i['operands'][0].update(operand='%b',typed='i64 %b');i['instruction']=i['instruction'].replace('i64 %a,','i64 %b,')
   elif fault=='missing-freeze':
    br=rows['seedA'][-1];br['condition']='%eA';br['operands'][0].update(operand='%eA',typed='i1 %eA');br['instruction']=br['instruction'].replace('%fA','%eA');bs['seedA']['terminator']=br['instruction'];rows['seedA'].remove(next(i for i in rows['seedA'] if i['operand']=='%fA'))
   elif fault=='double-freeze':
    freeze=copy.deepcopy(ds['%fA']);freeze['operand']='%double';freeze['instruction']='  %double = freeze i1 %eA';rows['seedA'].insert(-2,freeze);ds['%fA']['operands'][0].update(operand='%double',typed='i1 %double');ds['%fA']['instruction']='  %fA = freeze i1 %double'
   elif fault=='wrong-predicate':ds['%cA']['predicate']='ule';ds['%cA']['instruction']=ds['%cA']['instruction'].replace('ugt','ule')
   elif fault=='wrong-original-direction':
    br=rows['fallbackA'][-1];br['operands'][1],br['operands'][2]=br['operands'][2],br['operands'][1];bs['fallbackA']['successors'][0]['target'],bs['fallbackA']['successors'][1]['target']=bs['fallbackA']['successors'][1]['target'],bs['fallbackA']['successors'][0]['target'];br['instruction']='  br i1 %cA, label %rootA, label %shared';bs['fallbackA']['terminator']=br['instruction']
   elif fault=='skip-selected-root':
    br=rows['gateA'][-1];br['operands'][0].update(operand='%cloneDispatchA',typed='label %cloneDispatchA');br['instruction']='  br label %cloneDispatchA';bs['gateA']['terminator']=br['instruction'];bs['gateA']['successors'][0]['target']='cloneDispatchA'
   elif fault=='wrong-load-address':
    ds['%cloadA']['operands'][0].update(operand='%tp',typed='ptr %tp');ds['%cloadA']['instruction']=ds['%cloadA']['instruction'].replace('ptr %table','ptr %tp')
   elif fault=='changed-load-alignment':ds['%cloadA']['instruction']=ds['%cloadA']['instruction'].replace('align 1','align 8')
   elif fault=='wrong-phi-value':next(v for v in ds['%answer']['incoming'] if v['block']=='fallbackB').update(value='%a',typed='i64 %a')
   elif fault=='wrong-new-exit-phi-value':next(v for v in ds['%answer']['incoming'] if v['block']=='targetFallbackA').update(value='%a',typed='i64 %a')
   elif fault=='missing-phi-slot':ds['%answer']['incoming'].pop()
   elif fault=='omitted-CFG-edge':bs['rootB']['successors']=[]
   elif fault=='duplicate-slot-id':bs['entry']['successors'][1]['slot']=0
   elif fault=='missing-hint':hs.pop()
   elif fault=='duplicate-hint':hs.append(copy.deepcopy(hs[0]))
   else:hs[1]['function']='other'
   refuse(mode+':'+fault,lambda:state.projected(before,bad,hs))
 result=dict(positive_cases=positive,negative_controls=len(controls),controls=controls,fixtures_sha256=hashlib.sha256((root/'MANIFEST.json').read_bytes()).hexdigest(),native_executions=0,deobfuscation_passes=0,scope='Synthetic LLVM-verifier-valid structures and corrupted inventory controls; not real program semantics or loop execution evidence')
 if args.output:args.output.write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result,indent=2));return 0

if __name__=='__main__':raise SystemExit(main())
