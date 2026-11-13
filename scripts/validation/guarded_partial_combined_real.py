#!/usr/bin/env python3
"""Frozen D full-state oracle plus explicit F, tiny cleanup, constant branches and E passes."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
import constant_branches_real as previous
PREVIOUS_SHA='f4c35425046fd1e5882ef4042ef9e2853130de15af92f2bfcdfe2fdc772b5449'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,x):p.write_text(json.dumps(x,indent=2)+'\n')
def main():
 parser=argparse.ArgumentParser(add_help=False);parser.add_argument('--combined-probe',type=Path,required=True);parser.add_argument('--combined-hints',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);args,rest=parser.parse_known_args()
 if sha(previous.__file__)!=PREVIOUS_SHA:raise RuntimeError('frozen D adapter changed')
 import dragon as d
 original_pipeline,original_ir=d.PassPipeline,d.LLVMLifter.ir
 records=[];cache=[];oldargv=sys.argv;status=None
 def final_ir(view):
  for known,text in cache:
   if view is known:return text
  return original_ir(view)
 class CombinedFinalPipeline:
  def __init__(self,*a,**kw):self.inner=original_pipeline(*a,**kw);self.steps=[]
  def __enter__(self):self.inner.__enter__();return self
  def __exit__(self,*a):return self.inner.__exit__(*a)
  def register(self,*a,**kw):self.inner.register(*a,**kw);return self
  def add(self,name,**options):self.steps.append(dict(name=name,options=options));self.inner.add(name,**options);return self
  def run(self,view,**options):
   report=self.inner.run(view,**options)
   if [s['name'] for s in self.steps]!=['constant-branches']:return report
   if self.steps[0]['options']!=previous.FOLD or options!=dict(max_iterations=1,max_pass_runs=1):raise RuntimeError('E stage not attached to frozen D')
   if any(view is known for known,_ in cache):raise RuntimeError('E stage repeated')
   manifest=view.guarded_entry_manifest();entry=int(manifest['entry'],16)
   case,level=previous.previous.previous.inherited.previous.CASE_NAMES[entry]
   directory=args.out/'partial-combined-tail'/case/level;directory.mkdir(parents=True,exist_ok=False)
   inputfile=directory/'D-final.ll';inputfile.write_text(original_ir(view))
   hintfile=args.combined_hints/(case+'-'+level+'.json');hint=json.loads(hintfile.read_text())
   if hint['function']!=manifest['view_symbol']:raise RuntimeError('hint view mismatch')
   command=[str(args.combined_probe),'--full',str(inputfile),str(hintfile),str(directory/'module')]
   row=dict(case=case,level=level,input_sha256=sha(inputfile),hint_sha256=sha(hintfile),probe_sha256=sha(args.combined_probe),command=command,inherited_pass_runs=37,additional_pass_runs=19,total_pass_runs=56,state='started');records.append(row);save(directory/'activation.json',row)
   with (directory/'stdout').open('w') as so,(directory/'stderr').open('w') as se:process=subprocess.run(command,stdout=so,stderr=se,timeout=180)
   row['returncode']=process.returncode;save(directory/'activation.json',row)
   if process.returncode:raise RuntimeError('E public pass driver failed')
   verify=json.loads((directory/'module.verify.json').read_text());result=json.loads((directory/'module.tail-E-report.json').read_text())
   if not verify['after'] or not verify['entry_manifest_unchanged'] or not result['ok'] or result['iterations']!=1 or [r['name'] for r in result['runs']]!=['guarded-target-sets']:raise RuntimeError('E recipe/verification failed')
   for stage,names in [('F',['guarded-state-corridors']),('H',['guarded-store-corridors']),('H-resume',['guarded-store-corridors']),('tiny',['mem2reg','instcombine','gvn','instcombine','dce']),('constant',['constant-branches']),('base-E',['guarded-target-sets']),('K',['invariant-self-loops']),('J',['guarded-partial-corridors']),('tail-tiny',['mem2reg','instcombine','gvn','instcombine','dce']),('tail-constant',['constant-branches'])]:
    stage_report=json.loads((directory/('module.'+stage+'-report.json')).read_text())
    if not stage_report['ok'] or stage_report['iterations']!=1 or [r['name'] for r in stage_report['runs']]!=names:raise RuntimeError('F tail recipe mismatch '+stage)
    row[stage+'_report']=stage_report
   text=(directory/'module.after.ll').read_text();cache.append((view,text));row.update(state='finished',after_sha256=sha(directory/'module.after.ll'),report=result);save(directory/'activation.json',row)
   return report # Original D pipeline retains its own public report.
 try:
  d.PassPipeline=CombinedFinalPipeline;d.LLVMLifter.ir=final_ir
  sys.argv=[previous.__file__]+rest+['--out',str(args.out)];status=previous.main()
  if len(records)!=4 or any(r['state']!='finished' for r in records):raise RuntimeError('missing E stages')
 finally:
  d.PassPipeline=original_pipeline;d.LLVMLifter.ir=original_ir;sys.argv=oldargv
  if args.out.exists():save(args.out/'partial-combined-tail-activation.json',dict(status=status,records=records,driver_sha256=sha(__file__),previous_driver_sha256=PREVIOUS_SHA,recipe='frozen D37 followed by original47 tail then K1+J1+tiny5+constant1+E1;56 total',module_delivery='independent verified parsed output supplied as final IR to unchanged native compiler/comparator; source modules and original in-memory D view not replaced',native_comparator_changed=False))
 return status
if __name__=='__main__':raise SystemExit(main())
