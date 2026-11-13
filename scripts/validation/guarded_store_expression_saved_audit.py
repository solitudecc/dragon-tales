#!/usr/bin/env python3
"""Audit saved streams only: no native libraries, LLVM tools or passes are used."""
from pathlib import Path
import argparse,ast,collections,gzip,hashlib,json,struct,time,traceback
ORACLE_SHA='49b9a8035c932dabf6c3274e7a039261d36882db6080744151b0b08e5716d224'
RUNS=('native-O0-normal','native-O0-wrong','native-O2-normal','native-O2-wrong')
FRAME_ARENA=0x12000
FRAME_OUTPUT=8+FRAME_ARENA
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def spec(source):
 assert digest(source)==ORACLE_SHA
 tree=ast.parse(source.read_text())
 constants={'MASK','ARENA','REGS','EXIT','DESC','CONFIG','GUEST','FAILURE','MODES','FAULTS','TARGETS','VALUES','INPUTS'}
 functions={'key','encoded','get','put','initial','model'}
 nodes=[]
 for n in tree.body:
  if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id in constants for target in n.targets for t in ast.walk(target)):nodes.append(n)
  if isinstance(n,ast.FunctionDef) and n.name in functions:nodes.append(n)
 ns={'struct':struct}
 exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),ns)
 assert ns['ARENA']==FRAME_ARENA and len(ns['INPUTS'])==64 and len(ns['MODES'])==10 and len(ns['FAULTS'])==8
 original=ns['initial'];cache={}
 def immutable_input_cache(width,key,guest):
  k=(width,tuple(key),guest)
  if k not in cache:cache[k]=bytes(original(width,key,guest))
  return bytearray(cache[k])
 ns['initial']=immutable_input_cache
 return ns
def exact(f,size):
 value=f.read(size)
 assert len(value)==size,('truncated stream',size,len(value))
 return value
def header(raw,w,m,i):
 assert len(raw)==12 and struct.unpack('<III',raw)==(w,m,i),('wrong header',raw.hex(),(w,m,i))
def changes(a,b):
 assert len(a)==len(b)==FRAME_OUTPUT
 result=[]
 for start in range(0,len(a),256):
  aa,bb=a[start:start+256],b[start:start+256]
  if aa!=bb:result.extend(start+i for i,(x,y) in enumerate(zip(aa,bb)) if x!=y)
 return result
