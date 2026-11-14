"""Owned sequential ROCK before/fault controller, fixed CPU0-3/180s/512-64MiB.

This controller never builds a rewrite producer and never calls the Batch API.
All descendants share the guarded process group. No SDK/source mutation occurs.
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

ROOT = Path('/home/ubuntu/vmc-runs/dispatch-corridor-batch-native-20261115-v1')
REPO = Path('/home/ubuntu/dragon-tales-rock5b')
SDK = REPO/'build/lib/libdragon.so'
SOLVER = Path('/home/ubuntu/vmc-runs/rock5b-bootstrap-20261114-v1/venv/lib/python3.10/site-packages/z3/lib/libz3.so.4.13')
PYTHON = Path('/home/ubuntu/vmc-runs/rock5b-ucdev-reference-20261114-v1/venv/bin/python')
EXPECTED = {
 str(SDK):'4a89d8a97d996e6423cb2a670924cee21714623bc28e74378d25bd93c0f3b080',
 str(SOLVER):'4da2bb3189b8fa131a6d5a1ee5dac1b1a82792a4ba8a0374b0df4715eb85d82a',
 str(REPO/'lib/LLVMDispatchCorridor.cpp'):'81f40177a93da64362e6600b78eda701295ca7ab035db42d6c84fd81fed30e29',
 str(REPO/'include/dragon/lifters/llvm/DispatchCorridor.hpp'):'2f7cc6080b148ede586859b6a5fad60eef76272e72d1362d771ee881f3a2c467',
 str(REPO/'include/dragon/lifters/llvm/Execution.h'):'b0674f3684a1393a89c06b00beee6402eaa20e388f418491b7c1713016b1b560',
}


def pin(path):
    path = Path(path); before = path.stat(); h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''): h.update(chunk)
    after = path.stat()
    if (before.st_dev,before.st_ino,before.st_size,before.st_mtime_ns,before.st_ctime_ns) != (after.st_dev,after.st_ino,after.st_size,after.st_mtime_ns,after.st_ctime_ns):
        raise ValueError('file changed while hashing: '+str(path))
    return dict(path=str(path),resolved=str(path.resolve()),size=before.st_size,sha256=h.hexdigest())


def put(path, value):
    with path.open('x') as f: json.dump(value,f,indent=2);f.write('\n')


def checked_pins():
    result = {name:pin(name) for name in EXPECTED}
    for name, wanted in EXPECTED.items():
        if result[name]['sha256'] != wanted: raise ValueError('declared SDK/source pin mismatch: '+name)
    for folder in ('source','raw'):
        for p in sorted((ROOT/folder).rglob('*')):
            if p.is_file(): result[str(p)] = pin(p)
    return result


def main():
    receipts = ROOT/'controller-receipts';receipts.mkdir(exist_ok=False)
    rows = [];result = dict(passed=False,new_api_calls=0,new_unicorn_calls=0)
    env = dict(os.environ,LD_LIBRARY_PATH=str(SDK.parent)+':'+str(SOLVER.parent),LD_PRELOAD='',PYTHONDONTWRITEBYTECODE='1')
    def run(name, argv):
        if shutil.disk_usage(ROOT).free < 512*1024*1024: raise RuntimeError('512MiB startup gate')
        argv = ['/usr/bin/taskset','-c','0-3',*[str(x) for x in argv]]
        begin = time.monotonic(); reason = None
        with (receipts/(name+'.stdout')).open('xb') as out, (receipts/(name+'.stderr')).open('xb') as error:
            p = subprocess.Popen(argv,stdout=out,stderr=error,env=env,start_new_session=True)
            while p.poll() is None:
                if time.monotonic()-begin > 180: reason = '180s deadline'
                if shutil.disk_usage(ROOT).free < 64*1024*1024: reason = '64MiB running floor'
                if reason: os.killpg(p.pid,signal.SIGKILL);p.wait();break
                time.sleep(0.2)
        row = dict(name=name,argv=argv,exit=p.returncode,reason=reason,seconds=time.monotonic()-begin,
                   pgid=p.pid,affinity='0-3',timeout=180,start_bytes=512*1024*1024,floor_bytes=64*1024*1024,
                   stdout=pin(receipts/(name+'.stdout')),stderr=pin(receipts/(name+'.stderr')))
        rows.append(row);put(receipts/(name+'.json'),row)
        if p.returncode or reason: raise RuntimeError('guarded command failed: '+name)
    try:
        before = checked_pins()
        tools = [pin(p) for p in (Path('/usr/bin/g++-14'),Path('/usr/bin/gcc-14'),Path('/usr/bin/llc-21'),Path('/usr/bin/opt-21'),PYTHON)]
        sdk_manifest = ROOT/'SDK.json'
        put(sdk_manifest,dict(runtime=dict(path=str(SDK),sha256=EXPECTED[str(SDK)]),solver=dict(path=str(SOLVER),sha256=EXPECTED[str(SOLVER)])))
        source = ROOT/'source/scripts/validation';runner = source/'dispatch_corridor_batch_native.py'
        probe = source/'dispatch_corridor_batch_native_probe.cpp';raw = ROOT/'raw'
        build = ROOT/'before-producer-build';build.mkdir()
        compile_argv = ['/usr/bin/g++-14','-std=c++20','-O0','-g','-DDRAGON_DISPATCH_CORRIDOR_BEFORE_ONLY',
                        '-I'+str(REPO/'include'),'-I/usr/lib/llvm-21/include','-fPIC','-c',probe,'-o',build/'probe.o']
        link_argv = ['/usr/bin/g++-14',build/'probe.o','-L'+str(SDK.parent),'-Wl,-rpath,'+str(SDK.parent)+':'+str(SOLVER.parent),
                     '-ldragon',SOLVER,'-o',build/'before-parser']
        put(ROOT/'PLAN.json',dict(scope='before calibration and faults only; no analysis/API calls',source_pins=before,tools=tools,
                                 controller=pin(__file__),compile=[str(x) for x in compile_argv],link=[str(x) for x in link_argv],
                                 environment={k:env[k] for k in ('LD_LIBRARY_PATH','LD_PRELOAD','PYTHONDONTWRITEBYTECODE')},
                                 cpu='0-3',timeout_seconds=180,start_bytes=512*1024*1024,running_bytes=64*1024*1024))
        for name,path in (('cxx','/usr/bin/g++-14'),('cc','/usr/bin/gcc-14'),('llc','/usr/bin/llc-21'),('opt','/usr/bin/opt-21')):
            run('version-'+name,[path,'--version'])
        run('compile-before-parser',compile_argv);run('link-before-parser',link_argv)
        put(build/'BUILD.json',dict(before_only=True,compile=[str(x) for x in compile_argv],link=[str(x) for x in link_argv],
                                   source=pin(probe),object=pin(build/'probe.o'),executable=pin(build/'before-parser'),sdk=pin(SDK),solver=pin(SOLVER)))
        run('prepare-before',[PYTHON,runner,'prepare-before','--raw',raw,'--out',ROOT/'before-prepared',
                              '--producer',build/'before-parser','--producer-sha256',pin(build/'before-parser')['sha256'],
                              '--runtime',SDK,'--runtime-sha256',EXPECTED[str(SDK)],'--producer-build',build/'BUILD.json','--sdk-manifest',sdk_manifest])
        base = [PYTHON,runner,'run','--raw',raw,'--before',ROOT/'before-prepared','--cc','/usr/bin/gcc-14',
                '--llc','/usr/bin/llc-21','--opt','/usr/bin/opt-21','--include-root',REPO/'include']
        for opt in ('O0','O2'):
            run('before-'+opt,[*base,'--phase','before','--host-opt',opt,'--out',ROOT/('before-'+opt)])
            run('audit-before-'+opt,[PYTHON,runner,'audit','--audit',ROOT/('before-'+opt),'--out',ROOT/('audit-before-'+opt)])
        for opt in ('O0','O2'):
            run('fault-'+opt,[*base,'--phase','fault','--host-opt',opt,'--out',ROOT/('fault-'+opt),
                              '--calibration',ROOT/'before-O0',ROOT/'before-O2','--calibration-audit',ROOT/'audit-before-O0',ROOT/'audit-before-O2'])
            run('audit-fault-'+opt,[PYTHON,runner,'audit','--audit',ROOT/('fault-'+opt),'--out',ROOT/('audit-fault-'+opt)])
        if checked_pins() != before: raise ValueError('SDK/source changed during calibration')
        result.update(passed=True,source_pins=before,results={name:pin(ROOT/name/'RESULTS.json') for name in ('before-O0','before-O2','fault-O0','fault-O2')},
                      audits={name:pin(ROOT/name/'REPORT.json') for name in ('audit-before-O0','audit-before-O2','audit-fault-O0','audit-fault-O2')})
    except Exception as error: result['failure'] = type(error).__name__+': '+str(error)
    result['commands'] = rows;put(ROOT/'BEFORE-SUMMARY.json',result)
    return 0 if result['passed'] else 1


if __name__ == '__main__': sys.exit(main())
