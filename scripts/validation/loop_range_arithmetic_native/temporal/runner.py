from pathlib import Path
import json,hashlib,importlib.util,os,subprocess,time,signal,shutil,lzma,struct,sys
sys.dont_write_bytecode=True
R=Path(__file__).resolve().parent;P=json.loads((R/'PLAN.json').read_text());M=Path(P['ram'])
import model
spec=importlib.util.spec_from_file_location('shared',R/'shared.py');shared=importlib.util.module_from_spec(spec);spec.loader.exec_module(shared)
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
def put(n,d):(R/n).write_text(json.dumps(d,indent=2,sort_keys=True)+'\n')
assert all(Path(x['path']).stat().st_size==x['size'] and sha(x['path'])==x['sha256'] for x in P['pins'])
rows=[];commands=[];inputs=[model.initial(t) for t in range(16)]
def run(n,args,data=None):
 assert min(shutil.disk_usage(R).free,shutil.disk_usage(M).free)>=536870912
 st=time.monotonic();reason=None
 with (R/(n+'.stdout')).open('xb') as out,(R/(n+'.stderr')).open('xb') as err:
  p=subprocess.Popen([str(x) for x in args],stdin=subprocess.PIPE if data else subprocess.DEVNULL,stdout=out,stderr=err,start_new_session=True)
  if data:
   import threading
   def write():
    try:p.stdin.write(data);p.stdin.close()
    except BrokenPipeError:pass
   writer=threading.Thread(target=write);writer.start()
  while p.poll() is None:
   if time.monotonic()-st>180:reason='180s'
   if min(shutil.disk_usage(R).free,shutil.disk_usage(M).free)<67108864:reason='64MiB floor'
   if reason:os.killpg(p.pid,signal.SIGKILL);p.wait();break
   time.sleep(.02)
  if data:writer.join()
 commands.append(dict(label=n,argv=[str(x) for x in args],exit=p.returncode,reason=reason,seconds=time.monotonic()-st));put('COMMANDS.json',commands);print(n,p.returncode,flush=True);assert p.returncode==0 and reason is None
 return (R/(n+'.stdout')).read_bytes()
def compare(phase,opt,engine,exe,stream):
 a=run(phase+'-'+opt+'-'+engine,[exe],b''.join(inputs));assert len(a)==16*model.FRAME
 for t,b in enumerate(inputs):
  wanted,cov=model.specification(b);actual=a[t*model.FRAME:(t+1)*model.FRAME];match=actual==wanted;wrong=phase=='candidate' and cov['accepted'];c=shared.controls(actual,wanted,model) if phase=='before' else 0
  if wrong:assert not match and actual[32:36]!=wanted[32:36] and actual[32768:32772]!=wanted[32768:32772]
  else:assert match
  d=dict(phase=phase,opt=opt,engine=engine,trial=t,match=match,actual_candidate_wrong=wrong,controls=c,coverage=cov,sha256={k:hashlib.sha256(v).hexdigest() for k,v in [('input',b),('expected',wanted),('actual',actual)]});row=json.dumps(d,sort_keys=True).encode();stream.write(struct.pack('<I',len(row))+row+b+wanted+actual);rows.append(d)
def audit(phase,opt,path):
 shared.single_xz_eof(path);seen=set();count=0
 with lzma.open(path,'rb') as stream:
  while True:
   p=stream.read(4)
   if not p:break
   assert len(p)==4;n=struct.unpack('<I',p)[0];raw=stream.read(n);assert len(raw)==n;d=json.loads(raw);b,w,a=[stream.read(model.FRAME) for _ in range(3)];assert all(len(x)==model.FRAME for x in [b,w,a]);wanted,cov=model.specification(b);assert b==model.initial(d['trial']) and w==wanted and d['coverage']==cov
   key=(d['engine'],d['trial']);assert key not in seen;seen.add(key);wrong=phase=='candidate' and cov['accepted'];assert d['actual_candidate_wrong']==wrong and d['match']==(a==w)
   assert d['sha256']=={k:hashlib.sha256(v).hexdigest() for k,v in [('input',b),('expected',w),('actual',a)]}
   if wrong:assert a!=w and a[32:36]!=w[32:36] and a[32768:32772]!=w[32768:32772]
   else:assert a==w
   count+=1
 assert seen=={(e,t) for e in (('C','LLVM') if phase=='before' else ('LLVM',)) for t in range(16)}
 put(phase+'-'+opt+'-AUDIT.json',dict(frames=count,passed=True,stream_sha256=sha(path),singleXZ_EOF=True,new_executions=0))
(R/'wrapper.c').write_text(model.WRAPPER);(R/'callee.c').write_text(model.CALLEE);(R/'reference.c').write_text(model.C_SPEC)
for opt in ('O0','O2'):
 c=M/('C-'+opt);b=M/('before-'+opt);o=M/('before-'+opt+'.o')
 run('C-'+opt+'-compile',[P['cc'],'-'+opt,R/'wrapper.c',R/'callee.c',R/'reference.c','-o',c]);run('before-'+opt+'-llc',[P['llc'],'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic','-'+opt,R/'before.ll','-o',o]);run('before-'+opt+'-link',[P['cc'],'-'+opt,R/'wrapper.c',R/'callee.c',o,'-o',b])
 path=R/('before-'+opt+'.xz')
 with lzma.open(path,'xb',preset=1) as stream:compare('before',opt,'C',c,stream);compare('before',opt,'LLVM',b,stream)
 audit('before',opt,path)
assert sum(x['match'] for x in rows)==64
put('BEFORE-CALIBRATION.json',dict(matches=64,controls=sum(x['controls'] for x in rows),API_calls=0))
for opt in ('O0','O2'):
 a=M/('candidate-'+opt);o=M/('candidate-'+opt+'.o');run('candidate-'+opt+'-llc',[P['llc'],'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic','-'+opt,R/'candidate-after.ll','-o',o]);run('candidate-'+opt+'-link',[P['cc'],'-'+opt,R/'wrapper.c',R/'callee.c',o,'-o',a]);path=R/('candidate-'+opt+'.xz')
 with lzma.open(path,'xb',preset=1) as stream:compare('candidate',opt,'LLVM',a,stream)
 audit('candidate',opt,path)
assert all(Path(x['path']).stat().st_size==x['size'] and sha(x['path'])==x['sha256'] for x in P['pins'])
s=dict(before_matches=64,controls=sum(x['controls'] for x in rows),candidate_observations=32,candidate_wrong=sum(x['actual_candidate_wrong'] for x in rows),candidate_nontrigger_matches=sum(x['phase']=='candidate' and x['match'] for x in rows),frames=len(rows),streams=4,observed_fields=[32,32768],all_guards_and_pins_passed=True,new_API_calls=0,status='Actual wrong execution of unreleased SDK1239 candidate; not old merged main20c',rows=rows);assert s['candidate_wrong']==16 and s['candidate_nontrigger_matches']==16
put('SUMMARY.json',s);print(json.dumps({k:v for k,v in s.items() if k!='rows'}),flush=True)
