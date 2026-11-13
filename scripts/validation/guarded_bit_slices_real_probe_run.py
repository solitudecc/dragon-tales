#!/usr/bin/env python3
"""One declared bit-slice pass per four sealed59 inputs; no target-native run."""
import argparse,hashlib,json,os,shutil,signal,subprocess,time
from pathlib import Path

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,x):
 if p.exists():raise RuntimeError('refuse to overwrite '+str(p))
 p.write_text(json.dumps(x,indent=2)+'\n')
ENV=None

def execute(command,prefix):
 start=time.monotonic();reason=None
 with prefix.with_name(prefix.name+'.stdout').open('x') as so,prefix.with_name(prefix.name+'.stderr').open('x') as se:
  process=subprocess.Popen(command,stdout=so,stderr=se,env=ENV,start_new_session=True)
  while process.poll() is None:
   if time.monotonic()-start>180:reason='original180second-timeout'
   if shutil.disk_usage(prefix.parent).free<67108864:reason='original64MiB-running-floor'
   if reason:
    os.killpg(process.pid,signal.SIGKILL);process.wait();break
   time.sleep(0.2)
 result=dict(command=command,returncode=process.returncode,reason=reason,elapsed_seconds=time.monotonic()-start,LD_LIBRARY_PATH=ENV['LD_LIBRARY_PATH'])
 save(prefix.with_name(prefix.name+'.execution.json'),result)
 return result

def alias_evidence(source,target):
 source,target=Path(source),Path(target)
 a,b=source.read_bytes(),target.read_bytes()
 if a!=b or not source.samefile(target):raise RuntimeError('alias bytes/inode differ')
 s=source.stat();t=target.stat()
 return dict(source=str(source),target=str(target),device=s.st_dev,inode=s.st_ino,nlink=s.st_nlink,size=len(a),sha256=hashlib.sha256(a).hexdigest(),target_sha256=hashlib.sha256(b).hexdigest(),target_device=t.st_dev,target_inode=t.st_ino)
def bits_import(args,directory,inputfile,source_prefix,row):
 # This is the unchanged independently validated pure-LLVM reference. It
 # compares its parse-render expectation with the actual inherited V frame.
 expected=directory/'bits-import';command=[str(args.import_expectation_probe),str(inputfile),str(source_prefix),str(expected)]
 result=execute(command,directory/'bits-import')
 row['bits_import_command']=command;row['bits_import_returncode']=result['returncode'];row['bits_import_execution']=result
 if result['returncode'] not in (0,5) or result['reason']:raise RuntimeError('independent checked import parse/verify failed')
 code=result['returncode']
 report=json.loads(expected.with_suffix('.json').read_text())
 if report['passes_executed']!=0 or report['target_native_executions']!=0 or report['mutation_control']!='none':raise RuntimeError('reference import has unexpected execution scope')
 if not report['JSON_exact_independent_parse_render']:raise RuntimeError('source V inventory differs from independent import; C not executed')
 import_aliases=[]
 if code==0:
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
 row['bits_import_report']=report
 row['bits_import_exact_alias_evidence']=import_aliases
 return expected.with_name(expected.name+'.expected.ll'),expected.with_name(expected.name+'.expected.json')

