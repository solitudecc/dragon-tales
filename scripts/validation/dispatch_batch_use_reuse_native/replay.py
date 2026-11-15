from pathlib import Path
import argparse,hashlib,json,os,struct,subprocess
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('producer',type=Path);p.add_argument('old',type=Path);p.add_argument('main',type=Path);p.add_argument('noz3',type=Path);a=p.parse_args()
root=a.root;root.mkdir(exist_ok=False);here=Path(__file__).resolve().parent;commands=[]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(name,x):(root/name).write_text(json.dumps(x,indent=2,sort_keys=True)+'\n')
def run(cmd,tag,data=None,env=None):
 cmd=list(map(str,cmd));result=subprocess.run(cmd,input=data,env=env,capture_output=True,timeout=180)
 (root/(tag+'.stdout')).write_bytes(result.stdout);(root/(tag+'.stderr')).write_bytes(result.stderr)
 commands.append(dict(command=cmd,tag=tag,exit=result.returncode));save('COMMANDS.json',commands)
 assert result.returncode==0,(tag,result.returncode,result.stderr[-1000:]);return result.stdout
pins={str(p):sha(p) for p in [a.producer,*[here/n for n in ['producer.cpp','fixture.hpp','driver.c','replay.py']]]}
for sdk in [a.old,a.main,a.noz3]:pins[str(sdk/'lib/libdragon.so')]=sha(sdk/'lib/libdragon.so')
save('PLAN.json',dict(source_runtime_pins=pins,fixtures=[16,70],carried_values=20,rows_per_fixture=512,levels=['O0','O2'],normal_comparisons=4096,comparator_controls=12288,fault_observations=1024,specified_fault_detections=1024,frame_bytes=80,reference='Independent modular-u32/byte-array Python recurrence; raw calibration precedes transformed execution',undefined_inputs_executed=False,exclusions=0,skips=0,normalization=0,hard_business_iteration_limit=256))
for role,sdk in [('old',a.old),('main',a.main),('noz3',a.noz3)]:
 env=dict(os.environ,LD_LIBRARY_PATH=str(sdk/'lib')+':/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib');env.pop('LD_PRELOAD',None)
 run([a.producer,root/role],role+'-producer',env=env)
 rows=[line.split('\t') for line in (root/role/'rows.tsv').read_text().splitlines()];assert len(rows)==4
 for row in rows:
  rejected=role=='old' and row[:2]==['70','180000'];assert row[2:5]==(['0','0','1'] if rejected else ['1','1','0']),row
for count in [16,70]:
 stem='n'+str(count);before=(root/'old'/(stem+'-before.ll')).read_bytes();after=(root/'main'/(stem+'-after.ll')).read_bytes()
 for role in ['main','noz3']:assert (root/role/(stem+'-before.ll')).read_bytes()==before
 for role in ['old','main','noz3']:assert (root/role/(stem+'-high.ll')).read_bytes()==after
 assert (root/'noz3'/(stem+'-after.ll')).read_bytes()==after
 assert (root/'old'/(stem+'-after.ll')).read_bytes()==(before if count==70 else after)
def inputs():
 raw=[0,1,2,3,0xffffffff,0xfffffffe,0x80000000,0x7fffffff,0xaaaaaaaa,0x55555555,0x12345678,0xdeadbeef,69,70,71,256]
 rows=[]
 for value in raw:
  for alias in range(4):
   for limit in [0,1,2,69,70,71,256,0xffffffff]:
    b=bytearray(96);struct.pack_into('<I',b,0,value);b[17]=alias;struct.pack_into('<I',b,20,limit);b[32:]=bytes((i*37+len(rows)*11)&255 for i in range(64));rows.append(bytes(b))
 assert len(rows)==512;return rows
def model(count,b,fault=False):
 raw=struct.unpack_from('<I',b)[0];at=[32,0,1,2][b[17]];arena=bytearray(b[32:]);arena[at:at+4]=b[20:24]
 carried=[raw^(0x9e37*(j+1)) for j in range(20)];state=counter=0
 while True:
  advanced=counter+1;stamp=carried[0]^carried[1];arena[:4]=stamp.to_bytes(4,'little');limit=int.from_bytes(arena[at:at+4],'little')
  if advanced>limit or advanced>256:break
  carried=[(v+state+j+1)&0xffffffff for j,v in enumerate(carried)];state=(state+1)%count;counter=advanced
 result=advanced
 for v in carried:result^=v
 if fault:result^=1
 return result.to_bytes(8,'little')+arena+at.to_bytes(8,'little')
wires=inputs();wire=b''.join(wires);(root/'inputs.bin').write_bytes(wire);normal=controls=faults=0;records=[]
for count in [16,70]:
 stem='n'+str(count);gold=b''.join(model(count,b) for b in wires);(root/(stem+'-expected.bin')).write_bytes(gold)
 for variant in ['before','after']:
  for level in ['O0','O2']:
   tag=stem+'-'+variant+'-'+level;source=root/'main'/(stem+'-'+variant+'.ll')
   if level=='O2':
    selected=root/(tag+'.ll');run(['opt-21','-S','-passes=default<O2>',source,'-o',selected],tag+'-opt');source=selected
   obj=root/(tag+'.o');exe=root/tag;run(['llc-21','-O0','-filetype=obj','-mtriple=aarch64-linux-gnu',source,'-o',obj],tag+'-llc');run(['gcc','-O0',here/'driver.c',obj,'-o',exe],tag+'-link')
   actual=run([exe],tag+'-execute',data=wire);assert actual==gold,(tag,'full 80-byte reference mismatch')
   for i in range(512):
    frame=actual[i*80:(i+1)*80];want=gold[i*80:(i+1)*80];assert frame==want;normal+=1
    for offset in [0,8,72]:
     bad=bytearray(frame);bad[offset]^=1;assert bad!=want;controls+=1
   records.append(dict(tag=tag,normal_frames=512,frame_bytes=80,actual_sha256=sha(root/(tag+'-execute.stdout')),expected_sha256=sha(root/(stem+'-expected.bin'))))
 text=(root/'main'/(stem+'-after.ll')).read_text();assert text.count('ret i32 %result19')==1
 source=root/(stem+'-fault.ll');source.write_text(text.replace('ret i32 %result19','%altered=xor i32 %result19,1\n ret i32 %altered'))
 obj=root/(stem+'-fault.o');exe=root/(stem+'-fault');run(['llc-21','-O0','-filetype=obj','-mtriple=aarch64-linux-gnu',source,'-o',obj],stem+'-fault-llc');run(['gcc','-O0',here/'driver.c',obj,'-o',exe],stem+'-fault-link');actual=run([exe],stem+'-fault-execute',data=wire)
 mutant=b''.join(model(count,b,True) for b in wires);assert actual==mutant
 for i in range(512):assert actual[i*80:(i+1)*80]!=gold[i*80:(i+1)*80];faults+=1
 print(stem,'normal',normal,'fault detections',faults,flush=True)
assert (normal,controls,faults)==(4096,12288,1024);assert all(sha(p)==h for p,h in pins.items())
save('RESULT.json',dict(passed=True,normal_complete_matches=normal,raw_calibration_matches=normal//2,transformed_matches=normal//2,comparator_controls=controls,fault_observations=faults,specified_fault_detections=faults,records=records,old_high_work_main_noz3_low_work_IR_byte_equal=2,skipped=0,excluded_bytes=0,normalization=0,source_runtime_pins_stable=True,new_machine_instruction_Unicorn_campaign=False))
