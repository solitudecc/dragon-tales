"""Preserve v1 failing Python fixture IR before context-manager teardown; no Guest."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import sys
import traceback
sys.dont_write_bytecode=True

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--test',type=Path,required=True)
    ap.add_argument('--test-sha256',required=True);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();test=a.test.resolve();out=a.out.resolve();assert sha(test)==a.test_sha256 and not out.exists();out.mkdir()
    captured=[False]
    def write(name,data):
        if not isinstance(data,str):data=json.dumps(data,sort_keys=True,indent=2)+'\n'
        (out/name).write_text(data)
    def trace(frame,event,arg):
        if event=='exception' and Path(frame.f_code.co_filename)==test and isinstance(arg[1],AssertionError) and not captured[0]:
            local=frame.f_locals
            if 'view' in local and 'original' in local:
                captured[0]=True
                write('guarded.before.ll',local['original']);write('guarded.current.ll',local['view'].ir());write('source.raw.ll',local['source'].ir())
                write('PIPELINE-REPORT.json',local['report']);write('guarded-manifest.json',local['view'].guarded_entry_manifest())
                write('CAPTURE.json',dict(schema='python-loop-v1-live-frame-diagnostic-v1',
                    test=str(test),test_sha256=a.test_sha256,line=frame.f_lineno,view_symbol=local['symbol'],
                    before_current_IR_equal=(out/'guarded.before.ll').read_bytes()==(out/'guarded.current.ll').read_bytes(),
                    Guest_runs=0,library_or_fixture_changes=0,precision_closed=False,diagnostic_only=True))
                maps=Path('/proc/self/maps')
                if maps.exists():write('process.maps',maps.read_text())
        return trace
    sys.settrace(trace)
    try:runpy.run_path(str(test),run_name='__main__')
    except BaseException as error:
        sys.settrace(None);write('FAILURE.json',dict(type=type(error).__name__,detail=str(error),traceback=traceback.format_exc(),
            captured=captured[0],expected_precision_assertion_preserved=True,Guest_runs=0));return 1
    finally:sys.settrace(None)
    write('UNEXPECTED-PASS.json',dict(captured=captured[0]));return 2

if __name__=='__main__':raise SystemExit(main())
