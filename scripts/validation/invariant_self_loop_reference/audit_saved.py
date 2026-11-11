from pathlib import Path
import hashlib,json,gzip,struct,sys,subprocess,os
r=Path(sys.argv[1]);sys.path.insert(0,str(r/'source-v3'));import oracle
assert hashlib.sha256((r/'source-v3/oracle.py').read_bytes()).hexdigest()=='6af1994d196080bcd75b3ff035652dbee0cf534f06785170bebe57f6a353475e'
fixtures=r/'fixtures-v3';stages=('before','after-main','after-noz3')+oracle.FAULTS
normal=faults=specified=frames=0;rows=[];witnesses=0
for stage in stages:
 report=json.loads((fixtures/(stage+'.report.json')).read_text());assert [tuple(x['input']) for x in report['rows']]==oracle.INPUTS
 assert report['source_sha256']==hashlib.sha256((fixtures/(stage+'.ll')).read_bytes()).hexdigest()
 assert report['runner_sha256']==hashlib.sha256((r/'observe-v3').read_bytes()).hexdigest()
 expected_source=('fixtures-v3/'+stage+'.ll') if stage=='before' or stage in oracle.FAULTS else str(fixtures/(stage+'.ll'))
 assert all(c['exit']==0 for c in report['commands']);assert any(c['argv'][0]=='clang-21' and '-O2' in c['argv'] and c['argv'][5]==expected_source for c in report['commands'])
 expected_files={stage+f'-{i}.bin.gz' for i in range(len(oracle.INPUTS))};assert {p.name for p in fixtures.glob(stage+'-*.bin.gz')}==expected_files
 for i,key in enumerate(oracle.INPUTS):
  raw=gzip.decompress((fixtures/(stage+f'-{i}.bin.gz')).read_bytes());w=struct.unpack('<10Q',raw[:80]);kind=w[1];count=w[2]
  assert w[0]==0x4b5350494e303031 and tuple(w[8:])==key[:2] and kind in (0,1) and count==(2 if kind else 1)
  assert len(raw)==80+count*(16+oracle.N);states=[raw[80+n*(16+oracle.N):80+(n+1)*(16+oracle.N)] for n in range(count)]
  if kind:assert w[3]==w[5] and w[4]==w[6]==0x14000000 and w[3]+4<=w[7] and states[0]==states[1];witnesses+=1
  isfault=stage in oracle.FAULTS;wanted_kind,wanted=oracle.model(key,stage if isfault else '')
  assert kind==wanted_kind and all(x==wanted for x in states)
  nominal=oracle.model(key);needed=(wanted_kind,wanted)!=nominal
  assert needed==report['rows'][i]['fault_required'];assert hashlib.sha256(raw).hexdigest()==report['rows'][i]['raw_sha256']
  if needed:assert kind!=nominal[0] or states[0]!=nominal[1];specified+=1
  faults+=isfault;normal+=not isfault;frames+=len(states)
  damaged=bytearray(states[0]);damaged[16+60000+48]^=1;assert damaged!=wanted
 rows.append({'stage':stage,'records':len(oracle.INPUTS),'source_sha256':report['source_sha256']})
assert (normal,faults,specified,frames)==(108,144,72,378)
assert (fixtures/'after-main.ll').read_bytes()==(fixtures/'after-noz3.ll').read_bytes()
# Rebind the actual exported/compiled IR to immutable copies of the exact same
# production library bytes, not to a later build. This is API replay only.
bind=r/'frozen-bindings';bind.mkdir(exist_ok=False);bindings=[]
for mode in ['main','noz3']:
 runtime=Path.home()/('vmc-runs/invariant-self-loops-20261112/runtime-'+mode+'-v1');manifest=json.loads((runtime/'MANIFEST.json').read_text());lib=runtime/'libdragon.so';sha=hashlib.sha256(lib.read_bytes()).hexdigest();assert sha==manifest['files']['libdragon.so']['sha256']
 assert (r/(mode+'-runtime-before.sha256')).read_text().split()[0]==(r/(mode+'-runtime-after.sha256')).read_text().split()[0]==sha
 env=dict(os.environ,LD_LIBRARY_PATH=str(runtime));command=[str(r/'transform'),str(fixtures/'before.ll'),str(bind/(mode+'.ll')),str(bind/(mode+'.json'))]
 ldd=subprocess.run(['ldd',str(r/'transform')],env=env,capture_output=True);(bind/(mode+'.ldd')).write_bytes(ldd.stdout+ldd.stderr);assert ldd.returncode==0 and str(lib).encode() in ldd.stdout
 ran=subprocess.run(command,env=env,capture_output=True);(bind/(mode+'.stdout')).write_bytes(ran.stdout);(bind/(mode+'.stderr')).write_bytes(ran.stderr);assert ran.returncode==0
 assert (bind/(mode+'.ll')).read_bytes()==(fixtures/('after-'+mode+'.ll')).read_bytes();assert hashlib.sha256(lib.read_bytes()).hexdigest()==sha
 bindings.append({'mode':mode,'argv':command,'LD_LIBRARY_PATH':str(runtime),'exit':ran.returncode,'runtime_sha256':sha,'actual_native_ir_sha256':hashlib.sha256((fixtures/('after-'+mode+'.ll')).read_bytes()).hexdigest()})
result={'normal':normal,'fault_observations':faults,'specified_fault_detections':specified,'records':normal+faults,'full_saved_frames':frames,'two_PC_self_branch_witnesses':witnesses,'single_Exit_corruption_rejections':normal+faults,'stages':rows,'runtime_bindings':bindings,'native_reexecutions':0,'exclusions':0,'skips':0,'counted_run':'source-v3 only; v2 original runs retained separately','focused_native_optimization':'clang-21 -O2 only, as preregistered'}
(r/'saved-audit-v3.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ('stages','runtime_bindings')}))
