#!/usr/bin/env python3
"""Independent profile59: unchanged D37 + base19 + mask1 + value1 + checked1."""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
import constant_branches_real as previous
PREVIOUS_SHA='f4c35425046fd1e5882ef4042ef9e2853130de15af92f2bfcdfe2fdc772b5449'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,x):p.write_text(json.dumps(x,indent=2)+'\n')
def alias_evidence(source,target):
 source,target=Path(source),Path(target)
 a,b=source.read_bytes(),target.read_bytes()
 if a!=b or not source.samefile(target):raise RuntimeError('alias bytes/inode differ')
 s=source.stat();t=target.stat()
 return dict(source=str(source),target=str(target),device=s.st_dev,inode=s.st_ino,nlink=s.st_nlink,size=len(a),sha256=hashlib.sha256(a).hexdigest(),target_sha256=hashlib.sha256(b).hexdigest(),target_device=t.st_dev,target_inode=t.st_ino)
def validate_full_gate(args):
 gate=json.loads(args.validation_gates.read_text())
 if gate.get('schema')!='guarded-checked-real59-gates-v1' or gate.get('scope')!='fresh-full59' or gate.get('profile')!=59:raise RuntimeError('fresh full59 validation gate missing')
 for key in ('native_main_passed','native_noz3_passed','strict_checked_structure_all_four_passed'):
  if gate.get(key) is not True:raise RuntimeError('required validated gate not passed: '+key)
 if sorted(gate.get('cases',[]))!=['f-cff-O0','f-cff-O2','f-combo-O0','f-combo-O2']:raise RuntimeError('full59 gate has different input scope')
 evidence=gate.get('evidence',[])
 if len(evidence)<3:raise RuntimeError('native and strict structure evidence references missing')
 for row in evidence:
  if sha(row['path'])!=row['sha256']:raise RuntimeError('validation evidence changed: '+row['path'])
 for key,path in [('base19',args.combined_probe),('mask_value',args.mask_value_probe),('checked',args.checked_probe),('import',args.import_expectation_probe)]:
  if sha(path)!=gate['producer_pins'][key]:raise RuntimeError('frozen producer differs: '+key)
 for key in ('runtime_backend','runtime_c_shim'):
  if sha(gate[key]['path'])!=gate[key]['sha256']:raise RuntimeError('frozen runtime differs: '+key)
 if os.environ.get('DRAGON_C_LIB')!=gate['runtime_c_shim']['path']:raise RuntimeError('Python C shim is not the declared runtime')
 first_ld=os.environ.get('LD_LIBRARY_PATH','').split(':')[0]
 if Path(first_ld).resolve()!=Path(gate['runtime_backend']['path']).parent.resolve():raise RuntimeError('backend search path is not the declared runtime')
 parent=args.out
 while not parent.exists():parent=parent.parent
 if shutil.disk_usage(parent).free<1073741824:raise RuntimeError('full59 start requires at least 1GiB free')
 return gate

