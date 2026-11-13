from pathlib import Path
import argparse,datetime,hashlib,json,os,shutil,signal,subprocess,time
p=argparse.ArgumentParser();p.add_argument('backend',choices=('main','noz3'));p.add_argument('stage',choices=('prepare','O0','O2','auditO0','auditO2'));a=p.parse_args()
r=Path.home()/'vmc-runs/guarded-bit-slices-independent-20261113';pub=r/'dragon-bit-slices-public-v2';runtime=Path.home()/'vmc-runs/guarded-bit-loop-discovery-20261113'/('runtime-'+a.backend+'-v2')
expected={'main':('02d730ce862ec7bf60ccaccb05bd1b61cd3b971b8b444b56d3a0e108b60992e3','17c3831dbe81c46194f3bc459f01bb023069dfe32b87fb13b944cb5758c6fe9f'),'noz3':('be804e7bef457bcd3fce984c2e16ed504b96152da8961a3da026e2f9c0e604e4','f162ebfdf9fb5bf9645a9eb3d4246a0fb209106b5b3257ebf103a1c3ca0da76e')}
sha=lambda x:hashlib.sha256(Path(x).read_bytes()).hexdigest()
assert (sha(runtime/'libdragon.so'),sha(runtime/'MANIFEST.json'))==expected[a.backend]
pre=json.loads((pub/'PREDECLARED.json').read_text());files=[pub/n for n in pre['files']]+[pub/'driver',*sorted(runtime.glob('*'))]
files=[x for x in files if x.is_file()];pins={str(x):{'size':x.stat().st_size,'sha256':sha(x)} for x in files}
for n,v in pre['files'].items():assert pins[str(pub/n)]==v,n
startfree=shutil.disk_usage(r).free;assert startfree>=512*1024**2,('start disk guard',startfree)
control=r/'candidate-controls-final-v2';control.mkdir(exist_ok=True);out=control/(a.backend+'-'+a.stage);out.mkdir(exist_ok=False)
label=a.backend+'-final-v2';native=a.backend+'-'+a.stage+'-final-v2'
if a.stage=='prepare':cmd=['python3',str(pub/'source/guarded_bit_slices_prepare.py'),str(pub),label,str(runtime)]
elif a.stage in ('O0','O2'):cmd=['python3',str(pub/'source/guarded_bit_slices_oracle.py'),'run',str(pub/'before'),str(pub/label/'normal'),str(pub/label/'wrong'),str(r/native),'--opt',a.stage[1:]]
else:
 native=a.backend+'-O'+a.stage[-1]+'-final-v2';cmd=['python3',str(pub/'source/guarded_bit_slices_saved_audit.py'),str(r),native,'--source',str(pub/'source/guarded_bit_slices_oracle.py')]
env=dict(os.environ,LD_LIBRARY_PATH=str(runtime));ldd=subprocess.check_output(['ldd',str(pub/'driver')],text=True,env=env)
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(ldd);(out/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
start=time.monotonic();minimum=startfree;failure=None;returncode=None
with (out/'run.log').open('wb') as log:
 process=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
 while True:
  free=shutil.disk_usage(r).free;minimum=min(minimum,free)
  if free<64*1024**2:failure='running disk guard below 64MiB'
  if time.monotonic()-start>=180:failure='stage wall limit 180s'
  if failure:
   os.killpg(process.pid,signal.SIGKILL);returncode=process.wait();break
  try:returncode=process.wait(timeout=1);break
  except subprocess.TimeoutExpired:pass
post={str(x):{'size':x.stat().st_size,'sha256':sha(x)} for x in files};stable=pins==post
(out/'pins-after.json').write_text(json.dumps(post,indent=2)+'\n')
receipt=dict(backend=a.backend,stage=a.stage,command=cmd,exit=returncode,failure=failure,LD_LIBRARY_PATH=str(runtime),runtime_backend_sha256=expected[a.backend][0],runtime_manifest_sha256=expected[a.backend][1],source_runtime_driver_pins_stable=stable,seconds=time.monotonic()-start,start_free_bytes=startfree,minimum_running_free_bytes=minimum,start_guard_bytes=512*1024**2,running_guard_bytes=64*1024**2,wall_limit_seconds=180)
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt));assert not failure and returncode==0 and stable
