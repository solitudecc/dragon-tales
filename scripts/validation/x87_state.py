"""Shared raw x87 initial states for real-block executor/LLVM validation."""
import dragon
from unicorn import x86_const as U

def initial(seed):
    top=seed%8
    values=[(0x8000000000000000,0x3fff),(0xc000000000000000,0xbfff),
            (0,0),(0x8000000000000001,0x3fff)]
    pairs=[values[(i+seed)%4] for i in range(8)]
    tags=sum((3 if ((i-top)&7)>=4 else 1 if pairs[i][0]==0 else 0)<<(2*i) for i in range(8))
    state=[((480,8,'FPTOP'),top),((482,16,'FPTAG'),tags),((484,16,'FPCW'),0x37f|((seed%4)<<10)),((486,16,'FPSW'),top<<11)]
    state += [((400+10*i,64,f'FPU{i}L'),lo) for i,(lo,hi) in enumerate(pairs)]
    state += [((408+10*i,16,f'FPU{i}H'),hi) for i,(lo,hi) in enumerate(pairs)]
    return state

def seed_uc(uc,state):
    vals={reg[0]:value for reg,value in state}
    for i in range(8):uc.reg_write(U.UC_X86_REG_FP0+i,(vals[400+10*i],vals[408+10*i]))
    uc.reg_write(U.UC_X86_REG_FPTAG,vals[482]);uc.reg_write(U.UC_X86_REG_FPCW,vals[484]);uc.reg_write(U.UC_X86_REG_FPSW,vals[486])

def seed_ex(ex,state):
    for reg,value in state:ex.set_register(reg,value)

def expected(uc):
    result=[((480,8,'FPTOP'),(uc.reg_read(U.UC_X86_REG_FPSW)>>11)&7),
            ((482,16,'FPTAG'),uc.reg_read(U.UC_X86_REG_FPTAG)),
            ((484,16,'FPCW'),(uc.reg_read(U.UC_X86_REG_FPCW)&0x1f3f)|0x40)]
    for i in range(8):
        lo,hi=uc.reg_read(U.UC_X86_REG_FP0+i)
        result += [((400+10*i,64,f'FPU{i}L'),lo),((408+10*i,16,f'FPU{i}H'),hi)]
    return result

def fix_compare_flags(uc,insns):
    """Unicorn leaves AF/SF/OF set after FCOMI; native x86 clears them.
    Correct the oracle before the next instruction can consume the flags.
    x87_native.c reproduces the discrepancy; no other flags are changed.
    """
    from unicorn import UC_HOOK_CODE
    ends={i.address+i.size for i in insns if i.mnemonic.startswith(('fcomi','fucomi','fcompi','fucompi'))}
    def clear():uc.reg_write(U.UC_X86_REG_EFLAGS,uc.reg_read(U.UC_X86_REG_EFLAGS)&~0x890)
    uc.hook_add(UC_HOOK_CODE,lambda u,address,size,data:clear() if address in ends else None)
    return lambda: clear() if insns[-1].address+insns[-1].size in ends else None
