"""Exact string/REP differential test, including overlap and zero-count guards.

Run with Unicorn dev. Tests i386, x86-64 and x86-64 addr32, DF=0/1,
RCX=0/1/2/5, both repeat prefixes and forward/backward overlap. Exit nonzero
for any difference, unresolved value, unsupported instruction or harness fault.
"""
import os, sys, random
from collections import Counter
sys.path.insert(0,os.path.join(os.path.dirname(__file__),'..','..','python'))
import dragon
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_MODE_64
from unicorn import x86_const as C

CODE,DATA=0x100000,0x200000
FLAGS=[('CF',0),('PF',2),('AF',4),('ZF',6),('SF',7),('DF',10),('OF',11)]
GPRS=['AX','CX','DX','BX','SP','BP','SI','DI']
stats=Counter()
rng=random.Random(199)
for bits,addr32 in [(32,False),(64,False),(64,True)]:
    arch=dragon.Architecture.X86_64 if bits==64 else dragon.Architecture.X86_32
    names=[('R' if bits==64 else 'E')+name for name in GPRS]
    if bits==64: names += [f'R{i}' for i in range(8,16)]
    asm=dragon.Assembler(arch,dragon.Configuration()); dis=dragon.Disassembler(arch,dragon.Configuration())
    for operation in ['movs','stos','lods','cmps','scas']:
        for suffix,size in [('b',1),('w',2),('d',4)]+([('q',8)] if bits==64 else []):
            for prefix in ['', 'rep ', 'repne ']:
                text=prefix+operation+suffix
                if os.environ.get('ONLY') and os.environ['ONLY'] not in text: continue
                code=bytes(asm.assemble(text)); code=(b'\x67'+code) if addr32 else code
                block=dis.lift_trace(dis.disassemble(code,CODE).instructions())
                if block.unlifted():
                    stats['unlifted']+=1; print('UNLIFTED',bits,addr32,text,flush=True); continue
                bad=False
                for count in ([0,1,2,5] if prefix else [5]):
                    for df in [0,1]:
                        for overlap in [-size,0,size,128]:
                            page=bytearray(rng.getrandbits(8) for _ in range(512))
                            source=DATA+0x100; destination=source+overlap
                            initial={name:rng.getrandbits(bits) for name in names}
                            lead='R' if bits==64 else 'E'
                            initial[lead+'CX']=count
                            initial[lead+'SI']=source; initial[lead+'DI']=destination
                            if addr32:
                                for name in ['RCX','RSI','RDI']: initial[name] |= 0xaabbccdd00000000
                            # Make comparison exit modes reproducible as well as random.
                            if operation=='scas':
                                index=destination-DATA+(size if not df else -size)*(prefix=='repne ')
                                value=int.from_bytes(page[index:index+size],'little')
                                initial[lead+'AX']=(initial[lead+'AX'] & ~((1<<(size*8))-1)) | value
                            elif operation=='cmps' and overlap==128 and prefix=='rep ':
                                index=destination-DATA
                                page[index:index+size]=page[source-DATA:source-DATA+size]
                            flags=2 | (df<<10) | sum(rng.getrandbits(1)<<bit for _,bit in FLAGS if bit!=10)
                            uc=Uc(UC_ARCH_X86,UC_MODE_64 if bits==64 else UC_MODE_32)
                            uc.mem_map(CODE,4096);uc.mem_map(DATA,4096)
                            uc.mem_write(CODE,code);uc.mem_write(DATA,bytes(page))
                            for name,value in initial.items(): uc.reg_write(getattr(C,'UC_X86_REG_'+name),value)
                            uc.reg_write(C.UC_X86_REG_EFLAGS,flags)
                            uc.emu_start(CODE,CODE+len(code),count=1000)
                            assert uc.reg_read(C.UC_X86_REG_RIP if bits==64 else C.UC_X86_REG_EIP)==CODE+len(code)
                            executor=dragon.SymbolicExecutor()
                            for name,value in initial.items():executor.set_register(getattr(dragon.x86,name),value)
                            for name,bit in FLAGS:executor.set_register(getattr(dragon.x86,name),(flags>>bit)&1)
                            executor.map_memory(DATA,bytes(page));result=executor.run(block)
                            failures=[] if result.complete else ['incomplete execution']
                            for name in names:
                                actual=executor.concrete_register(getattr(dragon.x86,name));expected=uc.reg_read(getattr(C,'UC_X86_REG_'+name))
                                if actual!=expected: failures.append((name,actual,expected))
                            expected_flags=uc.reg_read(C.UC_X86_REG_EFLAGS)
                            for name,bit in FLAGS:
                                actual=executor.concrete_register(getattr(dragon.x86,name));expected=(expected_flags>>bit)&1
                                if actual!=expected:failures.append((name,actual,expected))
                            expected_page=bytes(uc.mem_read(DATA,len(page)))
                            for offset in range(0,len(page),8):
                                actual=executor.concrete_memory(DATA+offset,64);expected=int.from_bytes(expected_page[offset:offset+8],'little')
                                if actual!=expected: failures.append(('memory',offset,actual,expected));break
                            stats['executions']+=1
                            if failures:
                                stats['wrong']+=1;bad=True
                                print('DIFF',bits,addr32,text,'count',count,'df',df,'overlap',overlap,failures[:3],flush=True)
                                break
                        if bad:break
                    if bad:break
                stats['templates']+=1
print(dict(stats),flush=True)
if stats['wrong'] or stats['unlifted']: sys.exit(1)
