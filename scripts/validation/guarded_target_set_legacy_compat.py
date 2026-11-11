#!/usr/bin/env python3
"""Recheck the original single-candidate API byte-for-byte; no native executions."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('oldroot',type=Path);p.add_argument('label');p.add_argument('runtime',type=Path);a=p.parse_args()
out=a.root/a.label;out.mkdir(exist_ok=False);env=dict(os.environ,LD_LIBRARY_PATH=str(a.runtime))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
inputs=[a.oldroot/'before'/(m+'.ll') for m in ('plain','store_before','store_after','side_reentry','seed_call','duplicate_seed')]
pins={str(f):sha(f) for f in [a.oldroot/'driver',a.oldroot/'source/guarded_transition_native_driver.cpp',*inputs,*a.runtime.glob('*.so')]}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(a.oldroot/'driver')],env=env,text=True))
rows=[]
for api in ('core','auto'):
 for hint in (43,99):
  dest=out/f'{api}-hint{hint}';dest.mkdir()
  for source in inputs:
   output=dest/source.name;report=output.with_suffix('.json');old=a.root/'legacy-baseline-D-main'/dest.name/source.name
   run=subprocess.run([str(a.oldroot/'driver'),str(source),str(output),str(report),api,str(hint)],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
   output.with_suffix('.log').write_bytes(run.stdout);output.with_suffix('.exit').write_text(str(run.returncode)+'\n')
   assert run.returncode==0,(api,hint,source,run.stdout)
   assert old.read_bytes()==output.read_bytes(),(api,hint,source)
   rows.append({'api':api,'hint':hint,'mode':source.stem,'input':str(source),'input_sha':sha(source),'exit':run.returncode,'old_sha':sha(old),'new_sha':sha(output)})
assert pins=={f:sha(Path(f)) for f in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2)+'\n');(out/'comparison.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps({'outputs_byte_equal':len(rows),'native_executions':0}))
