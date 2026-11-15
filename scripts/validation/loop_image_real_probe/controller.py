from pathlib import Path
import os,json,hashlib,subprocess,time,signal,shutil
R=Path('/home/ubuntu/vmc-runs/loop-image-intrinsic-import-probe-20261115-v2');SDK=Path('/home/ubuntu/vmc-runs/loop-image-branch-accounting-sdk-20261115-v1');BASE=Path('/home/ubuntu/vmc-runs/loop-image-observation-range-milestone-20261115-v1/source-tree');Z3=Path('/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib');PINS=R.parent/'loop-image-branch-accounting-20261115-v1/INPUT-PLAN.json'
def h(p):
 x=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1<<20),b''):x.update(b)
 return x.hexdigest()
def pin(p):
 s=p.stat();return {'path':str(p),'size':s.st_size,'sha256':h(p),'dev':s.st_dev,'ino':s.st_ino,'mtime_ns':s.st_mtime_ns,'ctime_ns':s.st_ctime_ns}
def put(n,d):
 with (R/n).open('x') as f:json.dump(d,f,indent=2,sort_keys=True);f.write('\n')
def run(a,n,env):
 assert shutil.disk_usage(R).free>=512*1024**2
 begin=time.monotonic();reason=None
 with (R/(n+'.stdout')).open('xb') as so,(R/(n+'.stderr')).open('xb') as se:
  p=subprocess.Popen(['/usr/bin/taskset','-c','0-3']+a,env=env,cwd=R,stdout=so,stderr=se,start_new_session=True)
  while p.poll() is None:
   if time.monotonic()-begin>180:reason='180s timeout'
   if shutil.disk_usage(R).free<64*1024**2:reason='64MiB floor'
   if reason:os.killpg(p.pid,signal.SIGKILL);p.wait();break
   time.sleep(.2)
 d={'argv':a,'affinity':'0-3','exit':p.returncode,'reason':reason,'seconds':time.monotonic()-begin,'env':{k:env.get(k) for k in ['LD_LIBRARY_PATH','LD_PRELOAD','LD_DEBUG']}};put(n+'.execution.json',d);print(n,d['exit'],round(d['seconds'],3),flush=True)
 if reason or p.returncode:raise RuntimeError(n+' failed')
 return d
result={'passed':False,'Image_API_calls':0,'Guest_native_runs':0}
try:
 frozen=json.loads((R/'SOURCE-FREEZE.json').read_text());assert all(h(R/n)==v['sha256'] for n,v in frozen['files'].items())
 spec=json.loads(PINS.read_text());paths=[p for p in (R/'source').rglob('*') if p.is_file()]+[R/'SOURCE-FREEZE.json',SDK/'lib/libdragon.so',SDK/'SDK-FROZEN.json',Z3/'libz3.so.4.13',PINS]+[Path(spec[k]['path']) for k in ['raw','elf','initial_frame']]
 before={str(p):pin(p) for p in paths};assert h(SDK/'lib/libdragon.so')=='d446226f9ff402bf1e096e197993aa8f2301d2ab81af3c573b50ae13ed2e0ed4'
 env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',LD_PRELOAD='',LD_LIBRARY_PATH=str(SDK/'lib')+':'+str(Z3))
 flags=['/usr/bin/g++-14','-std=c++20','-O1','-g0','-I'+str(R/'source'),'-I'+str(SDK/'source/include'),'-I'+str(BASE/'include'),'-I/usr/lib/llvm-21/include'];link=['-L'+str(SDK/'lib'),'-Wl,-rpath,'+str(SDK/'lib'),'-ldragon','-pthread']
 c=flags+[str(R/'source/identity_controls.cpp')]+link+['-o',str(R/'identity-controls')];p=flags+[str(R/'source/probe.cpp')]+link+['-o',str(R/'probe')]
 put('PLAN.json',{'schema':'narrow-intrinsic-import-probe-calibration-plan-v1','pins':before,'controls_compile':c,'controls_run':[str(R/'identity-controls')],'producer_compile':p,'producer_run':[str(R/'probe'),str(PINS),str(R/'results')],'affinity':'0-3','limits':{'start_free':536870912,'floor':67108864,'seconds':180},'only_added_function_allowlist':['llvm.fshl.i32'],'required_successful_closed_and_changed':True,'control_source_frozen_before_execution':True,'before_API_calls':0,'no_source_Model_input_budget_changes':True,'no_active_repo_or_build_inputs':True,'Guest_native_runs':0})
 result['controls_compile']=run(c,'controls-compile',env);result['controls']=run([str(R/'identity-controls')],'controls',dict(env,LD_DEBUG='libs'))
 controls=json.loads((R/'controls.stdout').read_text());assert controls['failures']==0 and len(controls['cases'])==14 and all(x['IR_valid'] and x['control_detected'] for x in controls['cases'])
 undefined=subprocess.check_output(['/usr/bin/nm','-D','--undefined-only',str(R/'identity-controls')],text=True);assert 'runLoopImageTableContext' not in undefined
 put('CONTROL-RESULT.json',{'passed':True,'control_count':len(controls['cases']),'Image_API_calls':0,'controls_sha256':h(R/'controls.stdout'),'no_Image_API_symbol_import':True})
 result['producer_compile']=run(p,'producer-compile',env)
 undefined=subprocess.check_output(['/usr/bin/nm','-D','--undefined-only',str(R/'probe')],text=True);defined=subprocess.check_output(['/usr/bin/nm','-C','--defined-only',str(R/'probe')],text=True);assert 'runLoopImageTableContext' in undefined and ' T dragon::llvmlifter::detail::runLoopImageTableContext(' not in defined
 put('SYMBOL-BINDING.json',{'installed_runLoop_undefined_import':[s for s in undefined.splitlines() if 'runLoopImageTableContext' in s],'Image_private_implementation_linked':False,'no_extra_shared_LLVM':True})
 result['probe']=run([str(R/'probe'),str(PINS),str(R/'results')],'probe',dict(env,LD_DEBUG='libs'))
 report=json.loads((R/'results/REPORT.json').read_text());assert report['ok'] and report['image_calls']==2 and report['native_C_UC_runs']==0
 result['Image_API_calls']=2
 for name in ['controls','probe']:assert any('calling init:' in s and str(SDK/'lib/libdragon.so') in s for s in (R/(name+'.stderr')).read_text().splitlines())
 after={str(p):pin(p) for p in paths};assert before==after
 result.update(passed=True,pins_before=before,pins_after=after,control_cases=14,report_sha256=h(R/'results/REPORT.json'),stages=report['stages'],source_ordinary_exact=report['source_ordinary_exact'],old_failure_preserved=True)
except Exception as e:result['failure']={'type':type(e).__name__,'message':str(e)}
finally:
 if (R/'results/REPORT.json').exists():
  observed=json.loads((R/'results/REPORT.json').read_text());result['observed_image_calls']=observed.get('image_calls');result['observed_report_ok']=observed.get('ok')
 put('RESULT.json',result)
raise SystemExit(0 if result['passed'] else 1)
