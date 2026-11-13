from pathlib import Path
import json,hashlib
r=Path('/home/cc/vmc-runs/partial-address-subset-native-20261113');sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
def rows(label):return json.loads((r/label/'summary.json').read_text())
def path(label,x):return r/label/(x['profile'] if x['action']=='J' else x['action']+'-'+x['profile'])/(x['mode']+'.ll')
new=rows('main-v3');nz=rows('noz3-v3');assert len(new)==len(nz)==62
result={'main_noz3_byte_equal':0,'damaged_byte_controls':0,'old_new_unchanged_each':{},'old_precision_fail_each':{},'native_executions_added':0}
for x,y in zip(new,nz):
 assert (x['action'],x['profile'],x['mode'])==(y['action'],y['profile'],y['mode']);a=path('main-v3',x).read_bytes();b=path('noz3-v3',y).read_bytes();assert a==b;result['main_noz3_byte_equal']+=1
 damaged=bytearray(b);damaged[len(b)//2]^=1
 try:assert a==damaged
 except AssertionError:result['damaged_byte_controls']+=1
 else:raise AssertionError('damaged IR escaped')
for old,newlabel in [('old-main','main-v3'),('old-noz3','noz3-v3')]:
 oldrows=rows(old);newrows=rows(newlabel);assert len(oldrows)==62;n=0
 for x,y in zip(oldrows,newrows):
  precision=x['action']=='J' and x['profile'] in ('missing_first','missing_last','short_row','noncase','malicious') and x['mode']!='call_inside'
  if precision:assert x['report']['unchanged'] and not y['report']['unchanged']
  else:assert path(old,x).read_bytes()==path(newlabel,y).read_bytes(),(old,x['action'],x['profile'],x['mode']);n+=1
 result['old_new_unchanged_each'][old]=n
 gate=json.loads((r/old/'API-GATES.json').read_text());assert gate['common_errors']==[] and len(gate['precision'])==15 and not any(x['passed'] for x in gate['precision']);result['old_precision_fail_each'][old]=15
(r/'COMPAT-REVIEW.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
