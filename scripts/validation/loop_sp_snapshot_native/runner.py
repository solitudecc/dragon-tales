"""One finite SP oracle; uses existing arena wrapper/comparator/stream layout."""
from pathlib import Path
import hashlib,importlib.util,json,lzma,os,shutil,signal,struct,subprocess,time
ROOT=Path(__file__).resolve().parent
PLAN=json.loads((ROOT/'PLAN.json').read_text())
RAM=Path(PLAN['ram'])
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def put(p,v):Path(p).write_text(json.dumps(v,indent=2,sort_keys=True)+'\n')
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
model=load('new_sp_model',ROOT/'source.py')
shared=load('existing_corridor_tools',ROOT/'shared/dispatch_corridor_native.py')
def stable():
 return all(Path(x['path']).stat().st_size==x['size'] and sha(x['path'])==x['sha256'] for x in PLAN['pins'])
assert stable() and sorted(os.sched_getaffinity(0))==[0,1,2,3]
env={k:v for k,v in os.environ.items() if k not in ('LD_PRELOAD','LD_LIBRARY_PATH','DRAGON_C_LIB','LIBUNICORN_PATH')}
env['PYTHONDONTWRITEBYTECODE']='1'
commands=[]
def run(label,argv,data=None,extra=None):
 assert min(shutil.disk_usage(ROOT).free,shutil.disk_usage(RAM).free)>=512*1024*1024
 start=time.monotonic();failure=None
 with (ROOT/(label+'.stdout')).open('wb') as out,(ROOT/(label+'.stderr')).open('wb') as err:
  child=subprocess.Popen([str(x) for x in argv],cwd=ROOT,env=dict(env,**(extra or {})),stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,stdout=out,stderr=err,start_new_session=True)
  try:
   # Frame files are small enough for a dedicated pipe writer, avoiding a
   # synchronous stdin/output deadlock while the outer loop guards the child.
   if data is not None:
    import threading
    def write():
     try:child.stdin.write(data);child.stdin.close()
     except BrokenPipeError:pass
    writer=threading.Thread(target=write);writer.start()
   while child.poll() is None:
    if time.monotonic()-start>180:raise RuntimeError('180s deadline')
    if min(shutil.disk_usage(ROOT).free,shutil.disk_usage(RAM).free)<64*1024*1024:raise RuntimeError('64MiB running floor')
    time.sleep(.02)
   if data is not None:writer.join()
  except BaseException as exc:
   failure=str(exc)
   try:os.killpg(child.pid,signal.SIGKILL)
   except ProcessLookupError:pass
   child.wait()
 rec=dict(label=label,argv=[str(x) for x in argv],returncode=child.returncode,seconds=time.monotonic()-start,failure=failure,PGID=child.pid,cpus=sorted(os.sched_getaffinity(0)))
 commands.append(rec);put(ROOT/'COMMANDS.json',commands);print(label,child.returncode,round(rec['seconds'],3),flush=True)
 assert failure is None and child.returncode==0,rec
 return (ROOT/(label+'.stdout')).read_bytes()

