"""V2 function execution vs Unicorn, O0 and O2.

Executes Guest CALL/RET, or stops at each fixture's explicit terminal boundary.
PC, SP and every mapped byte are compared without Legacy normalizations.
Missing target exits stop Unicorn before executing that target.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import tempfile

import function_llvm_diff as base

def compile_case(fixture, entry, level, work, args, model_factory=None, architecture='x86_64'):
    arch_id={'x86_64':2,'x86_32':1,'arm64':4}[architecture]
    pc_offset=264 if architecture=='arm64' else 128
    regs_size=800 if architecture=='arm64' else 512
    lifter=base.dragon.LLVMLifter(arch_id,
                                execution_mode=base.dragon.LLVMExecutionMode.STATEFUL_V2)
    call_sites=set()
    local_functions=fixture.get('local_functions',[])
    image_graph=None
    if fixture.get('image_format'):
        from function_image_fixture import make_image
        data=make_image(fixture,entry,base.ROOT);(work/('image.'+fixture['image_format'])).write_bytes(data)
        image=base.dragon.LoadedImage.load_bytes(data)
        if image.architecture!=arch_id or not any(s.imported and s.name==fixture['expected_import_name'] for s in image.symbols()):
            raise RuntimeError('fixture image import identity mismatch')
        image_graph=image.disassemble(base.dragon.Configuration(strategy=0))
    for address,code in [(entry,fixture['code_hex'])]+[(s['address'],s['code_hex']) for s in local_functions]+([] if model_factory else [(s['address'],s['code_hex']) for s in fixture['stubs']]):
        if image_graph is not None and address in {entry}|{s['address'] for s in local_functions}:graph=image_graph
        else:
            dis=base.dragon.Disassembler(arch_id,base.dragon.Configuration(strategy=0))
            graph=dis.analyze(bytes.fromhex(code),address)
        function=graph.get_function(address)
        if not function or not function.is_valid: raise RuntimeError('invalid function')
        if not lifter.lift(function,graph): raise RuntimeError('V2 lift rejected')
        call_sites.update(i.address for i in graph.instructions() if i.is_call)
    raw=work/'raw.ll';raw.write_text(lifter.ir())
    if fixture.get('image_format'):
        symbol=f'dragon_v2_{architecture}_{entry:x}'
        record=r'!\d+ = !\{!"'+re.escape(symbol)+r'", i64 \d+, i64 '+str(fixture['image_slot'])+r', !"'+re.escape(fixture['expected_import_name'])+r'"\}'
        if not re.search(record,lifter.ir()):raise RuntimeError('root import provenance missing or incorrect')
    subprocess.run([args.opt,'-passes=verify','-disable-output',str(raw)],check=True,capture_output=True)
    source=raw
    if level=='O2':
        source=work/'optimized.ll'
        subprocess.run([args.opt,'-S','-passes=default<O2>',str(raw),'-o',str(source)],check=True,capture_output=True)
    subprocess.run([args.opt,'-passes=verify','-disable-output',str(source)],check=True,capture_output=True)
    ir=raw.read_text()
    def entries(address,symbol):
        header=re.search(r'define void @'+symbol+r'\([^\n]*\) #(\d+)',ir)
        if header:
            attributes=re.search(r'^attributes #'+header[1]+r' = \{([^\n]*)',ir,re.M)
            metadata=re.search(r'"dragon.entrypoints"="([^"]*)"',attributes[1]) if attributes else None
            if metadata:return [(int(pc,16),symbol) for pc in metadata[1].split(',')]
        return [(address,symbol)]
    root_symbol=f'dragon_v2_{architecture}_{entry:x}'
    names=entries(entry,root_symbol);root_count=len(names)
    for local in local_functions:
        address=local['address'];names.extend(entries(address,f'dragon_v2_{architecture}_{address:x}'))
    wrappers=[]
    for stub in fixture['stubs']:
        address=stub['address'];symbol=f'dragon_v2_{architecture}_{address:x}';wrapper=f'model_{address:x}'
        if model_factory:
            wrappers.append(model_factory(stub));names.append((address,wrapper))
        else:
            wrappers.append(f'void {symbol}(S*,M*,C*,E*); void {wrapper}(S*s,M*m,C*c,E*e) {{if(get64(s->regs_bytes+{pc_offset})=={address})++calls{address:x}; {symbol}(s,m,c,e);}}')
            names.extend((pc,wrapper) for pc,_ in entries(address,symbol))
    declarations='\n'.join(f'void {symbol}(S*,M*,C*,E*);' for _,symbol in names)
    table=','.join(f'{{{address}ULL,{symbol}}}' for address,symbol in names)
    resume=''
    if fixture.get('initially_missing'):
        resume=f'if(e.reason!=DRAGON_EXIT_NEED_TARGET || get64(regs+32)!=0xc7f8 || calls2000) return 94; c.entry_count={len(names)};dragon_llvm_execute_v2(&s,&m,&c,&e,0xf000);'
    driver=work/'driver.c';driver.write_text(DRIVER.replace('DECLARATIONS',declarations+'\n'+'\n'.join(wrappers)).replace('ENTRIES',table).replace('INITIAL_BUDGET',str(fixture.get('context_budget',1000))).replace('INITIAL_COUNT',str(root_count if fixture.get('initially_missing') else len(names))).replace('RESUME_CODE',resume).replace('regs[512]',f'regs[{regs_size}]').replace('DRAGON_ARCH_X86_64',{'x86_64':'DRAGON_ARCH_X86_64','x86_32':'DRAGON_ARCH_X86_32','arm64':'DRAGON_ARCH_ARM64'}[architecture]))
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
#include <string.h>
#include <dragon_c.h>
typedef dragon_guest_state_v2_t S;
typedef dragon_guest_memory_v2_t M;
typedef dragon_exec_context_v2_t C;
typedef dragon_exec_exit_v2_t E;
static unsigned char regs[512],memory[65536];
static uint64_t calls2000,calls3000;
static uint64_t get64(const unsigned char *p){uint64_t v;memcpy(&v,p,8);return v;}
static uint16_t get16(const unsigned char *p){uint16_t v;memcpy(&v,p,2);return v;}
static void put64(unsigned char *p,uint64_t v){memcpy(p,&v,8);}
static void put16(unsigned char *p,uint16_t v){memcpy(p,&v,2);}
DECLARATIONS
int main(int argc,char **argv) {
  if(argc!=3) return 90;
  FILE *f=fopen(argv[1],"rb");
  if(!f || fread(regs,1,sizeof(regs),f)!=sizeof(regs) || fread(memory,1,sizeof(memory),f)!=sizeof(memory)) return 91;
  fclose(f);
  const dragon_target_entry_v2_t entries[]={ENTRIES};
  S s={2,DRAGON_ARCH_X86_64,sizeof(regs),regs}; M m={memory,sizeof(memory)};
  C c={entries,INITIAL_COUNT,INITIAL_BUDGET,0}; E e={0};
  dragon_llvm_execute_v2(&s,&m,&c,&e,0xf000);
  RESUME_CODE
  f=fopen(argv[2],"wb"); if(!f) return 92;
  const unsigned char magic[8]={'D','T','F','L','O','W','1',0};
  fwrite(magic,1,8,f);fwrite(&calls2000,8,1,f);fwrite(&calls3000,8,1,f);
  fwrite(regs,1,sizeof(regs),f);fwrite(memory,1,sizeof(memory),f);
  fwrite(&e,1,sizeof(e),f);fwrite(&c.budget,8,1,f);
  return fclose(f)?93:0;
}
'''

def main(*, fixtures_path=None, model_factory=None, stop_before_calls=True, full_guest=False):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',action='append',dest='cases')
    parser.add_argument('--trials',type=int,default=8)
    parser.add_argument('--out',type=Path)
    parser.add_argument('--corrupt-output',action='store_true')
    parser.add_argument('--corrupt-fpsw',action='store_true',help='negative control: flip defined C1, never an excluded condition bit')
    parser.add_argument('--opt',default='opt-21');parser.add_argument('--llc',default='llc-21');parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    if args.trials<1:parser.error('positive trials required')
    fixtures=json.loads((fixtures_path or base.ROOT/'tests/fixtures/llvm-function-flow.json').read_text())
    selected=[f for f in fixtures['cases'] if not args.cases or f['id'] in args.cases]
    if not selected or (args.cases and set(args.cases)!={f['id'] for f in selected}):parser.error('unknown case')
    out=(args.out or Path(tempfile.mkdtemp(prefix='dragon-dispatch-'))).resolve();out.mkdir(parents=True,exist_ok=True)
    rows=[];match_status='match' if full_guest else 'prefix_match'
    for fixture in selected:
        for level in ('O0','O2'):
            work=out/fixture['id']/level;work.mkdir(parents=True,exist_ok=True)
            try:exe,bad_ir,sites=compile_case(fixture,fixtures['entry'],level,work,args,model_factory)
            except Exception as error:
                detail=error.stderr.decode(errors='replace') if isinstance(error,subprocess.CalledProcessError) and isinstance(error.stderr,bytes) else str(error)
                rows.append(dict(case=fixture['id'],opt=level,status='infrastructure_error',detail=detail));continue
            for trial in range(args.trials):
                row=dict(case=fixture['id'],opt=level,trial=trial)
                regs,memory=base.initial(fixture,trial,fixtures['entry'])
                try:expected=base.reference(fixture,regs,memory,fixtures['entry'],sites,stop_before_control=True,stop_before_calls=stop_before_calls,skip_stub_returns=bool(model_factory),full_guest=full_guest)
                except Exception as error:row.update(status='reference_error',detail=str(error));rows.append(row);continue
                actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64)
                if error:row.update(status=error['kind'],detail=error)
                else:
                    if args.corrupt_fpsw:
                        damaged=bytearray(actual[0]);damaged[487]^=2
                        actual=(bytes(damaged),)+actual[1:]
                    differences=base.compare(fixture,expected,actual,bad_ir,args.corrupt_output,include_pc=True)
                    record=struct.unpack('<8Q',actual[3]);exit=record[:7]
                    if exit!=expected[4]:differences.append(dict(field='exit',got=exit,expected=expected[4]))
                    row.update(status='different' if differences else match_status,differences=differences,exit=exit,budget_left=record[7],undefined_flag_fields=len(fixture['undefined_flags']),undefined_fpsw_bits=len(fixture.get('undefined_fpsw_bits',[])),register_fields_compared=len(base.FIELDS)+1-len(fixture['undefined_flags']),memory_bytes_compared=base.MEM_SIZE)
                rows.append(row)
            print(fixture['id'],level,dict(collections.Counter(r['status'] for r in rows if r['case']==fixture['id'] and r['opt']==level)),flush=True)
    statuses=dict(collections.Counter(r['status'] for r in rows))
    summary=dict(stage='full Guest execution or declared terminal boundary' if full_guest else ('before CALL/RET' if stop_before_calls else 'before RET; call models execute Guest RET')+' or explicit terminal boundary; NOT full-function success',cases=len(selected),trials_per_opt=args.trials,results=len(rows),statuses=statuses,skipped=0,legacy_normalizations=0,
      undefined_flag_fields=sum(r.get('undefined_flag_fields',0) for r in rows),undefined_fpsw_bits=sum(r.get('undefined_fpsw_bits',0) for r in rows),
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
        if row['status']!=match_status:print('FAIL',json.dumps(row,indent=2))
    return 0 if all(r['status']==match_status for r in rows) and len(rows)==len(selected)*2*args.trials else 1

if __name__=='__main__':sys.exit(main(stop_before_calls=False,full_guest=True))
