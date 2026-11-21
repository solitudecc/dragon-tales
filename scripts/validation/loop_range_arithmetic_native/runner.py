"""Owned new math oracle: exact prior I/O controls; staged before before after."""
from pathlib import Path
import hashlib,importlib.util,json,lzma,os,shutil,signal,struct,subprocess,time
ROOT=Path(__file__).resolve().parent
PLAN=json.loads((ROOT/'RUN-PLAN.json').read_text());RAM=Path(PLAN['ram'])
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
def put(p,v):Path(p).write_text(json.dumps(v,indent=2,sort_keys=True)+'\n')
def load(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
model=load('arithmetic_model',ROOT/'source.py');shared=load('frozen_complete_comparator',ROOT/'shared/dispatch_corridor_native.py')
def stable():return all(Path(x['path']).stat().st_size==x['size'] and sha(x['path'])==x['sha256'] for x in PLAN['pins'])
assert stable() and sorted(os.sched_getaffinity(0))==[0,1,2,3]
env={k:v for k,v in os.environ.items() if k not in ('LD_PRELOAD','LD_LIBRARY_PATH','DRAGON_C_LIB','LIBUNICORN_PATH')};env['PYTHONDONTWRITEBYTECODE']='1';commands=[];records=[]
def run(label,argv,data=None,extra=None):
 assert min(shutil.disk_usage(ROOT).free,shutil.disk_usage(RAM).free)>=536870912
 start=time.monotonic();failure=None
 with (ROOT/(label+'.stdout')).open('xb') as out,(ROOT/(label+'.stderr')).open('xb') as err:
  p=subprocess.Popen([str(x) for x in argv],cwd=ROOT,env=dict(env,**(extra or {})),stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,stdout=out,stderr=err,start_new_session=True)
  if data is not None:
   import threading
   def write():
    try:p.stdin.write(data);p.stdin.close()
    except BrokenPipeError:pass
   writer=threading.Thread(target=write);writer.start()
  try:
   while p.poll() is None:
    if time.monotonic()-start>180:raise RuntimeError('180s deadline')
    if min(shutil.disk_usage(ROOT).free,shutil.disk_usage(RAM).free)<67108864:raise RuntimeError('64MiB floor')
    time.sleep(.02)
   if data is not None:writer.join()
  except BaseException as exc:
   failure=str(exc)
   try:os.killpg(p.pid,signal.SIGKILL)
   except ProcessLookupError:pass
   p.wait()
 d=dict(label=label,argv=[str(x) for x in argv],returncode=p.returncode,seconds=time.monotonic()-start,failure=failure,cpus=sorted(os.sched_getaffinity(0)))
 commands.append(d);put(ROOT/'COMMANDS.json',commands);print(label,p.returncode,round(d['seconds'],3),flush=True);assert failure is None and p.returncode==0,d
 return (ROOT/(label+'.stdout')).read_bytes()
cc=PLAN['cc'];cxx=PLAN['cxx'];llc=PLAN['llc'];sdk=Path(PLAN['runtime']['path']);z=Path(PLAN['solver']['path']);raw=ROOT/'raw';inc=ROOT/'include'
rawplan=json.loads((raw/'PLAN.json').read_text());inputs=[(raw/f'input-{t}.bin').read_bytes() for t in range(16)]
for x in rawplan['files']:assert (raw/x['relative_path']).stat().st_size==x['size'] and sha(raw/x['relative_path'])==x['sha256']
for case in model.CASES:
 for t,b in enumerate(inputs):assert b==model.initial(t) and (raw/f'{case}.expected-{t}.bin').read_bytes()==model.specification(b,case)[0]
api_env=dict(LD_PRELOAD='',LD_LIBRARY_PATH=str(sdk.parent)+':'+str(z.parent),LD_DEBUG='files')
def compile_producer(before):
 p=RAM/('before-producer' if before else 'after-producer');label='before-producer' if before else 'after-producer'
 args=[cxx,'-std=c++20','-O0','-fno-rtti','-I'+str(inc),'-isystem','/usr/lib/llvm-21/include']
 if before:args+=['-DBEFORE_ONLY']
 run(label+'-compile',args+[ROOT/'producer.cpp',sdk,'-Wl,-rpath,'+str(sdk.parent),'-Wl,-rpath,'+str(z.parent),'-Wl,-rpath-link,'+str(z.parent),'-pthread','-ldl','-o',p])
 syms=run(label+'-imports',['/usr/bin/nm','-D','-C','--undefined-only',p]).decode();assert ('runLoopImageTableContext(' in syms)==(not before)
 return p
beforeproducer=compile_producer(True)
for case in model.CASES:
 run('parse-'+case,[beforeproducer,raw/(case+'.before.ll'),ROOT/(case+'.parsed.ll'),ROOT/(case+'.before-parse.json'),case],extra=api_env)
 d=json.loads((ROOT/(case+'.before-parse.json')).read_text());assert d['api_calls']==0 and d['verified'] and d['identity_outside_exact']

def compile_llvm(ir,opt,tag):
 o=RAM/(tag+'.o');p=RAM/(tag+'.native');run(tag+'-llc',[llc,'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic','-'+opt,ir,'-o',o]);run(tag+'-link',[cc,'-'+opt,raw/'wrapper.c',raw/'callee.c',o,'-o',p]);return p

def fault_rule(case,variant,t):
 rows=[x for x in rawplan['faults'] if x['case']==case and x['name']==variant]
 return bool(rows) and t in rows[0]['specified_trials']
def observe(phase,opt,case,variant,engine,exe,stream):
 data=run(f'{phase}-{opt}-{case}-{variant}-{engine}-run',[exe],b''.join(inputs));assert len(data)==16*model.FRAME
 for t,b in enumerate(inputs):
  wanted,cov=model.specification(b,case);actual=data[t*model.FRAME:(t+1)*model.FRAME];match=actual==wanted;specified=fault_rule(case,variant,t) if phase=='fault' else False
  controls=shared.controls(actual,wanted,model) if phase!='fault' else 0
  row=dict(phase=phase,opt=opt,case=case,variant=variant,engine=engine,trial=t,match=match,controls=controls,specified=specified,coverage=cov,sha256={k:hashlib.sha256(v).hexdigest() for k,v in [('input',b),('expected',wanted),('actual',actual)]})
  encoded=json.dumps(row,sort_keys=True).encode();stream.write(struct.pack('<I',len(encoded))+encoded+b+wanted+actual);records.append(row)
  assert (not match if specified else match),row
  if phase!='fault':assert controls==len(model.CONTROLS),row

def audit(phase,opt,path):
 shared.single_xz_eof(path);rows=[];keys=set()
 with lzma.open(path,'rb') as stream:
  while True:
   prefix=stream.read(4)
   if not prefix:break
   assert len(prefix)==4;n=struct.unpack('<I',prefix)[0];assert 0<n<8192
   encoded=stream.read(n);assert len(encoded)==n;d=json.loads(encoded);parts=[stream.read(model.FRAME) for _ in range(3)];assert all(len(b)==model.FRAME for b in parts)
   b,wanted,actual=parts;key=(d['case'],d['variant'],d['engine'],d['trial']);assert key not in keys;keys.add(key)
   ref,cov=model.specification(b,d['case']);assert b==model.initial(d['trial']) and wanted==ref and cov==d['coverage'];match=actual==wanted;specified=fault_rule(d['case'],d['variant'],d['trial']) if phase=='fault' else False
   controls=shared.controls(actual,wanted,model) if phase!='fault' else 0
   assert d['phase']==phase and d['opt']==opt and d['match']==match and d['specified']==specified and d['controls']==controls
   assert d['sha256']=={k:hashlib.sha256(v).hexdigest() for k,v in [('input',b),('expected',wanted),('actual',actual)]};assert (not match if specified else match)
   rows.append(d)
 if phase=='fault':expected={(x['case'],x['name'],'LLVM',t) for x in rawplan['faults'] for t in range(16)}
 else:expected={(case,'source',engine,t) for case in model.CASES for engine in (('C','LLVM') if phase=='before' else ('LLVM',)) for t in range(16)}
 assert keys==expected and rows==[r for r in records if r['phase']==phase and r['opt']==opt]
 put(ROOT/f'{phase}-{opt}-AUDIT.json',dict(passed=True,frames=len(rows),saved_sha256=sha(path),keys_complete=True,single_XZ_EOF=True,whole_frame=model.FRAME,new_executions=0))

# All independent C/LLVM calibration and bad-IR controls precede product API.
for opt in ('O0','O2'):
 path=ROOT/('before-'+opt+'-frames.xz')
 with lzma.open(path,'xb',preset=1) as stream:
  for case in model.CASES:
   p=RAM/(case+'-reference-'+opt);run(case+'-C-'+opt+'-compile',[cc,'-'+opt,raw/'wrapper.c',raw/'callee.c',raw/(case+'.reference.c'),'-o',p]);q=compile_llvm(raw/(case+'.before.ll'),opt,case+'-before-'+opt)
   observe('before',opt,case,'source','C',p,stream);observe('before',opt,case,'source','LLVM',q,stream)
 audit('before',opt,path)
put(ROOT/'BEFORE-CALIBRATION.json',dict(fullmatches=sum(r['match'] for r in records),source_fixtures=2,unique_module_inputs=32,API_calls=0,compared_arena_bytes=model.FRAME))
for opt in ('O0','O2'):
 path=ROOT/('fault-'+opt+'-frames.xz')
 with lzma.open(path,'xb',preset=1) as stream:
  for fault in rawplan['faults']:
   case,name=fault['case'],fault['name'];exe=compile_llvm(raw/fault['file'],opt,case+'-fault-'+name+'-'+opt);observe('fault',opt,case,name,'LLVM',exe,stream)
 audit('fault',opt,path)
assert stable();assert sum(r['phase']=='before' for r in records)==128
put(ROOT/'BEFORE-AND-FAULTS-PASSED.json',dict(before_fullmatches=128,fault_observations=sum(r['phase']=='fault' for r in records),specified_detected=sum(r['phase']=='fault' and r['specified'] and not r['match'] for r in records),API_calls=0))
afterproducer=compile_producer(False)
for case in model.CASES:
 run('API-'+case,[afterproducer,raw/(case+'.before.ll'),ROOT/(case+'.after.ll'),ROOT/(case+'.API-REPORT.json'),case],extra=api_env)
 d=json.loads((ROOT/(case+'.API-REPORT.json')).read_text());assert d['api_calls']==1 and d['closed'] and d['changed'] and not d['budget_exhausted'] and d['verified'] and d['identity_outside_exact'];assert d['runtime_maps'] and all(str(sdk) in x for x in d['runtime_maps']);assert d['solver_maps'] and all(str(z) in x for x in d['solver_maps'])
for opt in ('O0','O2'):
 path=ROOT/('after-'+opt+'-frames.xz')
 with lzma.open(path,'xb',preset=1) as stream:
  for case in model.CASES:
   exe=compile_llvm(ROOT/(case+'.after.ll'),opt,case+'-after-'+opt);observe('after',opt,case,'source','LLVM',exe,stream)
 audit('after',opt,path)
assert stable()
summary=dict(passed=True,before_fullmatches=128,after_fullmatches=sum(r['phase']=='after' and r['match'] for r in records),normal_controls=sum(r['controls'] for r in records),fault_observations=sum(r['phase']=='fault' for r in records),specified_detected=sum(r['phase']=='fault' and r['specified'] and not r['match'] for r in records),nontrigger_fullmatches=sum(r['phase']=='fault' and not r['specified'] and r['match'] for r in records),source_fixtures=2,initial_frames=16,unique_module_inputs=32,frame_bytes=model.FRAME,saved_streams=6,saved_frames=len(records),API_calls=2,Unicorn_calls=0,rows=records,pins_unchanged=True)
assert (summary['after_fullmatches'],summary['normal_controls'],summary['fault_observations'],summary['specified_detected'],summary['nontrigger_fullmatches'],summary['saved_frames'])==(64,5760,320,192,128,512)
put(ROOT/'SUMMARY.json',summary);print(json.dumps({k:v for k,v in summary.items() if k!='rows'}),flush=True)
