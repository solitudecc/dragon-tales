#!/usr/bin/env python3
"""Narrow pure-intrinsic admission and unchanged observer effect rejections."""
import argparse,hashlib,json,struct,subprocess
from pathlib import Path
MASK=(1<<64)-1
BASE='''target datalayout = "e-p:64:64"
declare i64 @llvm.bswap.i64(i64) nocallback nounwind speculatable memory(none)
define i64 @observed(i64 %input, ptr %guest) {
entry:
  %v = call i64 @llvm.bswap.i64(i64 %input)ATTRS
  %old = load i64, ptr %guest, align 1
  %next = xor i64 %old, %v
  store i64 %next, ptr %guest, align 1
  ret i64 %next
}
'''
DRIVER='''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern uint64_t observed(uint64_t,void*);
#ifdef OBSERVER
extern uint64_t dragon_observer_counts[];
extern const uint64_t dragon_observer_count;
#endif
int main(int n,char**v){if(n!=4)return 90;uint64_t x=strtoull(v[1],0,0),a[4]={strtoull(v[2],0,0),1,UINT64_MAX,1ULL<<63};uint64_t z=observed(x,a);if(fwrite(&z,8,1,stdout)!=1||fwrite(a,8,4,stdout)!=4)return 91;
#ifdef OBSERVER
if(fwrite(&dragon_observer_count,8,1,stdout)!=1||fwrite(dragon_observer_counts,8,dragon_observer_count,stdout)!=dragon_observer_count)return 92;
#endif
return 0;}
'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--tool',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--expect-old-rejection',action='store_true');a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=False);commands=[];records=[]
 def run(label,args,ok=True):
  r=subprocess.run([str(x) for x in args],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180);(out/(label+'.stdout')).write_bytes(r.stdout);(out/(label+'.stderr')).write_bytes(r.stderr);(out/(label+'.exit')).write_text(str(r.returncode)+'\n');commands.append({'label':label,'command':[str(x) for x in args],'exit':r.returncode,'timeout':180});(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  if ok:assert r.returncode==0,(label,r.returncode,r.stderr)
  return r
 def plan(src,label):
  pf=out/(label+'-plan.json');pf.write_text(json.dumps({'schema':'final-ir-loop-observation-v1','function':'observed','input_ir_sha256':sha(src),'central_blocks':[],'sites':[{'id':0,'block':'entry','kind':'visit','label':'root_entry'}]},indent=2)+'\n');return pf
 driver=out/'driver.c';driver.write_text(DRIVER);positive=[];negative=[]
 for name,attrs in (('inherited',''),('explicit',' nocallback speculatable memory(none)')):
  src=out/(name+'.ll');src.write_text(BASE.replace('ATTRS',attrs));sourcehash=sha(src);pf=plan(src,name);dest=out/(name+'-observed.ll');report=out/(name+'-report.json');r=run(name+'-instrument',[a.tool,src,pf,dest,report],False)
  if a.expect_old_rejection:
   assert r.returncode==2 and not dest.exists() and not report.exists() and b'callsite has restrictive effect attributes' in r.stderr;positive.append(name);continue
  assert r.returncode==0,(name,r.stderr);run(name+'-verify',['opt-21','-passes=verify','-disable-output',dest]);canonical=out/(name+'-canonical.ll');run(name+'-canonical',['opt-21','-passes=verify','-S',src,'-o',canonical])
  before=canonical.read_text();after=dest.read_text()
  def relevant(text):return [line.strip() for line in text.splitlines() if '@llvm.bswap.i64' in line or line.startswith('attributes #')]
  assert relevant(before)==relevant(after),(name,relevant(before),relevant(after));assert 'nocallback' in after and 'speculatable' in after and 'memory(none)' in after
  d=json.loads(report.read_text());assert d['declared_sites']==1 and len(d['emitted_sites'])==1 and d['guest_loads_added']==0 and not d['cfg_changed'] and not d['optimized_after_instrumentation'] and d['input_ir_sha256']==sourcehash
  for variant,module in (('plain',src),('counted',dest)):
   obj=out/(name+'-'+variant+'.o');exe=out/(name+'-'+variant);run(name+'-'+variant+'-llc',['llc-21','-O0','-filetype=obj',module,'-o',obj]);run(name+'-'+variant+'-link',['clang-21','-O0',*(['-DOBSERVER'] if variant=='counted' else []),driver,obj,'-o',exe])
  for x in (0,1,255,1<<31,1<<63,MASK,0x0102030405060708,0x8000000000000001):
   for old in (0,1,MASK):
    expected=((old^int.from_bytes(x.to_bytes(8,'little'),'big')),old^int.from_bytes(x.to_bytes(8,'little'),'big'),1,MASK,1<<63);frames={}
    for variant in ('plain','counted'):
     label=name+'-'+variant+'-'+str(len(records));r=run(label,[out/(name+'-'+variant),str(x),str(old),'unused']);frames[variant]=r.stdout;assert r.stdout[:40]==struct.pack('<5Q',*expected),(name,variant,x,old)
     assert len(r.stdout)==(40 if variant=='plain' else 56)
     if variant=='counted':assert struct.unpack('<2Q',r.stdout[40:])==(1,1)
    assert frames['plain']==frames['counted'][:40];records.append({'mode':name,'input':x,'guest0':old,'state_equal':True,'count':1})
  assert sha(src)==sourcehash;positive.append(name)
 base=BASE.replace('ATTRS','')
 readonly='''target datalayout = "e-p:64:64"
