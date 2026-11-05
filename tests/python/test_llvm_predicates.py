"""Real A64 predicates through the existing optimize() API, plus C/Python limits."""
import os,re,sys
from pathlib import Path
sys.path.insert(0,os.environ.get('DRAGON_TEST_PYTHON',str(Path(__file__).resolve().parents[2]/'python')))
import dragon

available='--no-z3' not in sys.argv
cases=[('even32','01040011017c011b2100001261000035e00080d2c0035fd6600c80d2c0035fd6',True),
       ('even64','01040091017c019b21004092610000b5e00080d2c0035fd6600c80d2c0035fd6',True),
       ('square32','01040011017c001b2100001261000035e00080d2c0035fd6600c80d2c0035fd6',False),
       ('square64','01040091017c009b21004092610000b5e00080d2c0035fd6600c80d2c0035fd6',False)]
for name,code,constant in cases:
    graph=dragon.Disassembler(4,dragon.Configuration(strategy=0)).analyze(bytes.fromhex(code),0x1000)
    with dragon.LLVMLifter(4,execution_mode=2) as lifter:
        assert lifter.lift(graph.get_function(0x1000),graph),name
        lifter.optimize()
        count=len(re.findall(r'\bbr i1\b',lifter.ir()))
        assert count==(0 if available and constant else 1),(name,count,lifter.ir())

# The checks above use only the old optimize() API and fail on the old backend.
graph=dragon.Disassembler(4,dragon.Configuration(strategy=0)).analyze(bytes.fromhex(cases[1][1]),0x1000)
with dragon.LLVMLifter(4,execution_mode=2) as lifter:
    assert lifter.lift(graph.get_function(0x1000),graph)
    lifter.run_mem2reg();lifter.run_inst_combine();lifter.run_gvn();lifter.run_dce()
    before=lifter.ir()
    stats=lifter.run_predicate_folding(timeout_ms=200,resource_limit=1)
    assert stats.available==available and stats.conditions_folded==0 and lifter.ir()==before
    if available:
        assert stats.solver_queries==stats.unknown_queries==2
        assert stats.unsupported_conditions==stats.budget_skipped==0
    stats=lifter.run_predicate_folding(max_expression_nodes=1)
    assert stats.conditions_folded==stats.solver_queries==0 and lifter.ir()==before
    if available:assert stats.budget_skipped>0
with dragon.LLVMLifter(4) as lifter:
    stats=lifter.run_predicate_folding()
    assert stats.available==available
    assert stats.conditions_visited==stats.conditions_folded==stats.solver_queries==0
    for limits in ({'timeout_ms':-1},{'max_queries':1<<32},{'max_expression_nodes':'bad'}):
        try:lifter.run_predicate_folding(**limits)
        except ValueError:pass
        else:raise AssertionError(limits)
out=dragon.ffi.new('dragon_predicate_fold_stats_t *')
assert not dragon.lib.dragon_llvm_lifter_fold_predicates(dragon.ffi.NULL,25,64,256,200000,out)
assert out.available==0
print('LLVM predicate folding: real A64 branch/negative controls and C/Python mirrors passed; Z3',available)
