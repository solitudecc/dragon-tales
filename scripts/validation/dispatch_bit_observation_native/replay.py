from pathlib import Path
import hashlib,json,os,struct,subprocess,time

base=Path('/home/ubuntu/vmc-runs')
control=base/'analysis-view-diagnostic-20261115-v2'
root=base/'analysis-bit-native-20261115-v1';root.mkdir(exist_ok=False)
producer=control/'native-producer-v2'
sdks={'old':base/'loop-pipeline-sp-main-sdk-20261115-v1',
      'new':base/'analysis-view-production-20261115-v1/SDK-main',
      'noz3':base/'analysis-view-production-20261115-v1/SDK-noz3'}
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,v):p.write_text(json.dumps(v,indent=2,sort_keys=True)+'\n')
def run(cmd,tag,env=None,data=None):
 cmd=[str(x) for x in cmd]
 p=subprocess.run(cmd,input=data,capture_output=True,env=env,timeout=180)
 (root/(tag+'.stdout')).write_bytes(p.stdout);(root/(tag+'.stderr')).write_bytes(p.stderr)
 commands.append(dict(argv=cmd,tag=tag,exit=p.returncode))
 save(root/'COMMANDS.json',commands)
 if p.returncode:raise RuntimeError(tag+': '+str(p.returncode)+' '+p.stderr.decode(errors='replace')[-1000:])
 return p.stdout
commands=[]
pins={str(p):sha(p) for p in [producer,control/'native_producer_v2.cpp',control/'native_driver.c',Path(__file__)]}
for sdk in sdks.values():
 for n in ['lib/libdragon.so','c_shim/libdragon_c.so']:pins[str(sdk/n)]=sha(sdk/n)
save(root/'PLAN.json',dict(pins=pins,word_widths=[8,16,32,64,128],modes=['one_hot_high','logical_sign','masked_sign','business_sub_or_sign'],input_rows_per_fixture=128,fixtures=20,levels=['O0','O2'],normal_runs=['before','after'],normal_comparisons=10240,comparator_controls=30720,fault_observations=2560,frame_bytes=80,reference='independent Python mathematical bit/word and byte-store model; raw LLVM calibration first',undefined_inputs_executed=False,excluded_bytes=0,skipped=0))
for role,sdk in sdks.items():
 env=dict(os.environ,LD_LIBRARY_PATH=str(sdk/'lib')+':/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib');env.pop('LD_PRELOAD',None)
 run([producer,root/role],role+'-produce',env=env)
 rows=(root/role/'rows.tsv').read_text().splitlines();assert len(rows)==20
 assert all(r.split('\t')[1:4]==(['0','0','0'] if role=='old' else ['1','1','0']) for r in rows)
for old in (root/'old').glob('*-before.ll'):
 assert old.read_bytes()==(root/'new'/old.name).read_bytes()==(root/'noz3'/old.name).read_bytes()
 assert old.read_bytes()==(root/'old'/old.name.replace('-before','-after')).read_bytes()
for p in (root/'new').glob('*-after.ll'):assert p.read_bytes()==(root/'noz3'/p.name).read_bytes()

