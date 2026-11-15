import argparse,collections,hashlib,json,os,struct,time,traceback
from pathlib import Path
from types import SimpleNamespace
import tigress_function_diff as tf
import practical_holdout_source as spec

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,x):Path(p).write_text(json.dumps(x,indent=2,sort_keys=True)+'\n')

p=argparse.ArgumentParser();p.add_argument('--input',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(exist_ok=False)
assert (a.input/'original.c').read_text()==spec.source(spec.SEED)
sources={str(Path(m.__file__).resolve()):sha(m.__file__) for m in [tf,spec,tf.guest,tf.compiler,tf.base,tf.base.dragon]}
save(a.out/'PLAN.json',dict(inputs=spec.INPUTS,guest_step_limit=1000000,source_pins=sources,source_root=str(a.input),profiles=['original','combined'],levels=['O0','O2'],library=tf.base.dragon._LIB,source_input_sha={n:sha(a.input/n) for n in ['original.c','combined.c']}))
adapter=tf.guest.Adapter('x86_64');rows=[];images={}
args=SimpleNamespace(clang='clang-21',ld='ld.lld-21')
for family in ['original','combined']:
 for level in ['O0','O2']:
  work=a.out/(family+'-'+level)
  try:
   data,regions,symbols=tf.build_image(a.input/(family+'.c'),work,level,args)
   images[family+'-'+level]=dict(elf=str(work/'fixture.elf'),sha256=hashlib.sha256(data).hexdigest(),symbols=symbols,regions=[dict(address=x,size=len(b)) for x,b in regions])
   for i,x in enumerate(spec.INPUTS):
    regs,memory=tf.initial(adapter,regions,i)
    (work/f'input-{i}.bin').write_bytes(regs+memory)
    last={};uc0=tf.guest.Uc
    def observed(*args):
     uc=uc0(*args)
     def hook(u,pc,size,_):last.update(pc=pc,size=size,bytes=bytes(u.mem_read(pc,min(size,15))).hex())
     uc.hook_add(tf.guest.UC_HOOK_CODE,hook)
     return uc
    tf.guest.Uc=observed
    row=dict(family=family,level=level,trial=i,x=x)
    try:
     result=adapter.reference(dict(stubs=[],undefined_flags=[],max_instructions=1000000),regs,memory,tf.ENTRY)
     got=tf.observables(result);want=spec.specification(spec.SEED,x)
     frame=result[0]+result[1]+struct.pack('<8Q',*result[3],0)
     (work/f'expected-{i}.bin').write_bytes(frame)
     row.update(status='match' if got==want else 'different',got=[got[0],got[1].hex()],expected=[want[0],want[1].hex()],exit=result[3],frame_sha256=hashlib.sha256(frame).hexdigest())
     # Prove the independent observable comparator detects changed return/output.
     assert ((got[0]^1,got[1])!=want) if got==want else True
     assert ((got[0],bytes([got[1][0]^1])+got[1][1:])!=want) if got==want else True
    except Exception as e:row.update(status='reference_error',error=type(e).__name__+': '+str(e),last_instruction=last)
    finally:tf.guest.Uc=uc0
    rows.append(row)
  except Exception as e:
   rows.append(dict(family=family,level=level,status='build_error',error=str(e),traceback=traceback.format_exc()))
  save(a.out/'REFERENCE.json',dict(rows=rows,images=images,summary=dict(collections.Counter(r['status'] for r in rows)),source_pins_stable=all(sha(p)==s for p,s in sources.items()),skipped=0,normalization=0))
  print(family,level,dict(collections.Counter(r['status'] for r in rows if r['family']==family and r['level']==level)),flush=True)
