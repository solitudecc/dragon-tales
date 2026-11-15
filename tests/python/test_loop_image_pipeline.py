"""Loop/function builtins and Python encoding; LLVM-only, no Guest execution claim."""
import re
import sys
from collections import UserDict
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'python'))
import dragon as d

U64=(1<<64)-1
LOOP_COUNTERS={'available','complete','closed','budget_exhausted','guarded_bytes',
               'preserved_stores','constant_values','work'}
FLOW_COUNTERS={name+'_'+suffix for name in ('blocks','instructions','conditional_branches','switches')
               for suffix in ('before','after')}

def rejects(error,action):
    try:action()
    except error:return
    raise AssertionError('expected '+error.__name__)

class Capture:
    """Observe the public helper's one add call without a native pipeline."""
    add_loop_image_context=d.PassPipeline.add_loop_image_context
    def __init__(self):self.calls=[]
    def _check(self):pass
    def add(self,name,**options):self.calls.append((name,options));return self

capture=Capture()
assert capture.add_loop_image_context('view',1<<63,(x for x in [U64,0,1<<63,U64])) is capture
assert capture.calls==[('loop-image-context',dict(view_symbol='view',table_offset='0x8000000000000000',
    targets='0xffffffffffffffff,0x0,0x8000000000000000,0xffffffffffffffff',
    max_blocks=4096,max_instructions=65536,max_work=1000000))]
for symbol in ['x','x'*4096,'é'*2048,'😀'*1024]:
    capture.add_loop_image_context(symbol,U64-4096,[U64]*512,max_blocks=1,max_instructions=1,max_work=1)
    assert capture.calls[-1][1]['view_symbol']==symbol
    assert capture.calls[-1][1]['targets'].split(',')==['0xffffffffffffffff']*512
    assert capture.calls[-1][1]['table_offset']==hex(U64-4096)

bad_inputs=[
    (TypeError,dict(view_symbol=None)),(TypeError,dict(view_symbol=1)),
    (ValueError,dict(view_symbol='')),(ValueError,dict(view_symbol='a\0b')),
    (ValueError,dict(view_symbol='x'*4097)),(ValueError,dict(view_symbol='é'*2049)),
    (ValueError,dict(view_symbol='\ud800')),
    *[(ValueError,dict(table_offset=x)) for x in [-1,U64+1,True,False,1.5,'0x4000',None]],
    *[(TypeError,dict(targets=x)) for x in ['1',b'\x01',bytearray([1]),memoryview(b'1'),
        {1:2},UserDict({1:2}),{1,2},frozenset({1}),{1:2}.keys(),None,7]],
    (ValueError,dict(targets=[])),(ValueError,dict(targets=range(513))),
    *[(ValueError,dict(targets=[x])) for x in [-1,U64+1,True,False,1.5,'1',None]],
    (ValueError,dict(table_offset=U64-7)),
    (ValueError,dict(table_offset=U64-4095,targets=[0]*512)),
]
for option,cap in [('max_blocks',4096),('max_instructions',65536),('max_work',1000000)]:
    bad_inputs.extend((ValueError,{option:x}) for x in [0,-1,cap+1,True,'1',None])
for error,changes in bad_inputs:
    args=dict(view_symbol='view',table_offset=0x4000,targets=[0x1020]);args.update(changes)
    before=list(capture.calls);rejects(error,lambda:capture.add_loop_image_context(**args));assert capture.calls==before

class Infinite:
    def __init__(self):self.reads=0
    def __iter__(self):return self
    def __next__(self):
        self.reads+=1
        assert self.reads<=513,'helper read past its bounded prefix'
        return 0

stream=Infinite();before=list(capture.calls)
rejects(ValueError,lambda:capture.add_loop_image_context('view',0,stream))
assert stream.reads==513 and capture.calls==before
def raises_midway():
    yield 1
    raise RuntimeError('deliberate iterator failure')
rejects(RuntimeError,lambda:capture.add_loop_image_context('view',0,raises_midway()))
assert capture.calls==before

