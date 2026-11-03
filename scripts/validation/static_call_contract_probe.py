"""Probe a proposed static-call IR contract, without changing the lifter.

An unresolved declaration must preserve dependencies without a function body.
Independent C reference/stub code is linked only for this probe's native tests.
Negative controls deliberately reuse a stale alloca or promise memory(none).
This is a generic LLVM effect test, not a machine-instruction difftest.
"""
import argparse,collections,hashlib,json,platform,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
FIELDS=[('EAX','regs',0,32),('CF','regs',136,8),('R15','regs',120,64),
        ('XMM15H','regs',391,64),('FPU0','regs',400,80),('FPSW','regs',486,16),
        ('SP','regs',32,64),('PC','regs',128,64),('memory','mem',64,64),
        ('exit_reason','exit',0,64),('exit_next_pc','exit',24,64)]

def ir_for(base,offset,bits,variant):
    attrs=' memory(none)' if variant=='wrong_attrs' else ''
    read='%cache' if variant=='stale_cache' else '%field'
    return f'''declare void @contract_external(ptr, ptr, ptr){attrs}
define void @probe(ptr %regs, ptr %mem, ptr %exit) {{
entry:
  %field = getelementptr i8, ptr %{base}, i64 {offset}
  %cache = alloca i{bits}
  store i{bits} 0, ptr %cache
  store i{bits} 0, ptr %field, align 1
  call void @contract_external(ptr %regs, ptr %mem, ptr %exit)
  %loaded = load i{bits}, ptr {read}, align 1
  %value = freeze i{bits} %loaded
  %condition = icmp ne i{bits} %value, 0
  br i1 %condition, label %yes, label %done
yes:
  store i8 7, ptr %mem, align 1
  br label %done
done:
  ret void
}}
'''

def driver_for(base,offset,bits):
    size=bits//8
    return f'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char regs[512],mem[256],exit_data[56],payload[16];
void contract_external(unsigned char *r,unsigned char *m,unsigned char *e){{
  unsigned char *base={'r' if base=='regs' else 'm' if base=='mem' else 'e'};
  memcpy(base+{offset},payload,{size});
}}
void probe(unsigned char*,unsigned char*,unsigned char*);
int main(int argc,char **argv){{
 if(argc!=2)return 90;
 unsigned choice=(unsigned)strtoul(argv[1],0,10);
 if(choice==1)payload[0]=1;
 if(choice==2)memset(payload,255,{size});
 if(choice==3)payload[{size-1}]=128;
 contract_external(regs,mem,exit_data);
 unsigned char *field={'regs' if base=='regs' else 'mem' if base=='mem' else 'exit_data'}+{offset};
 int nonzero=0;for(unsigned i=0;i<{size};++i)nonzero|=field[i]!=0;
 unsigned expected=nonzero?7:0;
 memset(regs,0,sizeof(regs));memset(mem,0,sizeof(mem));memset(exit_data,0,sizeof(exit_data));
 probe(regs,mem,exit_data);
 printf("%u %u\\n",(unsigned)mem[0],expected);
 return 0;
}}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--opt',default='opt-21');p.add_argument('--llc',default='llc-21');p.add_argument('--cc',default='gcc')
    p.add_argument('--require-current-mode',action='store_true',help='also require the proposed production mode; expected to fail before implementation')
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    host=platform.machine();triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}.get(host)
    if not triple:p.error('native probe requires Linux aarch64/x86_64')
    rows=[];structural=[]
    for name,base,offset,bits in FIELDS:
      for variant in ('correct','stale_cache','wrong_attrs'):
        folder=out/name/variant;folder.mkdir(parents=True,exist_ok=True)
        raw=folder/'raw.ll';raw.write_text(ir_for(base,offset,bits,variant));driver=folder/'driver.c';driver.write_text(driver_for(base,offset,bits))
        subprocess.run([args.opt,'-passes=verify','-disable-output',str(raw)],check=True,capture_output=True)
        optimized=folder/'optimized.ll';subprocess.run([args.opt,'-S','-passes=default<O2>',str(raw),'-o',str(optimized)],check=True,capture_output=True)
        subprocess.run([args.opt,'-passes=verify','-disable-output',str(optimized)],check=True,capture_output=True)
        text=optimized.read_text();structural.append(dict(field=name,variant=variant,conditional='br i1' in text,external_call='call void @contract_external' in text))
        for opt,source in [('O0',raw),('O2',optimized)]:
          obj=folder/(opt+'.o');exe=folder/(opt+'.native')
          subprocess.run([args.llc,'-O0','-filetype=obj','-mtriple='+triple,str(source),'-o',str(obj)],check=True,capture_output=True)
          subprocess.run([args.cc,'-O0',str(driver),str(obj),'-o',str(exe)],check=True,capture_output=True)
          for edge in range(4):
            r=subprocess.run([str(exe),str(edge)],check=True,capture_output=True,text=True)
            got,want=map(int,r.stdout.split());rows.append(dict(field=name,variant=variant,opt=opt,edge=edge,got=got,expected=want,status='match' if got==want else 'different'))
    current=[]
    sys.path.insert(0,str(ROOT/'python'))
    import dragon
    for arch in (dragon.Architecture.X86_32,dragon.Architecture.X86_64,dragon.Architecture.ARM64):
      try:lifter=dragon.LLVMLifter(arch,execution_mode=2)
      except ValueError:current.append(dict(architecture=arch,proposed_mode_available=False))
      else:current.append(dict(architecture=arch,proposed_mode_available=True));lifter.__del__()
    counts={v:dict(collections.Counter(r['status'] for r in rows if r['variant']==v)) for v in ('correct','stale_cache','wrong_attrs')}
    summary=dict(fields=len(FIELDS),native_cases=len(rows),statuses=counts,skipped=0,structural_checks=structural,current_api=current,
                 tools={t:subprocess.run([t,'--version'],check=True,capture_output=True,text=True).stdout.splitlines()[0] for t in (args.opt,args.llc,args.cc)},
                 source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (out/'results.json').write_text(json.dumps(dict(summary=summary,results=rows),indent=2))
    print(json.dumps({k:v for k,v in summary.items() if k!='structural_checks'},indent=2))
    # llc -O0 does not exploit memory(none); the deliberately false promise
    # becomes observable when opt -O2 removes the reload and conditional.
    # Check every record, including the zero edge which cannot expose either
    # fault. These are predictions of the controls, not accepted semantics.
    for row in rows:
      corrupt = row['variant']=='stale_cache' or (row['variant']=='wrong_attrs' and row['opt']=='O2')
      row['expected_status']='different' if corrupt and row['edge']!=0 else 'match'
    passed=all(row['status']==row['expected_status'] for row in rows)
    passed &= all(r['conditional'] and r['external_call'] for r in structural if r['variant']=='correct')
    passed &= all(not r['conditional'] for r in structural if r['variant']!='correct')
    if args.require_current_mode:passed &= all(x['proposed_mode_available'] for x in current)
    (out/'results.json').write_text(json.dumps(dict(summary=summary,results=rows),indent=2))
    return 0 if passed else 1
if __name__=='__main__':sys.exit(main())
