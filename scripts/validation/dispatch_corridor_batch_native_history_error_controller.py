"""New-input history-error supplement; immutable prior roots and zero API calls."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT=Path('/home/ubuntu/vmc-runs/dispatch-corridor-batch-history-error-supplement-20261115-v2')
VM=Path('/home/ubuntu/vmc-runs')
BEFORE=VM/'dispatch-corridor-batch-native-20261115-v1'
MAIN=VM/'dispatch-corridor-batch-native-after-main-20261115-v1'
NZ=VM/'dispatch-corridor-batch-native-after-noz3-20261115-v1'
PYTHON=VM/'rock5b-ucdev-reference-20261114-v1/venv/bin/python'


def pin(path):
    path=Path(path);old=path.stat();h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
    new=path.stat()
    if (old.st_dev,old.st_ino,old.st_size,old.st_mtime_ns,old.st_ctime_ns)!=(new.st_dev,new.st_ino,new.st_size,new.st_mtime_ns,new.st_ctime_ns):raise ValueError('changed during hash')
    return dict(path=str(path),size=old.st_size,sha256=h.hexdigest())


def put(path,value):
    with path.open('x') as f:json.dump(value,f,indent=2);f.write('\n')


def main():
    receipts=ROOT/'controller-receipts';receipts.mkdir(exist_ok=False);rows=[]
    result=dict(passed=False,new_api_calls=0,new_unicorn_calls=0,old_counts_not_repeated=True)
    env=dict(os.environ,LD_PRELOAD='',LD_LIBRARY_PATH='',PYTHONDONTWRITEBYTECODE='1')
    def run(name,argv):
        if shutil.disk_usage(ROOT).free<512*1024*1024:raise RuntimeError('512MiB startup gate')
        argv=['/usr/bin/taskset','-c','0-3',*[str(v) for v in argv]];start=time.monotonic();reason=None
        with (receipts/(name+'.stdout')).open('xb') as out,(receipts/(name+'.stderr')).open('xb') as error:
            p=subprocess.Popen(argv,stdout=out,stderr=error,env=env,start_new_session=True)
            while p.poll() is None:
                if time.monotonic()-start>180:reason='180s deadline'
                if shutil.disk_usage(ROOT).free<64*1024*1024:reason='64MiB running floor'
                if reason:os.killpg(p.pid,signal.SIGKILL);p.wait();break
                time.sleep(0.2)
        row=dict(name=name,argv=argv,exit=p.returncode,reason=reason,seconds=time.monotonic()-start,pgid=p.pid,
                 affinity='0-3',timeout=180,start_bytes=512*1024*1024,running_bytes=64*1024*1024,
                 stdout=pin(receipts/(name+'.stdout')),stderr=pin(receipts/(name+'.stderr')))
        rows.append(row);put(receipts/(name+'.json'),row)
        if p.returncode or reason:raise RuntimeError('guarded command failed: '+name)
    try:
        source=ROOT/'source/dispatch_corridor_batch_native_history_error.py'
        manifest=json.loads((ROOT/'SOURCE.json').read_text())
        if pin(source)['sha256']!=manifest['runner_sha256']:raise ValueError('supplement runner changed')
        header=ROOT/'include/dragon/lifters/llvm/Execution.h'
        if pin(header)['sha256']!='b0674f3684a1393a89c06b00beee6402eaa20e388f418491b7c1713016b1b560':raise ValueError('frozen Execution.h differs')
        base=[PYTHON,source,'--before',BEFORE,'--main',MAIN,'--noz3',NZ]
        common=['--cc','/usr/bin/gcc-14','--llc','/usr/bin/llc-21','--opt','/usr/bin/opt-21','--include-root',ROOT/'include']
        put(ROOT/'PLAN.json',dict(scope='3 supplementary frames using unchanged C/before/main/NZ native ELFs; only new fault is raw error PHI replacement',
                                 controller=pin(__file__),runner=pin(source),header=pin(header),source_manifest=pin(ROOT/'SOURCE.json'),
                                 cpu='0-3',timeout=180,start_bytes=512*1024*1024,running_bytes=64*1024*1024,
                                 environment={key:env[key] for key in ('LD_PRELOAD','LD_LIBRARY_PATH','PYTHONDONTWRITEBYTECODE')},
                                 new_api_calls=0,new_normal_comparisons=24,new_controls=648,new_fault_observations=6))
        for opt in ('O0','O2'):
            run('normal-'+opt,[*base,'run','--phase','normal','--host-opt',opt,'--out',ROOT/('normal-'+opt),*common])
            run('audit-normal-'+opt,[*base,'audit','--audit',ROOT/('normal-'+opt),'--out',ROOT/('audit-normal-'+opt)])
        for opt in ('O0','O2'):
            run('fault-'+opt,[*base,'run','--phase','fault','--host-opt',opt,'--out',ROOT/('fault-'+opt),*common,
                              '--normal-result',ROOT/'normal-O0',ROOT/'normal-O2','--normal-audit',ROOT/'audit-normal-O0',ROOT/'audit-normal-O2'])
            run('audit-fault-'+opt,[*base,'audit','--audit',ROOT/('fault-'+opt),'--out',ROOT/('audit-fault-'+opt)])
        result.update(passed=True,normal=[pin(ROOT/('normal-'+v)/'RESULTS.json') for v in ('O0','O2')],
                      faults=[pin(ROOT/('fault-'+v)/'RESULTS.json') for v in ('O0','O2')],
                      audits=[pin(ROOT/(phase+'-'+v)/'REPORT.json') for phase in ('audit-normal','audit-fault') for v in ('O0','O2')])
    except Exception as error:result['failure']=type(error).__name__+': '+str(error)
    result['commands']=rows;put(ROOT/'SUMMARY.json',result)
    return 0 if result['passed'] else 1


if __name__=='__main__':sys.exit(main())