# Native configuration failures must not leave an extra queued step.
with d.PassPipeline(['mem2reg']) as pipeline,d.LLVMLifter() as legacy:
    raw=dict(view_symbol='missing',table_offset='0x4000',targets='0x1020')
    for changes in [dict(view_symbol=''),dict(view_symbol='bad\0name'),dict(view_symbol=1),
                    dict(table_offset=-1),dict(table_offset=True),dict(table_offset=1.5),
                    dict(table_offset='18446744073709551616'),dict(table_offset='-1'),
                    dict(table_offset=hex(U64-7)),dict(targets=''),dict(targets='1,,2'),
                    dict(targets='0x10000000000000000'),dict(targets='-1'),dict(targets=[1,2]),
                    dict(targets=','.join('0' for _ in range(513))),dict(unknown=1)]:
        options=dict(raw);options.update(changes)
        rejects(ValueError,lambda:pipeline.add('loop-image-context',**options))
    for name,cap in [('max_blocks',4096),('max_instructions',65536),('max_work',1000000)]:
        for bad in [0,-1,cap+1,True,'1']:
            options=dict(raw);options[name]=bad
            rejects(ValueError,lambda:pipeline.add('loop-image-context',**options))
    for error,changes in bad_inputs:
        args=dict(view_symbol='view',table_offset=0x4000,targets=[0x1020]);args.update(changes)
        rejects(error,lambda:pipeline.add_loop_image_context(**args))
    for options in [{},{'function':''},{'function':'bad\0name'},{'function':False},{'function':'x','unknown':1}]:
        rejects(ValueError,lambda:pipeline.add('function-control-flow',**options))
    report=pipeline.run(legacy)
    assert report['ok'] and len(report['runs'])==1 and report['runs'][0]['name']=='mem2reg'

closed=d.PassPipeline();closed.close();stream=Infinite()
rejects(ReferenceError,lambda:closed.add_loop_image_context('view',0,stream));assert stream.reads==0

# A full-width ordered high-bit CSV is accepted through the actual C JSON API.
with d.PassPipeline() as pipeline,d.LLVMLifter() as legacy:
    assert 'loop-image-context' not in d.PassPipeline.DEFAULT_LLVM
    assert 'function-control-flow' not in d.PassPipeline.DEFAULT_LLVM
    assert pipeline.add_loop_image_context('é'*2048,U64-4096,[U64]*512) is pipeline
    before=legacy.ir();report=pipeline.run(legacy);run=report['runs'][0]
    assert report['ok'] and not report['changed'] and legacy.ir()==before
    assert run['status']==d.PassStatus.UNAVAILABLE
    assert set(run['counters'])==LOOP_COUNTERS and all(v==0 for v in run['counters'].values())

def body(ir,symbol):
    found=re.search(r'^define\b[^\n]*@'+re.escape(symbol)+r'\([^\n]*\).*?^}',ir,re.M|re.S)
    assert found is not None,symbol
    return found.group(0)

