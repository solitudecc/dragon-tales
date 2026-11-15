from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,time
r=Path('/home/ubuntu/vmc-runs/accepted-view-cleanup-emitter-build-20261115-v1');ram=Path('/dev/shm/accepted-view-cleanup-emitter-build-20261115-v1');p=json.loads((r/'BUILD-PLAN.json').read_text());sdk=Path(p['runtime']['path']);z=Path(p['solver']['path']);H=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
pins=[p[k] for k in ('source','runtime','solver')]+[dict(path='/usr/bin/g++-14',size=1100672,sha256='857e944b567af664186d95215289a76955c537a485ba76251864f8bc87c3b41a')]
stable=lambda: all(Path(x['path']).stat().st_size==x['size'] and H(x['path'])==x['sha256'] for x in pins)
assert stable() and sorted(os.sched_getaffinity(0))==[0,1,2,3]
commands=[('compile',['/usr/bin/g++-14','-std=c++20','-O0','-g','-fno-rtti','-D_GNU_SOURCE','-D__STDC_CONSTANT_MACROS','-D__STDC_FORMAT_MACROS','-D__STDC_LIMIT_MACROS','-isystem','/usr/lib/llvm-21/include','-c',str(p['source']['path']),'-o',str(ram/'emitter.o')],0),('link',['/usr/bin/g++-14',str(ram/'emitter.o'),str(sdk),'-Wl,-rpath,'+str(sdk.parent),'-Wl,-rpath,'+str(z.parent),'-Wl,-rpath-link,'+str(z.parent),'-pthread','-ldl','-o',str(ram/'emitter')],0),('standalone-not-ready',[str(ram/'emitter'),str(r/'not-ready-plan.json'),str(r/'not-ready-output')],1)]
env={k:v for k,v in os.environ.items() if k not in ('LD_LIBRARY_PATH','LD_PRELOAD','DRAGON_C_LIB','LIBUNICORN_PATH')};env['LD_LIBRARY_PATH']=str(sdk.parent)+':'+str(z.parent);receipts=[]
for name,argv,expected in commands:
 assert stable() and min(shutil.disk_usage(r).free,shutil.disk_usage(ram).free)>=512*1024*1024
 e=dict(env)
 if name=='standalone-not-ready':e['LD_DEBUG']='libs,files'
 start=time.monotonic();failure=None;maps=[]
 with (r/(name+'.stdout')).open('wb') as out,(r/(name+'.stderr')).open('wb') as err:
  child=subprocess.Popen(argv,cwd=r,env=e,stdout=out,stderr=err,start_new_session=True)
  try:
   while child.poll() is None:
    if time.monotonic()-start>180:raise RuntimeError('180s deadline')
    if min(shutil.disk_usage(r).free,shutil.disk_usage(ram).free)<64*1024*1024:raise RuntimeError('64MiB running floor')
    if name=='standalone-not-ready':
     try:
      lines=Path('/proc/'+str(child.pid)+'/maps').read_text().splitlines();relevant=[x for x in lines if 'libdragon.so' in x or 'libz3.so' in x or 'libLLVM' in x]
      if any(str(sdk) in x for x in relevant) and any(str(z) in x for x in relevant):maps=relevant
     except FileNotFoundError:pass
    time.sleep(.001 if name=='standalone-not-ready' else .2)
  except BaseException as exc:
   failure=str(exc)
   try:os.killpg(child.pid,signal.SIGKILL)
   except ProcessLookupError:pass
   child.wait()
 rec=dict(name=name,argv=argv,returncode=child.returncode,expected_returncode=expected,seconds=time.monotonic()-start,failure=failure,pins_unchanged=stable(),PGID=child.pid,allowed_cpus=sorted(os.sched_getaffinity(0)))
 if name=='standalone-not-ready':
  rec['proc_maps_captured']=bool(maps);(r/'ACTUAL-PROC-MAPS.json').write_text(json.dumps(dict(PID=child.pid,lines=maps,runtime_sha256=p['runtime']['sha256'],solver_sha256=p['solver']['sha256'],captured=bool(maps)),indent=2,sort_keys=True)+'\n')
 (r/(name+'.json')).write_text(json.dumps(rec,indent=2,sort_keys=True)+'\n');receipts.append(rec);(r/'COMMANDS.json').write_text(json.dumps(receipts,indent=2,sort_keys=True)+'\n');print(name,child.returncode,round(rec['seconds'],3),flush=True)
 assert child.returncode==expected and failure is None and rec['pins_unchanged']
 if name=='standalone-not-ready':
  report=json.loads((r/'not-ready-output/REPORT.json').read_text());assert report['passes_attempted']==0 and not report['ok'];assert maps and not any('libLLVM' in x for x in maps)
(r/'PRODUCTS.json').write_text(json.dumps({str(ram/name):dict(size=(ram/name).stat().st_size,sha256=H(ram/name)) for name in ['emitter.o','emitter']},indent=2,sort_keys=True)+'\n')
