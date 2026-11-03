"""C ABI and Python opt-in mirror for V2; no host ABI inference."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'python'))
import dragon

def main():
    dis=dragon.Disassembler(dragon.Architecture.X86_64)
    graph=dis.analyze(bytes.fromhex('e9fb1f0000'),0x1000)
    function=graph.get_function(0x1000)
    assert function and function.is_valid
    with dragon.LLVMLifter(execution_mode=dragon.LLVMExecutionMode.STATEFUL_V2) as lifter:
        assert lifter.lift(function,graph)
        assert 'define void @dragon_v2_x86_64_1000(ptr %state, ptr %memory, ptr %runtime, ptr %exit)' in lifter.ir()
        assert not dragon.lib.dragon_llvm_lifter_set_execution_mode(lifter._ptr,0)
    with dragon.LLVMLifter() as lifter:
        assert not lifter.lift(function,graph)
    try:dragon.LLVMLifter(execution_mode=99)
    except ValueError:pass
    else:raise AssertionError('invalid mode accepted')
    ffi,lib=dragon.ffi,dragon.lib
    regs=ffi.new('uint8_t[512]');memory=ffi.new('uint8_t[64]')
    state=ffi.new('dragon_guest_state_v2_t *',dict(abi_version=2,architecture=2,regs_size=512,regs_bytes=regs))
    mem=ffi.new('dragon_guest_memory_v2_t *',dict(data=memory,size=64))
    runtime=ffi.new('dragon_exec_context_v2_t *',dict(budget=3));exit=ffi.new('dragon_exec_exit_v2_t *')
    lib.dragon_llvm_execute_v2(state,mem,runtime,exit,0xffff)
    assert exit.reason==3 and exit.next_pc_valid==1 and exit.next_pc==0
    lib.dragon_llvm_execute_v2(ffi.NULL,mem,runtime,exit,0xffff)
    assert exit.reason==5 and exit.detail==1
    print('LLVM V2 C/Python mirror: passed')

if __name__=='__main__':main()
