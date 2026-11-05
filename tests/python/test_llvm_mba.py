"""MBA shapes through the existing optimize API, plus C/Python proof limits."""
import json,os,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,os.environ.get('DRAGON_TEST_PYTHON',str(root/'python')));import dragon
sys.path.insert(0,str(root/'scripts/validation'));from block_deobf_study import return_cone
available='--no-z3' not in sys.argv
fixtures=json.loads((root/'tests/fixtures/llvm-mba-functions.json').read_text())['cases']
selected=[f for f in fixtures if f['kind'] in ('add','xor')]
for f in selected:
    aid=4 if f['architecture']=='arm64' else 2
    graph=dragon.Disassembler(aid).analyze(bytes.fromhex(f['code_hex']),0x1000)
    with dragon.LLVMLifter(aid,execution_mode=2) as lifter:
        assert lifter.lift(graph.get_function(0x1000),graph)
        lifter.optimize();cone,ops=return_cone(lifter.ir(),f'dragon_static_{f["architecture"]}_1000')
        limit=(3 if aid==4 else 2) if f['width']==32 else 1
        assert cone is not None
        assert (cone<=limit if available else cone>limit),(f['id'],cone,ops)

# Only existing APIs above: the frozen fafea17 pipeline fails its first cone assertion.
f=next(f for f in selected if f['id']=='a64-add64')
g=dragon.Disassembler(4).analyze(bytes.fromhex(f['code_hex']),0x1000)
with dragon.LLVMLifter(4,execution_mode=2) as lifter:
    assert lifter.lift(g.get_function(0x1000),g)
    lifter.run_mem2reg();lifter.run_inst_combine();lifter.run_gvn()
    before=lifter.ir();s=lifter.run_mba_simplification(resource_limit=1)
    assert s.available==available and s.expressions_simplified==0 and lifter.ir()==before
    if available:assert s.solver_queries==s.unknown_queries==1
    s=lifter.run_mba_simplification(max_expression_nodes=1)
    assert s.expressions_simplified==s.solver_queries==0 and lifter.ir()==before
    for kw in ({'timeout_ms':-1},{'max_queries':1<<32},{'max_variables':'bad'}):
        try:lifter.run_mba_simplification(**kw)
        except ValueError:pass
        else:raise AssertionError(kw)
with dragon.LLVMLifter(4) as lifter:
    s=lifter.run_mba_simplification();assert s.available==available and s.expressions_visited==0
out=dragon.ffi.new('dragon_mba_simplify_stats_t *')
assert not dragon.lib.dragon_llvm_lifter_simplify_mba(dragon.ffi.NULL,100,16,128,3,1000000,out)
assert not out.available
print('LLVM MBA: existing optimize shapes and C/Python limits match; Z3',available)
