#!/usr/bin/env python3
"""Four frozen real boundary scenarios; original-LLVM reference, then actual views.

No C/Unicorn reference or product pass runs here. Eight original-LLVM frames
are crosschecked in four O0/O2 pairs; sixteen actual after frames use these
references. The two explicit wrong-IR controls are counted separately.
"""
import argparse,ast,hashlib,importlib.util,json,lzma,os,platform,random,shutil,struct,subprocess,time,traceback
from pathlib import Path
R=Path('/home/cc/vmc-runs/ordered-dispatch-formal-validation-20261114-v1')
N=Path('/home/cc/vmc-runs/ordered-dispatch-corridor-real-native-20261114-v1')
INPUT_SHA='d6aa0341509f8ddecc2aeae9d5fcdd22301ad24c261c3577b36d502825b62a8c'
EMITTER_SHA='21104608e148a1d13756c8e38ec18a661251817153a713afa7ebc647e919d281'
def sha(b):return hashlib.sha256(b).hexdigest()
def digest(p):return sha(Path(p).read_bytes())
def module(path,name):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def put(p,v):Path(p).write_text(json.dumps(v,indent=2,sort_keys=True)+'\n')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--inputs',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 assert not a.out.exists();a.out.mkdir(parents=True);rows=[];commands=[];failure=None;active={};pins={}
 def read(p):
  p=Path(p);assert p.is_file() and not p.is_symlink();b=p.read_bytes();h=sha(b)
  assert str(p) not in pins or pins[str(p)]==h;pins[str(p)]=h;return b
 def space():
  assert min(shutil.disk_usage('/home/cc').free,shutil.disk_usage('/dev/shm').free)>=64*1024**2
 def run(argv,work,tag,data=None):
  space();t=time.monotonic();v=subprocess.run([str(x) for x in argv],input=data,capture_output=True,cwd=work,timeout=180)
  (work/(tag+'.stderr')).write_bytes(v.stderr)
  commands.append(dict(argv=[str(x) for x in argv],exit=v.returncode,seconds=time.monotonic()-t,stdin_sha256=sha(data) if data is not None else None,
                       stdout_sha256=sha(v.stdout),stdout_bytes=len(v.stdout),stderr_file=str(work/(tag+'.stderr'))))
  put(a.out/'COMMANDS.json',commands)
  assert v.returncode==0,(tag,v.returncode,v.stderr.decode(errors='replace')[-1000:]);return v.stdout
 def controls(want,got,C):
  found=[]
  for field,off in [('RAX',24),('memory',536+32768),('exit',66072)]:
   b=bytearray(got);b[off]^=1;diff=C.differences(want,bytes(b));assert len(diff)==1 and diff[0]['field']==field
   found.append(dict(field=field,offset=off,differences=diff))
  return found
 try:
  assert min(shutil.disk_usage('/home/cc').free,shutil.disk_usage('/dev/shm').free)>=512*1024**2
  ib=read(a.inputs/'INPUTS.json');assert sha(ib)==INPUT_SHA;inputs=json.loads(ib)
  emitter_path=R/'source/dispatch_corridor_real_boundaries.py';assert sha(read(emitter_path))==EMITTER_SHA
  E=module(emitter_path,'frozen_boundary_emitter')
  checker_path=N/'source/dispatch_corridor_real_saved_audit.py';assert sha(read(checker_path))=='503279501290c1ee73ba2ff08e1bf1ff44b70137832d947fec79f988b02e5f44'
  C=module(checker_path,'frozen_boundary_snapshot_compare')
  nominal=json.loads(read(N/'replay-plan-READY-v1.json'));assert digest(N/'replay-plan-READY-v1.json')=='9bacce5f629b95d9582b299f7f2457d2fc0f67f9a3340e56f3dceedcc2b21458'
  driver=a.inputs/inputs['logical_driver']['path'];assert sha(read(driver))==inputs['logical_driver']['sha256']
  spec_path=next(v['path'] for v in nominal['source_compatibility']['unchanged'] if v['path'].endswith('/practical_holdout_source.py'))
  spec_bytes=read(spec_path);assert sha(spec_bytes)=='6ce5d1417bd1ada1c12e4b4a4adf32e35233f60445868c9ffdbde004245bd5b5'
  tree=ast.parse(spec_bytes);names={'SEED','INPUTS','MASK'};funcs={'_constants','_rotate_word','_octet_replacement','specification'}
  nodes=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in funcs or isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id in names for target in n.targets for t in ast.walk(target))]
  spec={'random':random,'struct':struct};exec(compile(ast.Module(body=nodes,type_ignores=[]),spec_path,'exec'),spec)
  assert spec['SEED']==2026111307 and spec['INPUTS']==[x['value'] for x in nominal['inputs']]
  business=spec['specification'](spec['SEED'],0)
  scenarios={v['name']:v for v in inputs['scenarios']};assert list(scenarios)==['sp-miss','table-miss','size-zero','size-late-ret']
  for s in scenarios.values():
   b=read(a.inputs/s['input']['path']);assert len(b)==66048 and sha(b)==s['input']['sha256']
  assert len({v['input']['sha256'] for v in scenarios.values()})==3
  tools={k:Path(v['path']) for k,v in nominal['tools'].items() if k in ['opt','llc','cc']}
  for v in nominal['source_compatibility']['unchanged']:
   if v['path'].endswith(('/c_shim/dragon_c.h','/include/dragon/lifters/llvm/Execution.h')):assert sha(read(v['path']))==v['sha256']
  for k,v in nominal['tools'].items():
   if k in tools:assert sha(read(v['path']))==v['sha256']
  ready={b:json.loads(read(R/b/'REAL-READY-v1.json')) for b in ['main','noz3']}
  sources={};before_source=Path(ready['main']['bindings']['source_parsed']['path']);before_bytes=read(before_source)
  assert sha(before_bytes)=='c8212fcc824336041a44e2d6846333a657b574c71feb8c57eba13ad6cad08dd3'
  for b,plan in ready.items():
   result=R/b/'real-replay-v1/RESULTS.json';audit=R/b/'real-replay-saved-audit-v1/REPORT.json'
   v=json.loads(read(result));w=json.loads(read(audit));assert v['correct'] and v['native_comparisons']==32 and w['passed'] and w['result_sha256']==sha(read(result))
   key=plan['bindings']['final_bound_wrapper'];sources[b]=read(key['path']);assert sha(sources[b])==key['sha256']
  triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}[platform.machine()]
  def compile_target(folder,ir,opt,source_path=None):
   folder.mkdir(parents=True);(folder/'driver.c').write_bytes(driver.read_bytes())
   run([tools['opt'],'-passes=verify','-disable-output','-'],folder,'verify-source',ir)
   selected=ir;literal=ir.decode().splitlines()[0]
   if opt=='O2':
    argv=[tools['opt'],'-S','-passes=default<O2>',source_path if source_path else '-']
    selected=run(argv,folder,'stock-opt',None if source_path else ir)
    expected="; ModuleID = '"+(str(source_path) if source_path else '<stdin>')+"'"
    assert selected.decode().splitlines()[0]==expected
    (folder/'stock-O2.ll.xz').write_bytes(lzma.compress(selected,preset=1));assert lzma.decompress((folder/'stock-O2.ll.xz').read_bytes())==selected
    literal=expected
   run([tools['opt'],'-passes=verify','-disable-output','-'],folder,'verify-final',selected)
   run([tools['llc'],'-O0','-filetype=obj','-mtriple='+triple,'-o','module.o','-'],folder,'llc',selected)
   run([tools['cc'],'-O0','-I/home/cc/dragon-tales-overlap/include','-I/home/cc/dragon-tales-overlap/c_shim','driver.c','module.o','-o','native'],folder,'link')
   put(folder/'SOURCE.json',dict(input_sha256=sha(ir),input_ModuleID=ir.decode().splitlines()[0],selected_sha256=sha(selected),selected_ModuleID=literal,
       selected_complete_IR_in_stock_xz=opt=='O2',normalizations=0))
   return folder/'native'
  def execute(exe,scenario,work):
   s=scenarios[scenario];out=work/(scenario+'.bin');source=a.inputs/s['input']['path']
   run([exe,source,out,str(s['logical_Guest_size'])],work,'run-'+scenario)
   blob=read(out);assert len(blob)==66136 and blob[:8]==b'DTFLOW1\0'
   assert C.parsed(blob)[3][7]==0;return blob,out
  def properties(name,blob):
   rr,mm,cc,ee=C.parsed(blob);source=(a.inputs/scenarios[name]['input']['path']).read_bytes();initial=source[512:]
   if name=='sp-miss':
    assert ee[:2]==[1,3] and ee[3]==61440 and ee[4]==1
    assert int.from_bytes(rr[32:40],'little')==0xc820
    assert (int.from_bytes(rr[:4],'little'),mm[32768:32784])==business
   elif name=='table-miss':
    target=inputs['table_mutations'][0]['new']
    assert ee==[2,1,4787,target,1,0,0,0],'table-miss did not reach its exact concrete StaticAnalysis transfer'
    assert int.from_bytes(rr[128:136],'little')==target
    assert mm[32768:32784]==initial[32768:32784]
    assert mm[8192:8240]==initial[8192:8240]
   elif name=='size-zero':
    assert ee[0]==5 and ee[6]==3,'size-zero did not actually reach MemoryFault'
    assert mm==initial,'size-zero produced a successful Guest write'
   else:
    assert ee[0]==5 and ee[6]==3 and ee[2]==4111,'late-RET did not actually fault at root RET'
    assert (int.from_bytes(rr[:4],'little'),mm[32768:32784])==business
   return dict(exit=ee,RSP=int.from_bytes(rr[32:40],'little'),business_return=int.from_bytes(rr[:4],'little'),business_output=mm[32768:32784].hex())
  before={}
  for opt in ['O0','O2']:
   active=dict(phase='before',opt=opt);w=a.out/'before'/opt;exe=compile_target(w,before_bytes,opt,before_source)
   for name in scenarios:
    active.update(scenario=name);b,pth=execute(exe,name,w);before[(opt,name)]=(b,pth);properties(name,b)
  for opt in ['O0','O2']:
   peer='O2' if opt=='O0' else 'O0'
   for name in scenarios:
    blob,path=before[(opt,name)];want,reference=before[(peer,name)]
    assert blob==want and C.differences(want,blob)==[],'originalLLVM O0/O2 complete-state calibration disagreement'
    rows.append(dict(phase='before',opt=opt,scenario=name,match=True,actual=str(path),expected=str(reference),expected_source='originalLLVM peer optimization; not independent C/Unicorn',
                     properties=properties(name,blob),controls=controls(want,blob,C)))
  put(a.out/'BEFORE-CALIBRATION.json',dict(passed=True,original_LLVM_executions=8,full_raw_crosschecked_frames=8,independent_O0_O2_pairs=4,
      new_C=0,new_Unicorn=0,rows=rows.copy(),scope='Same original LLVM model; not an independent machine reference for logical faults.'))
  for backend in ['main','noz3']:
   for opt in ['O0','O2']:
    active=dict(phase='after',backend=backend,opt=opt);w=a.out/backend/'after'/opt
    exe=compile_target(w,sources[backend],opt,Path(ready[backend]['bindings']['final_bound_wrapper']['path']))
    for name in scenarios:
     active.update(scenario=name);blob,path=execute(exe,name,w);want,reference=before[(opt,name)]
     assert blob==want and C.differences(want,blob)==[],'actual bound full-state boundary mismatch'
     rows.append(dict(phase='after',backend=backend,opt=opt,scenario=name,match=True,actual=str(path),expected=str(reference),
                      properties=properties(name,blob),controls=controls(want,blob,C)))
   for fault,scenario,mutation in [('force-SP-guard-accept','sp-miss',E.forced_sp_accept),('wrong-fault-effect','size-zero',E.wrong_fault_effect)]:
    ir=mutation(sources[backend].decode()).encode();w=a.out/backend/'faults'/fault;w.mkdir(parents=True)
    (w/'mutant.ll.xz').write_bytes(lzma.compress(ir,preset=1));assert lzma.decompress((w/'mutant.ll.xz').read_bytes())==ir
    for opt in ['O0','O2']:
     active=dict(phase='fault',backend=backend,opt=opt,scenario=scenario,fault=fault);work=w/opt;exe=compile_target(work,ir,opt)
     blob,path=execute(exe,scenario,work);want,reference=before[(opt,scenario)];assert blob!=want
     differences=[i for i,(x,y) in enumerate(zip(want,blob)) if x!=y]
     if fault=='force-SP-guard-accept':
      assert blob[56:64]!=want[56:64] and blob[536:66072]!=want[536:66072],'wrong guard missed required RSP and Guest differences'
     else:
      assert set(differences)=={536+32768,66072}
      assert blob[536+32768]==(want[536+32768]^1) and blob[66072]==(want[66072]^1)
     rows.append(dict(phase='fault',backend=backend,opt=opt,scenario=scenario,fault=fault,match=False,specified_detected=True,
                      actual=str(path),expected=str(reference),changed_offsets=differences,controls=[]))
  assert len(rows)==32 and sum(r['phase']!='fault' for r in rows)==24 and sum(r.get('specified_detected',False) for r in rows)==8
  for p,h in pins.items():assert digest(p)==h,'frozen input changed during boundary run '+p
  logical=sum(p.stat().st_size for p in a.out.rglob('*') if p.is_file());assert logical<12*1024**2,'boundary artifact budget12MiB exceeded'
 except BaseException as e:
  failure=dict(type=type(e).__name__,detail=str(e),active=active,traceback=traceback.format_exc());put(a.out/'FAILURE.json',failure);raise
 finally:
  put(a.out/'RESULTS.json',dict(passed=failure is None and len(rows)==32,failure=failure,rows=rows,source=pin_self(),pins=pins,
      normal_frames=sum(x['phase']!='fault' for x in rows),original_LLVM_calibration_frames=sum(x['phase']=='before' for x in rows),
      actual_after_matches=sum(x['phase']=='after' and x['match'] for x in rows),specified_fault_detections=sum(x.get('specified_detected',False) for x in rows),
      comparator_only_controls=sum(len(x['controls']) for x in rows),new_C=0,new_Unicorn=0,new_product_passes=0,
      logical_scenarios=4,physical_initial_frames=3,normalizations=0,skips=0,
      layout=dict(header=24,raw_registers=512,compared_register_fields=76,Guest=65536,Exit_fields=7,Exit_bytes=56,budget_bytes=8,total=66136)))
  print('boundary',len(rows),'rows','failure',failure)
def pin_self():
 p=Path(__file__).resolve();return dict(path=str(p),sha256=digest(p))
if __name__=='__main__':main()