def inputs(width):
 mask=(1<<width)-1;top=1<<(width-1)
 values=[0,1,2,7,mask,mask-1,top,top-1,16,31,32,1<<(width-5),int('aa'*(width//8),16),int('55'*(width//8),16),3,15]
 rows=[]
 for raw in values:
  raw&=mask
  for take in [0,1]:
   for alias in [0,1]:
    for limit in [0,64]:
     b=bytearray(96);struct.pack_into('<QQ',b,0,raw&((1<<64)-1),raw>>64)
     b[16]=take;b[17]=alias;struct.pack_into('<I',b,20,limit)
     b[32:]=bytes((i*37+len(rows)*11+width)&255 for i in range(64));rows.append(bytes(b))
 assert len(rows)==128;return rows

def model(width,mode,wire):
 lo,hi=struct.unpack_from('<QQ',wire);raw=(lo+(hi<<64))&((1<<width)-1)
 take,alias=wire[16:18];limit=struct.unpack_from('<I',wire,20)[0];offset=0 if alias else 32
 arena=bytearray(wire[32:]);arena[offset:offset+4]=limit.to_bytes(4,'little')
 top=1<<(width-1);base_value=2 if mode==0 else 60
 source=raw if mode!=3 else ((1<<width)-1) ^ (raw|((3-raw)% (1<<width)))
 sign=(source//top)%2
 amplitude=top if mode==0 else 1 if mode==1 else 16
 state=base_value+amplitude*sign if take else 7
 arena[:width//8]=state.to_bytes(width//8,'little')
 fits=int.from_bytes(arena[offset:offset+4],'little')>=width//8
 result=(11 if state==base_value else 22 if state==base_value+amplitude else 33) if fits else ((state&0xffffffff)^0x40000000)
 return result.to_bytes(8,'little')+arena+offset.to_bytes(8,'little')

normal=controls=faults=detected=nontrigger=0;records=[]
for width in [8,16,32,64,128]:
 for mode in range(4):
  stem=f'w{width}-m{mode}';wires=inputs(width);wire=b''.join(wires)
  expected=[model(width,mode,b) for b in wires];gold=b''.join(expected)
  (root/(stem+'-inputs.bin')).write_bytes(wire);(root/(stem+'-expected.bin')).write_bytes(gold)
  for variant in ['before','after']:
   source=root/'new'/(stem+'-'+variant+'.ll')
   for level in ['O0','O2']:
    tag=stem+'-'+variant+'-'+level;selected=source
    if level=='O2':
     selected=root/(tag+'.ll');run(['opt-21','-S','-passes=default<O2>',source,'-o',selected],tag+'-opt')
    obj=root/(tag+'.o');exe=root/tag
    run(['llc-21','-O0','-filetype=obj','-mtriple=aarch64-linux-gnu',selected,'-o',obj],tag+'-llc')
    run(['gcc','-O0',control/'native_driver.c',obj,'-o',exe],tag+'-link')
    actual=run([exe],tag+'-execute',data=wire)
    assert len(actual)==len(gold) and actual==gold,tag+' full model mismatch'
    for i,want in enumerate(expected):
     frame=actual[80*i:80*(i+1)];assert frame==want;normal+=1
     for offset in [0,8,72]:
      bad=bytearray(frame);bad[offset]^=1;assert bad!=want;controls+=1
    records.append(dict(tag=tag,complete_matches=128,bytes_per_frame=80,expected_sha256=hashlib.sha256(gold).hexdigest(),actual_sha256=hashlib.sha256(actual).hexdigest()))
  text=(root/'new'/(stem+'-after.ll')).read_text();assert text.count('ret i32 11')==1
  fault=root/(stem+'-fault.ll');fault.write_text(text.replace('ret i32 11','ret i32 12'))
  obj=root/(stem+'-fault.o');exe=root/(stem+'-fault')
  run(['llc-21','-O0','-filetype=obj','-mtriple=aarch64-linux-gnu',fault,'-o',obj],stem+'-fault-llc')
  run(['gcc','-O0',control/'native_driver.c',obj,'-o',exe],stem+'-fault-link')
  actual=run([exe],stem+'-fault-execute',data=wire);assert len(actual)==len(gold)
  for i,want in enumerate(expected):
   got=actual[80*i:80*(i+1)];trigger=int.from_bytes(want[:8],'little')==11
   assert (got!=want)==trigger;faults+=1
   if trigger:detected+=1
   else:assert got==want;nontrigger+=1
  save(root/'PROGRESS.json',dict(normal=normal,controls=controls,faults=faults,detected=detected,nontrigger=nontrigger,finished=stem))
  print(stem,'complete',normal,'faults',faults,flush=True)
assert normal==10240 and controls==30720 and faults==2560
assert all(sha(p)==h for p,h in pins.items())
save(root/'RESULT.json',dict(passed=True,normal_complete_matches=normal,raw_calibration_matches=normal//2,after_matches=normal//2,comparator_controls=controls,fault_observations=faults,specified_fault_detections=detected,full_nontrigger_matches=nontrigger,records=records,main_noz3_after_IR_byte_equal=20,source_runtime_pins_stable=True,skipped=0,excluded_bytes=0,normalization=0,new_machine_instruction_Unicorn_tests=False))
