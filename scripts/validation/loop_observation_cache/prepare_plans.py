"""Metadata only: freeze exact source overlays, tool/runtime pins and commands."""
from pathlib import Path
import hashlib
import json
import sys

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1048576),b''): h.update(block)
    return h.hexdigest()

def main():
    root=Path(sys.argv[1]).resolve()
    sdk=Path('/home/ubuntu/vmc-runs/loop-image-phi-regions-sdk-20261115-v1')
    library=sdk/'lib/libdragon.so'
    assert sha(library)=='b3483229d516183c0a323319fa4d7b6c407fa1c33f1fae0b7ca6c79560431291'
    assert sha(sdk/'SDK-FROZEN.json')=='e4a06791ed9b4ff7915cbc3e9f355de5c789d13aa11ce1d929e9f9c2c4804d7f'
    z3=Path('/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13')
    assert sha(z3)=='4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a'
    compiler=Path('/usr/bin/clang++-21')
    frozen=root/'frozen-v1'
    source=root/'source-v2/observation_cache_probe.cpp'
    payload=[x for x in frozen.rglob('*') if x.is_file()]+[source,root/'guard.py',root/'prepare_overlay.py',root/'prepare_plans.py']
    assert all(not x.is_symlink() for x in payload)
    pins={str(x):sha(x) for x in payload+[library,sdk/'SDK-FROZEN.json',z3,compiler]}
    modes={
      'good':('LLVMImageTableContext.overlay.cpp',True,[]),
      'mutant':('LLVMImageTableContext.no-parent-rollback.cpp',True,[]),
      'ordinary':('LLVMImageTableContext.overlay.cpp',False,['--ordinary'])}
    manifests={}
    (root/'plans').mkdir(exist_ok=False)
    for mode,(overlay,publication,args) in modes.items():
        executable=root/('cache-tests-'+mode)
        compile=['/usr/bin/taskset','-c','0-3',str(compiler),'-O1','-g0','-DNDEBUG','-std=c++20',
          '-I'+str(frozen),'-I'+str(frozen/'include'),'-I/usr/lib/llvm-21/include',
          '-DDRAGON_OBSERVATION_IMAGE_SOURCE="'+str(frozen/overlay)+'"']
        if publication: compile+=['-DDRAGON_LOOP_OBSERVATION_PUBLICATION_TESTING=1']
        compile += [str(source),str(library),'-Wl,-rpath,'+str(library.parent),
                    '-Wl,-rpath-link,'+str(z3.parent),'-pthread','-ldl','-o',str(executable)]
        plan={'schema':'dragon-tigress-context-guard-plan-v1','limits':dict(seconds=180,start_free=536870912,running_floor=67108864),
          'pins':pins,'library_path':[str(library.parent),str(z3.parent)],'executable':str(executable),
          'stages':{'compile':{'argv':compile},'probe':{'argv':['/usr/bin/taskset','-c','0-3',str(executable)]+args}},
          'scope':{'source_inclusion_actual_D':True,'installed_Image_execution':False,'guest_native':0,
            'C_or_UC':0,'mode':mode,'expected_exit':1 if mode=='mutant' else 0,
            'mutant_requires_failed_exact_cache_restoration':mode=='mutant',
            'test_budget_injection':'only after actual child publication: limit=stats.work; after asserting failure restore original1M ceiling without resetting spent counters',
            'ordinary':'same source body with publication macro off; budget-injection case intentionally absent, common real-query values/debts and full internal gates compared'},
          'compiler':{'logical':str(compiler),'resolved':str(compiler.resolve())},
          'root_GO':'independent source-inclusion unit only CPU0-3 180/512/64; b348 imported helpers; freeze first; no product edit/guest execution'}
        p=root/'plans'/(mode+'.json')
        p.write_text(json.dumps(plan,indent=2,sort_keys=True)+'\n')
        manifests[mode]={'path':str(p),'sha256':sha(p),'expected_probe_exit':plan['scope']['expected_exit']}
    out={'schema':'cache-transaction-stage-plan-set-v1','plans':manifests,
         'sources':{str(x.relative_to(root)):{'size':x.stat().st_size,'sha256':sha(x)} for x in payload},
         'baseline_source_sha256':sha(frozen/'LLVMImageTableContext.original.cpp'),
         'runtime_sha256':sha(library),'compiler_sha256':sha(compiler),'guest_native':0,
         'frozen':True,'compiled':False,'target_calls':0}
    p=root/'READY.json';p.write_text(json.dumps(out,indent=2,sort_keys=True)+'\n')
    print(json.dumps({'ready':str(p),'sha256':sha(p),'plans':manifests},indent=2))

if __name__=='__main__':main()
