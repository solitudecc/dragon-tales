from pathlib import Path
import argparse,hashlib,json,os,subprocess
from oracle import inputs,model
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('producer',type=Path);p.add_argument('old',type=Path);p.add_argument('main',type=Path);p.add_argument('noz3',type=Path);a=p.parse_args()
r=a.root;r.mkdir(exist_ok=False);here=Path(__file__).resolve().parent;commands=[]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(n,x):(r/n).write_text(json.dumps(x,indent=2,sort_keys=True)+'\n')
def run(cmd,tag,data=None,env=None):
 cmd=list(map(str,cmd));result=subprocess.run(cmd,input=data,env=env,capture_output=True,timeout=180);(r/(tag+'.stdout')).write_bytes(result.stdout);(r/(tag+'.stderr')).write_bytes(result.stderr);commands.append(dict(command=cmd,tag=tag,exit=result.returncode));save('COMMANDS.json',commands);assert result.returncode==0,(tag,result.stderr[-1000:]);return result.stdout
pins={str(p):sha(p) for p in [a.producer,*[here/n for n in ['producer.cpp','fixture.hpp','driver.c','oracle.py','replay.py']]]}
for sdk in [a.old,a.main,a.noz3]:pins[str(sdk/'lib/libdragon.so')]=sha(sdk/'lib/libdragon.so')
stems=['d40','r40','d80','r80']
save('PLAN.json',dict(pins=pins,fixtures=stems,rows_per_fixture=256,normal_frames=4096,comparator_controls=12288,fault_frames=1024,frame_bytes=80,reference='Independent Python u32 increment-loop and all arena bytes; raw calibration before transformed execution',undefined_inputs_executed=False,excluded_bytes=0,skipped=0,normalization=0))
for role,sdk in [('old',a.old),('main',a.main),('noz3',a.noz3)]:
 env=dict(os.environ,LD_LIBRARY_PATH=str(sdk/'lib')+':/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib');env.pop('LD_PRELOAD',None);run([a.producer,r/role],role+'-produce',env=env)
 rows=[line.split('\t') for line in (r/role/'rows.tsv').read_text().splitlines()];assert len(rows)==4 and all(row[1:4]==(['0','0','1'] if role=='old' else ['1','1','0']) for row in rows)
for stem in stems:
 before=(r/'old'/(stem+'-before.ll')).read_bytes()
 assert (r/'old'/(stem+'-after.ll')).read_bytes()==before
 for role in ['main','noz3']:assert (r/role/(stem+'-before.ll')).read_bytes()==before
 assert (r/'main'/(stem+'-after.ll')).read_bytes()==(r/'noz3'/(stem+'-after.ll')).read_bytes()
wires=inputs();wire=b''.join(wires);gold=b''.join(model(b) for b in wires);(r/'inputs.bin').write_bytes(wire);(r/'expected.bin').write_bytes(gold);normal=controls=faults=0
for stem in stems:
 for variant in ['before','after']:
  for level in ['O0','O2']:
   tag=stem+'-'+variant+'-'+level;source=r/'main'/(stem+'-'+variant+'.ll')
   if level=='O2':
    selected=r/(tag+'.ll');run(['opt-21','-S','-passes=default<O2>',source,'-o',selected],tag+'-opt');source=selected
   obj=r/(tag+'.o');exe=r/tag;run(['llc-21','-O0','-filetype=obj','-mtriple=aarch64-linux-gnu',source,'-o',obj],tag+'-llc');run(['gcc','-O0',here/'driver.c',obj,'-o',exe],tag+'-link');actual=run([exe],tag+'-execute',data=wire);assert actual==gold,tag
   for i in range(256):
    got=actual[80*i:80*(i+1)];want=gold[80*i:80*(i+1)];assert got==want;normal+=1
    for offset in [0,8,72]:
     corrupt=bytearray(got);corrupt[offset]^=1;assert corrupt!=want;controls+=1
 text=(r/'main'/(stem+'-after.ll')).read_text();value='observed' if stem.startswith('d') else 'next';needle='ret i32 %'+value;assert text.count(needle)==1;source=r/(stem+'-fault.ll');source.write_text(text.replace(needle,'%mutated=xor i32 %'+value+',1\n ret i32 %mutated'));obj=r/(stem+'-fault.o');exe=r/(stem+'-fault');run(['llc-21','-O0','-filetype=obj','-mtriple=aarch64-linux-gnu',source,'-o',obj],stem+'-fault-llc');run(['gcc','-O0',here/'driver.c',obj,'-o',exe],stem+'-fault-link');actual=run([exe],stem+'-fault-execute',data=wire);assert actual==b''.join(model(b,True) for b in wires)
 for i in range(256):assert actual[80*i:80*(i+1)]!=gold[80*i:80*(i+1)];faults+=1
 print(stem,normal,faults,flush=True)
assert (normal,controls,faults)==(4096,12288,1024) and all(sha(p)==h for p,h in pins.items())
save('RESULT.json',dict(passed=True,normal_frames=normal,raw_calibration_frames=normal//2,transformed_frames=normal//2,comparator_controls=controls,fault_frames=faults,specified_fault_detections=faults,main_noz3_IR_byte_equal=4,excluded_bytes=0,skipped=0,normalization=0,source_runtime_pins_stable=True))
