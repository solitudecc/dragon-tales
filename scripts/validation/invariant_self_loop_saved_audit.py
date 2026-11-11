#!/usr/bin/env python3
"""Read-only K native evidence audit, with no compiler/native/runtime execution.

Usage: invariant_self_loop_saved_audit.py ARCHIVE_ROOT --manifest MANIFEST.json
Imports the exact pinned reference by explicit path and private module identity;
never searches for a generic ``oracle`` basename or writes into frozen evidence.
"""
import argparse,gzip,hashlib,json,struct,types
from pathlib import Path

PINS={
 'PLAN.txt':'544aee3b4edc866eadfe60ebd8b3306efe716377a11d3b9c52f0223a8ae5e4a0',
 'observe.cpp':'d7db305769251cf6ad130dea502378bb2ee5068652c326081f45a35437564a8e',
 'oracle.py':'6af1994d196080bcd75b3ff035652dbee0cf534f06785170bebe57f6a353475e',
 'transform.cpp':'fa2aa398b9943816f8bca8dd10faae217990fb1605e54efe39ac94ae188f0455',
}
def sha(data):return hashlib.sha256(data).hexdigest()
def symbol_code(path):
 """Locate the exact oracle symbol in the saved AArch64 ELF, without executing it."""
 data=path.read_bytes();assert data[:6]==b'\x7fELF\x02\x01'
 header=struct.unpack_from('<HHIQQQIHHHHHH',data,16)
 assert header[1]==183
 phoff,shoff,phsize,phnum,shsize,shnum=header[4],header[5],header[8],header[9],header[10],header[11]
 assert shsize==64 and phsize==56
 sections=[struct.unpack_from('<IIQQQQIIQQ',data,shoff+n*shsize) for n in range(shnum)]
 matches=[]
 for section in sections:
  if section[1]!=11:continue
  assert section[9]==24 and section[5]%24==0
  strings=sections[section[6]];names=data[strings[4]:strings[4]+strings[5]]
  for off in range(section[4],section[4]+section[5],24):
   name,info,other,index,address,size=struct.unpack_from('<IBBHQQ',data,off)
   end=names.find(b'\0',name);assert end>=name
   if names[name:end]!=b'oracle':continue
   assert info&15==2 and index and size>=4
   for n in range(phnum):
    kind,flags,fileoff,vaddr,paddr,filesz,memsz,alignment=struct.unpack_from('<IIQQQQQQ',data,phoff+n*phsize)
    if kind==1 and address>=vaddr and address+size<=vaddr+filesz:
     start=fileoff+address-vaddr;code=data[start:start+size];assert len(code)==size
     matches.append((size,code))
 assert len(matches)==1
 return matches[0]
