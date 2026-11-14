#!/usr/bin/env python3
"""Read-only audit of real replay snapshots; no library/native/LLVM invocation."""
from pathlib import Path,PurePosixPath
import argparse,ast,collections,hashlib,json,re,struct,tarfile,time,traceback,random
ARCHIVE_SHA='9c623d69bf90ded9c486a70861224c85966bb9698a97f3a9ba791e0cd3f77359'
PAYLOAD_SHA='e34e8a645f1a685967bb7fad894c35ef5f0db27747ff56d4a23c0b2c8867d1b8'
FROZEN_SHA='0ec963329f95b987f93cc0de8f6700b8497068f76b88fdf7e7dbd49101d9f0db'
SPEC_SHA='6ce5d1417bd1ada1c12e4b4a4adf32e35233f60445868c9ffdbde004245bd5b5'
ROOT='practical-region-holdouts-20261113'
REGS,MEMORY,DATA=512,65536,0x8000
GPRS=['RAX','RCX','RDX','RBX','RSP','RBP','RSI','RDI']+[f'R{i}' for i in range(8,16)]
FIELDS=[(n,8*i,8) for i,n in enumerate(GPRS)]
FIELDS +=[(n,136+i,1) for i,n in enumerate(('CF','PF','AF','ZF','SF','OF','DF'))]
FIELDS +=[(f'XMM{i}',143+8*i,8) for i in range(16)]+[(f'XMM{i}H',271+8*i,8) for i in range(16)]
FIELDS +=[(f'FPU{i}L',400+10*i,8) for i in range(8)]+[(f'FPU{i}H',408+10*i,2) for i in range(8)]
FIELDS +=[('FPTOP',480,1),('FPTAG',482,2),('FPCW',484,2),('FPSW',486,2),('PC',128,8)]
assert len(FIELDS)==76
def sha_bytes(data):return hashlib.sha256(data).hexdigest()
def sha_file(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def parsed(blob):
 assert len(blob)==24+REGS+MEMORY+64 and blob[:8]==b'DTFLOW1\0'
 return blob[24:24+REGS],blob[24+REGS:24+REGS+MEMORY],struct.unpack_from('<QQ',blob,8),list(struct.unpack('<8Q',blob[-64:]))
def differences(want,got):
 wr,wm,wc,we=parsed(want);gr,gm,gc,ge=parsed(got);out=[]
 for name,off,size in FIELDS:
  w=int.from_bytes(wr[off:off+size],'little');g=int.from_bytes(gr[off:off+size],'little')
  if g!=w:out.append(dict(field=name,got=hex(g),expected=hex(w)))
 changed=[i for i,(g,w) in enumerate(zip(gm,wm)) if g!=w]
 if changed:out.append(dict(field='memory',count=len(changed),examples=[dict(address=hex(i),got=gm[i],expected=wm[i]) for i in changed[:8]]))
 assert gc==wc
 if ge[:7]!=we[:7]:out.append(dict(field='exit',got=ge[:7],expected=we[:7]))
 assert ge[7]==we[7]==0
 return out
def load_spec(p):
 assert sha_file(p)==SPEC_SHA
 tree=ast.parse(p.read_text());names={'SEED','INPUTS','MASK'}
 funcs={'_constants','_rotate_word','_octet_replacement','specification'}
 nodes=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in funcs or isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id in names for target in n.targets for t in ast.walk(target))]
 ns={'random':random,'struct':struct};exec(compile(ast.Module(body=nodes,type_ignores=[]),str(p),'exec'),ns)
 assert ns['SEED']==2026111307 and len(ns['INPUTS'])==16
 return ns
def metadata(text,hints,identifier):
 assert text.splitlines()[0]=="; ModuleID = '"+identifier+"'"
 m=re.search(r'^!dragon.guard-predictions = !\{!(\d+)\}',text,re.M);assert m
 node=re.search(r'^!'+m[1]+r' = !\{!"((?:\\[0-9a-fA-F]{2}|[^"\n])*)"\}',text,re.M);assert node
 decoded=re.sub(r'\\([0-9a-fA-F]{2})',lambda x:chr(int(x[1],16)),node[1]).encode()
 assert decoded==hints,'prediction metadata is not exact manifest bytes'
 assert '!dragon.guarded-entry =' not in text
