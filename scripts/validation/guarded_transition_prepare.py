import os,sys,json,hashlib,subprocess
from pathlib import Path
root=Path('/home/cc/vmc-runs/guarded-transition-native-20261112'); label=sys.argv[1];runtime=Path(sys.argv[2]);out=root/label;out.mkdir(exist_ok=False)
env=dict(os.environ,LD_LIBRARY_PATH=str(runtime))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
pins={str(p):sha(p) for p in [root/'driver',*sorted((root/'source').glob('*')),*sorted(runtime.glob('*.so'))]}
(out/'pins-before.json').write_text(json.dumps(pins,indent=2))
(out/'ldd.txt').write_text(subprocess.check_output(['ldd',str(root/'driver')],env=env,text=True))
results=[]
for api in ('core','auto'):
 for hint in (43,99):
  dest=out/f'{api}-hint{hint}';dest.mkdir()
  for mode in ('plain','store_before','store_after','side_reentry','seed_call','duplicate_seed'):
   output=dest/(mode+'.ll');report=dest/(mode+'.json');cmd=[str(root/'driver'),str(root/'before'/(mode+'.ll')),str(output),str(report),api,str(hint)]
   p=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
   (dest/(mode+'.log')).write_bytes(p.stdout);(dest/(mode+'.exit')).write_text(str(p.returncode)+'\n')
   results.append({'api':api,'hint':hint,'mode':mode,'exit':p.returncode,'report':json.loads(report.read_text()) if report.exists() else None})
(out/'positive-summary.json').write_text(json.dumps(results,indent=2))
neg=out/'reject';neg.mkdir()
for name,source,api,hint in [('call','reject_call','core',43),('budget','plain','core_budget',43),('core_missing','plain','core',777),('manifest_missing','plain','auto_none',43),('manifest_bad','plain','auto_bad',43),('layout32','plain','auto_32',43),('auto_missing','plain','auto',777)]:
 output=neg/(name+'.ll');report=neg/(name+'.json');p=subprocess.run([str(root/'driver'),str(root/'before'/(source+'.ll')),str(output),str(report),api,str(hint)],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 (neg/(name+'.log')).write_bytes(p.stdout);(neg/(name+'.exit')).write_text(str(p.returncode)+'\n')
 assert p.returncode==3,(name,p.returncode,p.stdout)
 r=json.loads(report.read_text());assert r['unchanged_on_rejection'] and r['verified_output'] and r['identity_preserved'] and not output.exists(),name
assert all(r['exit']==0 and r['report']['changed'] and r['report']['verified_output'] and r['report']['identity_preserved'] for r in results),[(r['api'],r['hint'],r['mode'],r['exit'],r['report']) for r in results if r['exit']]
assert pins=={p:sha(Path(p)) for p in pins}
(out/'pins-after.json').write_text(json.dumps(pins,indent=2));print(label,'24 positive transformed, 7 rejection controls passed')
