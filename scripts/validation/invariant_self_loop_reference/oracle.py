#!/usr/bin/env python3
import argparse,hashlib,json,struct,subprocess,gzip
from pathlib import Path
N=65536; MASK=(1<<64)-1
INPUTS=[(s,d,b) for s in (0,49,50,51,1<<63,MASK) for d in (0,1,MASK) for b in (0,1)]
FAULTS=('force_return','force_spin','wrong_store','wrong_exit')
def ir(fault=''):
 condition='false' if fault=='force_return' else 'true' if fault=='force_spin' else '%condition'
 start=18 if fault=='wrong_store' else 17
 extra='  %bad = xor i64 %result, 1\n' if fault=='wrong_exit' else ''
 ret='%bad' if fault=='wrong_exit' else '%result'
 return f'''source_filename = "independent-invariant-loop-oracle"
target datalayout = "e-m:e-i64:64-i128:128-n32:64-S128"
define i64 @oracle(ptr %regs, ptr %guest, ptr %exit, i64 %input, i64 %data, i1 %slot) {{
entry:
  %g = getelementptr i8, ptr %guest, i64 128
  %r = getelementptr i8, ptr %regs, i64 16
  store i64 {start}, ptr %g, align 1
  store i64 %data, ptr %r, align 1
  br i1 %slot, label %head, label %head
head:
  %state = phi i64 [ %input, %entry ], [ %input, %entry ], [ %state, %head ]
  %datum = phi i64 [ %data, %entry ], [ %data, %entry ], [ %datum, %head ]
  %condition = icmp ugt i64 %state, 50
  br i1 {condition}, label %head, label %done
done:
  store i64 %datum, ptr %g, align 1
  store i64 %state, ptr %regs, align 1
  %result = xor i64 %state, %datum
  store i64 1, ptr %exit, align 1
  %e1 = getelementptr i8, ptr %exit, i64 8
  store i64 0, ptr %e1, align 1
  %e2 = getelementptr i8, ptr %exit, i64 16
  store i64 4096, ptr %e2, align 1
  %e3 = getelementptr i8, ptr %exit, i64 24
  store i64 %state, ptr %e3, align 1
  %e4 = getelementptr i8, ptr %exit, i64 32
  store i64 1, ptr %e4, align 1
  %e5 = getelementptr i8, ptr %exit, i64 40
  store i64 %datum, ptr %e5, align 1
  %e6 = getelementptr i8, ptr %exit, i64 48
  store i64 %result, ptr %e6, align 1
{extra}  ret i64 {ret}
}}
'''
def model(key,fault=''):
 s,d,b=key;a=bytearray(((n*37+19)^(n>>8))&255 for n in range(N))
 def put(o,v):struct.pack_into('<Q',a,o,v)
 put(1152,18 if fault=='wrong_store' else 17);put(16,d)
 spin=not(fault=='force_return') and (fault=='force_spin' or s>50)
 if spin:return 1,struct.pack('<QQ',0,0xbed0bed0bed0bed0)+a
 put(1152,d);put(0,s)
 for i,v in enumerate((1,0,4096,s,1,d,s^d)):put(60000+8*i,v)
 return 0,struct.pack('<QQ',1,(s^d)^(fault=='wrong_exit'))+a
def parse(raw,key):
 if len(raw)<80:raise AssertionError('short header')
 w=struct.unpack('<10Q',raw[:80]);assert w[0]==0x4b5350494e303031 and tuple(w[8:10])==key[:2]
 kind,count=w[1:3];assert kind in (0,1) and count==(2 if kind else 1)
 assert len(raw)==80+count*(16+N)
 frames=[raw[80+i*(16+N):80+(i+1)*(16+N)] for i in range(count)]
 if kind:
  assert w[3]==w[5] and w[4]==w[6]==0x14000000 and w[3]+4<=w[7]
  assert frames[0]==frames[1]
 return kind,frames,w

def run(directory,runner,stage):
 source=directory/(stage+'.ll');so=directory/(stage+'.so');commands=[]
 def cmd(args):
  r=subprocess.run(args,capture_output=True);commands.append({'argv':args,'exit':r.returncode,'stdout':r.stdout.decode(errors='replace'),'stderr':r.stderr.decode(errors='replace')});assert r.returncode==0,args;return r.stdout
 cmd(['opt-21','-passes=verify','-disable-output',str(source)])
 cmd(['clang-21','-shared','-fPIC','-O2','-Wno-override-module',str(source),'-o',str(so)])
 (directory/(stage+'.disassembly')).write_bytes(cmd(['llvm-objdump-21','-d',str(so)]))
 rows=[];required=caught=0
 for num,key in enumerate(INPUTS):
  args=[str(runner),str(so.resolve()),*[str(x) for x in key]];r=subprocess.run(args,capture_output=True,timeout=5)
  (directory/(stage+f'-{num}.stderr')).write_bytes(r.stderr)
  with gzip.open(directory/(stage+f'-{num}.bin.gz'),'wb') as f:f.write(r.stdout)
  assert r.returncode==0,(stage,num,r.returncode,r.stderr)
  kind,frames,words=parse(r.stdout,key);fault=stage if stage in FAULTS else ''
  expected_kind,expected=model(key,fault);assert kind==expected_kind and all(f==expected for f in frames),(stage,num,'model')
  nominal=model(key);bad=(expected_kind,expected)!=nominal
  if bad:
   required+=1;assert kind!=nominal[0] or frames[0]!=nominal[1];caught+=1
  rows.append({'input':key,'kind':kind,'frames':len(frames),'pc_offsets':[words[3],words[5]] if kind else [],'opcode':words[4] if kind else None,'raw_sha256':hashlib.sha256(r.stdout).hexdigest(),'fault_required':bad})
 if stage in FAULTS:assert required>0 and caught==required
 report={'stage':stage,'normal':len(rows) if stage not in FAULTS else 0,'fault_observations':len(rows) if stage in FAULTS else 0,'required':required,'caught':caught,'rows':rows,'commands':commands,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'runner_sha256':hashlib.sha256(runner.read_bytes()).hexdigest()}
 (directory/(stage+'.report.json')).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in ('rows','commands')}),flush=True)
def main():
 p=argparse.ArgumentParser();p.add_argument('action',choices=['emit','run']);p.add_argument('directory',type=Path);p.add_argument('--runner',type=Path);p.add_argument('--stage');a=p.parse_args()
 if a.action=='emit':
  a.directory.mkdir(exist_ok=False)
  for stage in ('before',)+FAULTS:(a.directory/(stage+'.ll')).write_text(ir('' if stage=='before' else stage))
  (a.directory/'inputs.json').write_text(json.dumps(INPUTS)+'\n')
 else:run(a.directory,a.runner,a.stage)
if __name__=='__main__':main()
