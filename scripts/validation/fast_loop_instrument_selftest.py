#!/usr/bin/env python3
"""Independent exact-count controls for the validation-only LLVM observer.
Run on the authorized VM: --tool instrumenter --output fresh-directory.
All commands, outputs and return codes are retained; never compiles Dragon.
"""
import argparse, hashlib, json, os, struct, subprocess
from pathlib import Path

IR = r'''
target datalayout = "e-p:64:64"
define i64 @observed(i64 %bound, ptr %guest) {
entry:
  %admit = icmp ne i64 %bound, 0
  br i1 %admit, label %loop, label %reject
reject:
  ret i64 0
loop:
  %i = phi i64 [0, %entry], [%next, %latch]
  %value = load i64, ptr %guest, align 1
  %same = icmp eq i64 %value, 7
  br i1 %same, label %small, label %central
small:
  %p = getelementptr i8, ptr %guest, i64 8
  %actual = load i64, ptr %p, align 1
  switch i64 %actual, label %central [ i64 11, label %latch
                                      i64 13, label %latch ]
central:
  %selector = phi i64 [%value, %loop], [%actual, %small]
  switch i64 %selector, label %latch [ i64 7, label %latch
                                      i64 9, label %latch
                                      i64 11, label %latch
                                      i64 13, label %latch ]
latch:
  %out = getelementptr i8, ptr %guest, i64 16
  %old = load i64, ptr %out, align 1
  %updated = add i64 %old, %value
  store i64 %updated, ptr %out, align 1
  %next = add i64 %i, 1
  %again = icmp ult i64 %next, %bound
  br i1 %again, label %loop, label %done
done:
  ret i64 %updated
}
'''
DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern uint64_t observed(uint64_t,void*);
#ifdef OBSERVER
extern uint64_t dragon_observer_counts[];
extern const uint64_t dragon_observer_count;
#endif
int main(int argc,char **argv){
 if(argc!=5)return 90;
 uint64_t bound=strtoull(argv[1],0,0),memory[3]={strtoull(argv[2],0,0),strtoull(argv[3],0,0),5};
 uint64_t result=observed(bound,memory);
 FILE *f=fopen(argv[4],"wb");if(!f)return 91;
 fwrite(&result,8,1,f);fwrite(memory,8,3,f);
#ifdef OBSERVER
 fwrite(&dragon_observer_count,8,1,f);fwrite(dragon_observer_counts,8,dragon_observer_count,f);
#endif
 return fclose(f)?92:0;
}
'''

def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--tool',required=True);parser.add_argument('--output',required=True);parser.add_argument('--llc',default='llc-21');parser.add_argument('--cc',default='clang-21');a=parser.parse_args()
    out=Path(a.output);out.mkdir(parents=True,exist_ok=False)
    source=out/'source.ll';source.write_text(IR);driver=out/'driver.c';driver.write_text(DRIVER)
    sites=[]
    def site(block,kind,label,**kwargs):sites.append(dict(id=len(sites),block=block,kind=kind,label=label,**kwargs))
    site('entry','visit','root_entry');site('entry','branch','entry_admission',slot=0);site('reject','visit','entry_fallback')
    site('loop','branch','target_success',slot=0);site('loop','branch','target_failure',slot=1)
    site('small','membership','business_success',values=[11,13],member=True);site('small','membership','business_failure',values=[11,13],member=False)
    site('central','visit','central_dispatcher')
    plan=dict(schema='final-ir-loop-observation-v1',function='observed',input_ir_sha256=digest(source),central_blocks=['central'],sites=sites)
    planfile=out/'PLAN.json';planfile.write_text(json.dumps(plan,indent=2)+'\n')
    logs=[]
    def run(command,label,success=True):
        p=subprocess.run([str(x) for x in command],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        (out/(label+'.stdout')).write_bytes(p.stdout);(out/(label+'.stderr')).write_bytes(p.stderr);(out/(label+'.exit')).write_text(str(p.returncode)+'\n')
        logs.append(dict(command=[str(x) for x in command],label=label,returncode=p.returncode));(out/'commands.json').write_text(json.dumps(logs,indent=2)+'\n')
        if success and p.returncode:raise RuntimeError(f'{label} failed: {p.returncode}')
        return p.returncode
    modes=['plain','normal','double-count','omit-central'];emissions={}
    for mode in modes:
        ir=source
        if mode!='plain':
            ir=out/(mode+'.ll');report=out/(mode+'.json');cmd=[a.tool,source,planfile,ir,report]
            if mode!='normal':cmd.append(mode)
            run(cmd,mode+'-instrument');emissions[mode]=json.loads(report.read_text())
            expected=list(range(8)) if mode!='omit-central' else list(range(7))
            assert [x['id'] for x in emissions[mode]['emitted_sites']]==expected
            assert emissions[mode]['input_ir_sha256']==digest(source)
            assert emissions[mode]['output_ir_sha256']==digest(ir)
        obj=out/(mode+'.o');exe=out/mode
        run([a.llc,'-O0','-filetype=obj',ir,'-o',obj],mode+'-llc')
        run([a.cc,'-O0','-DIGNORE=0',*(['-DOBSERVER'] if mode!='plain' else []),driver,obj,'-o',exe],mode+'-link')
    records=[];faults={'double-count':0,'omit-central':0};normal=0
    for case,(bound,first,second) in enumerate((b,x,y) for b in [0,1,2,5] for x in [7,9] for y in [11,13,17]):
        success=bound if first==7 else 0;failure=bound-success;member=success if second in (11,13) else 0
        expected=[1,int(bound!=0),int(bound==0),success,failure,member,success-member,failure+success-member]
        expected_state=(5+bound*first if bound else 0,first,second,5+bound*first)
        for mode in modes:
            result=out/f'{mode}-{case}.bin';run([out/mode,str(bound),str(first),str(second),result],f'{mode}-{case}')
            data=result.read_bytes();state=struct.unpack('<4Q',data[:32]);assert state==expected_state,(mode,case,state,expected_state)
            if mode=='plain':assert len(data)==32;continue
            n,=struct.unpack('<Q',data[32:40]);assert n==8;counts=list(struct.unpack('<8Q',data[40:]));match=counts==expected
            if mode=='normal':assert match,(case,counts,expected);normal+=1
            elif mode=='double-count':assert counts==[2*x for x in expected] and not match;faults[mode]+=1
            else:
                assert counts==expected[:7]+[0]
                if expected[7]:assert not match;faults[mode]+=1
            records.append(dict(mode=mode,case=case,input=[bound,first,second],expected=expected,actual=counts,full_state_equal=True,count_oracle_equal=match))
    # Invalid plans must reject before creating any instrumented output.
    badplans=[]
    for kind in ['sha','membership','central','duplicate-id']:
        bad=json.loads(json.dumps(plan))
        if kind=='sha':bad['input_ir_sha256']='0'*64
        if kind=='membership':bad['sites'][5]['values']=[11,99]
        if kind=='central':bad['central_blocks']=[]
        if kind=='duplicate-id':bad['sites'][1]['id']=0
        p=out/f'bad-{kind}.json';p.write_text(json.dumps(bad));ir=out/f'bad-{kind}.ll'
        code=run([a.tool,source,p,ir,out/f'bad-{kind}-report.json'],'bad-'+kind,success=False)
        assert code and not ir.exists();badplans.append(kind)
    # Re-instrumentation must fail even with an honestly updated source hash.
    repeated=json.loads(json.dumps(plan));repeated['input_ir_sha256']=digest(out/'normal.ll')
    repeatplan=out/'repeat-plan.json';repeatplan.write_text(json.dumps(repeated))
    repeatout=out/'repeat-output.ll';sourcehash=digest(out/'normal.ll')
    code=run([a.tool,out/'normal.ll',repeatplan,repeatout,out/'repeat-report.json'],'repeat',success=False)
    assert code and not repeatout.exists() and digest(out/'normal.ll')==sourcehash
    # Reject attributes whose effects would become stale after host writes.
    attr_cases={
      'function-memory':IR.replace('define i64 @observed(i64 %bound, ptr %guest) {','define i64 @observed(i64 %bound, ptr %guest) memory(argmem: readwrite) {'),
      'function-speculatable':IR.replace('define i64 @observed(i64 %bound, ptr %guest) {','define i64 @observed(i64 %bound, ptr %guest) speculatable {'),
      'indirect-call-memory':IR+"\ndefine i64 @caller(ptr %callback, ptr %guest) {\nentry:\n %v = call i64 %callback(i64 2, ptr %guest) memory(argmem: readwrite)\n ret i64 %v\n}\n",
      'direct-call-memory':IR+"\ndefine i64 @caller(ptr %guest) {\nentry:\n %v = call i64 @observed(i64 2, ptr %guest) memory(argmem: readwrite)\n ret i64 %v\n}\n",
    }
    for label,text in attr_cases.items():
        src=out/(label+'.ll');src.write_text(text);before=digest(src)
        p=json.loads(json.dumps(plan));p['input_ir_sha256']=before;pf=out/(label+'-plan.json');pf.write_text(json.dumps(p))
        dest=out/(label+'-output.ll');code=run([a.tool,src,pf,dest,out/(label+'-report.json')],label,success=False)
        assert code and not dest.exists() and digest(src)==before
        assert 'restrictive effect attributes' in (out/(label+'.stderr')).read_text() or 'unsupported memory/speculatable attributes' in (out/(label+'.stderr')).read_text()
    assert digest(source)==plan['input_ir_sha256']
    summary=dict(attribute_rejections=list(attr_cases),repeat_instrumentation_rejected=True,input_unchanged=True,normal_observations=normal,all_modes_full_state_comparisons=4*normal,fault_detections=faults,structural_fault_emission_detected=True,invalid_plans_rejected=badplans,records=records,tool_sha256=digest(Path(a.tool)),source_sha256=digest(source),plan_sha256=digest(planfile),note='Observation controls only; no Dragon transform or real-program coverage claimed.')
    (out/'SUMMARY.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k!='records'},indent=2))
if __name__=='__main__':main()
