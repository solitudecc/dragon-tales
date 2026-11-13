"""Configuration, callbacks, cache lifetimes and compatible default execution."""
import gc,json,sys,weakref
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'python'))
import dragon as d

def expect(error,fn):
    try:fn()
    except error:return
    raise AssertionError('expected '+error.__name__)

def graph():return d.Disassembler(d.Architecture.X86_64).analyze(bytes.fromhex('b807000000eb008903c3'),0x1000)
def lift(g):
    l=d.LLVMLifter(execution_mode=d.LLVMExecutionMode.STATIC_ANALYSIS)
    assert l.lift(g.get_function(0x1000),g)
    return l

assert d.lib.DRAGON_PASS_GRAPH==d.PassStage.GRAPH==0
assert d.lib.DRAGON_PASS_ERROR==d.PassStatus.ERROR==3
leaf_fixtures=json.loads((Path(__file__).resolve().parents[1]/'fixtures/leaf-calls.json').read_text())
for fixture in leaf_fixtures['cases']:
    arch=d.Architecture.X86_64 if fixture['architecture']=='x86_64' else d.Architecture.X86_32
    with d.Disassembler(arch).analyze(bytes.fromhex(fixture['code_hex']),0x1000) as leaf_graph:
        old=leaf_graph.finite_values(entry_points=[0x1000])
        new=leaf_graph.finite_values(entry_points=[0x1000],prove_leaf_calls=True)
        assert old['proven_calls']==0 and new['proven_calls']==1 and new['rejected_calls']==0
        assert any(t['kind']==d.lib.DRAGON_VALUE_FINITE and
                   set(t['values'])==set(fixture['expected_targets']) for t in new['jump_targets'].values())
        expect(ValueError,lambda:leaf_graph.finite_values(prove_leaf_calls=1))
        assert d.lib.dragon_graph_finite_values_with_calls(leaf_graph._ptr,d.ffi.NULL,0,0,0,0,64,4096,1000000,262144,2)==d.ffi.NULL
        with d.PassPipeline() as leaf_pipeline:
            leaf_pipeline.add('finite-values',prove_leaf_calls=1)
            report=leaf_pipeline.run(leaf_graph)
            assert report['ok'] and not report['changed'] and report['runs'][0]['counters']['proven_calls']==1
stack_fixtures=json.loads((Path(__file__).resolve().parents[1]/'fixtures/stack-calls.json').read_text())
for fixture in stack_fixtures['cases']:
    arch=d.Architecture.X86_64 if fixture['architecture']=='x86_64' else d.Architecture.X86_32
    with d.Disassembler(arch).analyze(bytes.fromhex(fixture['code_hex']),0x1000) as stack_graph:
        facts=stack_graph.finite_values(entry_points=[0x1000],prove_leaf_calls=True)
        assert facts['complete'] and facts['proven_calls']==fixture['expected_proven_calls'] and facts['rejected_calls']==0
        assert any(t['kind']==d.lib.DRAGON_VALUE_FINITE and
                   set(t['values'])==set(fixture['expected_targets']) for t in facts['jump_targets'].values())
assert d.lib.dragon_pass_pipeline_run_llvm(d.ffi.NULL,d.ffi.NULL,1,64,1)==d.ffi.NULL
assert not d.lib.dragon_pass_pipeline_add(d.ffi.NULL,d.ffi.NULL,d.ffi.NULL)
g=graph()
with lift(g) as manual,lift(g) as managed:
    manual.run_mem2reg();manual.run_inst_combine();manual.run_gvn()
    manual.run_mba_simplification();manual.run_predicate_folding();manual.run_dce()
    assert managed.optimize() is None
    assert manual.ir()==managed.ir()

