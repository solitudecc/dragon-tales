#!/usr/bin/env python3
"""Re-read every saved full-arena observation, without native execution."""
import argparse
import collections
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);p.add_argument('model',type=Path);a=p.parse_args()
assert hashlib.sha256(a.model.read_bytes()).hexdigest()=='0d12c51e1088522592567f7520219aa213a486c7ddba3cb8d86f9f7c67003c1c','unexpected frozen model'
spec=importlib.util.spec_from_file_location('frozen_guarded_entry_model',a.model);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
r=json.loads((a.root/'report.json').read_text());address=r['arena_address']
assert r['inputs']==1120 and r['arena_bytes']==139264 and r['snapshot_bytes']==139272
assert r['ok'] and r['mismatches']==0
assert not any(r[k] for k in ('excluded','skipped','normalized','errors'))
assert r['comparator_controls']==15683 and r['guard_page_fault_control_rc'] in (-11,-7)
assert all(c['rc']==0 for c in r['commands'])
for f,h in r['artifact_sha256'].items():assert hashlib.sha256((a.root/f).read_bytes()).hexdigest()==h,f
assert {str(f.relative_to(a.root)) for f in a.root.rglob('*') if f.is_file()}==set(r['artifact_sha256'])|{'report.json'}
stages=('original','guarded','optimized') if r['guarded'] else ('original',)
expected_labels={s+'-'+o for s in stages for o in ('O0','O2')}|{'control-'+f+'-'+o for f in m.FAULTS for o in ('O0','O2')}
rows=collections.defaultdict(list)
for row in r['rows']:
    label=('control-'+row['fault'] if 'fault' in row else row['stage'])+'-'+row['level'];rows[label].append(row)
assert set(rows)==expected_labels
normal=faults=required=detected=0
for label,records in rows.items():
    fault=records[0].get('fault');keys=[k for k in m.INPUTS if not fault or k[0]==m.FAULTS[fault]]
    assert [tuple(row['key']) for row in records]==keys,label
    with gzip.open(a.root/(label+'.bin.gz'),'rb') as stream:
        for key,row in zip(keys,records):
            actual=stream.read(m.SNAPSHOT_BYTES);expected=m.model(key,address,fault)
            assert actual==expected,(label,key)
            assert hashlib.sha256(actual).hexdigest()==row['sha256']
            if fault:
                wanted=expected!=m.model(key,address)
                assert row['required']==wanted and row['detected']==wanted
                faults+=1;required+=wanted;detected+=wanted
            else:assert row['match'];normal+=1
        assert stream.read(1)==b'',label
with gzip.open(a.root/'expected.bin.gz','rb') as stream:
    for key in m.INPUTS:assert stream.read(m.SNAPSHOT_BYTES)==m.model(key,address)
    assert stream.read(1)==b''
assert normal==(6720 if r['guarded'] else 2240)
assert (faults,required,detected)==(1120,560,560)
assert (normal,faults,required,detected)==tuple(r[k] for k in ('normal','fault_observations','fault_required','fault_detected'))
expected_probes={(s,o,k) for s in stages for o in ('O0','O2') for k in ('short','wrong_pc','null_zero')}
assert {(q['stage'],q['level'],q['kind']) for q in r['guard_pages']}==expected_probes
assert len(r['guard_pages'])==len(expected_probes)
for q in r['guard_pages']:
    assert q['rc']==0
    data=json.loads((a.root/(q['stage']+'-'+q['level']+'-'+q['kind']+'.stdout')).read_text())
    snapshot=bytes.fromhex(data['snapshot']);assert len(snapshot)==880==data['bytes']
    assert data['ok'] and data['kind']==q['kind'] and data['stage']==q['stage']
    assert hashlib.sha256(snapshot).hexdigest()==data['sha256']
    assert snapshot[:8]==bytes(8) and snapshot[520:776]==bytes([0xab])*256
    pc=0x1001 if q['kind']=='wrong_pc' else 0x1000
    fields=(5,0,pc,pc,1,0,5) if q['kind']=='wrong_pc' else (5,0,pc,pc,0,0,3)
    assert snapshot[776:832]==struct.pack('<7Q',*fields)
    assert struct.unpack_from('<3Q',snapshot,832)==(2,2,512)
    assert struct.unpack_from('<Q',snapshot,872)[0]==(0 if q['kind']=='null_zero' else 255)
print(json.dumps({'ok':True,'guarded':r['guarded'],'normal':normal,'fault_observations':faults,'required':required,'detected':detected,
                  'streams':len(rows),'expected_streams':1,'guard_pages':len(expected_probes),'badread_control_rc':r['guard_page_fault_control_rc'],
                  'report_sha256':hashlib.sha256((a.root/'report.json').read_bytes()).hexdigest(),'model_sha256':hashlib.sha256(a.model.read_bytes()).hexdigest(),
                  'artifacts':len(r['artifact_sha256']),'new_native_executions':0},sort_keys=True))
