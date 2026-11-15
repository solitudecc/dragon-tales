"""Freeze narrow original-TU accounting probes against one immutable helper SDK."""
from pathlib import Path
import hashlib
import json
import shutil
import sys

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1048576),b''): h.update(block)
    return h.hexdigest()

def main():
    root=Path(sys.argv[1]).resolve()
    sdk=Path('/home/ubuntu/vmc-runs/loop-image-variable-shift-sdk-20261115-v1')
    stable=Path('/home/ubuntu/vmc-runs/loop-image-observation-range-milestone-20261115-v1/source-tree')
    library=sdk/'lib/libdragon.so'
    assert sha(library)=='777a373f959f3708bd0f8257de9c0181fea2949f32ad157e74f345b228521c38'
    assert sha(sdk/'SDK-FROZEN.json')=='58bfc4d9258f93f0690578ce433838479178154a74ac78a3107818bc1ac2ff3b'
    z3=Path('/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13')
    assert sha(z3)=='4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a'
    frozen=root/'frozen-v1'
    copied={}
    for name in ['LLVMImageTableContext.hpp','LLVMPhiLowBits.hpp','LLVMPathValueRanges.hpp',
                 'LLVMPhiConstant.hpp','LLVMOpaqueIntegerAdmission.hpp']:
        path=sdk/'source/lib'/name
        if not path.exists(): path=stable/'lib'/name
        target=frozen/name
        assert not target.exists()
        shutil.copyfile(path,target)
        copied[str(target.relative_to(root))]=dict(source=str(path),size=path.stat().st_size,sha256=sha(path))
        assert sha(target)==sha(path)
    for name in ['ImageTableContext.hpp','DispatchCorridor.hpp','Execution.h']:
        rel=Path('include/dragon/lifters/llvm')/name
        path=sdk/'source'/rel
        if not path.exists(): path=stable/rel
        target=frozen/rel
        target.parent.mkdir(parents=True,exist_ok=True)
        assert not target.exists()
        shutil.copyfile(path,target)
        copied[str(target.relative_to(root))]=dict(source=str(path),size=path.stat().st_size,sha256=sha(path))
        assert sha(target)==sha(path)
    (frozen/'HEADER-PINS.json').write_text(json.dumps(copied,indent=2,sort_keys=True)+'\n')
    source=root/'budget_probe.cpp'
    compiler=Path('/usr/bin/clang++-21')
    original=[root/('LLVMImageTableContext.'+mode+'.cpp') for mode in ['b6','df','opaque']]
    payload=[x for x in frozen.rglob('*') if x.is_file()]+original+[source,root/'guard.py',root/'prepare_plans.py',root/'prepare_overlay.py']
    assert all(not x.is_symlink() for x in payload)
    pins={str(x):sha(x) for x in payload+[library,sdk/'SDK-FROZEN.json',z3,compiler,compiler.resolve()]}
    (root/'plans').mkdir(exist_ok=False)
    manifests={}
    for mode in ['b6','df','opaque']:
        executable=root/('budget-tests-'+mode)
        overlay=frozen/('LLVMImageTableContext.'+mode+'.overlay.cpp')
        compile=['/usr/bin/taskset','-c','0-3',str(compiler),'-O1','-g0','-DNDEBUG','-std=c++20',
          '-I'+str(frozen),'-I'+str(frozen/'include'),'-I/usr/lib/llvm-21/include',
          '-DDRAGON_BUDGET_IMAGE_SOURCE="'+str(overlay)+'"',
          '-DDRAGON_BUDGET_VARIANT="'+mode+'"',str(source),str(library),
          '-Wl,-rpath,'+str(library.parent),'-Wl,-rpath-link,'+str(z3.parent),
          '-pthread','-ldl','-o',str(executable)]
        plan=dict(schema='dragon-tigress-context-guard-plan-v1',
          limits=dict(seconds=180,start_free=536870912,running_floor=67108864),pins=pins,
          library_path=[str(library.parent),str(z3.parent)],executable=str(executable),
          stages=dict(compile=dict(argv=compile),probe=dict(argv=['/usr/bin/taskset','-c','0-3',str(executable),str(root/('fixtures-'+mode))])),
          scope=dict(mode=mode,source_inclusion_actual_LoopRanges=True,guest_native=0,installed_Image_execution=False,
                     C_or_UC=0,actual_production_source_unchanged=True,
                     expected_probe_exit=0 if mode=='opaque' else 1,
                     new_positive_failures_expected=4 if mode=='b6' else (1 if mode=='df' else 0),
                     budget_contract='128 nodes, depth32, Work1M; per-query known/complete-root footprint accounting policy only',
                     no_optimizers=True,no_publication_or_mutation_hooks=True),
          compiler=dict(logical=str(compiler),resolved=str(compiler.resolve())),
          root_GO='Original full TU friend/rename-only unit, fixed777 helpers; CPU0-3 singlejob 180/512/64; no installed Image, no native, no private cache-transaction expansion')
        path=root/'plans'/(mode+'.json')
        path.write_text(json.dumps(plan,indent=2,sort_keys=True)+'\n')
        manifests[mode]=dict(path=str(path),sha256=sha(path),expected_probe_exit=plan['scope']['expected_probe_exit'])
    ready=dict(schema='original-LoopRanges-budget-policy-ready-v1',plans=manifests,
       payload={str(x.relative_to(root)):dict(size=x.stat().st_size,sha256=sha(x)) for x in payload},
       compiler_sha256=sha(compiler),runtime_sha256=sha(library),z3_sha256=sha(z3),
       sources={mode:sha(root/('LLVMImageTableContext.'+mode+'.cpp')) for mode in ['b6','df','opaque']},
       guest_native=0,installed_Image_execution=False,compiled=False,target_calls=0)
    path=root/'READY.json';path.write_text(json.dumps(ready,indent=2,sort_keys=True)+'\n')
    print(json.dumps(dict(ready=str(path),sha256=sha(path),plans=manifests),indent=2,sort_keys=True))

if __name__=='__main__':main()