def checked_import(args,directory,inputfile,source_prefix,row):
 # This is the unchanged independently validated pure-LLVM reference. It
 # compares its parse-render expectation with the actual inherited V frame.
 expected=directory/'checked-import';command=[str(args.import_expectation_probe),str(inputfile),str(source_prefix),str(expected)]
 with (directory/'checked-import.stdout').open('w') as so,(directory/'checked-import.stderr').open('w') as se:
  process=subprocess.run(command,stdout=so,stderr=se,timeout=180)
 row['checked_import_command']=command;row['checked_import_returncode']=process.returncode
 if process.returncode not in (0,5):raise RuntimeError('independent checked import parse/verify failed')
 report=json.loads(expected.with_suffix('.json').read_text())
 if report['passes_executed']!=0 or report['target_native_executions']!=0 or report['mutation_control']!='none':raise RuntimeError('reference import has unexpected execution scope')
 if not report['JSON_exact_independent_parse_render']:raise RuntimeError('source V inventory differs from independent import; C not executed')
 import_aliases=[]
 if process.returncode==0:
  if not report['IR_exact_independent_parse_render']:raise RuntimeError('reference returncode disagrees')
  for suffix in ('.ll','.json'):
   source=source_prefix.with_name(source_prefix.name+suffix);target=expected.with_name(expected.name+'.expected'+suffix)
   if target.exists():raise RuntimeError('refuse to replace expectation')
   os.link(source,target)
   import_aliases.append(alias_evidence(source,target))
 else:
  if report['IR_exact_independent_parse_render']:raise RuntimeError('reference returncode disagrees')
  # Keep the raw textual mismatch and the independently emitted expectation.
  # No text or metadata ID is normalized. The complete inventory must match.
  for suffix in ('.ll','.json'):
   if not expected.with_name(expected.name+'.expected'+suffix).is_file():raise RuntimeError('missing independently emitted expectation')
  source=source_prefix.with_name(source_prefix.name+'.json');target=expected.with_name(expected.name+'.expected.json')
  if source.read_bytes()!=target.read_bytes():raise RuntimeError('reference inventory full bytes differ')
  temporary=target.with_name(target.name+'.dedup-tmp')
  if temporary.exists():raise RuntimeError('refuse to replace dedup temporary')
  os.link(source,temporary);os.replace(temporary,target)
  import_aliases.append(alias_evidence(source,target))
 row['checked_import_report']=report
 row['checked_import_exact_alias_evidence']=import_aliases
 return expected.with_name(expected.name+'.expected.ll'),expected.with_name(expected.name+'.expected.json')

