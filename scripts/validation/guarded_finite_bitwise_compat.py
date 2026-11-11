#!/usr/bin/env python3
"""Re-run frozen F API recipes with candidate G; compare all 69 LLVM outputs."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('oldroot',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args()
out=a.root/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
rows=json.loads((a.oldroot/'main/summary.json').read_text());assert len(rows)==15
files=[a.oldroot/'driver',a.oldroot/'source/guarded_state_corridor_native_driver.cpp',*sorted((a.oldroot/'before').glob('*.ll')),*a.runtime.glob('*.so')]
pins={str(f):sha(f) for f in files};(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(a.oldroot/'driver')],env=env,text=True))
results=[]
for row in rows:
 recipe,mode=row['recipe'],row['mode'];dest=out/recipe;dest.mkdir(exist_ok=True)
 output=dest/(mode+'.ll');report=dest/(mode+'.json');source=a.oldroot/'before'/(mode+'.ll')
 run=subprocess.run([str(a.oldroot/'driver'),str(source),str(output),str(report),recipe],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (dest/(mode+'.log')).write_bytes(run.stdout);(dest/(mode+'.exit')).write_text(str(run.returncode)+'\n');assert run.returncode==0,(row,run.stdout)
 data=json.loads(report.read_text());assert data['verified_output'] and data['identity_preserved']
for f in sorted(out.glob('*/*.ll')):
 original=a.oldroot/'main'/f.relative_to(out)
 results.append({'path':str(f.relative_to(out)),'old_sha':sha(original),'new_sha':sha(f),'equal':original.read_bytes()==f.read_bytes()})
assert len(results)==69 and all(v['equal'] for v in results),results
assert pins=={f:sha(Path(f)) for f in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'comparison.json').write_text(json.dumps({'files':results,'all_equal':True,'native_executions_added':0},indent=2)+'\n');print('69 unchanged F IR outputs; no new native executions')
