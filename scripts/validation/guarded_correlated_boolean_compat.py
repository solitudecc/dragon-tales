#!/usr/bin/env python3
"""Replay frozen H/H2 public recipes without modifying their archived roots."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('oldroot',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args();out=a.root/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def same(a,b):
 if a!=b:raise AssertionError('legacy IR byte difference')
files=[a.oldroot/'driver',a.oldroot/'path-driver',*sorted((a.oldroot/'before').glob('*.ll')),*sorted((a.oldroot/'path-before').glob('*.ll')),*sorted((a.oldroot/'main').glob('*/*.ll')),*sorted((a.oldroot/'main-path').glob('*/*.ll')),*a.runtime.glob('*.so')]
pins={str(f):sha(f) for f in files};(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n')
results=[]
for group,driver,before,count in (('main','driver','before',21),('main-path','path-driver','path-before',9)):
 (out/(group+'-ldd.txt')).write_text(subprocess.check_output(['ldd',str(a.oldroot/driver)],env=env,text=True))
 rows=json.loads((a.oldroot/group/'summary.json').read_text());assert len(rows)==count
 for row in rows:
  recipe,mode=row['recipe'],row['mode'];dest=out/group/recipe;dest.mkdir(parents=True,exist_ok=True);output=dest/(mode+'.ll');report=dest/(mode+'.json')
  run=subprocess.run([str(a.oldroot/driver),str(a.oldroot/before/(mode+'.ll')),str(output),str(report),recipe],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  (dest/(mode+'.log')).write_bytes(run.stdout);(dest/(mode+'.exit')).write_text(str(run.returncode)+'\n');assert run.returncode==0
  data=json.loads(report.read_text());assert data['verified_output'] and data['identity_preserved'] and data['metadata_preserved']
 for f in sorted((out/group).glob('*/*.ll')):
  original=a.oldroot/group/f.relative_to(out/group);left=original.read_bytes();right=f.read_bytes();same(left,right)
  damaged=bytearray(right);damaged[len(damaged)//2]^=1
  try:same(left,damaged)
  except AssertionError:pass
  else:raise AssertionError('legacy byte comparator control escaped')
  results.append({'path':str(f.relative_to(out)),'old_sha':sha(original),'new_sha':sha(f),'bytes':len(left)})
assert len(results)==94
assert pins=={path:sha(Path(path)) for path in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n')
(out/'comparison.json').write_text(json.dumps({'files':results,'all_equal':True,'fault_controls':94,'native_executions_added':0},indent=2)+'\n');print('94 exact H/H2 LLVM outputs and 94 comparator controls; no new native executions')
