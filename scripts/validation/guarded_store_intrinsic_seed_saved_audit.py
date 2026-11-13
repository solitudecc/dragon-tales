#!/usr/bin/env python3
"""Pure saved-frame audit for the intrinsic-seed child; no native/LLVM calls."""
from pathlib import Path
import argparse,ast,collections,gzip,hashlib,json,struct,time,traceback
SOURCE_SHA='0861f17d006e9b4118c6b24ee902b0240e936525e6c084c2b09fe40055d7baed'
RUNS=('native-O0-normal','native-O0-wrong','native-O2-normal','native-O2-wrong')
ARENA=0x12000
FRAME=8+ARENA
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def load_spec(p):
 assert sha(p)==SOURCE_SHA
 names={'MASK','ARENA','REGS','EXIT','DESC','CONFIG','GUEST','FAILURE','MODES','OPS','FAULTS','LIMITS'}
 funcs={'key','enc','inputs','get','put','rotate','initial','model'}
 nodes=[]
 for n in ast.parse(p.read_text()).body:
  if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id in names for target in n.targets for t in ast.walk(target)):nodes.append(n)
  if isinstance(n,ast.FunctionDef) and n.name in funcs:nodes.append(n)
 ns={'struct':struct};exec(compile(ast.Module(body=nodes,type_ignores=[]),str(p),'exec'),ns)
 assert ns['ARENA']==ARENA and len(ns['MODES'])==4 and len(ns['OPS'])==2 and len(ns['FAULTS'])==6
 original=ns['initial'];cache={}
 def initial_copy(w,k,guest):
  key=(w,tuple(k),guest)
  if key not in cache:cache[key]=bytes(original(w,k,guest))
  return bytearray(cache[key])
 ns['initial']=initial_copy
 return ns
def exact(f,n):
 data=f.read(n);assert len(data)==n,('truncated',n,len(data));return data
def header(raw,expected):assert len(raw)==16 and struct.unpack('<4I',raw)==expected
def offsets(a,b):
 assert len(a)==len(b)==FRAME
 changed=[]
 for start in range(0,len(a),256):
  aa,bb=a[start:start+256],b[start:start+256]
  if aa!=bb:changed.extend(start+i for i,(x,y) in enumerate(zip(aa,bb)) if x!=y)
 return changed
