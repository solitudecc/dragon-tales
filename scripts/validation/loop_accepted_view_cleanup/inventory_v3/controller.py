from pathlib import Path
import json,hashlib,os,subprocess,time,signal,shutil
BASE=Path('/home/ubuntu/vmc-runs');R=BASE/'loop-accepted-view-cleanup-preparation-20261115-v3';SEAL=BASE/'loop-accepted-view-cleanup-inputs-20261115-v1';SDK=BASE/'loop-image-final-branch-sdks-20261115-v1';Z3=BASE/'rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib'
def h(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def pin(p):
 s=p.stat();return {'path':str(p),'size':s.st_size,'sha256':h(p),'dev':s.st_dev,'ino':s.st_ino,'mtime_ns':s.st_mtime_ns,'ctime_ns':s.st_ctime_ns}
def put(n,v):
 with (R/n).open('x') as f:json.dump(v,f,indent=2,sort_keys=True);f.write('\n')
def run(argv,name,env):
 assert shutil.disk_usage(R).free>=536870912;start=time.monotonic();reason=None
 with (R/(name+'.stdout')).open('xb') as o,(R/(name+'.stderr')).open('xb') as e:
  p=subprocess.Popen(['/usr/bin/taskset','-c','0-3']+argv,stdout=o,stderr=e,env=env,start_new_session=True)
  while p.poll() is None:
   if time.monotonic()-start>180:reason='180s timeout'
   if shutil.disk_usage(R).free<67108864:reason='64MiB floor'
   if reason:os.killpg(p.pid,signal.SIGKILL);p.wait();break
   time.sleep(.2)
 row={'argv':argv,'exit':p.returncode,'reason':reason,'seconds':time.monotonic()-start,'affinity':'0-3'};put(name+'.execution.json',row);print(name,p.returncode,row['seconds'],flush=True)
 if p.returncode or reason:raise RuntimeError(name+' failed')
 return row
result={'passed':False,'cleanup_pass_runs':0,'Guest_native_runs':0}
try:
 paths=[p for p in (SEAL).rglob('*') if p.is_file()]+[R/'source/readonly_table_lookup_inventory.cpp',R/'SOURCE-FREEZE.json',SDK/'main/lib/libdragon.so',SDK/'SDK-FROZEN.json',Z3/'libz3.so.4.13']
 before={str(p):pin(p) for p in paths};assert h(SDK/'main/lib/libdragon.so')=='d446226f9ff402bf1e096e197993aa8f2301d2ab81af3c573b50ae13ed2e0ed4'
 binary=R/'readonly-inventory';cpp=R/'source/readonly_table_lookup_inventory.cpp';view=json.loads((SEAL/'producer/manifest.json').read_text())
 compile=['/usr/bin/g++-14','-std=c++20','-O1','-g0','-I/usr/lib/llvm-21/include',str(cpp),'-L'+str(SDK/'main/lib'),'-Wl,-rpath,'+str(SDK/'main/lib'),'-ldragon','-pthread','-o',str(binary)]
 argv=[str(binary),str(SEAL/'source/production-B.ll'),'714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74',view['view_symbol'],view['root_symbol'],str(R/'LOOKUPS.json')]
 put('HELPER-PLAN.json',{'schema':'readonly-actual-B-lookup-compile-parse-v1','pins_before':before,'compile':compile,'parse':argv,'new_cleanup_pass_runs':0,'new_Guest_runs':0,'limits':{'seconds':180,'start_free':536870912,'floor':67108864},'affinity':'0-3'})
 env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',LD_PRELOAD='',LD_LIBRARY_PATH=str(SDK/'main/lib')+':'+str(Z3))
 result['compile']=run(compile,'compile',env);result['inventory']=run(argv,'inventory',dict(env,LD_DEBUG='libs'))
 q=json.loads((R/'LOOKUPS.json').read_text());assert 0<q['simple_i64_LOAD_count']<=512 and q['module_unchanged'] and q['new_pass_runs']==0
 assert any('calling init:' in s and str(SDK/'main/lib/libdragon.so') in s for s in (R/'inventory.stderr').read_text().splitlines())
 undefined=subprocess.check_output(['/usr/bin/nm','-D','--undefined-only',str(binary)],text=True);assert 'runLoopImageTableContext' not in undefined
 after={str(p):pin(p) for p in paths};assert before==after
 result.update(passed=True,lookups_sha256=h(R/'LOOKUPS.json'),all_simple_i64_LOADs=q['simple_i64_LOAD_count'],all_i64_LOADs=q['all_i64_LOAD_count'],exact_T_lookup_inventory=False,runtime_binding_actual=True,source_before=before,source_after=after,helper_binary=pin(binary),LLVM_version=q['LLVM_version'])
except Exception as e:result['failure']={'type':type(e).__name__,'message':str(e)}
finally:put('RESULT.json',result)
raise SystemExit(0 if result['passed'] else 1)
