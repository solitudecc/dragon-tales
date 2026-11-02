"""Whole-function execution oracle, step 1 (existing LLVM ABI only).

Strict default: A/B remain real failures on current main. --assert-baseline
checks that all controls match and the two documented defects are detected;
it does NOT label their semantics correct. --corrupt-output is a negative
control. Artifacts include raw IR, optimized IR, input/output and JSON results.
Linux host + llc-21/opt-21/gcc; run with ~/ucdev/bin/python3 on the test VM.
"""
import argparse
import collections
import json
import hashlib
from pathlib import Path
import platform
import random
import re
import resource
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'python'))
import dragon
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import x86_const as U

MEM_SIZE, REGS_SIZE = 0x10000, 512
DATA, STACK, SENTINEL = 0x8000, 0xc800, 0xf000
GPRS = ['RAX', 'RCX', 'RDX', 'RBX', 'RSP', 'RBP', 'RSI', 'RDI'] + [f'R{i}' for i in range(8, 16)]
FLAGS = [('CF', 136, 0), ('PF', 137, 2), ('AF', 138, 4), ('ZF', 139, 6), ('SF', 140, 7), ('OF', 141, 11), ('DF', 142, 10)]
EDGES = [0, 1, 0xffffffffffffffff, 0x8000000000000000, 0xffffffff, 0x7fffffffffffffff, 0x100000000, 0x80000000]
EDI_EDGES = [0, 1, 0x80000000, 0xffffffff, 0x100000000, 0x100000001, 0x8000000000000000, 0xffffffffffffffff]
FIELDS = [(name, i*8, 8) for i, name in enumerate(GPRS)]
FIELDS += [(name, off, 1) for name, off, _ in FLAGS]
FIELDS += [(f'XMM{i}', 143+8*i, 8) for i in range(16)] + [(f'XMM{i}H', 271+8*i, 8) for i in range(16)]
FIELDS += [(f'FPU{i}L', 400+10*i, 8) for i in range(8)] + [(f'FPU{i}H', 408+10*i, 2) for i in range(8)]
FIELDS += [('FPTOP', 480, 1), ('FPTAG', 482, 2), ('FPCW', 484, 2), ('FPSW', 486, 2)]

def put(buf, off, value, size=8):
    buf[off:off+size] = value.to_bytes(size, 'little')

def get(buf, off, size=8):
    return int.from_bytes(buf[off:off+size], 'little')

def initial(fixture, trial, entry):
    rng = random.Random(1901+trial)
    regs = bytearray(rng.randbytes(REGS_SIZE))
    for i in range(16): put(regs, 8*i, EDGES[(trial+i)%len(EDGES)] if trial < 8 else rng.getrandbits(64))
    put(regs, 24, DATA); put(regs, 32, STACK); put(regs, 40, STACK+0x100)
    put(regs, 56, EDI_EDGES[trial%len(EDI_EDGES)]); put(regs, 128, entry)
    for _, off, _ in FLAGS: regs[off] = rng.randrange(2)
    for i in range(8):
        put(regs, 400+10*i, 0x8000000000000000+i)
        put(regs, 408+10*i, 0x3fff | ((i&1)<<15), 2)
    top = trial%8
    regs[480] = top; put(regs, 482, 0xffff, 2); put(regs, 484, 0x37f, 2); put(regs, 486, top<<11, 2)
    memory = bytearray(rng.randbytes(MEM_SIZE))
    code = bytes.fromhex(fixture['code_hex']); memory[entry:entry+len(code)] = code
    for stub in fixture['stubs']:
        code = bytes.fromhex(stub['code_hex']); address = stub['address']; memory[address:address+len(code)] = code
    put(memory, STACK, SENTINEL)
    put(memory, DATA, [0, 1, 0xfffffffd, 0xffffffff, 0x7ffffffd, 0x80000000, 0xfffffffe, 0x7fffffff][trial%8], 4)
    return regs, memory

