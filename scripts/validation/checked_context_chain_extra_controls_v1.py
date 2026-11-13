from pathlib import Path
import copy,hashlib,json,os,shutil,signal,subprocess,sys,time
root=Path('/dev/shm/checked-chain-independent-whole-controls-20261113-v1');frozen=Path('/dev/shm/checked-context-chain-audit-20261113-root-v3');source=frozen/'source';plans=frozen/'plans'
assert root.exists() and not (root/'inputs').exists();(root/'inputs').mkdir();(root/'outputs').mkdir();(root/'commands').mkdir()
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
freeze=json.loads((frozen/'SOURCE-PINS-v3.json').read_text())
for n,h in freeze['files'].items():assert sha(source/n)==h,n
p0=json.loads((plans/'f-combo-O0.json').read_text());p2=json.loads((plans/'f-combo-O2.json').read_text())
rt=Path(p0['tools']['runtime']);assert p0['tools']==p2['tools'];assert sha(rt/'libdragon.so')=='02d730ce862ec7bf60ccaccb05bd1b61cd3b971b8b444b56d3a0e108b60992e3'
assert p0['tools']['inventory']['sha256']=='4ba0db00cb71d0ea283b2ad9766dbfaf54fa60afde377a8d48faf6dd2c207942'
assert sha(p0['tools']['inventory']['path'])==p0['tools']['inventory']['sha256']
files={frozen/'SOURCE-PINS-v3.json',plans/'f-combo-O0.json',plans/'f-combo-O2.json',root/'run_controls.py'}
files.update(source/n for n in freeze['files'])
for plan in (p0,p2):
 for frame in plan['frames'].values():files.update(Path(frame[k]) for k in ('ll','json'))
 files.update(Path(plan[k]['path']) for k in ('manifest','hints'))
 files.update(Path(plan['tools'][k]['path']) for k in ('inventory','anchor_inventory','bridge'))
files.update(rt/n for n in ('libdragon.so','libdragon_c.so','MANIFEST.json'))
def pins():
 result={}
 for p in sorted(files):
  stat=p.stat();result[str(p)]=dict(size=stat.st_size,sha256=sha(p),mtime_ns=stat.st_mtime_ns,ctime_ns=stat.st_ctime_ns,inode=stat.st_ino)
 return result
