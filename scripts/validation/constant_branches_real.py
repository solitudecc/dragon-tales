#!/usr/bin/env python3
"""Frozen business B oracle plus an explicit tiny tail and literal CFG pass."""
import argparse,hashlib,json,sys
from pathlib import Path
import guarded_business_real as previous
PREVIOUS_SHA='64402dbe389c3943c69065b7070d5052c1c705af3e4152e78a3dbc671febdb05'
TINY=['mem2reg','instcombine','gvn','instcombine','dce']
FOLD=dict(max_folds=4096,max_instructions=1000000,max_blocks=65536)
def save(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def sha(s):return hashlib.sha256(s.encode()).hexdigest()
def check(report,names):
 if report.get('ok') is not True or report.get('iterations')!=1 or [x['name'] for x in report.get('runs',[])]!=names:raise RuntimeError('D stage recipe drift')
 if any(x['status'] not in (0,2) for x in report['runs']):raise RuntimeError('D stage unavailable')
def main():
 parser=argparse.ArgumentParser(add_help=False);parser.add_argument('--out',type=Path,required=True);args,_=parser.parse_known_args()
 if hashlib.sha256(Path(previous.__file__).read_bytes()).hexdigest()!=PREVIOUS_SHA:raise RuntimeError('frozen business adapter changed')
 import dragon as d
 native_pipeline,native_ir=d.PassPipeline,d.LLVMLifter.ir
 records=[];seen=[];status=None
 class FinalPipeline:
  def __init__(self,*a,**kw):self.inner=native_pipeline(*a,**kw);self.steps=[]
  def __enter__(self):self.inner.__enter__();return self
  def __exit__(self,*a):return self.inner.__exit__(*a)
  def register(self,*a,**kw):self.inner.register(*a,**kw);return self
  def add(self,name,**options):self.steps.append(dict(name=name,options=options));self.inner.add(name,**options);return self
  def run(self,view,**options):
   report=self.inner.run(view,**options)
   if [s['name'] for s in self.steps]!=['guarded-transitions']:return report
   if self.steps[0]['options']!=previous.PROFILES['cover512'] or options!=dict(max_iterations=1,max_pass_runs=1):raise RuntimeError('D tail not attached to frozen C controller')
   if any(view is x for x in seen):raise RuntimeError('D tail repeated');
   seen.append(view);manifest=view.guarded_entry_manifest();entry=int(manifest['entry'],16)
   profile,level=previous.previous.inherited.previous.CASE_NAMES[entry]
   directory=args.out/'constant-branch-tail'/profile/level;directory.mkdir(parents=True,exist_ok=False)
   before=native_ir(view);(directory/'before-tiny.ll').write_text(before)
   receipt=dict(entry=entry,profile=profile,level=level,view_symbol=manifest['view_symbol'],before_sha256=sha(before),business_controller_report=report,tiny_steps=TINY,constant_options=FOLD,state='started')
   records.append(receipt);save(directory/'report.json',receipt)
   with native_pipeline() as pipeline:
    for name in TINY:pipeline.add(name)
    tiny=pipeline.run(view,max_iterations=1,max_pass_runs=5)
   check(tiny,TINY);prepared=native_ir(view);(directory/'before-fold.ll').write_text(prepared)
   receipt.update(state='tiny-finished',tiny_report=tiny,before_fold_sha256=sha(prepared));save(directory/'report.json',receipt)
   with native_pipeline() as pipeline:
    pipeline.add('constant-branches',**FOLD);fold=pipeline.run(view,max_iterations=1,max_pass_runs=1)
   check(fold,['constant-branches']);after=native_ir(view);(directory/'after.ll').write_text(after)
   receipt.update(state='finished',constant_report=fold,after_sha256=sha(after),inherited_pass_runs=31,additional_pass_runs=6,total_pass_runs=37);save(directory/'report.json',receipt)
   return report # Inherited stage reports keep their own scope and counters.
 try:
  d.PassPipeline=FinalPipeline;status=previous.main()
  if len(records)!=4 or any(x['state']!='finished' for x in records):raise RuntimeError('missing D records')
 finally:
  d.PassPipeline=native_pipeline
  if args.out.exists():save(args.out/'constant-branch-tail-activation.json',dict(status=status,records=records,driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),previous_driver_sha256=PREVIOUS_SHA,recipe='unchanged C cover512 recipe31 runs; five tiny scalar/memory passes then one literal constant-branches pass; total37',inherited_activation_scope='inherited31 runs remain in old reports; six D runs recorded here',native_comparator_changed=False))
 return status
if __name__=='__main__':raise SystemExit(main())
