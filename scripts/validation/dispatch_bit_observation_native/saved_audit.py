from pathlib import Path
import ast,hashlib,json,struct
root=Path('/home/ubuntu/vmc-runs/analysis-bit-native-20261115-v1')
source=Path('/home/ubuntu/vmc-runs/analysis-view-diagnostic-20261115-v2/native_replay.py')
tree=ast.parse(source.read_text());functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in ['inputs','model']]
assert len(functions)==2
ns={'struct':struct};exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),ns)
normal=controls=faults=detected=nontrigger=0;frames=[]
for width in [8,16,32,64,128]:
 for mode in range(4):
  stem=f'w{width}-m{mode}';wires=ns['inputs'](width)
  assert (root/(stem+'-inputs.bin')).read_bytes()==b''.join(wires)
  expected=[ns['model'](width,mode,b) for b in wires]
  assert (root/(stem+'-expected.bin')).read_bytes()==b''.join(expected)
  for variant in ['before','after']:
   for level in ['O0','O2']:
    name=stem+'-'+variant+'-'+level+'-execute.stdout';raw=(root/name).read_bytes();assert len(raw)==80*128
    for i,want in enumerate(expected):
     got=raw[80*i:80*(i+1)];assert got==want;normal+=1
     for pos in [0,8,72]:
      bad=bytearray(got);bad[pos]^=1;assert bad!=want;controls+=1
    frames.append(dict(file=name,sha256=hashlib.sha256(raw).hexdigest(),frames=128))
  name=stem+'-fault-execute.stdout';raw=(root/name).read_bytes();assert len(raw)==80*128
  for i,want in enumerate(expected):
   got=raw[80*i:80*(i+1)];trigger=int.from_bytes(want[:8],'little')==11
   changed=(12).to_bytes(8,'little')+want[8:] if trigger else want
   assert got==changed;faults+=1
   if trigger:detected+=1
   else:nontrigger+=1
  frames.append(dict(file=name,sha256=hashlib.sha256(raw).hexdigest(),frames=128))
assert (normal,controls,faults,detected,nontrigger)==(10240,30720,2560,648,1912)
result=dict(passed=True,normal_saved_frames=normal,comparator_controls_rechecked=controls,fault_saved_frames=faults,specified_fault_detections=detected,complete_nontrigger_matches=nontrigger,streams=frames,new_target_executions=0,excluded_bytes=0,normalization=0)
(root/'SAVED-AUDIT.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
print('saved audit passes',normal,'normal',faults,'faults; zero new target executions')
