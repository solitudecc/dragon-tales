"""Explicit static function-entry contracts and their C/Python mirrors."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'python'))
import dragon

Policy=dragon.LLVMStaticEntryPolicy
assert Policy.RESUMABLE==dragon.lib.DRAGON_LLVM_ENTRY_RESUMABLE==0
assert Policy.FUNCTION_START==dragon.lib.DRAGON_LLVM_ENTRY_FUNCTION_START==1
assert not dragon.lib.dragon_llvm_lifter_set_static_entry_policy(dragon.ffi.NULL,1)
for mode,policy in ((0,1),(1,1),(2,2)):
    try:
        dragon.LLVMLifter(execution_mode=mode,static_entry_policy=policy)
    except ValueError:
        pass
    else:
        raise AssertionError((mode,policy))
for arch,name,code in ((1,'x86_32','b807000000eb008903c3'),
                       (2,'x86_64','b807000000eb008903c3'),
                       (4,'arm64','e000805201000014600200b9c0035fd6')):
    graph=dragon.Disassembler(arch).analyze(bytes.fromhex(code),0x1000)
    fn=graph.get_function(0x1000)
    for policy in (None,Policy.RESUMABLE,Policy.FUNCTION_START):
        with dragon.LLVMLifter(arch,execution_mode=2,static_entry_policy=policy) as lifter:
            assert lifter.lift(fn,graph)
            prefix='entry_' if policy==Policy.FUNCTION_START else ''
            assert f'define void @dragon_static_{prefix}{name}_1000(' in lifter.ir()
            assert not dragon.lib.dragon_llvm_lifter_set_static_entry_policy(lifter._ptr,0)
            lifter.optimize()
            assert 'unreachable' not in lifter.ir()
print('static entry C/Python: modes, policies and three architectures passed')
