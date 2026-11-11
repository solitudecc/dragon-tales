#!/usr/bin/env python3
"""Frozen-input structural, history-byte and complete snapshot audit."""
import argparse,gzip,hashlib,importlib.util,json,re
from pathlib import Path
SOURCE='b8b34b5119207e67cb10f7a331f0ff6b5ad29f8a96cf22fd45e23ed3d1f4d411'
GAIN=('plain','frozen_leaves','call_before')
def sha(data):return hashlib.sha256(data).hexdigest()
def precision(root,group,model):
 g=root/group;old=group.startswith('old');stats=json.loads((g/'adapter-report.json').read_text());assert set(stats)==set(model.MODES)
 for mode,item in stats.items():
  assert item['exit']==0 and item['stats']['input_verified'] and item['stats']['output_verified']
  text=(g/'after'/(mode+'.ll')).read_text();gain=mode in GAIN and not old
  match=re.search(r'(?ms)^known:[^\n]*\n(.*?)(?=^[A-Za-z_.][A-Za-z_0-9.]*:|^})',text);assert match
  targets=re.findall(r'br label %([^\s]+)',match.group(1));assert len(targets)==1
  assert (targets[0]!='join')==gain
  assert re.search(r'(?m)^unknown:[^\n]*\n\s*br label %join\b',text)
  assert len(re.findall(r'%state = load i64, ptr %targetptr, align 1',text))==1
  if gain:
   assert item['stats']['loadsForwarded']==1 and item['stats']['changed'] and item['stats']['complete'] and item['stats']['budgetExhausted']==0
   copy=re.search(r'(?ms)^read\.guest\.edge[^:]*:[^\n]*\n(.*?)(?=^[A-Za-z_.][A-Za-z_0-9.]*:|^})',text);assert copy
   assert re.search(r'xor i64 43, %leaf0',copy.group(1)) and 'load i64' not in copy.group(1)
  else:assert item['stats']['loadsForwarded']==0 and not item['stats']['changed']
  if old and mode in GAIN:assert not item['stats']['complete'] and item['stats']['budgetExhausted']>0
 return {'required_gains':list(GAIN) if not old else [],'actual_gains':[] if old else list(GAIN),'unknown_paths_preserved':True}