def audit(root,manifest=None):
 if manifest:
  expected=json.loads(manifest.read_text())['files']
  actual={}
  for p in sorted(root.rglob('*')):
   assert not p.is_symlink()
   if p.is_file():actual[str(p.relative_to(root))]={'size':p.stat().st_size,'sha256':sha(p.read_bytes())}
  assert actual==expected,'exact evidence file set/content mismatch'
 source=root/'source-v3'
 for name,pin in PINS.items():assert sha((source/name).read_bytes())==pin
 # A private namespace and compile/exec avoid basename collisions and pycache writes.
 oracle=types.ModuleType('_dragon_invariant_loop_frozen_v3')
 oracle.__file__=str(source/'oracle.py')
 exec(compile((source/'oracle.py').read_bytes(),oracle.__file__,'exec'),oracle.__dict__)
 fixtures=root/'fixtures-v3';runner_sha=sha((root/'observe-v3').read_bytes())
 assert json.loads((fixtures/'inputs.json').read_text())==[list(x) for x in oracle.INPUTS]
 normal=faults=specified=frames=witnesses=0;rows=[]
 for stage in ('before','after-main','after-noz3')+oracle.FAULTS:
  fault=stage if stage in oracle.FAULTS else ''
  ir=(fixtures/(stage+'.ll')).read_bytes();report=json.loads((fixtures/(stage+'.report.json')).read_text())
  if stage=='before' or fault:assert ir==oracle.ir(fault).encode()
  assert report['stage']==stage and report['source_sha256']==sha(ir) and report['runner_sha256']==runner_sha
  assert [tuple(x['input']) for x in report['rows']]==oracle.INPUTS
  assert all(c['exit']==0 for c in report['commands'])
  compile_commands=[c for c in report['commands'] if c['argv'][0]=='clang-21']
  assert len(compile_commands)==1 and '-O2' in compile_commands[0]['argv']
  assert not any(x in compile_commands[0]['argv'] for x in ('-O0','-O1','-O3'))
  assert Path(compile_commands[0]['argv'][5]).name==stage+'.ll'
  expected_files={stage+f'-{n}.bin.gz' for n in range(len(oracle.INPUTS))}
  assert {p.name for p in fixtures.glob(stage+'-*.bin.gz')}==expected_files
  size,code=symbol_code(fixtures/(stage+'.so'));required=0
  for n,key in enumerate(oracle.INPUTS):
   raw=gzip.decompress((fixtures/(stage+f'-{n}.bin.gz')).read_bytes());row=report['rows'][n]
   kind,states,words=oracle.parse(raw,key)
   assert words[7]==size and row['kind']==kind and row['frames']==len(states)
   assert sha(raw)==row['raw_sha256']
   if kind:
    assert row['pc_offsets']==[words[3],words[5]] and row['opcode']==0x14000000
    for pc in (words[3],words[5]):
     assert pc%4==0 and pc+4<=size and code[pc:pc+4]==b'\0\0\0\x14'
    witnesses+=1
   else:assert row['pc_offsets']==[] and row['opcode'] is None
   expected_kind,expected=oracle.model(key,fault)
   assert kind==expected_kind and all(state==expected for state in states)
   nominal=oracle.model(key);needed=(expected_kind,expected)!=nominal
   assert row['fault_required']==needed
   if needed:
    assert fault and (kind!=nominal[0] or states[0]!=nominal[1]);required+=1
   damaged=bytearray(states[0]);damaged[16+60000+48]^=1;assert damaged!=expected
   normal+=not bool(fault);faults+=bool(fault);frames+=len(states)
  assert report['normal']==(0 if fault else 36) and report['fault_observations']==(36 if fault else 0)
  assert report['required']==report['caught']==required
  if fault:assert required==18
  specified+=required;rows.append({'stage':stage,'records':36,'required_faults':required,'ir_sha256':sha(ir),'elf_sha256':sha((fixtures/(stage+'.so')).read_bytes())})
 assert (normal,faults,specified,frames,witnesses)==(108,144,72,378,126)
 saved=json.loads((root/'saved-audit-v3.json').read_text())
 for key,value in [('normal',normal),('fault_observations',faults),('specified_fault_detections',specified),('full_saved_frames',frames),('two_PC_self_branch_witnesses',witnesses)]:assert saved[key]==value
 assert saved['native_reexecutions']==saved['exclusions']==saved['skips']==0
 for binding in saved['runtime_bindings']:
  mode=binding['mode'];expected=fixtures/('after-'+mode+'.ll');bound=root/'frozen-bindings'/ (mode+'.ll')
  assert bound.read_bytes()==expected.read_bytes() and sha(expected.read_bytes())==binding['actual_native_ir_sha256']
  assert binding['exit']==0 and not (root/'frozen-bindings'/(mode+'.stderr')).read_bytes()
  for period in ('before','after'):
   assert (root/(mode+'-runtime-'+period+'.sha256')).read_text().split()[0]==binding['runtime_sha256']
  assert (binding['LD_LIBRARY_PATH']+'/libdragon.so') in (root/'frozen-bindings'/(mode+'.ldd')).read_text()
 assert (fixtures/'after-main.ll').read_bytes()==(fixtures/'after-noz3.ll').read_bytes()
 return {'normal':normal,'fault_observations':faults,'specified_faults':specified,'full_saved_frames':frames,'two_sample_native_self_branch_witnesses':witnesses,'records':normal+faults,'saved_ELF_opcode_checks':witnesses*2,'comparator_Exit_corruption_rejections':normal+faults,'focused_optimization':'clang-21 -O2 only','native_executions_added':0,'source_pins':PINS,'source_manifest_exact':bool(manifest),'saved_binding_replay_checked':True,'stages':rows,'runtime_bindings':saved['runtime_bindings']}
def main():
 p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('--manifest',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
 result=audit(a.root.resolve(),a.manifest);text=json.dumps(result,indent=2)+'\n'
 if a.output:a.output.write_text(text)
 print(text,end='')
if __name__=='__main__':main()
