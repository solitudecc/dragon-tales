#!/usr/bin/env python3
"""Independent final-IR observation. Original driver and whole state bytes stay intact."""
from pathlib import Path
import argparse,subprocess,json,hashlib,struct,os,time
HELPER=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern uint64_t dragon_observer_counts[];
extern const uint64_t dragon_observer_count;
__attribute__((destructor)) static void dump_observer(void) {
 const char *path=getenv("DRAGON_OBSERVER_OUTPUT");if(!path)_Exit(96);
 FILE *f=fopen(path,"wb");if(!f || dragon_observer_count>100000)_Exit(97);
 if(fwrite(&dragon_observer_count,8,1,f)!=1 || fwrite(dragon_observer_counts,8,dragon_observer_count,f)!=dragon_observer_count || fclose(f))_Exit(98);
}
'''
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def gate(plan,counts):
 totals={label:sum(counts[s['id']] for s in plan['sites'] if s['label']==label) for label in ('root_entry','entry_admission','entry_fallback','target_failure','business_failure','central_dispatcher')}
 repeated=[dict(id=s['id'],block=s['block'],count=counts[s['id']]) for s in plan['sites'] if s['label'] in ('target_success','business_success') and s.get('in_fast_scc') and counts[s['id']]>=2]
 base=totals['root_entry']==totals['entry_admission']==1 and all(totals[k]==0 for k in ('entry_fallback','target_failure','business_failure','central_dispatcher'))
 return dict(totals=totals,repeated_same_guard_sites=repeated,entry_and_fallback_gate=base,loop_witness=bool(plan['conditional_closure'] and base and repeated))
def main():
 p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);a=p.parse_args();r=a.root;pre=json.loads((r/'PREDECLARED.json').read_text());source=r/'frozen';out=r/'run';out.mkdir(exist_ok=False);plan=json.loads((source/'PLAN.json').read_text());assert plan['conditional_closure']
 pins={str(source/n):h for n,h in pre['files'].items()};pins.update(pre['external_pins']);assert all(sha(n)==h for n,h in pins.items());tool=pre['tool'];commands=[]
 def run(cmd,name,env=None):
  start=time.monotonic()
  with (out/(name+'.stdout')).open('w') as so,(out/(name+'.stderr')).open('w') as se:proc=subprocess.run(cmd,stdout=so,stderr=se,env=env,timeout=180)
  commands.append(dict(command=cmd,name=name,returncode=proc.returncode,seconds=time.monotonic()-start));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');assert proc.returncode==0,(name,proc.returncode)
 helper=out/'observer_dump.c';helper.write_text(HELPER);rows=[];emission_controls=0
 for mode in ('normal','double-count','omit-central'):
  ir=out/(mode+'.ll');report=out/(mode+'.json');cmd=[tool,str(source/'raw.ll'),str(source/'PLAN.json'),str(ir),str(report)]+([] if mode=='normal' else [mode]);run(cmd,mode+'-instrument');record=json.loads(report.read_text());assert record['input_ir_sha256']==sha(source/'raw.ll') and record['output_ir_sha256']==sha(ir)
  ids=[s['id'] for s in record['emitted_sites']];allids=list(range(len(plan['sites'])))
  if mode=='omit-central':
   first_central=next(s['id'] for s in plan['sites'] if s['label']=='central_dispatcher')
   assert ids!=allids and ids==[i for i in allids if i!=first_central] and record['omitted_sites']==[first_central];emission_controls+=1;continue
  assert ids==allids
  obj=out/(mode+'.o');exe=out/mode
  run(['llc-21','-O0','-filetype=obj','-mtriple=aarch64-linux-gnu',str(ir),'-o',str(obj)],mode+'-llc')
  run(['gcc','-O0','-I'+str(source/'include'),str(source/'driver.c'),str(helper),str(obj),'-o',str(exe)],mode+'-link')
  for item in plan['native_inputs']:
   i=item['index'];state=out/f'{mode}-{i}.state';countsfile=out/f'{mode}-{i}.counts';env=dict(os.environ,DRAGON_OBSERVER_OUTPUT=str(countsfile));run([str(exe),str(source/f'input-{i}.bin'),str(state)],f'{mode}-{i}',env)
   assert state.read_bytes()==(source/f'output-{i}.bin').read_bytes(),(mode,i,'full state mismatch')
   blob=countsfile.read_bytes();n,=struct.unpack('<Q',blob[:8]);assert n==len(plan['sites']) and len(blob)==8*(n+1);counts=list(struct.unpack('<'+'Q'*n,blob[8:]));result=gate(plan,counts)
   if mode=='normal':assert result['entry_and_fallback_gate'],(i,result)
   else:
    normal=next(row for row in rows if row['mode']=='normal' and row['input']==i);assert counts==[2*x for x in normal['counts']] and not result['entry_and_fallback_gate']
   rows.append(dict(mode=mode,input=i,complete_state_equal=True,counts=counts,gate=result));(out/'observations.json').write_text(json.dumps(rows,indent=2)+'\n')
 normal=[x for x in rows if x['mode']=='normal'];assert len(normal)==4 and any(x['gate']['loop_witness'] for x in normal)
 checks={n:sha(n)==h for n,h in pins.items()};assert all(checks.values());summary=dict(normal_full_state_matches=4,controlled_full_state_matches=4,count_faults_detected=4,omitted_central_emission_detected=emission_controls,loop_witness_inputs=[x['input'] for x in normal if x['gate']['loop_witness']],rows=rows,pins_unchanged=checks,native_original_recipe_repeated=False,scope='Independent G-final cffO0 view-local observation only; 4 inputs plus 4 bad-counter controls; not four-module completion.')
 (out/'SUMMARY.json').write_text(json.dumps(summary,indent=2)+'\n');print({k:v for k,v in summary.items() if k not in ('rows','pins_unchanged')})
if __name__=='__main__':main()
