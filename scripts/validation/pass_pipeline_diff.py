"""Exercise both pass stages and a class-based plugin against full Guest state.

Reuses the strict static-entry oracle: normal/interior execution uses Unicorn;
rejected PCs use the explicit no-execution contract. No new exclusions. Modes
cover the compatible default, bounded repetition, and registered custom passes.
The oracle's result/memory controls remain available and must detect corruption.
"""
import argparse,json,sys
from pathlib import Path
import static_entry_diff as study
d=study.base.dragon

class FunctionCount:
    def __call__(self,ctx,options):
        functions=tuple(ctx.target.function_addresses())
        ctx.analyses['functions']=functions
        return d.PassResult(counters={'functions':len(functions)})

class CachedFunctionCount:
    def __call__(self,ctx,options):
        assert 'functions' in ctx.analyses
        return d.PassResult(counters={'cache_hit':1})

class ObserveIR:
    def __call__(self,ctx,options):
        ctx.analyses['size']=len(ctx.target.ir())
        return d.PassResult(counters={'ir_bytes':ctx.analyses['size']})

class Cleanup:
    def __call__(self,ctx,options):
        assert 'size' in ctx.analyses
        ctx.target.run_inst_combine()

class FreshAnalysis:
    def __call__(self,ctx,options):
        assert 'size' not in ctx.analyses
        return d.PassResult(counters={'cache_invalidated':1})

def main():
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument('--pipeline-mode',choices=('default','repeat','custom'),default='custom')
    mode,remaining=parser.parse_known_args();sys.argv=[sys.argv[0],*remaining]
    out=Path(remaining[remaining.index('--out')+1]).expanduser();out.mkdir(parents=True,exist_ok=True)
    reports=[]
    analyze=d.Disassembler.analyze;image=d.LoadedImage.disassemble;optimize=d.LLVMLifter.optimize
    def audit(graph):
        with d.PassPipeline(['graph-audit']) as p:
            p.register('functions',FunctionCount(),stage=d.PassStage.GRAPH,read_only=True)
            p.register('cached-functions',CachedFunctionCount(),stage=d.PassStage.GRAPH,read_only=True,requires=('functions',))
            p.add('functions').add('cached-functions');r=p.run(graph)
            assert r['ok'] and not r['changed'] and r['runs'][-1]['counters']['cache_hit']==1
            reports.append({'stage':'graph',**r})
        return graph
    def analyze_with_passes(self,*args,**kwargs):return audit(analyze(self,*args,**kwargs))
    def image_with_passes(self,*args,**kwargs):return audit(image(self,*args,**kwargs))
    def optimize_with_passes(self,pipeline=None):
        if pipeline is not None:return optimize(self,pipeline)
        if mode.pipeline_mode=='default':return optimize(self)
        with d.PassPipeline.llvm_default() as p:
            if mode.pipeline_mode=='custom':
                p.register('observe',ObserveIR(),read_only=True).register('cleanup-plugin',Cleanup())
                p.register('fresh-analysis',FreshAnalysis(),read_only=True,requires=('cleanup-plugin',))
                p.add('observe').add('cleanup-plugin').add('fresh-analysis')
            r=p.run(self,max_iterations=3,max_pass_runs=32)
            assert r['ok'] and len(r['runs'])<=32
            if mode.pipeline_mode=='custom':assert any(x['counters'].get('cache_invalidated')==1 for x in r['runs'])
            reports.append({'stage':'llvm',**r})
    d.Disassembler.analyze=analyze_with_passes;d.LoadedImage.disassemble=image_with_passes
    d.LLVMLifter.optimize=optimize_with_passes
    try:return study.main()
    finally:
        d.Disassembler.analyze=analyze;d.LoadedImage.disassemble=image;d.LLVMLifter.optimize=optimize
        (out/'pass-reports.json').write_text(json.dumps({'mode':mode.pipeline_mode,'reports':reports},indent=2))

if __name__=='__main__':raise SystemExit(main())
