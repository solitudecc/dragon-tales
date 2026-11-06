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
    assert names==set(d.PassPipeline.DEFAULT_LLVM)|{'graph-audit'}
    for name,opts in [('bad',{}),('mba',{'max_queries':True}),('mba',{'max_queries':-1}),('mba',{'unknown':1})]:
        expect(ValueError,lambda:p.add(name,**opts))
    p.add('instcombine').add('graph-audit');before=l.ir()
    expect(d.PipelineError,lambda:p.run(l));assert l.ir()==before
    p.clear()
    p.register('dependent',lambda c,o:None,read_only=True,requires=('dce',)).add('dependent')
    expect(d.PipelineError,lambda:p.run(l));assert l.ir()==before
    expect(ValueError,lambda:p.register('dependent',lambda c,o:None))
    expect(ValueError,lambda:p.register('wrong',lambda c,o:None,stage=7))

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
print('Pass pipeline: configuration, callbacks, cache, budgets and lifetime checks passed')
