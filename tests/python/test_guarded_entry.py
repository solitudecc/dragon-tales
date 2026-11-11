"""Guarded view C/Python ownership and validation; no native execution claim."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'python'))
import dragon as d

def rejects(error, action):
    try: action()
    except error: return
    raise AssertionError('expected '+error.__name__)

facts=d.FiniteEntryState(0x1000,registers=[(32,64,0xc7f8)],memory=[(0x4000,bytes.fromhex('4433221100000000'))])
code=bytes.fromhex('488b042500400000c3')
with d.Disassembler(d.Architecture.X86_64).analyze(code,0x1000) as graph:
    with d.LLVMLifter(d.Architecture.X86_64,execution_mode=2,static_entry_policy=1) as source:
        assert source.lift(graph.get_function(0x1000),graph)
        before=source.ir()
        assert source.guarded_entry_manifest()=={}
        for kwargs,error in [({'context_id':'','provenance':'p'},ValueError),
                             ({'context_id':'a\0b','provenance':'p'},ValueError),
                             ({'context_id':'bad space','provenance':'p'},ValueError),
                             ({'context_id':'a','provenance':''},ValueError),
                             ({'context_id':'a','provenance':'x\0y'},ValueError),
                             ({'context_id':1,'provenance':'p'},TypeError),
                             ({'context_id':'a','provenance':False},TypeError)]:
            rejects(error,lambda:source.guarded_entry_view(facts,**kwargs))
            assert source.ir()==before
        rejects(TypeError,lambda:source.guarded_entry_view({},context_id='a',provenance='p'))
        for bad in [d.FiniteEntryState(0x1000),
                    d.FiniteEntryState(0x1000,memory=[(0x4000,b'a'),(0x4000,b'a')]),
                    d.FiniteEntryState(0x1000,memory=[(0x4000,b'a'*4097)]),
                    d.FiniteEntryState(0x1000,memory=[(0x4000+i,b'a') for i in range(257)])]:
            rejects(ValueError,lambda:source.guarded_entry_view(bad,context_id='a',provenance='p'))
            assert source.ir()==before
        view=source.guarded_entry_view(facts,context_id='python',provenance='Python binding fixture')
        manifest=view.guarded_entry_manifest()
        assert manifest['view_symbol']=='dragon_static_entry_x86_64_1000.guarded.python'
        assert manifest['entry']=='0x1000'
        assert manifest['check_kind']=='runtime-byte-equality'
        assert manifest['registers']==[{'offset':'0x20','bytes':'f8c7000000000000'}]
        assert manifest['memory']==[{'offset':'0x4000','bytes':'4433221100000000'}]
        assert manifest['minimum_register_bytes']=='40'
        assert manifest['minimum_guest_bytes']=='16392'
        with d.PassPipeline() as pipeline:
            def callback(context, options):
                rejects(RuntimeError, lambda:context.target.guarded_entry_view(
                    facts,context_id='reentrant',provenance='callback'))
            pipeline.register('check-reentry',callback,read_only=True).add('check-reentry')
            assert pipeline.run(source)['ok']
        assert source.ir()==before
    rejects(ReferenceError,lambda:source.guarded_entry_view(facts,context_id='a',provenance='p'))
    with view:
        assert view.guarded_entry_manifest()==manifest
        with d.PassPipeline(['mem2reg','instcombine','gvn']) as pipeline:
            assert pipeline.run(view)['ok']
        assert '.guarded.python' in view.ir()
    rejects(ReferenceError,view.guarded_entry_manifest)
print('Guarded entry: strict inputs, independent lifetime, source preservation and pipeline routing passed')