ENTRY=0x1000;HEAD=0x1005;LOOKUP=0x100b;TARGETS=[0x1020,0x1030];TABLE=0x4000
# Independent finite program: mov ecx,0; cmp rcx,2; jae outside RET; table jump.
# Case A selects state 1 and returns to head. Case B returns EAX=7.
# The outside-bound RET is retained, with no artificial state Phi self-incoming.
code=bytearray(b'\x90'*0x36)
code[:0x12]=bytes.fromhex('b9000000004883f902730dff24cd00400000')
code[0x20:0x27]=bytes.fromhex('b901000000ebde')
code[0x30:0x36]=bytes.fromhex('b807000000c3')
code[0x18:0x19]=bytes.fromhex('c3')
leaders=[ENTRY,HEAD,LOOKUP,0x1018,*TARGETS]
table_bytes=b''.join(x.to_bytes(8,'little') for x in TARGETS)
# Decoded-region coverage is explicit. No image/trusted-memory reader supplies
# a target value; the only table-byte assertion is an executable entry guard.
with d.AnalysisHints(entry_points=leaders) as hints:
    dis=d.Disassembler(d.Architecture.X86_64,d.Configuration(strategy=d.Strategy.LINEAR_SWEEP))
    with dis.analyze(bytes(code),ENTRY,hints=hints) as graph:
        assert graph.get_instruction(TARGETS[0]).address==TARGETS[0]
        assert graph.get_instruction(TARGETS[1]).address==TARGETS[1]
        with d.LLVMLifter(d.Architecture.X86_64,execution_mode=d.LLVMExecutionMode.STATIC_ANALYSIS,
                         static_entry_policy=d.LLVMStaticEntryPolicy.FUNCTION_START) as source:
            assert source.lift_static_region(graph.get_function(ENTRY),graph,leaders,
                max_blocks=128,rebuild_decoded_blocks=True)
            source_before=source.ir()
            facts=d.FiniteEntryState(ENTRY,registers=[(32,64,0xc000)],memory=[(TABLE,table_bytes)])
            with source.guarded_entry_view(facts,context_id='loop_pipeline',provenance='Python checked-table pipeline fixture') as view:
                symbol=view.guarded_entry_manifest()['view_symbol']
                ordinary='dragon_static_entry_x86_64_1000';original=view.ir();ordinary_before=body(original,ordinary)
                # Missing symbol and a mismatched candidate table are precision refusals, not errors.
                for requested,targets,options in [('not-a-view',TARGETS,{}),(symbol,[TARGETS[0],TARGETS[0]],{}),
                                                   (symbol,TARGETS,{'max_work':1})]:
                    with d.PassPipeline() as pipeline:
                        pipeline.add_loop_image_context(requested,TABLE,targets,**options)
                        report=pipeline.run(view);run=report['runs'][0];counters=run['counters']
                        assert report['ok'] and not report['changed'] and view.ir()==original,report
                        assert run['status']==d.PassStatus.INCOMPLETE and run['message'],report
                        assert set(counters)==LOOP_COUNTERS and counters['available']==1
                        assert counters['closed']==counters['complete']==0
                        if options:assert counters['budget_exhausted']==1
                with d.PassPipeline() as pipeline:
                    pipeline.add_loop_image_context(symbol,TABLE,TARGETS)
                    report=pipeline.run(view);run=report['runs'][0];counters=run['counters']
                    assert report['ok'] and report['changed'] and run['status']==d.PassStatus.SUCCESS,report
                    assert set(counters)==LOOP_COUNTERS and counters['available']==counters['closed']==counters['complete']==1
                    assert not counters['budget_exhausted'] and 0<counters['work']<=1000000
                    assert counters['guarded_bytes']==16 and counters['constant_values']==0
                    assert body(view.ir(),ordinary)==ordinary_before and source.ir()==source_before
                # Cleanup selects only this Function; ordinary/root ownership stays exact.
                with d.PassPipeline() as pipeline:
                    pipeline.add('function-control-flow',function=symbol)
                    report=pipeline.run(view);run=report['runs'][0]
                    assert report['ok'] and run['status']==d.PassStatus.SUCCESS,report
                    assert set(run['counters'])==FLOW_COUNTERS
                    assert body(view.ir(),ordinary)==ordinary_before and source.ir()==source_before
                with d.PassPipeline() as pipeline:
                    pipeline.add('function-control-flow',function='absent')
                    before=view.ir()
                    try:pipeline.run(view)
                    except d.PipelineError as error:
                        assert error.report['runs'][0]['status']==d.PassStatus.ERROR and error.report['changed'] is True
                        assert 'requires a defined function' in error.report['error'] and 'absent' in error.report['error']
                    else:raise AssertionError('missing cleanup Function was accepted')
                    assert view.ir()==before

print('Loop pipeline: bounded ordered uint64 encoding, atomic validation, Legacy/refusal/checked closure and function-only cleanup passed')