def main():
 parser=argparse.ArgumentParser(add_help=False);parser.add_argument('--combined-probe',type=Path,required=True);parser.add_argument('--mask-value-probe',type=Path,required=True);parser.add_argument('--value-hints',type=Path,required=True);parser.add_argument('--combined-hints',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);parser.add_argument('--checked-probe',type=Path,required=True);parser.add_argument('--import-expectation-probe',type=Path,required=True);parser.add_argument('--validation-gates',type=Path,required=True);args,rest=parser.parse_known_args()
 if sha(previous.__file__)!=PREVIOUS_SHA:raise RuntimeError('frozen D adapter changed')
 gate=validate_full_gate(args)
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
   directory=args.out/'checked-corridor-tail'/case/level;directory.mkdir(parents=True,exist_ok=False)
   inputfile=directory/'D-final.ll';inputfile.write_text(original_ir(view))
   hintfile=args.combined_hints/(case+'-'+level+'.json');hint=json.loads(hintfile.read_text())
   if hint['function']!=manifest['view_symbol']:raise RuntimeError('hint view mismatch')
   command=[str(args.combined_probe),'--full',str(inputfile),str(hintfile),str(directory/'base56')]
   row=dict(case=case,level=level,input_sha256=sha(inputfile),hint_sha256=sha(hintfile),probe_sha256=sha(args.combined_probe),command=command,inherited_pass_runs=37,base_additional_pass_runs=19,mask_additional_pass_runs=1,value_additional_pass_runs=1,checked_additional_pass_runs=1,total_pass_runs=59,state='started');records.append(row);save(directory/'activation.json',row)
   with (directory/'stdout').open('w') as so,(directory/'stderr').open('w') as se:process=subprocess.run(command,stdout=so,stderr=se,timeout=180)
   row['returncode']=process.returncode;save(directory/'activation.json',row)
   if process.returncode:raise RuntimeError('E public pass driver failed')
   verify=json.loads((directory/'base56.verify.json').read_text());result=json.loads((directory/'base56.tail-E-report.json').read_text())
   if not verify['after'] or not verify['entry_manifest_unchanged'] or not result['ok'] or result['iterations']!=1 or [r['name'] for r in result['runs']]!=['guarded-target-sets']:raise RuntimeError('E recipe/verification failed')
   for stage,names in [('F',['guarded-state-corridors']),('H',['guarded-store-corridors']),('H-resume',['guarded-store-corridors']),('tiny',['mem2reg','instcombine','gvn','instcombine','dce']),('constant',['constant-branches']),('base-E',['guarded-target-sets']),('K',['invariant-self-loops']),('J',['guarded-partial-corridors']),('tail-tiny',['mem2reg','instcombine','gvn','instcombine','dce']),('tail-constant',['constant-branches'])]:
    stage_report=json.loads((directory/('base56.'+stage+'-report.json')).read_text())
    if not stage_report['ok'] or stage_report['iterations']!=1 or [r['name'] for r in stage_report['runs']]!=names:raise RuntimeError('F tail recipe mismatch '+stage)
    row[stage+'_report']=stage_report
   value_hint=args.value_hints/(case+'-'+level+'.json')
   value_command=[str(args.mask_value_probe),str(directory/'base56.after.ll'),str(value_hint),str(directory/'value58')]
   row.update(value_hint_sha256=sha(value_hint),mask_value_probe_sha256=sha(args.mask_value_probe),value_command=value_command);save(directory/'activation.json',row)
   with (directory/'value.stdout').open('w') as so,(directory/'value.stderr').open('w') as se:value_process=subprocess.run(value_command,stdout=so,stderr=se,timeout=180)
   row['value_returncode']=value_process.returncode;save(directory/'activation.json',row)
   if value_process.returncode:raise RuntimeError('value-stage public driver failed')
   value_verify=json.loads((directory/'value58.verify.json').read_text())
   if not value_verify['after'] or not value_verify['entry_manifest_unchanged'] or value_verify['additional_runs']!=2 or value_verify['total_runs']!=58:raise RuntimeError('value recipe verification failed')
   for stage,names in [('M',['guarded-partial-masks']),('V',['guarded-state-values'])]:
    stage_report=json.loads((directory/('value58.'+stage+'-report.json')).read_text())
    if not stage_report['ok'] or stage_report['iterations']!=1 or [r['name'] for r in stage_report['runs']]!=names:raise RuntimeError('value recipe mismatch '+stage)
    row['value_'+stage+'_report']=stage_report
   bindings=json.loads((directory/'value58.bindings.json').read_text())
   if bindings['schema']!='partial-masks-state-values-profile58-v1' or not bindings['archive_dereference_required']:raise RuntimeError('stage frame bindings missing')
   expected_stems={'mask_before':'mask.before','mask_after':'mask.after','value_before':'value.before','value_after':'value.after','final':'after'}
   if any(bindings[key]!=str(directory/('value58.'+suffix)) for key,suffix in expected_stems.items()):raise RuntimeError('unexpected stage frame path')
   if bindings['source_input']!=str(directory/'base56.after.ll') or bindings['value_hint_input']!=str(value_hint):raise RuntimeError('stage input binding differs')
   for phase in ('mask_before','mask_after','value_before','value_after'):
    stem=Path(bindings[phase])
    for suffix in ('.ll','.json'):
     if not stem.with_name(stem.name+suffix).is_file():raise RuntimeError('missing independent phase frame')
   for linked in bindings['exact_byte_hardlinks']:
    source,target=Path(linked['source']),Path(linked['target'])
    if not source.samefile(target) or sha(source)!=sha(target):raise RuntimeError('snapshot hardlink identity differs')
   row['stage_bindings']=bindings
   row['V_input_sha256']=sha(directory/'value58.value.before.ll')
   row['M_input_sha256']=sha(directory/'value58.mask.before.ll')
   row['M_output_sha256']=sha(directory/'value58.mask.after.ll')
   if sha(directory/'value58.value.after.ll')!=sha(directory/'value58.after.ll'):raise RuntimeError('V/final output binding differs')
   checked_input=directory/'value58.after.ll';source_inventory=directory/'value58.value.after.json'
   expected_ir,expected_inventory=checked_import(args,directory,checked_input,directory/'value58.value.after',row)
   save(directory/'activation.json',row)
   checked_command=[str(args.checked_probe),str(checked_input),str(expected_ir),str(expected_inventory),str(source_inventory),str(directory/'checked59')]
   row['checked_command']=checked_command;row['checked_probe_sha256']=sha(args.checked_probe);save(directory/'activation.json',row)
   with (directory/'checked.stdout').open('w') as so,(directory/'checked.stderr').open('w') as se:
    checked_process=subprocess.run(checked_command,stdout=so,stderr=se,timeout=180)
   row['checked_returncode']=checked_process.returncode;save(directory/'activation.json',row)
   if checked_process.returncode:raise RuntimeError('checked-stage public driver failed')
   checked_verify=json.loads((directory/'checked59.verify.json').read_text());checked_report=json.loads((directory/'checked59.checked-report.json').read_text())
   if not checked_verify['after'] or not checked_verify['entry_manifest_unchanged'] or checked_verify['additional_runs']!=1 or checked_verify['total_runs']!=59 or not checked_verify['import_checked_before_candidate']:raise RuntimeError('checked recipe verification failed')
   if not checked_report['ok'] or checked_report['iterations']!=1 or [r['name'] for r in checked_report['runs']]!=['guarded-checked-corridors']:raise RuntimeError('checked pass recipe changed')
   cb=json.loads((directory/'checked59.bindings.json').read_text())
   if cb['schema']!='guarded-checked-step59-v2' or cb['source_input']!=str(checked_input) or cb['source58_inventory']!=str(source_inventory) or not cb['archive_dereference_required']:raise RuntimeError('checked stage bindings missing')
   for key,suffix in [('checked_before','checked.before'),('checked_after','checked.after'),('final','after')]:
    if cb[key]!=str(directory/('checked59.'+suffix)):raise RuntimeError('checked stage path differs')
   checked_aliases=[]
   for linked in cb['exact_byte_hardlinks']:
    source,target=Path(linked['source']),Path(linked['target'])
    if not source.samefile(target) or sha(source)!=sha(target):raise RuntimeError('checked snapshot alias differs')
    checked_aliases.append(alias_evidence(source,target))
   row['checked_exact_alias_evidence']=checked_aliases
   if sha(directory/'checked59.checked.before.ll')!=sha(expected_ir) or sha(directory/'checked59.checked.before.json')!=sha(expected_inventory) or sha(expected_inventory)!=sha(source_inventory):raise RuntimeError('checked actual input frame mismatch')
   if sha(directory/'checked59.checked.after.ll')!=sha(directory/'checked59.after.ll'):raise RuntimeError('checked final output differs')
   row['checked_stage_bindings']=cb;row['checked_report']=checked_report;row['checked_before_sha256']=sha(directory/'checked59.checked.before.ll');row['checked_after_sha256']=sha(directory/'checked59.checked.after.ll')
   text=(directory/'checked59.after.ll').read_text();cache.append((view,text));row.update(state='finished',after_sha256=sha(directory/'checked59.after.ll'),report=result);save(directory/'activation.json',row)
   return report # Original D pipeline retains its own public report.
 try:
  d.PassPipeline=CombinedFinalPipeline;d.LLVMLifter.ir=final_ir
  sys.argv=[previous.__file__]+rest+['--out',str(args.out)];status=previous.main()
  if len(records)!=4 or {(r['case'],r['level']) for r in records}!={('f-cff','O0'),('f-cff','O2'),('f-combo','O0'),('f-combo','O2')} or any(r['state']!='finished' for r in records):raise RuntimeError('missing full59 stages')
 finally:
  d.PassPipeline=original_pipeline;d.LLVMLifter.ir=original_ir;sys.argv=oldargv
  if args.out.exists():save(args.out/'checked-corridor-tail-activation.json',dict(status=status,records=records,driver_sha256=sha(__file__),previous_driver_sha256=PREVIOUS_SHA,recipe='frozen D37 + frozen base19 + unchanged frozen mask1+V1 producer + checked1;59 total',validation_gate_sha256=sha(args.validation_gates),validation_gate=gate,module_delivery='independent verified parsed output supplied as final IR to unchanged native compiler/comparator; source modules and original in-memory D view not replaced',native_comparator_changed=False))
 return status
if __name__=='__main__':raise SystemExit(main())