cc=PLAN['cc'];cxx=PLAN['cxx'];llc=PLAN['llc'];sdk=Path(PLAN['runtime']['path']);z=Path(PLAN['solver']['path'])
inc=ROOT/'include';raw=ROOT/'raw'
sources=[(raw/f'input-{t}.bin').read_bytes() for t in range(9)]
for t,source in enumerate(sources):assert source==model.initial(t) and (raw/f'expected-{t}.bin').read_bytes()==model.specification(source)[0]
records=[]
def checks(actual,wanted):return shared.controls(actual,wanted,model)
def observe(phase,opt,variant,engine,executable,stream):
 actual_all=run(f'{phase}-{opt}-{variant}-{engine}-run',[executable,'0'],b''.join(sources))
 assert len(actual_all)==9*model.FRAME
 for t,source in enumerate(sources):
  wanted,coverage=model.specification(source);actual=actual_all[t*model.FRAME:(t+1)*model.FRAME]
  offset=32768 if variant=='fault-output' and coverage['accepted'] else 66104 if variant=='fault-fallback' and coverage['fallback'] else None
  specified=actual[offset:offset+(4 if offset==32768 else 8)]!=wanted[offset:offset+(4 if offset==32768 else 8)] if offset is not None else None
  match=actual==wanted;control=checks(actual,wanted) if phase!='fault' else 0
  row=dict(phase=phase,opt=opt,variant=variant,engine=engine,trial=t,match=match,controls=control,specified_offset=offset,specified=specified,coverage=coverage,sha256={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',wanted),('actual',actual)]})
  encoded=json.dumps(row,sort_keys=True).encode();stream.write(struct.pack('<I',len(encoded))+encoded+source+wanted+actual);records.append(row)
  if phase=='fault':assert (not match and specified) if offset is not None else match,row
  else:assert match and control==13,row

def compile_llvm(ir,opt,tag):
 obj=RAM/(tag+'.o');exe=RAM/(tag+'.native')
 run(tag+'-llc',[llc,'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic','-'+opt,ir,'-o',obj])
 run(tag+'-link',[cc,'-'+opt,'-I'+str(inc),raw/'wrapper.c',raw/'callee.c',obj,'-o',exe]);return exe

def audit(phase,opt,path):
 shared.single_xz_eof(path);rows=[];keys=set()
 with lzma.open(path,'rb') as stream:
  while True:
   prefix=stream.read(4)
   if not prefix:break
   assert len(prefix)==4;n=struct.unpack('<I',prefix)[0];assert 0<n<8192
   encoded=stream.read(n);assert len(encoded)==n;row=json.loads(encoded)
   source,wanted,actual=(stream.read(model.FRAME) for _ in range(3));assert len(source)==len(wanted)==len(actual)==model.FRAME
   key=(row['variant'],row['engine'],row['trial']);assert key not in keys;keys.add(key)
   assert source==model.initial(row['trial']);expected,coverage=model.specification(source);assert wanted==expected and coverage==row['coverage']
   match=actual==wanted;offset=32768 if row['variant']=='fault-output' and coverage['accepted'] else 66104 if row['variant']=='fault-fallback' and coverage['fallback'] else None
   specified=actual[offset:offset+(4 if offset==32768 else 8)]!=wanted[offset:offset+(4 if offset==32768 else 8)] if offset is not None else None
   controls=checks(actual,wanted) if phase!='fault' else 0
   assert row['phase']==phase and row['opt']==opt and row['match']==match and row['controls']==controls and row['specified_offset']==offset and row['specified']==specified
   assert row['sha256']=={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',wanted),('actual',actual)]}
   if phase=='fault':assert (not match and specified) if offset is not None else match
   else:assert match and controls==13
   rows.append(row)
 expected_keys={(variant,engine,t) for variant in (('fault-output','fault-fallback') if phase=='fault' else ('source',)) for engine in (('C','LLVM') if phase=='before' else ('LLVM',)) for t in range(9)}
 assert keys==expected_keys and rows==[r for r in records if r['phase']==phase and r['opt']==opt]
 put(ROOT/f'{phase}-{opt}-AUDIT.json',dict(passed=True,frames=len(rows),saved_sha256=sha(path),keys_complete=True,single_XZ_EOF=True,whole_frame=66112,new_executions=0))

# Independent calibration and explicit faults complete before any product API.
for opt in ('O0','O2'):
 cexe=RAM/('reference-'+opt)
 run('C-'+opt+'-compile',[cc,'-'+opt,'-I'+str(inc),raw/'wrapper.c',raw/'callee.c',raw/'reference.c','-o',cexe])
 llvmexe=compile_llvm(raw/'before.ll',opt,'before-'+opt)
 path=ROOT/('before-'+opt+'-frames.xz')
 with lzma.open(path,'xb',preset=1) as stream:
  observe('before',opt,'source','C',cexe,stream);observe('before',opt,'source','LLVM',llvmexe,stream)
 audit('before',opt,path)
for opt in ('O0','O2'):
 path=ROOT/('fault-'+opt+'-frames.xz')
 with lzma.open(path,'xb',preset=1) as stream:
  for variant in ('fault-output','fault-fallback'):
   exe=compile_llvm(raw/(variant+'.ll'),opt,variant+'-'+opt);observe('fault',opt,variant,'LLVM',exe,stream)
 audit('fault',opt,path)
assert len([r for r in records if r['phase']=='before'])==36
assert sum(r['specified'] is True for r in records)==16
assert stable()

producer=RAM/'producer'
run('producer-compile',[cxx,'-std=c++20','-O0','-fno-rtti','-D_GNU_SOURCE','-D__STDC_CONSTANT_MACROS','-D__STDC_FORMAT_MACROS','-D__STDC_LIMIT_MACROS','-I'+str(inc),'-isystem','/usr/lib/llvm-21/include',ROOT/'producer.cpp',sdk,'-Wl,-rpath,'+str(sdk.parent),'-Wl,-rpath,'+str(z.parent),'-Wl,-rpath-link,'+str(z.parent),'-pthread','-ldl','-o',producer])
api='dragon::llvmlifter::detail::runLoopImageTableContext('
defined=run('producer-defined',['/usr/bin/nm','-C','--defined-only',producer]).decode();undefined=run('producer-undefined',['/usr/bin/nm','-D','-C','--undefined-only',producer]).decode()
assert api not in defined and api in undefined
run('producer',[producer,raw/'before.ll',ROOT/'parsed-before.ll',ROOT/'after.ll',ROOT/'API-REPORT.json'],extra=dict(LD_LIBRARY_PATH=str(sdk.parent)+':'+str(z.parent),LD_DEBUG='libs,bindings'))
report=json.loads((ROOT/'API-REPORT.json').read_text());assert report['api_calls']==1 and report['closed'] and report['changed'] and not report['budget_exhausted'] and report['verified_before'] and report['verified_after'] and report['identity'] and report['outside_unchanged']
assert report['runtime_maps'] and all(str(sdk) in x for x in report['runtime_maps'])
assert report['solver_maps'] and all(str(z) in x for x in report['solver_maps'])
for opt in ('O0','O2'):
 exe=compile_llvm(ROOT/'after.ll',opt,'after-'+opt);path=ROOT/('after-'+opt+'-frames.xz')
 with lzma.open(path,'xb',preset=1) as stream:observe('after',opt,'source','LLVM',exe,stream)
 audit('after',opt,path)
assert stable()
summary=dict(passed=True,before_fullmatches=sum(r['match'] for r in records if r['phase']=='before'),after_fullmatches=sum(r['match'] for r in records if r['phase']=='after'),normal_controls=sum(r['controls'] for r in records),fault_observations=sum(r['phase']=='fault' for r in records),specified_faults=sum(r['specified'] is True for r in records),nontrigger_fullmatches=sum(r['phase']=='fault' and r['specified'] is None and r['match'] for r in records),saved_streams=6,saved_frames=len(records),frame_bytes=66112,initial_frames=9,api_calls=1,Unicorn_calls=0,rows=records,pins_unchanged=True)
assert (summary['before_fullmatches'],summary['after_fullmatches'],summary['normal_controls'],summary['fault_observations'],summary['specified_faults'],summary['nontrigger_fullmatches'],summary['saved_frames'])==(36,18,702,36,16,20,90)
put(ROOT/'SUMMARY.json',summary);print(json.dumps({k:v for k,v in summary.items() if k!='rows'}),flush=True)
