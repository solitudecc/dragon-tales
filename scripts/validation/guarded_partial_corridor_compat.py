#!/usr/bin/env python3
"""Check all 45 frozen I2 IR outputs using their original I1 input paths."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('i1',type=Path);p.add_argument('i2',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args();out=a.root/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def same(a,b):
 if a!=b:raise AssertionError('I2 LLVM bytes changed')
files=[a.i1/'driver',*a.i1.glob('before/*.ll'),*a.i1.glob('supplement-before/*.ll'),*a.i2.glob('main/*/*.ll'),*a.i2.glob('main-supplement/*/*.ll'),*a.runtime.glob('*.so')]
pins={str(f):sha(f) for f in files};(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n')
(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(a.i1/'driver')],env=env,text=True))
results=[]
for group,before,count in (('main','before',13),('main-supplement','supplement-before',1)):
 rows=json.loads((a.i2/group/'summary.json').read_text());assert len(rows)==count
 for row in rows:
  recipe,mode=row['recipe'],row['mode'];dest=out/group/recipe;dest.mkdir(parents=True,exist_ok=True);output=dest/(mode+'.ll');report=dest/(mode+'.json')
  args=[str(a.i1/'driver'),str(a.i1/before/(mode+'.ll')),str(output),str(report),recipe]
  run=subprocess.run(args,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  (dest/(mode+'.command.json')).write_text(json.dumps(args)+'\n');(dest/(mode+'.log')).write_bytes(run.stdout);(dest/(mode+'.exit')).write_text(str(run.returncode)+'\n');assert run.returncode==0
  d=json.loads(report.read_text());assert d['verified_output'] and d['identity_preserved'] and d['metadata_preserved']
 for f in sorted((out/group).glob('*/*.ll')):
  original=a.i2/group/f.relative_to(out/group);left=original.read_bytes();right=f.read_bytes();same(left,right)
  damaged=bytearray(right);damaged[len(damaged)//2]^=1
  try:same(left,damaged)
  except AssertionError:pass
  else:raise AssertionError('I2 byte control escaped')
  results.append({'path':str(f.relative_to(out)),'old_sha':sha(original),'new_sha':sha(f),'bytes':len(left)})
assert len(results)==45
assert pins=={path:sha(Path(path)) for path in pins};(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n')
(out/'comparison.json').write_text(json.dumps({'files':results,'all_equal':True,'fault_controls':45,'native_executions_added':0},indent=2)+'\n');print('45 exact I2 LLVM outputs and 45 comparator controls; no new native executions')