def main():
 p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--group');p.add_argument('--precision-only',action='store_true');a=p.parse_args();r=a.root
 report={'native_executions_added':0,'precision':{},'history':{},'snapshots':{},'errors':[]}
 try:
  source=r/'source/guest_index_cache_oracle.py';assert sha(source.read_bytes())==SOURCE
  spec=importlib.util.spec_from_file_location('cache_model',source);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
  for mode in m.MODES:assert (r/'before'/(mode+'.ll')).read_text()==m.ir(mode)
  for fault,mode in m.FAULTS.items():assert (r/'before'/('control_'+fault+'.ll')).read_text()==m.ir(mode,fault)
  assert json.loads((r/'before/inputs.json').read_text())==[list(x) for x in m.INPUTS]
  assert (r/'before/helper.c').read_text()==m.HELPER
  groups=(a.group,) if a.group else ('old-main','old-noz3','main-v1','noz3-v1')
  for group in groups:report['precision'][group]=precision(r,group,m)
  if a.precision_only:return
  ref=r/'history-reference';manifest=json.loads((ref/'REFERENCE-MANIFEST.json').read_text())
  for name,item in manifest['files'].items():
   data=(ref/name).read_bytes();assert sha(data)==item['sha256'] and len(data)==item['size']
  prior=json.loads((ref/'prior-report.json').read_text());assert prior['ok'] and prior['normal_comparisons']==6480 and prior['controls_detected']==2196
  assert prior['precision_ok'] is False and prior['single_precision_ok'] and prior['additional_precision_ok']
  for name,pin in prior['source_sha256'].items():assert sha((r/'source'/name).read_bytes())==pin
  for group in groups:
   g=r/group;pins=json.loads((g/'pins-before.json').read_text());assert pins==json.loads((g/'pins-after.json').read_text())
   for name,pin in pins.items():
    if '/runtime-' not in name or not group.startswith('old'):assert sha(Path(name).read_bytes())==pin
   backend=[name for name in pins if name.endswith('/libdragon.so')];assert len(backend)==1
   assert str(Path(backend[0]).parent) in (g/'driver-ldd.txt').read_text()
   history=json.loads((g/'history-report.json').read_text());assert len(history)==9
   for name,stats in history.items():
    for stage in ('before','after'):assert (g/'history'/(name+'-'+stage+'.ll')).read_bytes()==(ref/(name+'-'+stage+'.ll')).read_bytes()
    assert stats['unknown_retained'] and not stats['invalid']
   gained=lambda n:bool(history[n]['redirected'] and history[n]['forwarded'] and history[n]['target_loads']==1)
   whole=all(gained(n) for n in history if not n.endswith('_branch'))
   single=all(gained(n) for n in history if n.endswith('_one'));branch=all(gained(n) for n in history if n.endswith('_branch'))
   assert not whole and single and branch
   report['history'][group]={'nine_before_and_after_byte_equal_to_prior':True,'original_whole6_gate':whole,'single3_gate':single,'branch3_gate':branch,'native_recounted':0}
   if group.startswith('old'):continue
   frames=streams=normal=faultobs=required_total=comparator=0
   for opt in (0,2):
    out=g/f'results-O{opt}';stats=json.loads((out/'report.json').read_text());assert not stats['errors'];guest=stats['guest_address'];expected_names=set()
    assert stats['normal']==len(m.INPUTS)*2 and stats['fault_observations']==288 and stats['comparator_controls']==len(m.INPUTS)*22
    normal+=stats['normal'];faultobs+=stats['fault_observations'];comparator+=stats['comparator_controls']
    for stage in ('before','after'):
     for mode in m.MODES:
      name=stage+'-'+mode+'.snapshots.gz';expected_names.add(name)
      with gzip.open(out/name,'rb') as stream:
       for key in m.INPUTS:
        if key[0]!=mode:continue
        m.same(stream.read(16+m.ARENA),m.model(key,guest),(group,opt,stage,key));frames+=1
       assert not stream.read(1)
      streams+=1
    for fault,mode in m.FAULTS.items():
     name='control-'+fault+'.snapshots.gz';expected_names.add(name);required=caught=0
     with gzip.open(out/name,'rb') as stream:
      for key in m.INPUTS:
       if key[0]!=mode:continue
       actual=stream.read(16+m.ARENA);damaged=m.model(key,guest,fault);expected=m.model(key,guest);m.same(actual,damaged,(group,opt,fault,key));frames+=1
       if damaged!=expected:
        required+=1
        try:m.same(actual,expected,'saved control')
        except AssertionError:caught+=1
        else:raise AssertionError('saved control escaped')
      assert not stream.read(1)
     streams+=1;required_total+=required;assert required==caught>0 and stats['faults'][fault]=={'required':required,'caught':caught}
    assert {p.name for p in out.glob('*.snapshots.gz')}==expected_names
   assert frames==1920 and streams==40 and normal==1344 and faultobs==576 and required_total==320 and comparator==14784
   report['snapshots'][group]={'frames':frames,'streams':streams,'normal':normal,'fault_observations':faultobs,'specified_faults_caught':required_total,'comparator_controls_recorded':comparator}
  if not a.group:
   for mode in m.MODES:assert (r/'main-v1/after'/(mode+'.ll')).read_bytes()==(r/'noz3-v1/after'/(mode+'.ll')).read_bytes()
 except Exception as error:report['errors'].append(repr(error));raise
 finally:
  name=('precision-'+a.group+'.json') if a.precision_only else 'final-audit.json'
  (r/name).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
if __name__=='__main__':main()
