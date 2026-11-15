import hashlib,json,struct
from pathlib import Path

base=Path('/home/ubuntu/vmc-runs')
rows=[];controls=0
for name,modules,count in [('large-combo-replay-main-20261115-v2',8,128),('large-combo-replay-noz3-20261115-v2',4,64)]:
 r=base/name;x=json.loads((r/'RESULT.json').read_text())
 assert x['summary']=={'match':count} and len(x['rows'])==count and len(x['modules'])==modules
 assert x['source_runtime_input_pins_stable']
 assert all(m['Graph_unchanged'] and not m['unexpected_unreachable'] and not m['unlifted'] and not m.get('error') for m in x['modules'])
 assert len({(row['name'],row['trial']) for row in x['rows']})==count
 for row in x['rows']:
  c_level=row['name'].split('-')[1]
  expected=(base/'large-combo-array16-reference-20261115-v1'/('combined-'+c_level)/f"expected-{row['trial']}.bin").read_bytes()
  raw=(r/row['name']/f"output-{row['trial']}.bin").read_bytes()
  initial=(r/row['name']/f"input-{row['trial']}.bin").read_bytes()
  source_input=(base/'large-combo-array16-reference-20261115-v1'/('combined-'+c_level)/f"input-{row['trial']}.bin").read_bytes()
  assert initial==source_input and len(initial)==66048
  assert len(raw)==66136 and raw[:8]==b'DTFLOW1\0' and struct.unpack('<QQ',raw[8:24])==(0,0)
  assert len(expected)==66112 and raw[24:]==expected
  assert row['complete_frame_equal'] and row['three_controls_detected'] and row['root_return']
  for offset in [0,512+32768,len(expected)-64]:
   bad=bytearray(raw[24:]);bad[offset]^=1
   assert bad!=expected;controls+=1
  rows.append(dict(root=name,name=row['name'],trial=row['trial'],input_sha256=hashlib.sha256(initial).hexdigest(),complete_output_sha256=hashlib.sha256(raw).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest()))
out=base/'large-combo-saved-audit-20261115-v1.json'
out.write_text(json.dumps(dict(passed=True,saved_complete_frames=len(rows),comparator_controls_rechecked=controls,rows=rows,raw_frame_bytes=66136,compared_frame_bytes=66112,new_target_executions=0,skipped=0,exclusion=0,normalization=0,all_compiler_unreachable_unlifted_and_Graph_gates_passed=True),indent=2)+'\n')
print('audited',len(rows),'complete saved frames,',controls,'controls; zero new target executions')
