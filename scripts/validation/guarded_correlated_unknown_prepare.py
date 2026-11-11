#!/usr/bin/env python3
"""Force scenario exploration before refusing an unknown integer arm."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args();r=a.root;out=r/a.label;out.mkdir(exist_ok=False);(out/'normal').mkdir();env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pins={str(p):sha(p) for p in [r/'driver',*sorted((r/'source').glob('*')),*sorted((r/'supplement-before').glob('*')),*sorted(a.runtime.glob('*.so'))] if p.is_file()}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(r/'driver')],env=env,text=True))
output=out/'normal/unknown_arm.ll';report=out/'normal/unknown_arm.json'
p=subprocess.run([str(r/'driver'),str(r/'supplement-before/unknown_arm.ll'),str(output),str(report),'normal'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
(out/'normal/unknown_arm.log').write_bytes(p.stdout);(out/'normal/unknown_arm.exit').write_text(str(p.returncode)+'\n')
d=json.loads(report.read_text());(out/'summary.json').write_text(json.dumps([{'mode':'unknown_arm','recipe':'normal','exit':p.returncode,'report':d}],indent=2)+'\n')
assert p.returncode==0 and d['option_accepted'] and d['verified_output'] and d['identity_preserved'] and d['metadata_preserved'] and d['unchanged']
assert len(d['rounds'])==1 and d['rounds'][0]['ok'] and d['rounds'][0]['runs'][0]['counters'].get('rewritten_edges',0)==0
assert output.read_bytes()==Path(str(output)+'.parsed-before.ll').read_bytes()
assert pins=={path:sha(Path(path)) for path in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');print(json.dumps({'label':a.label,'rows':1,'unknown_scenario_rejected_unchanged':True}))