def reference(fixture, regs, memory, entry, call_sites):
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    uc.mem_map(0, MEM_SIZE); uc.mem_write(0, bytes(memory))
    for i, name in enumerate(GPRS): uc.reg_write(getattr(U, 'UC_X86_REG_'+name), get(regs, i*8))
    flags = 2 | sum(regs[off]<<bit for _, off, bit in FLAGS)
    uc.reg_write(U.UC_X86_REG_EFLAGS, flags)
    for i in range(16): uc.reg_write(getattr(U, f'UC_X86_REG_XMM{i}'), get(regs, 143+8*i) | (get(regs, 271+8*i)<<64))
    for i in range(8): uc.reg_write(U.UC_X86_REG_FP0+i, (get(regs,400+10*i),get(regs,408+10*i,2)))
    uc.reg_write(U.UC_X86_REG_FPTAG,get(regs,482,2)); uc.reg_write(U.UC_X86_REG_FPCW,get(regs,484,2)); uc.reg_write(U.UC_X86_REG_FPSW,get(regs,486,2))
    events = collections.Counter()
    targets = {stub['address'] for stub in fixture['stubs']}
    ignored_calls=set();writes=[]
    def on_instruction(u, pc, size, data):
        if pc in targets: events.update([pc])
        if pc in call_sites:
            sp=u.reg_read(U.UC_X86_REG_RSP);ignored_calls.update(range(sp-8,sp))
    uc.hook_add(UC_HOOK_CODE,on_instruction)
    uc.hook_add(UC_HOOK_MEM_WRITE,lambda u,access,address,size,value,data:writes.append((u.reg_read(U.UC_X86_REG_RIP),address,size)))
    uc.emu_start(entry, SENTINEL, count=10000)
    if uc.reg_read(U.UC_X86_REG_RIP) != SENTINEL: raise RuntimeError('reference did not return to root sentinel within budget')
    out = bytearray(regs)
    for i, name in enumerate(GPRS): put(out, i*8, uc.reg_read(getattr(U,'UC_X86_REG_'+name)))
    # Existing IGNIL RET abstraction only: undo exactly the root return pop.
    put(out, 32, get(out,32)-8)
    flags = uc.reg_read(U.UC_X86_REG_EFLAGS)
    for _, off, bit in FLAGS: out[off] = (flags>>bit)&1
    for i in range(16):
        bits = uc.reg_read(getattr(U, f'UC_X86_REG_XMM{i}')); put(out,143+8*i,bits&((1<<64)-1));put(out,271+8*i,bits>>64)
    for i in range(8):
        low, high = uc.reg_read(U.UC_X86_REG_FP0+i);put(out,400+10*i,low);put(out,408+10*i,high,2)
    sw = uc.reg_read(U.UC_X86_REG_FPSW)
    out[480]=(sw>>11)&7;put(out,482,uc.reg_read(U.UC_X86_REG_FPTAG),2);put(out,484,uc.reg_read(U.UC_X86_REG_FPCW),2);put(out,486,sw,2)
    if any(pc not in call_sites and any(a in ignored_calls for a in range(address,address+size)) for pc,address,size in writes):
        raise RuntimeError('legacy adapter cannot exclude a CALL slot also written by ordinary code')
    return out, bytes(uc.mem_read(0,MEM_SIZE)), events, ignored_calls

DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned char regs[512], memory[65536];
static unsigned char *active_regs;
static uint64_t calls2000, calls3000;
void ROOT(unsigned char *, unsigned char *);
STUBS
int main(int argc, char **argv) {
  if (argc!=3) return 90;
  FILE *input=fopen(argv[1],"rb");
  if (!input || fread(regs,1,sizeof(regs),input)!=sizeof(regs) || fread(memory,1,sizeof(memory),input)!=sizeof(memory)) return 91;
  fclose(input);active_regs=regs;
  ROOT(memory,regs);
  FILE *output=fopen(argv[2],"wb");if(!output) return 92;
  const unsigned char magic[8]={'D','T','F','L','O','W','1',0};
  fwrite(magic,1,8,output);fwrite(&calls2000,8,1,output);fwrite(&calls3000,8,1,output);
  fwrite(regs,1,sizeof(regs),output);fwrite(memory,1,sizeof(memory),output);
  return fclose(output) ? 93 : 0;
}
'''

def compile_case(fixture, entry, level, work, args):
    dis = dragon.Disassembler(dragon.Architecture.X86_64, dragon.Configuration(strategy=0))
    graph = dis.analyze(bytes.fromhex(fixture['code_hex']),entry)
    function = graph.get_function(entry)
    if not function or not function.is_valid: raise RuntimeError('invalid root function')
    for block in function.block_addresses():
        ib = graph.get_ignil_block(block)
        if not ib or ib.unlifted(): raise RuntimeError('fixture contains missing/unlifted semantics')
    lifter = dragon.LLVMLifter(dragon.Architecture.X86_64)
    if not lifter.lift(function,graph): raise RuntimeError('function lift failed')
    ir = lifter.ir();name=f'sub_{entry:x}'
    header = re.search(r'define void @'+name+r'\(([^)]*)\)',ir)
    if not header or len(header[1].split(','))!=2 or any(not a.strip().startswith('ptr ') for a in header[1].split(',')):
        raise RuntimeError('legacy adapter requires the existing void(mem, regs) entry ABI')
    for stub in fixture['stubs']:
        symbol=f'fn_{stub["address"]:x}'
        declaration=re.search(r'declare void @'+symbol+r'\(([^)]*)\)',ir)
        if declaration and declaration[1].strip()!='ptr': raise RuntimeError('legacy adapter requires existing single-pointer callee ABI')
    raw=work/'raw.ll';raw.write_text(ir)
    subprocess.run([args.opt,'-passes=verify','-disable-output',str(raw)],check=True,capture_output=True)
    source=raw
    if level=='O2':
        source=work/'optimized.ll'
        subprocess.run([args.opt,'-S','-passes=default<O2>',str(raw),'-o',str(source)],check=True,capture_output=True)
    selected_ir=source.read_text()
    subprocess.run([args.opt,'-passes=verify','-disable-output',str(source)],check=True,capture_output=True)
    stubs=[]
    for stub in fixture['stubs']:
        address=stub['address'];counter=f'calls{address:x}'
        lines=[f'void fn_{address:x}(unsigned char *mem) {{ ++{counter};']
        if 'rax' in stub: lines += [f'uint64_t value={stub["rax"]}ULL;memcpy(active_regs,&value,8);']
        if 'store' in stub: lines += [f'uint64_t address;uint32_t value={stub["store"]};memcpy(&address,active_regs+24,8);memcpy(mem+address,&value,4);']
        lines += ['}'];stubs.append('\n'.join(lines))
    driver=work/'driver.c';driver.write_text(DRIVER.replace('ROOT',name).replace('STUBS','\n'.join(stubs)))
    obj=work/'module.o';exe=work/'native'
    triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}.get(platform.machine())
    if not triple: raise RuntimeError('minimal native adapter supports aarch64/x86_64 Linux hosts')
    subprocess.run([args.llc,'-O0','-filetype=obj','-mtriple='+triple,str(source),'-o',str(obj)],check=True,capture_output=True)
    subprocess.run([args.cc,'-O0',str(driver),str(obj),'-o',str(exe)],check=True,capture_output=True)
    call_sites={i.address for i in graph.instructions() if i.is_call}
    return exe, 'unreachable' in selected_ir, call_sites

def no_core(): resource.setrlimit(resource.RLIMIT_CORE,(0,0))

def native(exe, regs, memory, work, trial):
    source=work/f'input-{trial}.bin';output=work/f'output-{trial}.bin';source.write_bytes(regs+memory)
    output.unlink(missing_ok=True)
    try:
        run=subprocess.run([str(exe),str(source),str(output)],capture_output=True,timeout=2,preexec_fn=no_core)
    except subprocess.TimeoutExpired: return None,{'kind':'native_timeout'}
    if run.returncode: return None,{'kind':'native_exit','returncode':run.returncode,'stderr':run.stderr.decode(errors='replace')}
    if not output.exists(): return None,{'kind':'native_protocol','detail':'no output'}
    data=output.read_bytes()
    if len(data)!=24+REGS_SIZE+MEM_SIZE or data[:8]!=b'DTFLOW1\0': return None,{'kind':'native_protocol','detail':'invalid snapshot'}
    a,b=struct.unpack_from('<QQ',data,8)
    return (data[24:24+REGS_SIZE],data[24+REGS_SIZE:],collections.Counter({0x2000:a,0x3000:b})),None

def compare(fixture, expected, actual, bad_ir, corrupt):
    want_regs,want_mem,want_events,ignored=expected;got_regs,got_mem,got_events=actual
    if corrupt:
        damaged=bytearray(got_mem);damaged[DATA]^=1;got_mem=bytes(damaged)
    diffs=[];excluded_flags=set(fixture['undefined_flags'])
    for name,off,size in FIELDS:
        if name in excluded_flags: continue
        got,want=get(got_regs,off,size),get(want_regs,off,size)
        if got!=want: diffs.append({'field':name,'got':hex(got),'expected':hex(want)})
    mem_diffs=[i for i,(got,want) in enumerate(zip(got_mem,want_mem)) if got!=want and i not in ignored]
    if mem_diffs:
        diffs.append({'field':'memory','count':len(mem_diffs),'examples':[{'address':hex(i),'got':got_mem[i],'expected':want_mem[i]} for i in mem_diffs[:8]]})
    if want_events!=got_events: diffs.append({'field':'stub_calls','got':dict(got_events),'expected':dict(want_events)})
    if bad_ir: diffs.append({'field':'ir','detail':'unreachable in a fully modelled fixture without traps'})
    return diffs

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures',type=Path,default=ROOT/'tests/fixtures/llvm-function-flow.json')
    parser.add_argument('--case',action='append',dest='cases')
    parser.add_argument('--trials',type=int,default=8)
    parser.add_argument('--out',type=Path)
    parser.add_argument('--assert-baseline',action='store_true',help='require the two current defects and passing controls; not semantic success')
    parser.add_argument('--corrupt-output',action='store_true',help='negative control: flip one tested output byte')
    parser.add_argument('--opt',default='opt-21');parser.add_argument('--llc',default='llc-21');parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    if args.trials<1: parser.error('--trials must be positive')
    if sys.byteorder!='little': parser.error('minimal adapter requires little-endian host')
    for command in (args.opt,args.llc,args.cc):
        if not shutil.which(command): parser.error('missing tool: '+command)
    fixtures=json.loads(args.fixtures.read_text());entry=fixtures['entry']
    if fixtures.get('schema')!=1 or fixtures.get('architecture')!='x86_64': parser.error('minimal adapter accepts schema 1 / x86_64 only')
    selected=[f for f in fixtures['cases'] if not args.cases or f['id'] in args.cases]
    if not selected or (args.cases and set(args.cases)!={f['id'] for f in selected}): parser.error('unknown or empty case selection')
    out=(args.out or Path(tempfile.mkdtemp(prefix='dragon-function-flow-'))).resolve();out.mkdir(parents=True,exist_ok=True)
    results=[]
    for fixture in selected:
        for level in ('O0','O2'):
            work=out/fixture['id']/level;work.mkdir(parents=True,exist_ok=True)
            try: exe,bad_ir,call_sites=compile_case(fixture,entry,level,work,args)
            except Exception as error:
                detail=error.stderr.decode(errors='replace') if isinstance(error,subprocess.CalledProcessError) and isinstance(error.stderr,bytes) else str(error)
                results.append({'case':fixture['id'],'opt':level,'trial':None,'status':'infrastructure_error','detail':detail});continue
            for trial in range(args.trials):
                row={'case':fixture['id'],'opt':level,'trial':trial,'known_defect':fixture.get('known_defect'),'ir_unreachable':bad_ir}
                regs,memory=initial(fixture,trial,entry)
                try: expected=reference(fixture,regs,memory,entry,call_sites)
                except Exception as error: row.update(status='reference_error',detail=str(error));results.append(row);continue
                actual,error=native(exe,regs,memory,work,trial)
                exclusions={'legacy_pc_fields':1,'legacy_root_ret_rsp_adjustments':1,'undefined_flag_fields':len(fixture['undefined_flags']),'legacy_call_stack_bytes':len(expected[3])}
                if error: row.update(status=error['kind'],detail=error)
                else:
                    row['exclusions']=exclusions
                    row['register_fields_compared']=len(FIELDS)-len(fixture['undefined_flags'])
                    row['memory_bytes_compared']=MEM_SIZE-len(expected[3])
                    differences=compare(fixture,expected,actual,bad_ir,args.corrupt_output)
                    row.update(status='different' if differences else 'match',differences=differences)
                results.append(row)
            group=[r for r in results if r['case']==fixture['id'] and r['opt']==level]
            print(fixture['id'],level,dict(collections.Counter(r['status'] for r in group)),flush=True)
    statuses=collections.Counter(r['status'] for r in results);exclusions=collections.Counter()
    for row in results:exclusions.update(row.get('exclusions',{}))
    summary={'adapter':'legacy-mem-regs-v1','baseline_commit':'0aa4b73','cases':len(selected),'trials_per_opt':args.trials,'results':len(results),'statuses':dict(statuses),'skipped':0,'exclusions':dict(exclusions),'artifacts':str(out)}
    summary['snapshots_compared']=statuses['match']+statuses['different']
    summary['native_execution_failures']=sum(count for status,count in statuses.items() if status.startswith('native_'))
    summary['register_fields_compared']=sum(r.get('register_fields_compared',0) for r in results)
    summary['memory_bytes_compared']=sum(r.get('memory_bytes_compared',0) for r in results)
    summary['ir_violation_runs']=sum(bool(r.get('ir_unreachable')) for r in results)
    summary['value_mismatch_runs']=sum(any(d['field']!='ir' for d in r.get('differences',[])) for r in results)
    summary['unicorn_version']=__import__('unicorn').__version__
    summary['host']=platform.machine()
    summary['llvm_lifter_source_sha256']=hashlib.sha256((ROOT/'lib/LLVMLifter.cpp').read_bytes()).hexdigest()
    summary['tools']={command:subprocess.run([command,'--version'],capture_output=True,text=True,check=True).stdout.splitlines()[0] for command in (args.opt,args.llc,args.cc)}
    (out/'results.json').write_text(json.dumps({'summary':summary,'results':results},indent=2))
    print(json.dumps(summary,indent=2))
    failures=[r for r in results if r['status']!='match']
    reported=set()
    for row in failures:
        key=(row['case'],row['opt'],row['status'])
        if key not in reported:
            print('FAIL',json.dumps(row,indent=2));reported.add(key)
    if args.assert_baseline:
        valid=not args.corrupt_output and {f['id'] for f in selected}=={f['id'] for f in fixtures['cases']} and len(results)==len(selected)*2*args.trials
        for row in results:
            if not row.get('known_defect'):valid &= row['status']=='match'
            elif row['known_defect']=='call_state':valid &= row['status']=='different' and any(d['field']=='RAX' for d in row['differences'])
            elif row['known_defect']=='conditional_exit':valid &= row['ir_unreachable'] and row['status'] in ('different','native_exit','native_timeout')
        print('BASELINE DETECTED (A/B remain failing)' if valid else 'BASELINE ASSERTION FAILED')
        return 0 if valid else 1
    return 1 if failures else 0

if __name__=='__main__':sys.exit(main())
