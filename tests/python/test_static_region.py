"""Explicit decoded coverage and bounded C/Python argument routing."""
import itertools
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'python'))
import dragon as d

def expect(error,call):
    try:call()
    except error:return
    raise AssertionError('expected '+error.__name__)

api=d.lib.dragon_llvm_lifter_lift_static_region
assert not api(d.ffi.NULL,d.ffi.NULL,d.ffi.NULL,d.ffi.NULL,0,4096,65536,0)
for arch,code,target in ((1,'ffe0b907000000c3',0x1002),(2,'ffe0b907000000c3',0x1002),
                         (4,'00001fd6e10080d2c0035fd6',0x1004)):
    with d.Disassembler(arch).analyze(bytes.fromhex(code),0x1000) as graph:
        fn=graph.get_function(0x1000)
        before=(tuple(graph.block_addresses()),tuple(fn.block_addresses()),tuple(graph.get_instruction(0x1000).branches))
        with d.LLVMLifter(arch,execution_mode=2,static_entry_policy=1) as lifter:
            empty=lifter.ir()
            assert not lifter.lift_static_region(fn,graph,[target])
            assert lifter.ir()==empty
            for opts in ({'max_blocks':0},{'max_blocks':4097},{'max_dispatch_cases':65537},
                         {'max_blocks':True},{'rebuild_decoded_blocks':1}):
                expect(ValueError,lambda:lifter.lift_static_region(fn,graph,[],**opts))
            for addresses in ([-1],[1<<64],[True]):
                expect(ValueError,lambda:lifter.lift_static_region(fn,graph,addresses))
            seen=[]
            def endless():
                while True:seen.append(0x1000);yield 0x1000
            expect(ValueError,lambda:lifter.lift_static_region(fn,graph,endless(),max_blocks=1))
            assert len(seen)==2 and lifter.ir()==empty
            assert not api(lifter._ptr,fn._ptr,graph._ptr,d.ffi.NULL,1,4096,65536,0)
            assert not api(lifter._ptr,fn._ptr,graph._ptr,d.ffi.NULL,0,4096,65536,2)
            assert lifter.ir()==empty
            assert lifter.lift_static_region(fn,graph,[target],rebuild_decoded_blocks=True)
            python_ir=lifter.ir()
            assert '"dragon.static-coverage"="decoded-region"' in python_ir
            assert '"dragon.coverage-source"="decoded-machine"' in python_ir
            assert '"dragon.entrypoints"="1000"' in python_ir
        with d.LLVMLifter(arch,execution_mode=2,static_entry_policy=1) as lifter:
            addresses=d.ffi.new('uint64_t[]',[target])
            assert api(lifter._ptr,fn._ptr,graph._ptr,addresses,1,4096,65536,1)
            assert lifter.ir()==python_ir
        assert before==(tuple(graph.block_addresses()),tuple(fn.block_addresses()),tuple(graph.get_instruction(0x1000).branches))
        for mode,policy in ((0,None),(1,None),(2,None)):
            with d.LLVMLifter(arch,execution_mode=mode,static_entry_policy=policy) as lifter:
                empty=lifter.ir()
                assert not lifter.lift_static_region(fn,graph,[target],rebuild_decoded_blocks=True)
                assert lifter.ir()==empty
print('Static regions: C/Python routing, modes, limits, private rebuild and Graph preservation passed')