def main():
 global ENV
 parser=argparse.ArgumentParser();parser.add_argument('--prepared',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);parser.add_argument('--runtime',type=Path,required=True);parser.add_argument('--native-gates-archive',type=Path,required=True);parser.add_argument('--native-gates-sha256',required=True);parser.add_argument('--root-go',type=Path,required=True);args=parser.parse_args()
 if args.out.exists():raise RuntimeError('refuse to overwrite declared diagnostic')
 prepared=json.loads((args.prepared/'PREPARED-NOT-EXECUTED.json').read_text());ready=json.loads((args.prepared/'COMPILED-READY-v1.json').read_text())
 if ready['runtime_manifest_sha256']!=sha(args.runtime/'MANIFEST.json'):raise RuntimeError('runtime manifest differs')
 if sha(args.native_gates_archive)!=args.native_gates_sha256:raise RuntimeError('root-reviewed native gate archive differs')
 go=json.loads(args.root_go.read_text())
 if go.get('schema')!='guarded-bit-slices-real60-go-v1' or go.get('profile')!=60 or go.get('scope')!='four-checked59-plus-bits1-diagnostic':raise RuntimeError('wrong explicit root scope')
 if go.get('candidate_execution_authorized') is not True or go.get('native_main_passed') is not True or go.get('native_noz3_passed') is not True:raise RuntimeError('runtime/native diagnostic GO not established')
 if go.get('max_candidate_commands')!=4 or go.get('native_executions')!=0 or go.get('full_replays')!=0:raise RuntimeError('GO changes diagnostic execution scope')
 if go.get('runtime_manifest_sha256')!=sha(args.runtime/'MANIFEST.json') or go.get('runtime_backend_sha256')!=sha(args.runtime/'libdragon.so'):raise RuntimeError('formal runtime differs from root SHA')
 if go.get('prepared_sha256')!=sha(args.prepared/'PREPARED-NOT-EXECUTED.json') or go.get('ready_sha256')!=sha(args.prepared/'COMPILED-READY-v1.json'):raise RuntimeError('preparation/binary freeze differs from root GO')
 if go.get('native_archive_sha256')!=args.native_gates_sha256:raise RuntimeError('native archive differs from root GO')
 pin_go=dict(path=str(args.root_go),sha256=sha(args.root_go),document=go)

 if shutil.disk_usage(args.out.parent).free<536870912:raise RuntimeError('original512MiB-start-gate')
 ENV=dict(os.environ,LD_LIBRARY_PATH=str(args.runtime));args.import_expectation_probe=args.prepared/'parse-import'
 for item in ready['source_pins']:
  if sha(item['path'])!=item['sha256']:raise RuntimeError('frozen source changed')
 for item in ready['binaries']:
  if sha(item['path'])!=item['sha256']:raise RuntimeError('frozen binary changed')
 runtime=json.loads((args.runtime/'MANIFEST.json').read_text())
 for name,row in runtime['files'].items():
  if sha(args.runtime/name)!=row['sha256']:raise RuntimeError('runtime payload differs')
 args.out.mkdir(parents=True)
 save(args.out/'PREDECLARED.json',dict(schema='bit-slices-only-real60-diagnostic-v1',recipe='already executed sealed59 C.final + guarded-bit-slices once',additional_passes=1,cases=prepared['cases'],source_pins=ready['source_pins'],binary_pins=ready['binaries'],runtime=runtime,LD_LIBRARY_PATH=ENV['LD_LIBRARY_PATH'],native_gate_archive=dict(path=str(args.native_gates_archive),sha256=sha(args.native_gates_archive)),budget=dict(max_candidates=64,max_rewrites=32,max_blocks=128,max_instructions=16384),command_timeout_seconds=180,start_free_minimum=536870912,running_floor=67108864,target_native_executions=0,full_replays=0,root_GO=pin_go,archive_dereference_required=True))
 rows=[]
 for source in prepared['cases']:
  case=source['case'];directory=args.out/case;directory.mkdir();row=dict(case=case,state='started',candidate_pass_attempts=0)
  if sha(args.root_go)!=pin_go['sha256']:raise RuntimeError('root GO changed between cases')
  try:
   if shutil.disk_usage(directory).free<536870912:raise RuntimeError('original512MiB-case-start-gate')
   for item in ready['source_pins']+ready['binaries']:
    if sha(item['path'])!=item['sha256']:raise RuntimeError('case frozen source/binary differs')
   for key in ['source59_raw','source59_inventory','source59_manifest','source59_binding','audit_v2_raw','audit_v2_inventory','original_D37_anchor','inherited_V_predictions']:
    item=source[key]
    if sha(item['path'])!=item['sha256']:raise RuntimeError('case input differs: '+key)
   for name,item in runtime['files'].items():
    if sha(args.runtime/name)!=item['sha256']:raise RuntimeError('case runtime differs')
   inputfile=Path(source['source59_raw']['path']);source_prefix=inputfile.with_suffix('')
   expected_ir,expected_inventory=bits_import(args,directory,inputfile,source_prefix,row)
   prefix=directory/'bits60';command=[str(args.prepared/'probe-bits'),str(inputfile),str(expected_ir),str(expected_inventory),source['source59_inventory']['path'],str(prefix)]
   row['candidate_pass_attempts']=1;result=execute(command,directory/'bits');row['bits_execution']=result
   if result['returncode'] or result['reason']:raise RuntimeError('bit-slice command failed')
   verify=json.loads(prefix.with_name(prefix.name+'.verify.json').read_text());report=json.loads(prefix.with_name(prefix.name+'.bits-report.json').read_text());bindings=json.loads(prefix.with_name(prefix.name+'.bindings.json').read_text())
   if not verify['after'] or not verify['import_checked_before_candidate'] or verify['additional_runs']!=1 or verify['total_runs']!=60:raise RuntimeError('recipe verify differs')
   if not report['ok'] or report['iterations']!=1 or [r['name'] for r in report['runs']]!=['guarded-bit-slices']:raise RuntimeError('bit-slice pass report differs')
   if bindings['schema']!='guarded-bit-slices-step60-v1':raise RuntimeError('wrong producer protocol')
   aliases=[alias_evidence(item['source'],item['target']) for item in bindings['exact_byte_hardlinks']]
   save(directory/'EXACT-ALIAS-EVIDENCE.json',dict(aliases=aliases,archive_dereference_required=True))
   row.update(state='finished',bits_report=report,bindings=bindings,alias_evidence=aliases)
   for key in ['source59_raw','source59_inventory','source59_manifest','source59_binding','audit_v2_raw','audit_v2_inventory','original_D37_anchor','inherited_V_predictions']:
    if sha(source[key]['path'])!=source[key]['sha256']:raise RuntimeError('case input changed after execution')
  except Exception as e:row.update(state='failed',error=str(e))
  save(directory/'RESULT.json',row);rows.append(row);print(case,row['state'],row.get('error',''),flush=True)
  if shutil.disk_usage(directory).free<536870912:
   save(args.out/'STOP-DISK-START-GATE.json',dict(completed_cases=len(rows),free_bytes=shutil.disk_usage(directory).free));break
 save(args.out/'EXECUTION-RESULT.json',dict(rows=rows,attempted_cases=len(rows),candidate_commands=sum(r['candidate_pass_attempts'] for r in rows),native_executions=0,full_replays=0,strict_repaired_v2_audit_pending=True,no_hidden_retry=True,old58_T3_and_59_failures_preserved=True))
 return 0 if len(rows)==4 and all(r['state']=='finished' for r in rows) else 1
if __name__=='__main__':raise SystemExit(main())
