"""Audit saved native streams only; no target/compiler execution."""
from pathlib import Path
import argparse,ast,hashlib,json,struct
p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();r=a.root
source=Path(__file__).with_name('replay.py');tree=ast.parse(source.read_text())
defs=[node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name in ['inputs','model']]
assert len(defs)==2;scope={'struct':struct};exec(compile(ast.Module(body=defs,type_ignores=[]),str(source),'exec'),scope)
wires=scope['inputs']();assert (r/'inputs.bin').read_bytes()==b''.join(wires)
normal=controls=faults=streams=0
for count in [16,70]:
 stem='n'+str(count);expected=[scope['model'](count,b) for b in wires];assert (r/(stem+'-expected.bin')).read_bytes()==b''.join(expected)
 for variant in ['before','after']:
  for level in ['O0','O2']:
   data=(r/(stem+'-'+variant+'-'+level+'-execute.stdout')).read_bytes();assert len(data)==512*80;streams+=1
   for i,want in enumerate(expected):
    got=data[80*i:80*(i+1)];assert got==want;normal+=1
    for at in [0,8,72]:
     corrupt=bytearray(got);corrupt[at]^=1;assert corrupt!=want;controls+=1
 data=(r/(stem+'-fault-execute.stdout')).read_bytes();assert len(data)==512*80;streams+=1
 for i,want in enumerate(expected):
  got=data[80*i:80*(i+1)];assert got==scope['model'](count,wires[i],True) and got!=want;faults+=1
result=json.loads((r/'RESULT.json').read_bytes());assert result['passed'] and (normal,controls,faults)==(4096,12288,1024)
assert (result['normal_complete_matches'],result['comparator_controls'],result['specified_fault_detections'])==(normal,controls,faults)
out=dict(passed=True,saved_streams=streams,normal_frames=normal,fault_frames=faults,comparator_controls=controls,new_target_executions=0,reference_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
(r/'SAVED-AUDIT.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