before=pins();(root/'pins-before.json').write_text(json.dumps(before,indent=2)+'\n')
env=dict(os.environ,LD_LIBRARY_PATH=str(rt));(root/'ldd-shared.txt').write_text(subprocess.check_output(['ldd',p0['tools']['inventory']['path']],env=env,text=True))
rows=[]
def command(name,argv):
 starts=dict(tmpfs=shutil.disk_usage(root).free,disk=shutil.disk_usage(Path.home()).free);assert min(starts.values())>=512*1024**2,starts
 (root/'commands'/(name+'.command.json')).write_text(json.dumps(dict(argv=argv,LD_LIBRARY_PATH=str(rt)),indent=2)+'\n');start=time.monotonic();minimum=dict(starts);failure=None
 with (root/'commands'/(name+'.log')).open('wb') as log:
  proc=subprocess.Popen(argv,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
  while True:
   free=dict(tmpfs=shutil.disk_usage(root).free,disk=shutil.disk_usage(Path.home()).free)
   minimum={k:min(minimum[k],free[k]) for k in minimum}
   if min(free.values())<64*1024**2:failure='running free below64MiB'
   if time.monotonic()-start>=180:failure='command wall180s'
   if failure:os.killpg(proc.pid,signal.SIGKILL);rc=proc.wait();break
   try:rc=proc.wait(timeout=1);break
   except subprocess.TimeoutExpired:pass
 row=dict(name=name,argv=argv,exit=rc,failure=failure,LD_LIBRARY_PATH=str(rt),seconds=time.monotonic()-start,start_free_bytes=starts,minimum_observed_running_free_bytes=minimum,start_guard_bytes=512*1024**2,running_guard_bytes=64*1024**2,wall_limit_seconds=180)
 (root/'commands'/(name+'.receipt.json')).write_text(json.dumps(row,indent=2)+'\n');rows.append(row);assert failure is None,row
 return rc
# Private V-after raw: change exactly one ordinary metadata payload definition.
frame=p0['frames']['v_after'];raw=Path(frame['ll']).read_bytes();old=b'!5 = !{i64 67280}';new=b'!5 = !{i64 67281}';assert raw.count(old)==1
ll=root/'inputs/combo-O0-v-after-bad-md.ll';js=ll.with_suffix('.json');ll.write_bytes(raw.replace(old,new,1))
fn=json.loads(Path(frame['json']).read_text())['function']
assert command('metadata-independent-full-inventory',[p0['tools']['inventory']['path'],str(ll),fn,str(js)])==0
original_inventory=json.loads(Path(frame['json']).read_text());actual_inventory=json.loads(js.read_text());assert set(actual_inventory)==set(original_inventory) and 'state_value_structure' in actual_inventory
private=copy.deepcopy(p0);private['frames']['v_after']=dict(ll=str(ll),json=str(js),ll_sha256=sha(ll),json_sha256=sha(js));planpath=root/'inputs/metadata-payload.plan.json';planpath.write_text(json.dumps(private,indent=2)+'\n');out=root/'outputs/metadata-payload.json'
assert command('metadata-whole-chain',[sys.executable,str(source/'checked_context_chain_audit_v1.py'),'--plan',str(planpath),'--out',str(out)])==1
result=json.loads(out.read_text());assert not result['established'] and result['refusal']['stage']=='V4',result.get('refusal')
assert len(result['actual_LL_JSON_frame_bindings'])==5 and all(x['exact_independent_parse_verify_inventory'] for x in result['actual_LL_JSON_frame_bindings'])
metadata=dict(control='whole-chain V4 metadata payload',source_role='f-combo-O0/v_after',original_raw_sha256=frame['ll_sha256'],mutant_raw_sha256=sha(ll),mutant_full_json_sha256=sha(js),inventory_fields=sorted(actual_inventory),inventory_byte_equal_original=(js.read_bytes()==Path(frame['json']).read_bytes()),all_five_state_raw_bindings_exact=True,anchor_generic_raw_binding_exact=result['anchor_raw_binding']['exact_generic_schema_parse_verify_inventory'],refusal=result['refusal'],report=str(out))
# Enumerate the actual full family using only frozen verifier helper logic;
# no projector/closure claim is made for this unprojected enumeration.
sys.path.insert(0,str(source));import checked_context_chain_audit_v1 as chain
inv=chain.validate_plan(p2);manifest=json.loads(Path(p2['manifest']['path']).read_text());enumeration=chain.closure_metric(inv['anchor'],inv['bit_after'],manifest,{})
central=enumeration['central_blocks'];assert central;omitted=next((x for x in central if x!='memory.valid49'),central[0]);declared=[x for x in central if x!=omitted];assert len(declared)==len(central)-1
(root/'inputs/actual-central-family.json').write_text(json.dumps(dict(actual=central,omitted=omitted,declared=declared,scope='family enumeration only; no projected closure or product execution'),indent=2)+'\n')
private=copy.deepcopy(p2);private['declared_central']=declared;planpath=root/'inputs/central-omitted.plan.json';planpath.write_text(json.dumps(private,indent=2)+'\n');out=root/'outputs/central-omitted.json'
assert command('central-omission-whole-chain',[sys.executable,str(source/'checked_context_chain_audit_v1.py'),'--plan',str(planpath),'--out',str(out)])==1
result=json.loads(out.read_text());assert not result['established'] and result['refusal']['stage']=='Bit60' and 'central coverage omission or duplicate' in result['refusal']['reason'],result.get('refusal')
assert len(result['actual_LL_JSON_frame_bindings'])==5 and all(x['exact_independent_parse_verify_inventory'] for x in result['actual_LL_JSON_frame_bindings']);assert result['exact_whole_module_import_bridge']['reference_report']['IR_exact_independent_parse_render'] and result['exact_whole_module_import_bridge']['reference_report']['JSON_exact_independent_parse_render']
coverage=dict(control='whole-chain actual central member omitted',source_role='f-combo-O2',actual_central_count=len(central),declared_count=len(declared),omitted=omitted,all_five_state_raw_bindings_exact=True,anchor_generic_raw_binding_exact=result['anchor_raw_binding']['exact_generic_schema_parse_verify_inventory'],exact_whole_module_bridge=True,C59_copy_proof_passed='C59_copy_proof' in result,Bit60_copy_proof_passed='Bit60_copy_proof' in result,refusal=result['refusal'],report=str(out))
after=pins();(root/'pins-after.json').write_text(json.dumps(after,indent=2)+'\n');assert before==after,'frozen inputs/source/tools/runtime changed'
report=dict(schema='independent-whole-chain-negative-controls-v1',frozen_source_directory=str(source),frozen_source_pins=freeze,frozen_source_runtime_original_inputs_unchanged=True,metadata_control=metadata,central_coverage_control=coverage,commands=rows,product_passes_executed=0,target_native_executions=0,owned_build_paths_read=False)
(root/'REPORT.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
