"""Complete Guest CALL/RET oracle for x64, i386 and A64 (O0/O2).

Unicorn executes all fixture machine instructions to the root exit, or stops
at an explicitly declared unsupported/missing implementation boundary. Native
runs preserve all machine branches/calls, PC/SP, flags, SIMD/x87 and all memory.
There are NO Legacy CALL/RET normalizations. A/B bytes are unchanged.
"""
import argparse,collections,hashlib,json,platform,random,struct,sys,tempfile
from pathlib import Path
import function_dispatch_diff as compile
base=compile.base
from unicorn import Uc,UC_ARCH_X86,UC_ARCH_ARM64,UC_MODE_32,UC_MODE_64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import x86_const as X,arm64_const as A

class Adapter:
    def __init__(self,arch):
        self.arch=arch;self.a64=arch=='arm64';self.x32=arch=='x86_32'
        self.regs_size=800 if self.a64 else 512;self.pc_offset=264 if self.a64 else 128
        if self.a64:
            self.gprs=[(f'X{i}',8*i,8,getattr(A,f'UC_ARM64_REG_X{i}')) for i in range(31)]+[('SP',256,8,A.UC_ARM64_REG_SP)]
            self.flags=[('NF',784,31),('ZF',785,30),('CF',786,29),('VF',787,28)]
            self.vectors=[(f'V{i}',272+16*i,272+16*i+8,getattr(A,f'UC_ARM64_REG_Q{i}')) for i in range(32)]
            self.pc_reg=A.UC_ARM64_REG_PC;self.sp_offset=256;self.arch_id,self.mode=UC_ARCH_ARM64,UC_MODE_ARM
        else:
            names=['EAX','ECX','EDX','EBX','ESP','EBP','ESI','EDI'] if self.x32 else base.GPRS
            self.gprs=[(name,i*8,4 if self.x32 else 8,getattr(X,'UC_X86_REG_'+name)) for i,name in enumerate(names)]
            self.flags=base.FLAGS;self.vectors=[(f'XMM{i}',143+8*i,271+8*i,getattr(X,f'UC_X86_REG_XMM{i}')) for i in range(8 if self.x32 else 16)]
            self.pc_reg=X.UC_X86_REG_EIP if self.x32 else X.UC_X86_REG_RIP;self.sp_offset=32;self.arch_id,self.mode=UC_ARCH_X86,UC_MODE_32 if self.x32 else UC_MODE_64
        self.fields=[(n,o,w) for n,o,w,_ in self.gprs]+[(n,o,1) for n,o,_ in self.flags]
        self.fields += [(n,o,8) for n,o,h,_ in self.vectors]+[(n+'H',h,8) for n,o,h,_ in self.vectors]
        if self.a64:self.fields += [('XZR',248,8)]
        else:self.fields += [f for f in base.FIELDS if f[0].startswith(('FPU','FP'))]
        self.fields += [('PC',self.pc_offset,8)]
    def initial(self,f,trial,entry):
        rng=random.Random(1901+trial);regs=bytearray(rng.randbytes(self.regs_size));memory=bytearray(rng.randbytes(base.MEM_SIZE))
        for i,(_,off,width,_) in enumerate(self.gprs):
            value=base.EDGES[(trial+i)%8]&((1<<(width*8))-1);base.put(regs,off,value,8)
        for _,off,_ in self.flags:regs[off]=rng.randrange(2)
        for _,lo,hi,_ in self.vectors:base.put(regs,lo,rng.getrandbits(64));base.put(regs,hi,rng.getrandbits(64))
        if self.a64:
            base.put(regs,248,0);base.put(regs,152,base.DATA);base.put(regs,240,base.SENTINEL)
        else:
            base.put(regs,24,base.DATA);base.put(regs,40,base.STACK+0x100);base.put(regs,56,base.EDI_EDGES[trial%8]&(0xffffffff if self.x32 else (1<<64)-1))
            for i in range(8):base.put(regs,400+10*i,0x8000000000000000+i);base.put(regs,408+10*i,0x3fff|((i&1)<<15),2)
            regs[480]=trial%8;base.put(regs,482,0xffff,2);base.put(regs,484,0x37f,2);base.put(regs,486,(trial%8)<<11,2)
        base.put(regs,self.sp_offset,f.get('initial_sp',base.STACK));base.put(regs,self.pc_offset,entry)
        for name,value in f.get('set_gprs',{}).items():
            slot=next((o for n,o,w,u in self.gprs if n==name),None)
            if slot is None:raise ValueError('unknown input register '+name)
            base.put(regs,slot,value)
        for addr,code in [(entry,f['code_hex'])]+[(s['address'],s['code_hex']) for s in f['stubs']]:
            data=bytes.fromhex(code);memory[addr:addr+len(data)]=data
        if not self.a64:
            sp=base.get(regs,self.sp_offset);width=4 if self.x32 else 8
            base.put(memory,sp+f.get('root_return_offset',0),f.get('root_return_target',base.SENTINEL),width)
        for address in f.get('missing_targets',[]):memory[address]=0x90 if not self.a64 else 0
        for cell in f.get('memory_values',[]):base.put(memory,cell['address'],cell['value'],cell['width'])
        return regs,memory
    def reference(self,f,regs,memory,entry):
        uc=Uc(self.arch_id,self.mode);uc.mem_map(0,base.MEM_SIZE);uc.mem_write(0,bytes(memory))
        for _,off,width,reg in self.gprs:uc.reg_write(reg,base.get(regs,off,width))
        flags=sum(regs[o]<<b for _,o,b in self.flags)
        uc.reg_write(A.UC_ARM64_REG_NZCV if self.a64 else X.UC_X86_REG_EFLAGS,flags if self.a64 else flags|2)
        for _,lo,hi,reg in self.vectors:uc.reg_write(reg,base.get(regs,lo)|(base.get(regs,hi)<<64))
        if not self.a64:
            for i in range(8):uc.reg_write(X.UC_X86_REG_FP0+i,(base.get(regs,400+10*i),base.get(regs,408+10*i,2)))
            uc.reg_write(X.UC_X86_REG_FPTAG,base.get(regs,482,2));uc.reg_write(X.UC_X86_REG_FPCW,base.get(regs,484,2));uc.reg_write(X.UC_X86_REG_FPSW,base.get(regs,486,2))
        events=collections.Counter();targets={s['address'] for s in f['stubs']};last=[entry,0];stop=[]
        def hook(u,pc,size,data):
            if pc in f.get('unsupported_before',[]):stop.extend([5,0,pc,pc,1,0,2]);u.emu_stop();return
            if pc in f.get('missing_targets',[]):
                kind=f.get('boundary_kind',1);stop.extend([3,kind,last[0],pc,1,last[0]+last[1] if kind==2 else 0,0]);u.emu_stop();return
            if pc in targets:events.update([pc])
            last[:]=[pc,size]
        uc.hook_add(UC_HOOK_CODE,hook);uc.emu_start(entry,base.SENTINEL,count=10000)
        pc=uc.reg_read(self.pc_reg)
        if not stop:
            if pc!=base.SENTINEL:raise RuntimeError('reference did not reach root exit within budget')
            stop=[1,3,last[0],pc,1,0,0]
        out=bytearray(regs)
        for _,off,width,reg in self.gprs:base.put(out,off,uc.reg_read(reg),8)
        base.put(out,self.pc_offset,pc)
        bits=uc.reg_read(A.UC_ARM64_REG_NZCV if self.a64 else X.UC_X86_REG_EFLAGS)
        for _,off,bit in self.flags:out[off]=(bits>>bit)&1
        for _,lo,hi,reg in self.vectors:
            value=uc.reg_read(reg);base.put(out,lo,value&((1<<64)-1));base.put(out,hi,value>>64)
        if not self.a64:
            for i in range(8):lo,hi=uc.reg_read(X.UC_X86_REG_FP0+i);base.put(out,400+10*i,lo);base.put(out,408+10*i,hi,2)
            sw=uc.reg_read(X.UC_X86_REG_FPSW);out[480]=(sw>>11)&7
            for off,reg in [(482,X.UC_X86_REG_FPTAG),(484,X.UC_X86_REG_FPCW),(486,X.UC_X86_REG_FPSW)]:base.put(out,off,uc.reg_read(reg),2)
        return bytes(out),bytes(uc.mem_read(0,base.MEM_SIZE)),events,tuple(stop)
    def compare(self,f,expected,actual,corrupt):
        want_regs,want_mem,want_events,want_exit=expected;got_regs,got_mem,got_events,extra=actual
        if corrupt:damage=bytearray(got_mem);damage[base.DATA]^=1;got_mem=bytes(damage)
        differences=[]
        for name,off,size in self.fields:
            if name in f['undefined_flags']:continue
            got,want=base.get(got_regs,off,size),base.get(want_regs,off,size)
            if got!=want:differences.append(dict(field=name,got=hex(got),expected=hex(want)))
        addresses=[i for i,(g,w) in enumerate(zip(got_mem,want_mem)) if g!=w]
        if addresses:differences.append(dict(field='memory',count=len(addresses),examples=[dict(address=hex(i),got=got_mem[i],expected=want_mem[i]) for i in addresses[:8]]))
        if got_events!=want_events:differences.append(dict(field='calls',got=dict(got_events),expected=dict(want_events)))
        record=struct.unpack('<8Q',extra)
        if record[:7]!=want_exit:differences.append(dict(field='exit',got=record[:7],expected=want_exit))
        return differences,record

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--trials',type=int,default=8);parser.add_argument('--case',action='append',dest='cases');parser.add_argument('--out',type=Path);parser.add_argument('--corrupt-output',action='store_true')
    parser.add_argument('--opt',default='opt-21');parser.add_argument('--llc',default='llc-21');parser.add_argument('--cc',default='gcc');args=parser.parse_args()
    if args.trials<1:parser.error('positive trials required')
    fixtures=json.loads((base.ROOT/'tests/fixtures/llvm-guest-flow.json').read_text());selected=[f for f in fixtures['cases'] if not args.cases or f['id'] in args.cases]
    if not selected or (args.cases and set(args.cases)!={f['id'] for f in selected}):parser.error('unknown case')
    out=(args.out or Path(tempfile.mkdtemp(prefix='dragon-guest-'))).resolve();out.mkdir(parents=True,exist_ok=True);rows=[]
    for f in selected:
        adapter=Adapter(f['architecture'])
        for level in ('O0','O2'):
            work=out/f['id']/level;work.mkdir(parents=True,exist_ok=True)
            try:exe,bad_ir,sites=compile.compile_case(f,fixtures['entry'],level,work,args,architecture=f['architecture'])
            except Exception as error:
                detail=error.stderr.decode(errors='replace') if isinstance(error,compile.subprocess.CalledProcessError) and isinstance(error.stderr,bytes) else str(error)
                rows.append(dict(case=f['id'],opt=level,status='infrastructure_error',detail=detail));continue
            for trial in range(args.trials):
                row=dict(case=f['id'],architecture=f['architecture'],opt=level,trial=trial);regs,memory=adapter.initial(f,trial,fixtures['entry'])
                try:expected=adapter.reference(f,regs,memory,fixtures['entry'])
                except Exception as error:row.update(status='reference_error',detail=str(error));rows.append(row);continue
                actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64,regs_size=adapter.regs_size)
                if error:row.update(status=error['kind'],detail=error)
                else:
                    diffs,record=adapter.compare(f,expected,actual,args.corrupt_output)
                    if bad_ir:diffs.append(dict(field='ir',detail='unreachable in supported fixture'))
                    row.update(status='different' if diffs else 'match',differences=diffs,exit=record[:7],register_fields_compared=len(adapter.fields)-len(f['undefined_flags']),memory_bytes_compared=base.MEM_SIZE,undefined_flag_fields=len(f['undefined_flags']))
                rows.append(row)
            print(f['id'],level,dict(collections.Counter(r['status'] for r in rows if r['case']==f['id'] and r['opt']==level)),flush=True)
    summary=dict(cases=len(selected),trials_per_opt=args.trials,statuses=dict(collections.Counter(r['status'] for r in rows)),results=len(rows),skipped=0,legacy_normalizations=0,register_fields_compared=sum(r.get('register_fields_compared',0) for r in rows),memory_bytes_compared=sum(r.get('memory_bytes_compared',0) for r in rows),undefined_flag_fields=sum(r.get('undefined_flag_fields',0) for r in rows),exits=dict(collections.Counter(str(r['exit'][0]) for r in rows if 'exit' in r)),llvm_lifter_sha256=hashlib.sha256((base.ROOT/'lib/LLVMLifter.cpp').read_bytes()).hexdigest(),artifacts=str(out))
    summary['runtime_sha256']=hashlib.sha256((base.ROOT/'lib/LLVMExecution.cpp').read_bytes()).hexdigest()
    summary['unicorn_version']=__import__('unicorn').__version__;summary['host']=platform.machine()
    summary['tools']={command:compile.subprocess.run([command,'--version'],capture_output=True,text=True,check=True).stdout.splitlines()[0] for command in (args.opt,args.llc,args.cc)}
    (out/'results.json').write_text(json.dumps(dict(summary=summary,results=rows),indent=2));print(json.dumps(summary,indent=2))
    shown=set()
    for r in rows:
        if r['status']!='match' and (r['case'],r['opt'],r['status']) not in shown:print('FAIL',json.dumps(r,indent=2));shown.add((r['case'],r['opt'],r['status']))
    return 0 if len(rows)==len(selected)*args.trials*2 and all(r['status']=='match' for r in rows) else 1
if __name__=='__main__':sys.exit(main())
