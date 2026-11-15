from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,time
r=Path('/home/ubuntu/vmc-runs/accepted-view-cleanup-toy-20261115-v1');ram=Path('/dev/shm/accepted-view-cleanup-toy-20261115-v1');p=json.loads((r/'TOY-PLAN.json').read_text());sdk=Path(p['runtime']['path']);z3=Path(p['solver']['path']);H=lambda x:hashlib.sha256(Path(x).read_bytes()).hexdigest()
pins=[p[k] for k in ('toy_ir','emitter_source','test_source','runtime','solver')];pins.append(dict(path='/usr/bin/g++-14',size=1100672,sha256='857e944b567af664186d95215289a76955c537a485ba76251864f8bc87c3b41a'))
def stable():return all(Path(x['path']).stat().st_size==x['size'] and H(x['path'])==x['sha256'] for x in pins)
assert stable() and sorted(os.sched_getaffinity(0))==[0,1,2,3]
cpp=['/usr/bin/g++-14','-std=c++20','-O0','-g','-fno-rtti','-D_GNU_SOURCE','-D__STDC_CONSTANT_MACROS','-D__STDC_FORMAT_MACROS','-D__STDC_LIMIT_MACROS','-isystem','/usr/lib/llvm-21/include','-I'+str(r/'source'),'-c',str(r/'source/loop_accepted_view_cleanup_toy.cpp'),'-o',str(ram/'toy.o')]
link=['/usr/bin/g++-14',str(ram/'toy.o'),str(sdk),'-Wl,-rpath,'+str(sdk.parent),'-Wl,-rpath,'+str(z3.parent),'-Wl,-rpath-link,'+str(z3.parent),'-pthread','-ldl','-o',str(ram/'toy')]
run=[str(ram/'toy'),str(r/'TOY-PLAN.json'),str(r/'toy-output')]
commands=[('compile',cpp),('link',link),('toy',run)];receipts=[]
env={k:v for k,v in os.environ.items() if k not in ('LD_PRELOAD','LD_LIBRARY_PATH','DRAGON_C_LIB','LIBUNICORN_PATH')};env['LD_LIBRARY_PATH']=str(sdk.parent)+':'+str(z3.parent)
for name,argv in commands:
 assert stable() and min(shutil.disk_usage(r).free,shutil.disk_usage(ram).free)>=512*1024*1024
 start=time.monotonic();failure=None
 e=dict(env)
 if name=='toy':e['LD_DEBUG']='libs,files'
 with (r/(name+'.stdout')).open('wb') as out,(r/(name+'.stderr')).open('wb') as err:
  child=subprocess.Popen(argv,cwd=r,env=e,stdout=out,stderr=err,start_new_session=True)
  try:
   while child.poll() is None:
    if time.monotonic()-start>180:raise RuntimeError('180s deadline')
    if min(shutil.disk_usage(r).free,shutil.disk_usage(ram).free)<64*1024*1024:raise RuntimeError('64MiB running floor')
    time.sleep(.2)
  except BaseException as exc:
   failure=str(exc)
   try:os.killpg(child.pid,signal.SIGKILL)
   except ProcessLookupError:pass
   child.wait()
 rec=dict(name=name,argv=argv,returncode=child.returncode,seconds=time.monotonic()-start,failure=failure,pins_unchanged=stable(),PGID=child.pid,allowed_cpus=sorted(os.sched_getaffinity(0)))
 (r/(name+'.json')).write_text(json.dumps(rec,indent=2,sort_keys=True)+'\n');receipts.append(rec);(r/'COMMANDS.json').write_text(json.dumps(receipts,indent=2,sort_keys=True)+'\n');print(name,child.returncode,round(rec['seconds'],3),flush=True)
 assert child.returncode==0 and failure is None and rec['pins_unchanged']
