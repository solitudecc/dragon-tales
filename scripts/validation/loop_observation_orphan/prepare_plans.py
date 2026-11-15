"""Freeze the isolated orphan policy test. Metadata only, no compiler/probe."""
from pathlib import Path
import hashlib,json,sys
sys.dont_write_bytecode=True
def sha(p):
    h=hashlib.sha256()
    with Path(p).open('rb') as f:
        for b in iter(lambda:f.read(1048576),b''):h.update(b)
    return h.hexdigest()
def main():
    root=Path(sys.argv[1]).resolve()
    common=Path('/home/ubuntu/vmc-runs/loop-observation-cache-transactions-20261115-v2')
    frozen=common/'frozen-v1'
    framework=common/'source-v2/observation_cache_probe.cpp'
    assert sha(framework)=='cbddfd3302630a0573a5f9efedf0466fc6ecf4f659944d7df7087d9e42f6ac00'
    assert sha(common/'FILESET.json')=='742dd53ed6cca98df9fe39bbe8eed5b7e5c231bd28843b59e132e54f1ef320dc'
    assert sha(common/'CLOSURE.json')=='54fd1fa90509380378909f1550871f0498b31f9a953446f2c22bbb191c235c1c'
    sdk=Path('/home/ubuntu/vmc-runs/loop-image-phi-regions-sdk-20261115-v1')
    library=sdk/'lib/libdragon.so'
    assert sha(library)=='b3483229d516183c0a323319fa4d7b6c407fa1c33f1fae0b7ca6c79560431291'
    z3=Path('/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13')
    assert sha(z3)=='4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a'
    compiler=Path('/usr/bin/clang++-21')
    files=[p for p in root.rglob('*') if p.is_file()]
    files += [p for p in frozen.rglob('*.hpp') if p.is_file()]
    files += [frozen/'include/dragon/lifters/llvm/Execution.h',framework,common/'FILESET.json',common/'CLOSURE.json',
              library,sdk/'SDK-FROZEN.json',z3,compiler]
    pins={str(p):sha(p) for p in files}
    (root/'plans').mkdir(exist_ok=False)
    plans={}
    for mode,overlay in [('good','LLVMImageTableContext.overlay.cpp'),('mutant','LLVMImageTableContext.no-all-node-coverage.cpp')]:
        exe=root/('orphan-tests-'+mode)
        compile=['/usr/bin/taskset','-c','4-7',str(compiler),'-O1','-g0','-DNDEBUG','-std=c++20',
          '-I'+str(frozen),'-I'+str(frozen/'include'),'-I/usr/lib/llvm-21/include',
          '-DDRAGON_OBSERVATION_FRAMEWORK="'+str(framework)+'"',
          '-DDRAGON_OBSERVATION_IMAGE_SOURCE="'+str(root/'overlay'/overlay)+'"',
          str(root/'source-v1/orphan_coverage_probe.cpp'),str(library),'-Wl,-rpath,'+str(library.parent),
          '-Wl,-rpath-link,'+str(z3.parent),'-pthread','-ldl','-o',str(exe)]
        plan=dict(schema='dragon-tigress-context-guard-plan-v1',
          limits=dict(seconds=180,start_free=536870912,running_floor=67108864),pins=pins,
          executable=str(exe),library_path=[str(library.parent),str(z3.parent)],
          stages=dict(compile=dict(argv=compile),probe=dict(argv=['/usr/bin/taskset','-c','4-7',str(exe)])),
          root_GO='isolated orphan coverage control CPU4-7 only after bit releases NZbuild; no concurrent ctest; 180/512/64',
          scope=dict(logical_scenarios=1,assertions=8,expected_exit=0 if mode=='good' else 1,
            expected_mutant_failed_checks=2,source_inclusion=True,installed_Image=False,Guest_native=0,
            original_D_source='2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8',
            only_fault='mutant removes !covered refusal only; still spends per collected node'))
        path=root/'plans'/(mode+'.json');path.write_text(json.dumps(plan,indent=2,sort_keys=True)+'\n')
        plans[mode]=dict(path=str(path),sha256=sha(path))
    ready=root/'READY.json'
    ready.write_text(json.dumps(dict(schema='orphan-policy-ready-v1',plans=plans,pins=pins,
      target_calls=0,compiled=False),indent=2,sort_keys=True)+'\n')
    print(json.dumps(dict(ready=str(ready),sha256=sha(ready),plans=plans),indent=2))
if __name__=='__main__':main()