with lift(g) as l,d.PassPipeline() as p:
    names={x['name'] for x in p.passes()}
    assert names==set(d.PassPipeline.DEFAULT_LLVM)|{
        'graph-audit','finite-values','control-flow',
        'dispatch-specialize','dispatch-deflatten','guest-memory','entry-prefix',
        'guarded-transitions','constant-branches','guarded-target-sets','guarded-state-corridors','guarded-store-corridors',
        'invariant-self-loops','guarded-partial-corridors','guarded-state-values','guarded-partial-masks','guarded-checked-corridors'}
    for name,opts in [('bad',{}),('mba',{'max_queries':True}),('mba',{'max_queries':-1}),('mba',{'unknown':1})]:
        expect(ValueError,lambda:p.add(name,**opts))
    p.add('instcombine').add('graph-audit');before=l.ir()
    expect(d.PipelineError,lambda:p.run(l));assert l.ir()==before
    p.clear()
    p.register('dependent',lambda c,o:None,read_only=True,requires=('dce',)).add('dependent')
    expect(d.PipelineError,lambda:p.run(l));assert l.ir()==before
    expect(ValueError,lambda:p.register('dependent',lambda c,o:None))
    expect(ValueError,lambda:p.register('wrong',lambda c,o:None,stage=7))

with d.LLVMLifter() as l,d.PassPipeline(['constant-branches']) as p:
    assert 'constant-branches' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_folds',65536),('max_instructions',1000000),('max_blocks',65536)):
        expect(ValueError,lambda:p.add('constant-branches',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['available']==0

with d.LLVMLifter() as l,d.PassPipeline(['invariant-self-loops']) as p:
    assert 'invariant-self-loops' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_loops',65536),('max_work',1000000),('max_blocks',65536)):
        expect(ValueError,lambda:p.add('invariant-self-loops',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['available']==0
    assert r['runs'][0]['counters']['isolated_loops']==0

with lift(g) as l,d.PassPipeline(['guarded-partial-corridors']) as p:
    assert 'guarded-partial-corridors' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-partial-corridors',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['available']==0
    for counter in ('partial_candidate_edges','partial_attempted_edges',
                    'partial_rewritten_edges','partial_unresolved_edges'):
        assert r['runs'][0]['counters'][counter]==0

with lift(g) as l,d.PassPipeline(['guarded-state-corridors']) as p:
    assert 'guarded-state-corridors' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-state-corridors',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['fast_switches_created']==0

with lift(g) as l,d.PassPipeline(['guarded-store-corridors']) as p:
    assert 'guarded-store-corridors' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-store-corridors',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['fast_switches_created']==0

with lift(g) as l,d.PassPipeline(['guarded-target-sets']) as p:
    assert 'guarded-target-sets' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-target-sets',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['fast_switches_created']==0

with lift(g) as l,d.PassPipeline(['guarded-transitions']) as p:
    assert 'guarded-transitions' not in d.PassPipeline.DEFAULT_LLVM
    before=l.ir()
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-transitions',**{option:limit+1}))
    r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['available']==0
    assert r['runs'][0]['counters']['rewritten_edges']==0

with lift(g) as l,d.PassPipeline(['control-flow']) as p:
    assert 'control-flow' not in d.PassPipeline.DEFAULT_LLVM
    expect(ValueError,lambda:p.add('control-flow',assume_initialized=True))
    r=p.run(l)
    assert r['ok'] and r['runs'][0]['status']==d.PassStatus.SUCCESS
    assert set(r['runs'][0]['counters'])=={name+'_'+suffix for name in
        ('blocks','instructions','conditional_branches','switches') for suffix in ('before','after')}

with lift(g) as l:
    raw=d.lib.dragon_pass_pipeline_create(1)
    try:
        assert d.lib.dragon_pass_pipeline_add(raw,b'control-flow',b'{}')
        report=json.loads(d.PassPipeline._text(d.lib.dragon_pass_pipeline_run_llvm(raw,l._ptr,1,64,1)))
        assert report['ok'] and report['runs'][0]['status']==d.PassStatus.SUCCESS
        assert report['runs'][0]['counters']['blocks_before']>=1
    finally:d.lib.dragon_pass_pipeline_destroy(raw)

with lift(g) as l,d.PassPipeline() as p:
    events=[]
    def produce(c,o):c.analyses['fact']=17;events.append(('produce',c.revision))
    def read(c,o):assert c.analyses['fact']==17;events.append(('read',c.revision))
    def check(c,o):assert not c.analyses;events.append(('check',c.revision))
    p.register('produce',produce,read_only=True).register('read',read,read_only=True)
    p.register('check',check,read_only=True)
    for name in ('produce','read','instcombine','check'):p.add(name)
    r=p.run(l);assert r['ok'] and len(r['runs'])==4
    assert events==[('produce',0),('read',0),('check',1)]
    p.clear().add('check');p.run(l);assert events[-1]==('check',0)

with lift(g) as l,d.PassPipeline() as p:
    def failure(c,o):
        expect(RuntimeError,p.close)
        expect(RuntimeError,c.target.__del__)
        d.lib.dragon_pass_pipeline_destroy(p._ptr)
        assert 'running' in p._error()
        with d.PassPipeline() as other:
            expect(RuntimeError,lambda:other.run(c.target))
        expect(RuntimeError,lambda:p.run(c.target))
        expect(ValueError,lambda:p.add('dce'))
        raise ValueError('deliberate callback failure')
    p.register('failure',failure).add('failure').add('dce')
    try:p.run(l)
    except d.PipelineError as error:
        assert len(error.report['runs'])==1 and isinstance(error.__cause__,ValueError)
        assert 'deliberate callback failure' in str(error)
    else:raise AssertionError('callback exception was swallowed')

with lift(g) as l,d.PassPipeline(['instcombine']) as p:
    r=p.run(l,max_iterations=4);assert r['ok'] and r['converged']
    assert len(r['runs'])<=4
    r=p.run(l,max_pass_runs=0);assert r['budget_exhausted'] and not r['runs']
    expect(ValueError,lambda:p.run(l,max_iterations=0))
    expect(ValueError,lambda:p.run(l,max_pass_runs=4097))

with lift(g) as l,d.PassPipeline([('mba',{'max_queries':0}),('predicates',{'max_queries':0})]) as p:
    r=l.optimize(p)
    assert r['ok'] and all(x['counters']['solver_queries']==0 for x in r['runs'])

with lift(g) as l,d.PassPipeline() as p:
    p.register('invalid-result',lambda c,o:d.PassResult(counters={'n':True})).add('invalid-result')
    expect(d.PipelineError,lambda:p.run(l))

old_function=g.get_function(0x1000);old_block=g.get_ignil_block(0x1000)
with d.PassPipeline(['graph-audit']) as p:
    r=p.run(g);assert r['converged'] and r['runs'][0]['counters']['unlifted']==0
expect(ReferenceError,lambda:old_function.is_valid)
expect(ReferenceError,lambda:old_block.size)
assert g.get_function(0x1000).is_valid
with d.PassPipeline(builtins=False) as p:
    def count(c,o):
        expect(RuntimeError,c.target.__del__)
        return d.PassResult(counters={'n':len(c.target)})
    p.register('count',count,stage=d.PassStage.GRAPH,read_only=True).add('count')
    assert p.run(g)['runs'][0]['counters']['n']==len(g)

p=d.PassPipeline();ref=weakref.ref(p);p.close();del p;gc.collect();assert ref() is None

with lift(g) as l:
    raw=d.lib.dragon_pass_pipeline_create(0)
    @d.ffi.callback('dragon_pass_callback_t')
    def no_result(target,revision,options,userdata,out):pass
    assert d.lib.dragon_pass_pipeline_register(raw,b'no-result',d.PassStage.LLVM,no_result,d.ffi.NULL,1,b'[]')
    assert d.lib.dragon_pass_pipeline_add(raw,b'no-result',b'{}')
    try:
        report=json.loads(d.PassPipeline._text(d.lib.dragon_pass_pipeline_run_llvm(raw,l._ptr,1,64,1)))
        assert not report['ok'] and report['runs'][0]['status']==d.PassStatus.ERROR
    finally:d.lib.dragon_pass_pipeline_destroy(raw)

class BrokenMessage(Exception):
    def __str__(self):raise RuntimeError('exception formatting failed')
for failure in (BrokenMessage(),ValueError('\ud800')):
    with lift(g) as l,d.PassPipeline() as p:
        def fail(c,o):raise failure
        p.register('bad-exception-message',fail).add('bad-exception-message')
        expect(d.PipelineError,lambda:p.run(l))

with d.PassPipeline(['graph-audit']) as p:
    fresh=d.Disassembler().analyze(bytes.fromhex('c3'),0x1000,graph_pipeline=p)
    assert fresh.get_function(0x1000).is_valid
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'scripts/validation'))
from function_image_fixture import make_image
fixture=next(f for f in json.loads((root/'tests/fixtures/llvm-guest-flow.json').read_text())['cases'] if f['id']=='x86_64_image_elf')
with d.LoadedImage.load_bytes(make_image(fixture,0x1000,root)) as image,d.PassPipeline(['graph-audit']) as p:
    assert image.disassemble(graph_pipeline=p).get_function(0x1000).is_valid
with d.PassPipeline(builtins=False) as p:
    captured=[]
    def fail_graph(c,o):captured.append(c.target);raise ValueError('discard this graph')
    p.register('fail-graph',fail_graph,stage=d.PassStage.GRAPH).add('fail-graph')
    expect(d.PipelineError,lambda:d.Disassembler().analyze(bytes.fromhex('c3'),0x1000,graph_pipeline=p))
    assert captured[0]._ptr is None
assert d.ValueSetKind.FINITE==d.lib.DRAGON_VALUE_FINITE==1
live=g.get_function(0x1000)
facts=g.finite_values(entry_points=[0x1000],query_register=(0x1000,0,64))
assert facts['complete'] and facts['values'][0]['values']==[7]
assert live.is_valid
with d.PassPipeline(['finite-values']) as p:
    assert not p.run(g)['changed']
expect(ValueError,lambda:g.finite_values(query_register=(0x1000,0,65)))
expect(ValueError,lambda:g.finite_values(entry_points=[True]))
expect(ValueError,lambda:g.finite_values(max_values=True))

payload=bytearray(b'\x90'*0x31)
payload[:7]=bytes.fromhex('b810100000ffe0')
payload[0x10:0x17]=bytes.fromhex('b820100000ebee')
payload[0x20:0x27]=bytes.fromhex('b830100000ebde')
payload[0x30]=0xc3
for strategy in (d.Strategy.LINEAR_SWEEP,d.Strategy.RECURSIVE_TRAVERSAL):
    code=bytes(payload)
    target=d.Disassembler(config=d.Configuration(strategy=strategy)).analyze(code,0x1000)
    with d.PassPipeline() as p:
        owned=bytearray(code)
        p.register_finite_dispatch(owned,0x1000,entry_points=[0x1000]).add('finite-dispatch')
        owned[:]=b'\xcc'*len(owned)
        del owned
        expect(ValueError,lambda:p.add('finite-dispatch',max_values=257))
        expect(ValueError,lambda:p.add('finite-dispatch',max_rounds=True))
        assert p.run(target)['changed']
        assert target.get_instruction(0x1005).branches==[0x1010,0x1020,0x1030]
        assert not p.run(target)['changed']
    with d.PassPipeline() as p:
        expect(ValueError,lambda:p.register_finite_dispatch(b'',0x1000))
print('Pass pipeline: configuration, callbacks, cache, budgets and lifetime checks passed')

with lift(g) as l,d.PassPipeline(['guarded-state-values']) as p:
    assert 'guarded-state-values' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-state-values',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['available']==0

with lift(g) as l,d.PassPipeline(['guarded-partial-masks']) as p:
    assert 'guarded-partial-masks' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-partial-masks',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['available']==0

with lift(g) as l,d.PassPipeline(['guarded-checked-corridors']) as p:
    assert 'guarded-checked-corridors' not in d.PassPipeline.DEFAULT_LLVM
    for option,limit in (('max_candidates',4096),('max_rewrites',4096),
                         ('max_blocks',4096),('max_instructions',262144)):
        expect(ValueError,lambda:p.add('guarded-checked-corridors',**{option:limit+1}))
    before=l.ir();r=p.run(l)
    assert r['ok'] and not r['changed'] and l.ir()==before
    assert r['runs'][0]['status']==d.PassStatus.UNAVAILABLE
    assert r['runs'][0]['counters']['available']==0
