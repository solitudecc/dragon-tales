"""Static mode remains opt-in and keeps unknown calls as declarations."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'python'))
import dragon

for arch,name,code in [(1,'x86_32','e8fb0f0000c3'),(2,'x86_64','e8fb0f0000c3'),
                       (4,'arm64','00040094c0035fd6')]:
    graph=dragon.Disassembler(arch).analyze(bytes.fromhex(code),0x1000)
    fn=graph.get_function(0x1000)
    with dragon.LLVMLifter(arch,execution_mode=2) as lifter:
        assert lifter.lift(fn,graph)
        assert f'define void @dragon_static_{name}_1000(' in lifter.ir()
        assert 'declare void @dragon_static_external(ptr, ptr, ptr, ptr)' in lifter.ir()
        assert 'call void @dragon_static_external' in lifter.ir()
        assert not dragon.lib.dragon_llvm_lifter_set_execution_mode(lifter._ptr,0)
assert dragon.LLVMExecutionMode.STATIC_ANALYSIS==2
assert dragon.lib.DRAGON_LLVM_STATIC_ANALYSIS==2
print('LLVM static C/Python mirror: 3 architectures passed')
