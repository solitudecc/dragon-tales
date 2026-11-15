from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,time
r=Path('/home/ubuntu/vmc-runs/accepted-view-cleanup-real-B-20261115-v1-control');plan_path=Path('/home/ubuntu/vmc-runs/loop-accepted-view-cleanup-preparation-20261115-v3/ACTUAL-B-PLAN.json');outdir=Path('/home/ubuntu/vmc-runs/accepted-view-cleanup-real-B-20261115-v1');expected_plan='c0fff647988c3613d5a918a5f497128c5cf8439e1ba9f83012785129193080c1';H=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
assert H(plan_path)==expected_plan and not outdir.exists();p=json.loads(plan_path.read_text());assert len(p['lookups'])==419 and p['ready'] and p['sealed'] and not p['exact_73_table_lookup_inventory_claim']
pins=[p[k] for k in ['source','closed_report','seal_ledger','seal_status','emitter_source','emitter_executable','runtime','solver','inventory_report','inventory_helper_source']]
def stable():return H(plan_path)==expected_plan and all(Path(x['path']).stat().st_size==x['size'] and H(x['path'])==x['sha256'] for x in pins)
assert stable() and sorted(os.sched_getaffinity(0))==[0,1,2,3]
exe=Path(p['emitter_executable']['path']);assert min(shutil.disk_usage(r).free,shutil.disk_usage(exe.parent).free)>=512*1024*1024
argv=[str(exe),str(plan_path),str(outdir)];env={k:v for k,v in os.environ.items() if k not in ['LD_PRELOAD','LD_LIBRARY_PATH','DRAGON_C_LIB','LIBUNICORN_PATH']};env['LD_LIBRARY_PATH']=str(Path(p['runtime']['path']).parent)+':'+str(Path(p['solver']['path']).parent);env['LD_DEBUG']='libs,files'
start=time.monotonic();failure=None
with (r/'stdout').open('wb') as output,(r/'stderr').open('wb') as error:
 child=subprocess.Popen(argv,cwd=r,env=env,stdout=output,stderr=error,start_new_session=True)
 try:
  while child.poll() is None:
   if time.monotonic()-start>180:raise RuntimeError('180s deadline')
   if min(shutil.disk_usage(r).free,shutil.disk_usage(exe.parent).free)<64*1024*1024:raise RuntimeError('64MiB running floor')
   time.sleep(.2)
 except BaseException as exc:
  failure=str(exc)
  try:os.killpg(child.pid,signal.SIGKILL)
  except ProcessLookupError:pass
  child.wait()
receipt=dict(argv=argv,returncode=child.returncode,seconds=time.monotonic()-start,failure=failure,PGID=child.pid,allowed_cpus=sorted(os.sched_getaffinity(0)),pins_unchanged=stable(),plan_sha256=expected_plan,recipe_executions_requested=1,Guest_runs=0,inventory_scope='all419 simple i64 reads, not419 table reads')
(r/'RECEIPT.json').write_text(json.dumps(receipt,indent=2,sort_keys=True)+'\n');print(json.dumps(receipt),flush=True)
if (outdir/'REPORT.json').exists():
 report=json.loads((outdir/'REPORT.json').read_text());print(json.dumps({k:report.get(k) for k in ['ok','failure','passes_attempted','source_sha256','after_sha256','actual_parsed_ModuleID']}),flush=True)
assert failure is None and receipt['pins_unchanged'] and child.returncode==0
