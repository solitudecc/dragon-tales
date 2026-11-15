"""Fixed 180s/512MiB/64MiB command guard for the separately frozen probe plan."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
sys.dont_write_bytecode = True
LIMITS = dict(seconds=180, start_free=536870912, running_floor=67108864)

def sha(p):
    h = hashlib.sha256()
    with Path(p).open('rb') as f:
        for b in iter(lambda: f.read(1048576), b''): h.update(b)
    return h.hexdigest()

def require(v, why):
    if not v: raise RuntimeError(why)

def main():
    started = time.monotonic()
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan', type=Path, required=True)
    p.add_argument('--sha256', required=True)
    p.add_argument('--stage', choices=['compile', 'probe'], required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    require(sha(a.plan) == a.sha256, 'plan SHA differs')
    raw = a.plan.read_bytes(); plan = json.loads(raw)
    require(plan['schema'] == 'dragon-tigress-context-guard-plan-v1'
            and plan['limits'] == LIMITS, 'guard schema/limits differ')
    require(not a.out.exists() and a.out.parent.is_dir(), 'new output with existing parent required')
    require(shutil.disk_usage(a.out.parent).free >= LIMITS['start_free'], '512MiB startup gate')
    pins = plan['pins']
    require(all(Path(p).is_absolute() and sha(p) == h for p,h in pins.items()), 'precommand input/source/runtime pins differ')
    command = plan['stages'][a.stage]['argv']
    require(command[:3] == ['/usr/bin/taskset','-c','0-3'], 'CPU affinity differs')
    a.out.mkdir(); env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    require(not env.get('LD_PRELOAD'), 'LD_PRELOAD forbidden')
    env['LD_LIBRARY_PATH'] = ':'.join(plan['library_path'])
    if a.stage == 'probe':
        exe = Path(plan['executable'])
        require(exe.is_file() and not exe.is_symlink(), 'compiled executable absent')
        pins = dict(pins, **{str(exe):sha(exe)})
        env['LD_DEBUG'] = 'libs'
    invocation = dict(argv=command, stage=a.stage, plan_sha256=a.sha256, pins=pins,
                      library_path=plan['library_path'], limits=LIMITS)
    (a.out/'INVOCATION.json').write_text(json.dumps(invocation,indent=2,sort_keys=True)+'\n')
    failure = None
    captured_maps = False
    with (a.out/'stdout').open('wb') as stdout,(a.out/'stderr').open('wb') as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr, env=env, start_new_session=True)
        while process.poll() is None:
            if a.stage == "probe" and not captured_maps:
                try:
                    maps = Path("/proc") / str(process.pid) / "maps"
                    data = maps.read_bytes()
                    if b"/libdragon.so" in data and b"/libz3.so.4.13" in data:
                        (a.out / "process.maps").write_bytes(data)
                        (a.out / "MAPS-CAPTURE.json").write_text(json.dumps({"pid":process.pid,"executable":str(exe),"phase":"actual child maps after SDK mapped"},sort_keys=True,indent=2)+"\n")
                        captured_maps = True
                except OSError:
                    pass
            if time.monotonic()-started > LIMITS['seconds']: failure='180 second CLI deadline'
            elif shutil.disk_usage(a.out).free < LIMITS['running_floor']: failure='64MiB running floor'
            if failure:
                os.killpg(process.pid,signal.SIGKILL);break
            time.sleep(0.01)
        code=process.wait()
    if a.stage == "probe" and not captured_maps: failure="actual SDK process maps not captured"
    unchanged = a.plan.read_bytes()==raw and all(sha(p)==h for p,h in pins.items())
    if not unchanged: failure='postcommand pins changed'
    if time.monotonic()-started > LIMITS['seconds']: failure='180 second CLI deadline'
    result=dict(stage=a.stage,returncode=code,failure=failure,pins_unchanged=unchanged,
                elapsed_seconds=time.monotonic()-started,plan_sha256=a.sha256,
                free_after=shutil.disk_usage(a.out).free,limits=LIMITS)
    (a.out/'GUARD.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    return 1 if failure else code

if __name__ == '__main__': raise SystemExit(main())