def predicate_chain(plan,chain_path,read,pins):
 chain=json.loads(read(chain_path));assert chain['ready'] is True
 assert chain['plan_sha256']==sha_bytes(json.dumps(plan,indent=2,sort_keys=True).encode()+b'\n')
 assert chain['actual_product_passes_before_replay']==4 and chain['product_passes_inside_replay']==0
 assert chain['new_native_C_comparisons']==chain['old_native_comparisons_reused_or_recounted']==0
 for name,digest in chain['input_sha256'].items():
  assert sha_file(Path(name))==digest,name
  pins[name]=digest
 stage_paths=[Path(name) for name in chain['input_sha256'] if name.endswith('/real-predicate-v1/RESULTS.json')]
 assert len(stage_paths)==1
 stage=json.loads(read(stage_paths[0]));assert stage['established'] and stage['product_pass_runs']==4
 assert stage['lift_runs']==stage['graph_mutations']==0
 gp_paths=[Path(name) for name in chain['input_sha256'] if name.endswith('/real-modules-v1-plan.json')]
 gr_paths=[Path(name) for name in chain['input_sha256'] if name.endswith('/real-modules-v1-receipt/RECEIPT.json')]
 assert len(gp_paths)==len(gr_paths)==1
 gp,gr=json.loads(read(gp_paths[0])),json.loads(read(gr_paths[0]))
 assert gr['process_started'] and gr['exit']==0 and gr['failure'] is None and gr['plan_sha256']==sha_file(gp_paths[0])
 assert gr['input_runtime_source_pins_before']==gr['input_runtime_source_pins_after']
 assert gp['environment']['LD_PRELOAD']=='' and gp['environment']['LD_DEBUG']=='libs'
 backend=Path(gp['environment']['LD_LIBRARY_PATH'])/'libdragon.so'
 assert sha_file(backend)=='32c22c410e96aae4a6bbff544af815d9ce4f4d02f4fd4303cb4ca1a6984e396b'
 assert gr['input_runtime_source_pins_before'][str(backend)][-1]==sha_file(backend)
 sources=[Path(name) for name in chain['input_sha256'] if name.endswith('/predicate_module_probe.cpp')]
 assert len(sources)==1 and sha_file(sources[0])=='e1d4bdb24ca74b3d1ff8e5ba66f98e2cc6cc8f0fe21fea23430eb29a07a62ffb'
 for c,link,actual in zip(plan['cases'],chain['cases'],stage['results']):
  assert c['id']==link['case']==actual['case']
  assert c['after']==link['predicate_after'] and c['parsed_before']==link['original_guarded_raw_parser_binding']
  assert c['raw_input']==link['raw'] and c['hints']==link['hints'] and c['driver']==link['driver']
  for key in ('input','predicate_before','predicate_after'):
   pin=link[key];assert sha_file(Path(pin['path']))==pin['sha256']
  assert actual['input_path']==link['input']['path'] and actual['input_sha256']==link['input']['sha256']
  assert actual['argv'][1:]==[link['input']['path'],str(Path(link['predicate_after']['path']).with_suffix('').with_suffix(''))]
  assert gr['input_runtime_source_pins_before'][actual['argv'][0]][-1]==sha_file(Path(actual['argv'][0]))
  assert gr['input_runtime_source_pins_before'][actual['input_path']][-1]==actual['input_sha256']
  assert actual['stats']==link['actual_default_report']
  assert actual['stats']['verified'] and actual['stats']['public_API'] and actual['stats']['native_runs']==0
  hints=read(Path(c['hints']['path']))
  metadata(read(Path(link['input']['path'])).decode(),hints,c['raw_parse_identifier'])
  metadata(read(Path(link['predicate_before']['path'])).decode(),hints,'predicate-module-probe')
  metadata(read(Path(link['predicate_after']['path'])).decode(),hints,'predicate-module-probe')
 return dict(saved_chain_verified=True,actual_product_passes=4,pass_runs_inside_replay=0,
  backend_sha256=sha_file(backend),raw_parser_ModuleID='original raw identifier',predicate_ModuleID='predicate-module-probe',
  ModuleID_comparison='Exact separate stage identities; no banner stripping or normalization.')

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--root',type=Path,required=True);p.add_argument('--plan',type=Path,required=True)
 p.add_argument('--archive',type=Path,required=True);p.add_argument('--spec',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 p.add_argument('--chain',type=Path,required=True)
 a=p.parse_args();assert not a.out.exists();a.out.mkdir(parents=True)
 report=dict(schema='predicate-real-replay-saved-independent-v1',passed=False,failures=[],
             checker_sha256=sha_file(Path(__file__)),new_native_product_unicorn_runs=0)
 active={};pins={};started=time.monotonic()
 def read(p):
  assert p.is_file() and not p.is_symlink()
  b=p.read_bytes();key=str(p.relative_to(a.root)) if p.is_relative_to(a.root) else str(p)
  h=sha_bytes(b);assert key not in pins or pins[key]==h;pins[key]=h;return b
 try:
  result=json.loads(read(a.root/'RESULTS.json'));plan=json.loads(read(a.plan));inv=json.loads(read(a.root/'INVOCATION.json'))
  ns=load_spec(a.spec);pins[str(a.spec)]=SPEC_SHA
  assert result['correct'] is True and result['complete'] is True
  assert result['plan_sha256']==sha_file(a.plan)==inv['plan_sha256'] and inv['plan']==plan
  assert result['native_comparisons']==result['planned_native_comparisons']==result['root_returns']==128
  assert result['unicorn_python_spec_checks']==64 and result['comparator_controls']==384
  assert result['statuses']=={'match':128}
  for k in ('normalizations','skipped','undefined_exclusions','producer_runs','product_pass_runs','lift_runs','new_native_C_comparisons'):assert result[k]==0
  assert plan['ready'] is True and plan['archive']['sha256']==ARCHIVE_SHA
  chain_proof=predicate_chain(plan,a.chain,read,pins)
  assert plan['limits']==dict(guest_steps=1000000,subprocess_seconds=180,start_free=512*1024**2,running_free=64*1024**2)
  assert sha_file(a.archive)==ARCHIVE_SHA;pins[str(a.archive)]=ARCHIVE_SHA
  ids={'manual-O0','manual-O2','flatten-switch-O0','flatten-switch-O2'}
  assert len(plan['cases'])==4 and {c['id'] for c in plan['cases']}==ids
  requested={ROOT+'/B-PAYLOAD-MANIFEST-v1.json',ROOT+'/frozen-normal/FROZEN.json'}
  for c in plan['cases']:
   parent=PurePosixPath(c['raw']['member']).parent
   requested|={c['raw']['member'],c['driver']['member'],str(parent.parent/'frozen-input.json')}
   requested|={str(parent.parent/f'reference-input-{i}.bin') for i in range(16)}
  found={}
  with tarfile.open(a.archive,mode='r|xz') as tar:
   for item in tar:
    if item.name in requested:
     assert item.name not in found and item.isfile() and not item.issym() and not item.islnk()
     found[item.name]=tar.extractfile(item).read()
  assert set(found)==requested
  payload_bytes=found[ROOT+'/B-PAYLOAD-MANIFEST-v1.json'];assert sha_bytes(payload_bytes)==PAYLOAD_SHA
  payload=json.loads(payload_bytes)
  frozen_bytes=found[ROOT+'/frozen-normal/FROZEN.json'];assert sha_bytes(frozen_bytes)==FROZEN_SHA
  frozen=json.loads(frozen_bytes);assert frozen['inputs']==ns['INPUTS']
  for member,data in found.items():
   if member.endswith('/B-PAYLOAD-MANIFEST-v1.json'):continue
   row=payload['files'][str(PurePosixPath(member).relative_to(ROOT))]
   assert sha_bytes(data)==row['sha256'] and len(data)==row['size']
  references={(r['case'],r['trial']):r for r in result['references']}
  assert len(result['references'])==len(references)==64 and set(references)=={(c,i) for c in ids for i in range(16)}
  rows={(r['case'],r['host_llvm_level'],r['trial']):r for r in result['results']}
  assert len(result['results'])==len(rows)==128 and set(rows)=={(c,l,i) for c in ids for l in ('O0','O2') for i in range(16)}
  controls={(r['case'],r['host_llvm_level'],r['trial'],r['field']):r for r in result['controls']}
  assert len(result['controls'])==len(controls)==384 and set(controls)=={(c,l,i,f) for c in ids for l in ('O0','O2') for i in range(16) for f in ('RAX','memory','exit')}
  bindings={r['case']:r for r in result['binding']};assert len(bindings)==4
  case_reports=[];input_hashes=set()
  for c in plan['cases']:
   active=dict(case=c['id']);work=a.root/c['id'];parent=PurePosixPath(c['raw']['member']).parent;b=bindings[c['id']]
   assert c['id']==c['family']+'-'+c['c_level']
   assert read(work/'raw.ll')==found[c['raw']['member']]
   assert read(work/'driver.c')==found[c['driver']['member']]
   assert sha_bytes(found[c['raw']['member']])==c['raw_input']['sha256']==c['raw']['sha256']==b['raw_sha256']
   assert sha_bytes(found[c['driver']['member']])==c['driver']['sha256']==b['driver_sha256']
   assert b['raw_member']==c['raw']['member'] and b['driver_member']==c['driver']['member'] and b['exact_parser_hint_binding'] is True
   image=frozen['families'][c['family']]['images'][c['c_level']]
   assert sha_bytes(read(work/'fixture.elf'))==image['sha256']==c['elf_sha256']==b['elf_sha256']
   identity=json.loads(found[str(parent.parent/'frozen-input.json')]);assert identity['elf']==image['elf'] and identity['sha256']==image['sha256']
   hints=read(work/'hints.json')
   assert sha_bytes(hints)==c['hints']['sha256']
   for k in ('parsed_before','after'):
    text=read(work/(k+'.ll'));assert sha_bytes(text)==c[k]['sha256']==b[k+'_sha256']
    metadata(text.decode(),hints,c['raw_parse_identifier'] if k=='parsed_before' else 'predicate-module-probe')
   assert (work/'bind-parser-hints.stderr').read_bytes()==b''
   case_report=dict(case=c['id'],raw_blob_matches=0,matched_fields=0,original_B_input_bindings=[],control_fields=collections.Counter())
   for i,value in enumerate(ns['INPUTS']):
    active=dict(case=c['id'],trial=i)
    old_input=found[str(parent.parent/f'reference-input-{i}.bin')];assert len(old_input)==REGS+MEMORY
    ih=sha_bytes(old_input);input_hashes.add(ih)
    ref=references[(c['id'],i)];assert ref['status']=='match' and ref['value']==value and ref['input_sha256']==ih
    expected=read(work/f'expected-{i}.bin');assert sha_bytes(expected)==ref['expected_snapshot_sha256']
    regs,memory,calls,record=parsed(expected)
    assert record==ref['exit']+[0] and record[0]==1 and record[1]==3 and record[3]==61440 and record[4:]==[1,0,0,0]
    specified=ns['specification'](ns['SEED'],value)
    assert (int.from_bytes(regs[:4],'little'),memory[DATA:DATA+16])==specified
    for level in ('O0','O2'):
     active.update(level=level);row=rows[(c['id'],level,i)]
     assert read(work/level/f'input-{i}.bin')==old_input
     native=read(work/level/f'output-{i}.bin')
     assert native==expected,'raw complete snapshot bytes differ'
     assert differences(expected,native)==[]
     assert row['status']=='match' and row['root_return'] is True and row['differences']==[]
     assert row['register_fields_compared']==76 and row['memory_bytes_compared']==65536
     assert row['exit']==record[:7] and row['budget_left']==record[7]==0
     case_report['raw_blob_matches']+=1;case_report['matched_fields']+=76
     for field in ('RAX','memory','exit'):
      changed=bytearray(native);offset=24 if field=='RAX' else 24+REGS+DATA if field=='memory' else len(native)-64
      changed[offset]^=1
      diff=differences(expected,bytes(changed))
      assert len(diff)==1 and diff[0]['field']==field
      control=controls[(c['id'],level,i,field)]
      assert control['differences']==diff and control['scope']=='comparator-only snapshot corruption; no extra native run'
      case_report['control_fields'][field]+=1
    case_report['original_B_input_bindings'].append(dict(trial=i,value=value,sha256=ih,
          archived_member=str(parent.parent/f'reference-input-{i}.bin'),expected_snapshot_sha256=sha_bytes(expected)))
   for level in ('O0','O2'):assert read(work/level/'driver.c')==read(work/'driver.c')
   case_reports.append(case_report)
  assert len(input_hashes)==64
  for name,h in pins.items():
   q=Path(name) if Path(name).is_absolute() else a.root/name
   assert sha_file(q)==h,'input changed during audit '+name
  report.update(passed=True,cases=case_reports,input_sha256=pins,predicate_chain=chain_proof,
    totals=dict(raw_complete_snapshot_matches=128,raw_snapshot_bytes_each=66136,register_fields_per_comparison=76,
      matched_register_fields=128*76,Guest_bytes_per_comparison=65536,complete_Exit_words_per_comparison=8,
      root_returns=128,original_B_unique_inputs=64,input_copies_bound=128,
      saved_Unicorn_frames_matching_Python_specification=64,comparator_only_exact_single_field_controls=384,
      control_fields=dict(RAX=128,memory=128,exit=128),normalizations=0),
    provenance=dict(archive_sha256=ARCHIVE_SHA,payload_sha256=PAYLOAD_SHA,frozen_sha256=FROZEN_SHA,
      parser_hint_binding='exact copied raw/driver/archive and plan pins; exact decoded metadata bytes and separate raw/predicate ModuleIDs, verified saved product chain; no LLVM parser rerun'),
    scope='saved raw complete snapshots + original archive inputs + same frozen Python business spec + exact saved public-predicate chain. New native128/UC64 execution was performed by the separately recorded replay; this audit executes none. NewC0; old C checks are cited, not rerun. No padding removal or normalization.')
 except BaseException as e:
  report['failures'].append(dict(case=active,type=type(e).__name__,detail=str(e),traceback=traceback.format_exc()));raise
 finally:
  report['seconds']=time.monotonic()-started;(a.out/'REPORT.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
 print('128 raw snapshots +64 original B inputs/spec +384 exact field controls passed')
 print('REPORT '+sha_file(a.out/'REPORT.json'))
if __name__=='__main__':main()