def fields(offsets):
 def field(i):
  if i<8:return 'return'
  i-=8
  for name,start,size in (('regs',0x100,0x100),('Exit',0x400,64),('descriptor',0x500,16),('config',0x600,32),('Guest',0x1000,65536)):
   if start<=i<start+size:return name
  return 'other arena bytes'
 return dict(collections.Counter(field(i) for i in offsets))
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--root',type=Path,required=True);p.add_argument('--oracle',type=Path,required=True)
 p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 assert not a.out.exists();a.out.mkdir(parents=True)
 ns=spec(a.oracle)
 inputs,modes,faults=ns['INPUTS'],ns['MODES'],ns['FAULTS']
 ARENA,REGS,EXIT,DESC=ns['ARENA'],ns['REGS'],ns['EXIT'],ns['DESC']
 report=dict(schema='guarded-store-expression-saved-frames-audit-v1',passed=False,
             native_or_product_invocations=0,oracle_sha256=digest(a.oracle),
             checker_sha256=digest(Path(__file__)),runs=[],failures=[])
 active={};started=time.monotonic()
 try:
  for name in RUNS:
   root=a.root/name;rp=root/'RESULTS.json';r=json.loads(rp.read_bytes())
   assert r['source_sha256']==ORACLE_SHA and r['passed'] is True and r['completed'] is True and r['failures']==[]
   assert r['matches']==2560 and r['comparator_controls']==3840
   assert r['limits']['command_seconds']==180
   assert r['limits']['start_free_bytes']==512*1024**2 and r['limits']['running_free_bytes']==64*1024**2
   initial_hashes={32:set(),64:set()}
   run=dict(name=name,report_sha256=digest(rp),compressed_streams={},normal_records=0,
            native_matches=0,comparator_controls=0,normal_region_matches=collections.Counter(),
            fault_records=0,faults={},guest_pointer=None,
            normal_uncompressed_bytes=0,fault_uncompressed_bytes=0)
   for stream in ('full-frames.bin.gz','fault-frames.bin.gz'):
    q=root/stream;assert q.is_file() and not q.is_symlink()
    assert q.stat().st_size==r['streams'][stream]['bytes'] and digest(q)==r['streams'][stream]['sha256']
    run['compressed_streams'][stream]=dict(bytes=q.stat().st_size,sha256=digest(q))
   def check_init(init,width,key):
    guest=ns['get'](init,DESC)
    assert guest!=0 and 0<=guest<(1<<64)
    if run['guest_pointer'] is None:run['guest_pointer']=guest
    assert guest==run['guest_pointer']
    wanted=ns['initial'](width,key,guest)
    assert init==wanted,'input arena mismatch'
    initial_hashes[width].add(hashlib.sha256(init).hexdigest())
    assert ns['get'](init,REGS+8)==key[0] and ns['get'](init,REGS+16)==key[1]
    assert ns['get'](init,DESC+8)==key[3]
    return guest
   def check_output(frame,key):
    assert len(frame)==FRAME_OUTPUT
    result=struct.unpack_from('<Q',frame)[0]
    assert result==ns['get'](frame,8+REGS)
    values=struct.unpack_from('<7Q',frame,8+EXIT)
    assert values[0] in (1,5) and values[1]==0 and values[2]==36864 and values[5]==key[0]
    assert values[4]==(1 if values[0]==1 else 0)
   with gzip.open(root/'full-frames.bin.gz','rb') as f:
    for width in (32,64):
     for mode_index,mode in enumerate(modes):
      for i,key in enumerate(inputs):
       active=dict(run=name,phase='normal',width=width,mode=mode,input_number=i,input=list(key))
       raw=exact(f,12);header(raw,width,mode_index,i)
       init=exact(f,ARENA);want=exact(f,FRAME_OUTPUT);before=exact(f,FRAME_OUTPUT);after=exact(f,FRAME_OUTPUT)
       guest=check_init(init,width,key)
       reference=ns['model'](width,mode,key,guest)
       assert want==reference,'saved expected/model mismatch'
       assert before==want and after==want,'whole arena or return mismatch'
       check_output(want,key);check_output(before,key);check_output(after,key)
       for offset in (8+REGS,8+ns['GUEST']+128,8+EXIT):
        damaged=bytearray(want);damaged[offset]^=1;assert damaged!=want
        run['comparator_controls']+=1
       for field in ('u64 return','all arena bytes','regs256','Exit64','descriptor16','config32','Guest65536','other arena bytes'):
        run['normal_region_matches'][field]+=2
       run['native_matches']+=2;run['normal_records']+=1
       run['normal_uncompressed_bytes']+=12+ARENA+3*FRAME_OUTPUT
    assert f.read(1)==b'','extra normal data after fixed domain'
   assert run['normal_records']==1280 and run['native_matches']==2560 and run['comparator_controls']==3840
   with gzip.open(root/'fault-frames.bin.gz','rb') as f:
    for width in (32,64):
     for fault_index,(fault,mode) in enumerate(faults.items()):
      counts=dict(observations=0,specified=0,detected=0,
                  fault_model_full_matches=0,exact_difference_offset_sets=0,
                  required_difference_fields=collections.Counter(),extra_difference_fields=0)
      unique_inputs=set();unique_specified=set()
      for i,key in enumerate(inputs):
       active=dict(run=name,phase='fault',width=width,mode=mode,fault=fault,input_number=i,input=list(key))
       raw=exact(f,12);header(raw,width,fault_index,i)
       init=exact(f,ARENA);normal=exact(f,FRAME_OUTPUT);fault_expected=exact(f,FRAME_OUTPUT);actual=exact(f,FRAME_OUTPUT)
       guest=check_init(init,width,key)
       input_sha256=hashlib.sha256(init).hexdigest();unique_inputs.add(input_sha256)
       normal_reference=ns['model'](width,mode,key,guest)
       fault_reference=ns['model'](width,mode,key,guest,fault)
       assert normal==normal_reference and fault_expected==fault_reference
       assert actual==fault_expected,'fault output differs from independent fault model'
       expected_offsets=changes(fault_expected,normal)
       actual_offsets=changes(actual,normal)
       assert actual_offsets==expected_offsets,'fault difference set missing/extra bytes'
       counts['fault_model_full_matches']+=1;counts['exact_difference_offset_sets']+=1
       if expected_offsets:
        counts['specified']+=1;assert actual!=normal;counts['detected']+=1
        unique_specified.add(input_sha256)
        for field in fields(expected_offsets):counts['required_difference_fields'][field]+=1
       counts['observations']+=1;run['fault_records']+=1
       run['fault_uncompressed_bytes']+=12+ARENA+3*FRAME_OUTPUT
      old=r['faults'][f'i{width}-{fault}']
      assert {k:counts[k] for k in ('observations','specified','detected')}==old
      assert counts['observations']==64 and counts['specified']>0
      counts['unique_input_arenas']=len(unique_inputs)
      counts['unique_specified_input_arenas']=len(unique_specified)
      run['faults'][f'i{width}-{fault}']=counts
    assert f.read(1)==b'','extra fault data after fixed domain'
   assert run['fault_records']==1024
   assert sum(v['specified'] for v in run['faults'].values())==868
   assert sum(v['detected'] for v in run['faults'].values())==868
   assert digest(rp)==run['report_sha256']
   for stream,value in run['compressed_streams'].items():assert digest(root/stream)==value['sha256']
   run['unique_initial_arenas_by_width']={str(w):len(v) for w,v in initial_hashes.items()}
   assert run['unique_initial_arenas_by_width']=={'32':62,'64':64}
   report['runs'].append(run)
   print(name+' normal1280/2560 matches; faults1024/868 specified exact; EOF/CRC/header/domain complete',flush=True)
  report['totals']=dict(run_pairs=4,gzip_streams=8,normal_records=5120,native_matches=10240,
      comparator_controls=15360,fault_observations=4096,specified_fault_differences=3472,
      detected_specified_faults=3472,actual_fault_outputs_matching_fault_model=4096,
      exact_fault_difference_offset_sets=4096,
      declared_input_keys_per_fixture=64,
      effective_initial_arenas_by_width={'32':62,'64':64},
      unique_effective_fault_arenas_per_run=sum(v['unique_input_arenas'] for v in report['runs'][0]['faults'].values()),
      unique_effective_specified_fault_arenas_per_run=sum(v['unique_specified_input_arenas'] for v in report['runs'][0]['faults'].values()),
      duplicate_scope='four repetitions across O0/O2 and normal/wrong hints. Declared fault headers1024 per run; i32 override0xffffffff and u64 max truncate to the same initial state for each x, so effective input arenas are fewer; no new domain is inferred from repetition')
  report['scope']='read-only saved-frame audit; frozen Python reference reused, not a new independent semantic oracle or new native run; complete arena/return, no normalization/exclusions; width-high-half blind spot not claimed'
  report['passed']=True
 except BaseException as e:
  report['failures'].append(dict(case=active,type=type(e).__name__,detail=str(e),traceback=traceback.format_exc()))
  raise
 finally:
  report['seconds']=time.monotonic()-started
  (a.out/'REPORT.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
 print('REPORT '+digest(a.out/'REPORT.json'))
if __name__=='__main__':main()
