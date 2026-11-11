#!/usr/bin/env python3
"""Frozen B oracle plus explicit cleanup and terminal guarded transitions.

No input/comparator or inherited pass recipe is edited. The six additional
passes are recorded separately from the unchanged prefix/legacy activation.
"""
import argparse,hashlib,json,sys
from pathlib import Path
import entry_prefix_continuation_real as inherited
from guest_edge_worklist_real import validate_activation

INHERITED_SHA='d8b45cebf2497fc3d04251fda3185483b0f6df27b56605d8400305fa66cba447'
CLEANUP=['mem2reg','instcombine','gvn','control-flow','dce']
OPTIONS=dict(max_candidates=64,max_rewrites=32,max_blocks=128,max_instructions=16384)
def digest(s):return hashlib.sha256(s.encode()).hexdigest()
def save(path,value):path.write_text(json.dumps(value,indent=2)+'\n')
def check_report(report,names):
 if report.get('ok') is not True or report.get('iterations')!=1 or [r['name'] for r in report.get('runs',[])]!=names:
  raise RuntimeError('additional stage did not follow its exact single-iteration recipe')
 if any(r['status'] not in (0,2) for r in report['runs']):raise RuntimeError('additional stage unavailable')

def main():
 parser=argparse.ArgumentParser(add_help=False);parser.add_argument('--out',type=Path,required=True);args,_=parser.parse_known_args()
 if hashlib.sha256(Path(inherited.__file__).read_bytes()).hexdigest()!=INHERITED_SHA:raise RuntimeError('frozen inherited adapter changed')
 import dragon as d
 original_pipeline,original_ir=d.PassPipeline,d.LLVMLifter.ir
 records=[];seen=[];status=None
 class TailPipeline:
  def __init__(self,*a,**kw):self.inner=original_pipeline(*a,**kw);self.steps=[]
  def __enter__(self):self.inner.__enter__();return self
  def __exit__(self,*a):return self.inner.__exit__(*a)
  def register(self,*a,**kw):self.inner.register(*a,**kw);return self
  def add(self,name,**options):self.steps.append(dict(name=name,options=options));self.inner.add(name,**options);return self
  def run(self,view,**options):
   report=self.inner.run(view,**options)
   if [s['name'] for s in self.steps]==['entry-prefix']:return report
   validate_activation([dict(steps=self.steps,run_options=options)],True,expected_count=1)
   if any(view is x for x in seen):raise RuntimeError('tail stages repeated on a view')
   seen.append(view)
   manifest=view.guarded_entry_manifest();entry=int(manifest['entry'],16)
   if entry not in inherited.previous.CASE_NAMES or manifest['context_id']!='real_sp':raise RuntimeError('unexpected frozen view')
   profile,level=inherited.previous.CASE_NAMES[entry]
   directory=args.out/'guarded-transition-tail'/profile/level;directory.mkdir(parents=True,exist_ok=False)
   before=original_ir(view);(directory/'before-cleanup.ll').write_text(before)
   receipt=dict(entry=entry,profile=profile,level=level,view_symbol=manifest['view_symbol'],before_sha256=digest(before),legacy_report=report,cleanup_steps=CLEANUP,controller_options=OPTIONS,state='started',native_comparator='unchanged inherited full-state oracle')
   save(directory/'report.json',receipt);records.append(receipt)
   with original_pipeline() as cleanup:
    for name in CLEANUP:cleanup.add(name)
    prep=cleanup.run(view,max_iterations=1,max_pass_runs=5)
   check_report(prep,CLEANUP);prepared=original_ir(view);(directory/'prepared.ll').write_text(prepared)
   receipt.update(cleanup_report=prep,prepared_sha256=digest(prepared),state='prepared');save(directory/'report.json',receipt)
   with original_pipeline() as transitions:
    transitions.add('guarded-transitions',**OPTIONS)
    final=transitions.run(view,max_iterations=1,max_pass_runs=1)
   check_report(final,['guarded-transitions']);after=original_ir(view);(directory/'after.ll').write_text(after)
   receipt.update(controller_report=final,after_sha256=digest(after),state='finished',inherited_pass_runs=1+len(report['runs']),additional_pass_runs=len(prep['runs'])+len(final['runs']),total_pass_runs=1+len(report['runs'])+len(prep['runs'])+len(final['runs']))
   if receipt['total_pass_runs']!=31:raise RuntimeError('total pass count changed')
   save(directory/'report.json',receipt)
   return report # The original report remains the original three-round report.
 try:
  d.PassPipeline=TailPipeline
  status=inherited.main()
  if len(records)!=4 or any(x['state']!='finished' for x in records):raise RuntimeError('missing completed tail records')
 finally:
  d.PassPipeline=original_pipeline
  if args.out.exists():save(args.out/'guarded-transition-tail-activation.json',dict(status=status,records=records,driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),inherited_driver_sha256=INHERITED_SHA,recipe='frozen B prefix+3 legacy rounds, then 5 cleanup passes, then 1 controller, no post-controller cleanup',inherited_activation_scope='existing reports retain their 25 inherited runs; six additional runs are recorded here',native_comparator_changed=False))
 return status
if __name__=='__main__':raise SystemExit(main())
