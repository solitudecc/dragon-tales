"""Saved-stream audit only: no compiler or target execution."""
from pathlib import Path
import argparse,json
from oracle import inputs,model
p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();r=a.root
wires=inputs();gold=b''.join(model(b) for b in wires);assert (r/'inputs.bin').read_bytes()==b''.join(wires) and (r/'expected.bin').read_bytes()==gold
normal=controls=faults=streams=0
for stem in ['d40','r40','d80','r80']:
 for variant in ['before','after']:
  for level in ['O0','O2']:
   actual=(r/(stem+'-'+variant+'-'+level+'-execute.stdout')).read_bytes();assert actual==gold;streams+=1
   for i in range(256):
    got=actual[80*i:80*(i+1)];want=gold[80*i:80*(i+1)];assert got==want;normal+=1
    for offset in [0,8,72]:
     corrupt=bytearray(got);corrupt[offset]^=1;assert corrupt!=want;controls+=1
 actual=(r/(stem+'-fault-execute.stdout')).read_bytes();assert actual==b''.join(model(b,True) for b in wires);streams+=1
 for i in range(256):assert actual[80*i:80*(i+1)]!=gold[80*i:80*(i+1)];faults+=1
assert (normal,controls,faults,streams)==(4096,12288,1024,20)
result=json.loads((r/'RESULT.json').read_bytes());assert result['passed'] and (result['normal_frames'],result['comparator_controls'],result['specified_fault_detections'])==(normal,controls,faults)
out=dict(passed=True,saved_streams=streams,normal_frames=normal,fault_frames=faults,comparator_controls=controls,new_target_executions=0);(r/'SAVED-AUDIT.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out))
