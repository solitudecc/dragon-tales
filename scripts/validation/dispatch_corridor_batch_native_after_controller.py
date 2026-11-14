"""Installed main/No-Z3 Batch API replay after the frozen before/fault gate.

One new namespace per backend; fixed CPU0-3/180s/512MiB-64MiB guards.
Does not alter the frozen source/model/runner/producer or before namespace.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

BEFORE = Path('/home/ubuntu/vmc-runs/dispatch-corridor-batch-native-20261115-v1')
REPO = Path('/home/ubuntu/dragon-tales-rock5b')
PYTHON = Path('/home/ubuntu/vmc-runs/rock5b-ucdev-reference-20261114-v1/venv/bin/python')
SOLVER = Path('/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13')
BACKENDS = {
 'main': (REPO/'build/lib/libdragon.so','4a89d8a97d996e6423cb2a670924cee21714623bc28e74378d25bd93c0f3b080',SOLVER),
 'noz3': (REPO/'build-noz3/lib/libdragon.so','11f6046d353ae35e936c428a1e0007132df20d22f32a8fb26aa572e7fee589e0',None),
}
EXPECTED = {
 str(REPO/'lib/LLVMDispatchCorridor.cpp'):'81f40177a93da64362e6600b78eda701295ca7ab035db42d6c84fd81fed30e29',
 str(REPO/'include/dragon/lifters/llvm/DispatchCorridor.hpp'):'2f7cc6080b148ede586859b6a5fad60eef76272e72d1362d771ee881f3a2c467',
 str(REPO/'include/dragon/lifters/llvm/Execution.h'):'b0674f3684a1393a89c06b00beee6402eaa20e388f418491b7c1713016b1b560',
 str(BEFORE/'source/scripts/validation/dispatch_corridor_batch_native_source.py'):'ed67354cbf31feaf5e4429d827cd43c11b1d8460f3c8f830cf97fb9147ade3e4',
 str(BEFORE/'source/scripts/validation/dispatch_corridor_batch_native.py'):'f282c2acdd3d08045d094742f946f1b344e9a4fedbfe85d49df8d19d884673b5',
 str(BEFORE/'source/scripts/validation/dispatch_corridor_batch_native_probe.cpp'):'d7e4e066cf1a7f64a783601c568490b4b4255c68d43d8a9eada8ef054db32ab8',
 str(BEFORE/'BEFORE-FILESET.json'):'03956017c6a5800c396b8908be66ec07713441c3eae61759c29b2a0389fba11e',
 str(BEFORE/'BEFORE-SUMMARY.json'):'b13bfc202decaf4bb6d7a4a0be506256dd4e8f1cb481b0d1b064d23afc92e90f',
 str(SOLVER):'4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a',
}


def pin(path):
    path = Path(path); old = path.stat(); h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''): h.update(chunk)
    new = path.stat()
    if (old.st_dev,old.st_ino,old.st_size,old.st_mtime_ns,old.st_ctime_ns) != (new.st_dev,new.st_ino,new.st_size,new.st_mtime_ns,new.st_ctime_ns):
        raise ValueError('changed while hashing: '+str(path))
    return dict(path=str(path),size=old.st_size,sha256=h.hexdigest())


def put(path,value):
    with path.open('x') as f: json.dump(value,f,indent=2);f.write('\n')


def verify_before():
    manifest = json.loads((BEFORE/'BEFORE-FILESET.json').read_text())
    names = {str(p.relative_to(BEFORE)) for p in BEFORE.rglob('*') if p.is_file()}
    if names != set(manifest['files'])|{'BEFORE-FILESET.json'}: raise ValueError('before file set changed')
    for name,wanted in manifest['files'].items():
        actual = pin(BEFORE/name)
        if actual['sha256'] != wanted['sha256'] or actual['size'] != wanted['size']: raise ValueError('before payload changed: '+name)
    if not json.loads((BEFORE/'BEFORE-SUMMARY.json').read_text())['passed']: raise ValueError('before/fault did not pass')


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in BACKENDS: raise ValueError('backend main|noz3 required')
    backend = sys.argv[1]; sdk,sdk_sha,solver = BACKENDS[backend]
    root = Path('/home/ubuntu/vmc-runs/dispatch-corridor-batch-native-after-'+backend+'-20261115-v1');root.mkdir(exist_ok=False)
    receipt = root/'controller-receipts';receipt.mkdir();rows = [];result = dict(passed=False,backend=backend)
    env = dict(os.environ,LD_LIBRARY_PATH=str(sdk.parent)+(':'+str(solver.parent) if solver else ''),LD_PRELOAD='',PYTHONDONTWRITEBYTECODE='1')
    def run(name, argv):
        if shutil.disk_usage(root).free < 512*1024*1024: raise RuntimeError('512MiB startup gate')
        argv = ['/usr/bin/taskset','-c','0-3',*[str(x) for x in argv]];start = time.monotonic();reason = None
        with (receipt/(name+'.stdout')).open('xb') as out,(receipt/(name+'.stderr')).open('xb') as error:
            p = subprocess.Popen(argv,stdout=out,stderr=error,env=env,start_new_session=True)
            while p.poll() is None:
                if time.monotonic()-start > 180: reason = '180s deadline'
                if shutil.disk_usage(root).free < 64*1024*1024: reason = '64MiB running floor'
                if reason: os.killpg(p.pid,signal.SIGKILL);p.wait();break
                time.sleep(0.2)
        row = dict(name=name,argv=argv,exit=p.returncode,reason=reason,seconds=time.monotonic()-start,pgid=p.pid,
                   affinity='0-3',timeout=180,start_bytes=512*1024*1024,running_bytes=64*1024*1024,
                   stdout=pin(receipt/(name+'.stdout')),stderr=pin(receipt/(name+'.stderr')))
        rows.append(row);put(receipt/(name+'.json'),row)
        if reason or p.returncode: raise RuntimeError('guarded command failed: '+name)
    try:
        verify_before(); wanted = dict(EXPECTED,**{str(sdk):sdk_sha}); before = {name:pin(name) for name in wanted}
        for name,value in before.items():
            if value['sha256'] != wanted[name]: raise ValueError('frozen pin changed: '+name)
        source = BEFORE/'source/scripts/validation';probe = source/'dispatch_corridor_batch_native_probe.cpp';runner = source/'dispatch_corridor_batch_native.py'
        build = root/'producer-build';build.mkdir()
        compile_argv = ['/usr/bin/g++-14','-std=c++20','-O0','-g','-I'+str(REPO/'include'),'-I/usr/lib/llvm-21/include','-fPIC','-c',probe,'-o',build/'probe.o']
        link_argv = ['/usr/bin/g++-14',build/'probe.o','-L'+str(sdk.parent),'-Wl,-rpath,'+env['LD_LIBRARY_PATH'],'-ldragon']
        if solver: link_argv.append(solver)
        link_argv += ['-o',build/'producer']
        put(root/'SDK.json',dict(runtime=dict(path=str(sdk),sha256=sdk_sha),solver=dict(path=str(solver),sha256=EXPECTED[str(solver)]) if solver else None))
        put(root/'PLAN.json',dict(backend=backend,source_pins=before,controller=pin(__file__),
                                 compile=[str(x) for x in compile_argv],link=[str(x) for x in link_argv],
                                 expected_api_rewrites=[71,1],expected_cyclic_switches=[0,0],
                                 cpu='0-3',timeout=180,start_bytes=512*1024*1024,running_bytes=64*1024*1024,
                                 environment={key:env[key] for key in ('LD_LIBRARY_PATH','LD_PRELOAD','PYTHONDONTWRITEBYTECODE')},
                                 before_not_recounted=True))
        run('compile-producer',compile_argv);run('link-producer',link_argv)
        put(build/'BUILD.json',dict(compile=[str(x) for x in compile_argv],link=[str(x) for x in link_argv],source=pin(probe),
                                   object=pin(build/'probe.o'),executable=pin(build/'producer'),sdk=pin(sdk),solver=pin(solver) if solver else None))
        prerequisites = ['--before',BEFORE/'before-prepared','--calibration',BEFORE/'before-O0',BEFORE/'before-O2',
                         '--calibration-audit',BEFORE/'audit-before-O0',BEFORE/'audit-before-O2',
                         '--fault-result',BEFORE/'fault-O0',BEFORE/'fault-O2','--fault-audit',BEFORE/'audit-fault-O0',BEFORE/'audit-fault-O2']
        run('prepare-after',[PYTHON,runner,'prepare-after','--raw',BEFORE/'raw','--out',root/'after-prepared',
                             '--producer',build/'producer','--producer-sha256',pin(build/'producer')['sha256'],
                             '--runtime',sdk,'--runtime-sha256',sdk_sha,'--producer-build',build/'BUILD.json','--sdk-manifest',root/'SDK.json',*prerequisites])
        for opt in ('O0','O2'):
            run('after-'+opt,[PYTHON,runner,'run','--raw',BEFORE/'raw','--after',root/'after-prepared','--phase','after','--host-opt',opt,
                             '--cc','/usr/bin/gcc-14','--llc','/usr/bin/llc-21','--opt','/usr/bin/opt-21','--include-root',REPO/'include',
                             '--out',root/('after-'+opt),*prerequisites])
            run('audit-after-'+opt,[PYTHON,runner,'audit','--audit',root/('after-'+opt),'--out',root/('audit-after-'+opt)])
        verify_before()
        if {name:pin(name) for name in wanted} != before: raise ValueError('SDK/source changed during after')
        result.update(passed=True,new_api_calls=2,before_calibration_not_recounted=True,
                      results={opt:pin(root/('after-'+opt)/'RESULTS.json') for opt in ('O0','O2')},
                      audits={opt:pin(root/('audit-after-'+opt)/'REPORT.json') for opt in ('O0','O2')},source_pins=before)
    except Exception as error: result['failure'] = type(error).__name__+': '+str(error)
    result['commands'] = rows;put(root/'AFTER-SUMMARY.json',result)
    return 0 if result['passed'] else 1


if __name__ == '__main__': sys.exit(main())
