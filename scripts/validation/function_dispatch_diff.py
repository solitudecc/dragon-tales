"""Task 8 step 2: V2 state/dispatch prefixes vs Unicorn, O0 and O2.

Stops BEFORE CALL/RET (currently Unsupported); this is not a full-function
success claim. PC, SP and every mapped byte are compared without Legacy
normalizations. Missing target exits stop Unicorn before executing that target.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile

import function_llvm_diff as base

def compile_case(fixture, entry, level, work, args):
    lifter=base.dragon.LLVMLifter(base.dragon.Architecture.X86_64,
                                execution_mode=base.dragon.LLVMExecutionMode.STATEFUL_V2)
    call_sites=set()
    for address,code in [(entry,fixture['code_hex'])]+[(s['address'],s['code_hex']) for s in fixture['stubs']]:
        dis=base.dragon.Disassembler(base.dragon.Architecture.X86_64,base.dragon.Configuration(strategy=0))
        graph=dis.analyze(bytes.fromhex(code),address)
        function=graph.get_function(address)
        if not function or not function.is_valid: raise RuntimeError('invalid function')
        if not lifter.lift(function,graph): raise RuntimeError('V2 lift rejected')
        call_sites.update(i.address for i in graph.instructions() if i.is_call)
    raw=work/'raw.ll';raw.write_text(lifter.ir())
    subprocess.run([args.opt,'-passes=verify','-disable-output',str(raw)],check=True,capture_output=True)
    source=raw
    if level=='O2':
        source=work/'optimized.ll'
        subprocess.run([args.opt,'-S','-passes=default<O2>',str(raw),'-o',str(source)],check=True,capture_output=True)
    subprocess.run([args.opt,'-passes=verify','-disable-output',str(source)],check=True,capture_output=True)
    names=[(entry,f'dragon_v2_x86_64_{entry:x}')]
    wrappers=[]
    for stub in fixture['stubs']:
        address=stub['address'];symbol=f'dragon_v2_x86_64_{address:x}';wrapper=f'model_{address:x}'
        wrappers.append(f'void {symbol}(S*,M*,C*,E*); void {wrapper}(S*s,M*m,C*c,E*e) {{++calls{address:x}; {symbol}(s,m,c,e);}}')
        names.append((address,wrapper))
    declarations='\n'.join(f'void {symbol}(S*,M*,C*,E*);' for _,symbol in names)
    table=','.join(f'{{{address}ULL,{symbol}}}' for address,symbol in names)
    driver=work/'driver.c';driver.write_text(DRIVER.replace('DECLARATIONS',declarations+'\n'+'\n'.join(wrappers)).replace('ENTRIES',table).replace('INITIAL_BUDGET',str(fixture.get('context_budget',1000))))
    obj=work/'module.o';exe=work/'native'
    triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}.get(platform.machine())
    if not triple: raise RuntimeError('Linux aarch64/x86_64 host required')
    subprocess.run([args.llc,'-O0','-filetype=obj','-mtriple='+triple,str(source),'-o',str(obj)],check=True,capture_output=True)
    runtime=Path(base.dragon._LIB).resolve()
    subprocess.run([args.cc,'-O0','-I'+str(base.ROOT/'include'),'-I'+str(base.ROOT/'c_shim'),str(driver),str(obj),str(runtime),'-Wl,-rpath,'+str(runtime.parent),'-o',str(exe)],check=True,capture_output=True)
    return exe, 'unreachable' in source.read_text(), call_sites

DRIVER=r'''
#include <stdio.h>
#include <stdint.h>
#include <dragon_c.h>
typedef dragon_guest_state_v2_t S;
typedef dragon_guest_memory_v2_t M;
typedef dragon_exec_context_v2_t C;
typedef dragon_exec_exit_v2_t E;
static unsigned char regs[512],memory[65536];
static uint64_t calls2000,calls3000;
DECLARATIONS
int main(int argc,char **argv) {
  if(argc!=3) return 90;
  FILE *f=fopen(argv[1],"rb");
  if(!f || fread(regs,1,sizeof(regs),f)!=sizeof(regs) || fread(memory,1,sizeof(memory),f)!=sizeof(memory)) return 91;
  fclose(f);
  const dragon_target_entry_v2_t entries[]={ENTRIES};
  S s={2,DRAGON_ARCH_X86_64,sizeof(regs),regs}; M m={memory,sizeof(memory)};
  C c={entries,sizeof(entries)/sizeof(entries[0]),INITIAL_BUDGET,0}; E e={0};
  dragon_llvm_execute_v2(&s,&m,&c,&e,0xf000);
  f=fopen(argv[2],"wb"); if(!f) return 92;
  const unsigned char magic[8]={'D','T','F','L','O','W','1',0};
  fwrite(magic,1,8,f);fwrite(&calls2000,8,1,f);fwrite(&calls3000,8,1,f);
  fwrite(regs,1,sizeof(regs),f);fwrite(memory,1,sizeof(memory),f);
  fwrite(&e,1,sizeof(e),f);fwrite(&c.budget,8,1,f);
  return fclose(f)?93:0;
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',action='append',dest='cases')
    parser.add_argument('--trials',type=int,default=8)
    parser.add_argument('--out',type=Path)
    parser.add_argument('--corrupt-output',action='store_true')
    parser.add_argument('--opt',default='opt-21');parser.add_argument('--llc',default='llc-21');parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    if args.trials<1:parser.error('positive trials required')
    fixtures=json.loads((base.ROOT/'tests/fixtures/llvm-function-flow.json').read_text())
    selected=[f for f in fixtures['cases'] if not args.cases or f['id'] in args.cases]
    if not selected or (args.cases and set(args.cases)!={f['id'] for f in selected}):parser.error('unknown case')
    out=(args.out or Path(tempfile.mkdtemp(prefix='dragon-dispatch-'))).resolve();out.mkdir(parents=True,exist_ok=True)
    rows=[]
    for fixture in selected:
        for level in ('O0','O2'):
            work=out/fixture['id']/level;work.mkdir(parents=True,exist_ok=True)
            try:exe,bad_ir,sites=compile_case(fixture,fixtures['entry'],level,work,args)
            except Exception as error:
                detail=error.stderr.decode(errors='replace') if isinstance(error,subprocess.CalledProcessError) and isinstance(error.stderr,bytes) else str(error)
                rows.append(dict(case=fixture['id'],opt=level,status='infrastructure_error',detail=detail));continue
            for trial in range(args.trials):
                row=dict(case=fixture['id'],opt=level,trial=trial)
                regs,memory=base.initial(fixture,trial,fixtures['entry'])
                try:expected=base.reference(fixture,regs,memory,fixtures['entry'],sites,stop_before_control=True)
                except Exception as error:row.update(status='reference_error',detail=str(error));rows.append(row);continue
                actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64)
                if error:row.update(status=error['kind'],detail=error)
                else:
                    differences=base.compare(fixture,expected,actual,bad_ir,args.corrupt_output,include_pc=True)
                    record=struct.unpack('<8Q',actual[3]);exit=record[:7]
                    if exit!=expected[4]:differences.append(dict(field='exit',got=exit,expected=expected[4]))
                    row.update(status='different' if differences else 'prefix_match',differences=differences,exit=exit,budget_left=record[7],undefined_flag_fields=len(fixture['undefined_flags']),register_fields_compared=len(base.FIELDS)+1-len(fixture['undefined_flags']),memory_bytes_compared=base.MEM_SIZE)
                rows.append(row)
            print(fixture['id'],level,dict(collections.Counter(r['status'] for r in rows if r['case']==fixture['id'] and r['opt']==level)),flush=True)
    statuses=dict(collections.Counter(r['status'] for r in rows))
    summary=dict(stage='before CALL/RET, unsupported instruction, missing target or budget; NOT full-function success',cases=len(selected),trials_per_opt=args.trials,results=len(rows),statuses=statuses,skipped=0,legacy_normalizations=0,
      undefined_flag_fields=sum(r.get('undefined_flag_fields',0) for r in rows),
      register_fields_compared=sum(r.get('register_fields_compared',0) for r in rows),memory_bytes_compared=sum(r.get('memory_bytes_compared',0) for r in rows),
      llvm_lifter_sha256=hashlib.sha256((base.ROOT/'lib/LLVMLifter.cpp').read_bytes()).hexdigest(),artifacts=str(out))
    summary['runtime_sha256']=hashlib.sha256((base.ROOT/'lib/LLVMExecution.cpp').read_bytes()).hexdigest()
    summary['unicorn_version']=__import__('unicorn').__version__
    summary['host']=platform.machine()
    summary['tools']={command:subprocess.run([command,'--version'],capture_output=True,text=True,check=True).stdout.splitlines()[0] for command in (args.opt,args.llc,args.cc)}
    summary['exits']=dict(collections.Counter(str(r['exit'][0]) for r in rows if 'exit' in r))
    (out/'results.json').write_text(json.dumps(dict(summary=summary,results=rows),indent=2))
    print(json.dumps(summary,indent=2))
    for row in rows:
        if row['status']!='prefix_match':print('FAIL',json.dumps(row,indent=2))
    return 0 if all(r['status']=='prefix_match' for r in rows) and len(rows)==len(selected)*2*args.trials else 1

if __name__=='__main__':sys.exit(main())