def fields(changed):
 def field(i):
  if i<8:return 'return'
  i-=8
  for name,start,size in (('regs',0x100,256),('Exit',0x400,64),('descriptor',0x500,16),('config',0x600,32),
      ('seed-counter',0x1000+208,8),('seed-rotation-trace',0x1000+216,8),('future-rotation-trace',0x1000+224,8),
      ('Guest',0x1000,65536)):
   if start<=i<start+size:return name
  return 'other arena'
 return dict(collections.Counter(field(i) for i in changed))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True)
 p.add_argument('--oracle',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 a=p.parse_args();assert not a.out.exists();a.out.mkdir(parents=True)
 report=dict(schema='intrinsic-seed-saved-frame-audit-v1',passed=False,failures=[],runs=[],
             source_sha256=SOURCE_SHA,checker_sha256=sha(Path(__file__)),native_or_product_runs=0)
 active={};started=time.monotonic()
 try:
  ns=load_spec(a.oracle);modes,ops,faults=ns['MODES'],ns['OPS'],ns['FAULTS']
  for run_name in RUNS:
   root=a.root/run_name;rp=root/'RESULTS.json';rh=sha(rp);r=json.loads(rp.read_bytes())
   assert r['source_sha256']==SOURCE_SHA and r['passed'] is True and r['completed'] is True and r['failures']==[]
   assert r['matches']==512 and r['comparator_controls']==768
   assert r['limits']==ns['LIMITS']
   assert min(r['initial_free_bytes'].values())>=512*1024**2
   assert min(r['minimum_free_bytes'].values())>=64*1024**2
   assert r['seconds']<180
   rr=dict(name=run_name,source_report_sha256=rh,streams={},normal_records=0,
           native_matches=0,comparator_controls=0,fault_records=0,faults={},guest_pointer=None,
           full_arena_bytes=ARENA,return_bytes=8,normal_uncompressed_bytes=0,fault_uncompressed_bytes=0)
   initial_hashes={32:set(),64:set()}
   for stream in ('full-frames.bin.gz','fault-frames.bin.gz'):
    q=root/stream;assert q.is_file() and not q.is_symlink()
    assert q.stat().st_size==r['streams'][stream]['bytes'] and sha(q)==r['streams'][stream]['sha256']
    rr['streams'][stream]=dict(bytes=q.stat().st_size,sha256=sha(q))
   def input_state(data,w,k):
    guest=ns['get'](data,ns['DESC'])
    assert guest!=0
    if rr['guest_pointer'] is None:rr['guest_pointer']=guest
    assert guest==rr['guest_pointer']
    assert data==ns['initial'](w,k,guest),'input bytes differ from frozen source'
    initial_hashes[w].add(hashlib.sha256(data).hexdigest())
    assert ns['get'](data,ns['DESC']+8)==k[4]
    assert ns['get'](data,ns['REGS']+8)==k[0]
    assert ns['get'](data,ns['REGS']+80)==k[1]
    return guest
   def output_state(data,k):
    assert len(data)==FRAME
    assert struct.unpack_from('<Q',data)[0]==ns['get'](data,8+ns['REGS'])
    e=struct.unpack_from('<7Q',data,8+ns['EXIT'])
    assert e[0] in (1,5) and e[1]==0 and e[2]==36864 and e[5]==k[0]
    assert e[4]==(1 if e[0]==1 else 0)
   with gzip.open(root/'full-frames.bin.gz','rb') as f:
    for w in (32,64):
     keys=ns['inputs'](w);assert len(keys)==len(set(keys))==16
     for op_index,op in enumerate(ops):
      for mode_index,mode in enumerate(modes):
       for i,k in enumerate(keys):
        active=dict(run=run_name,phase='normal',width=w,op=op,mode=mode,input_number=i,input=list(k))
        header(exact(f,16),(w,op_index,mode_index,i))
        init=exact(f,ARENA);expected=exact(f,FRAME);before=exact(f,FRAME);after=exact(f,FRAME)
        guest=input_state(init,w,k)
        assert expected==ns['model'](w,op,mode,k,guest)
        assert before==after==expected,'whole return/arena mismatch'
        for data in (expected,before,after):output_state(data,k)
        for offset in (8+ns['REGS'],8+ns['GUEST']+208,8+ns['EXIT']):
         damaged=bytearray(expected);damaged[offset]^=1;assert damaged!=expected
         rr['comparator_controls']+=1
        rr['normal_records']+=1;rr['native_matches']+=2
        rr['normal_uncompressed_bytes']+=16+ARENA+3*FRAME
    assert f.read(1)==b'','extra normal bytes after domain'
   assert rr['normal_records']==256 and rr['native_matches']==512 and rr['comparator_controls']==768
   with gzip.open(root/'fault-frames.bin.gz','rb') as f:
    for w in (32,64):
     keys=ns['inputs'](w)
     for op_index,op in enumerate(ops):
      for fault_index,(fault,mode) in enumerate(faults.items()):
       counts=dict(observations=0,specified=0,detected=0,exact_difference_offset_sets=0,
                   required_difference_fields=collections.Counter())
       unique_inputs=set();unique_specified=set()
       for i,k in enumerate(keys):
        active=dict(run=run_name,phase='fault',width=w,op=op,fault=fault,input_number=i,input=list(k))
        header(exact(f,16),(w,op_index,fault_index,i))
        init=exact(f,ARENA);normal=exact(f,FRAME);fault_expected=exact(f,FRAME);actual=exact(f,FRAME)
        guest=input_state(init,w,k);ih=hashlib.sha256(init).hexdigest();unique_inputs.add(ih)
        assert normal==ns['model'](w,op,mode,k,guest)
        assert fault_expected==ns['model'](w,op,mode,k,guest,fault)
        assert actual==fault_expected
        planned=offsets(fault_expected,normal);observed=offsets(actual,normal)
        assert planned==observed,'extra/missing fault difference bytes'
        if planned:
         counts['specified']+=1;counts['detected']+=1;unique_specified.add(ih)
         for field in fields(planned):counts['required_difference_fields'][field]+=1
        counts['observations']+=1;counts['exact_difference_offset_sets']+=1;rr['fault_records']+=1
        rr['fault_uncompressed_bytes']+=16+ARENA+3*FRAME
       key=f'i{w}-{op}-{fault}'
       assert {x:counts[x] for x in ('observations','specified','detected')}==r['faults'][key]
       assert counts['observations']==16 and counts['specified']>0
       counts.update(unique_input_arenas=len(unique_inputs),unique_specified_arenas=len(unique_specified))
       rr['faults'][key]=counts
    assert f.read(1)==b'','extra fault bytes after domain'
   assert rr['fault_records']==384 and sum(c['specified'] for c in rr['faults'].values())==253
   rr['unique_initial_arenas_by_width']={str(w):len(v) for w,v in initial_hashes.items()}
   assert rr['unique_initial_arenas_by_width']=={'32':16,'64':16}
   rr['distinct_x_values_by_width']={str(w):len({k[0] for k in ns['inputs'](w)}) for w in (32,64)}
   for stream,pin in rr['streams'].items():assert sha(root/stream)==pin['sha256']
   assert sha(rp)==rh
   report['runs'].append(rr)
   print(run_name+' 256normal/512matches;384fault/253specified;header/domain/CRC/EOF passed',flush=True)
  report.update(passed=True,totals=dict(run_pairs=4,gzip_streams=8,normal_records=1024,
       native_full_return_arena_matches=2048,comparator_controls=3072,
       fault_observations=1536,specified_differences=1012,detected_specified=1012,
       exact_fault_difference_offset_sets=1536,
       effective_initial_arenas_per_width={'32':16,'64':16},
       unique_function_input_domain_per_run=256,unique_fault_function_input_domain_per_run=384,
       unique_specified_domain_per_run=253),
       scope='saved frames only; frozen pure Python reference reused, not a second independent semantic oracle; full bytes including descriptor/remainder/Exit, no normalization. Four repetitions across host O0/O2 and normal/wrong hints do not add unique business inputs. This checks transformation semantics only and does not prove full loop deflattening;48SCC topology remains separate evidence.')
 except BaseException as e:
  report['failures'].append(dict(case=active,type=type(e).__name__,detail=str(e),traceback=traceback.format_exc()));raise
 finally:
  report['seconds']=time.monotonic()-started;(a.out/'REPORT.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
 print('REPORT '+sha(a.out/'REPORT.json'))
if __name__=='__main__':main()