declare i64 @llvm.bswap.i64(i64) nocallback nounwind speculatable memory(none)
define i64 @observed(i64 %input, ptr %guest) {
entry:
 %v = call i64 @llvm.bswap.i64(i64 %input)
 ret i64 %v
}
define i64 @caller(i64 %x, ptr %g) {
entry:
 %v = call i64 @observed(i64 %x, ptr %g) memory(none)
 ret i64 %v
}
'''
 cases={
 'selected-function-memory':base.replace('define i64 @observed(i64 %input, ptr %guest) {','define i64 @observed(i64 %input, ptr %guest) memory(argmem: readwrite) {'),
 'readonly-selected-caller':readonly,
 'defined-pure-helper':base+'\ndefine i64 @helper(i64 %x) memory(none) {\nentry:\n ret i64 %x\n}\n',
 'external-pure-declaration':base.replace('declare i64 @llvm.bswap.i64(i64) nocallback nounwind speculatable memory(none)','declare i64 @opaque_pure(i64) nocallback nounwind speculatable memory(none)').replace('@llvm.bswap.i64','@opaque_pure'),
 'effectful-intrinsic':base.replace('declare i64 @llvm.bswap.i64(i64) nocallback nounwind speculatable memory(none)','declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture readonly, i64, i1 immarg) nocallback nounwind memory(argmem: readwrite)').replace('  %v = call i64 @llvm.bswap.i64(i64 %input)','  call void @llvm.memcpy.p0.p0.i64(ptr %guest, ptr %guest, i64 0, i1 false)\n  %v = add i64 %input, 0')}
 for name,text in cases.items():
  src=out/(name+'.ll');src.write_text(text);h=sha(src);run(name+'-verify',['opt-21','-passes=verify','-disable-output',src]);dest=out/(name+'-observed.ll');report=out/(name+'-report.json');r=run(name+'-instrument',[a.tool,src,plan(src,name),dest,report],False);assert r.returncode==2 and not dest.exists() and not report.exists() and sha(src)==h,(name,r.returncode,r.stderr)
  assert b'restrictive effect attributes' in r.stderr or b'unsupported memory/speculatable attributes' in r.stderr;negative.append(name)
 summary={'old_rejection_expected':a.expect_old_rejection,'positive_cases':positive,'negative_cases':negative,'native_state_pairs':len(records),'native_executions':2*len(records),'records':records,'tool_sha256':sha(a.tool),'selftest_sha256':sha(Path(__file__)),'attribute_bytes_preserved':not a.expect_old_rejection,'all_group_timeouts_seconds':180};(out/'SUMMARY.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({k:v for k,v in summary.items() if k!='records'}))
if __name__=='__main__':main()
