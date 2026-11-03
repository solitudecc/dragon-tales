"""Static IR vs complete Unicorn execution, using external C stubs only at link time.

The same fixture instructions independently execute in Unicorn. opt sees
only the opaque external declaration, never the C implementation. All state
fields and memory use the existing full-function oracle and exclusions.
"""
import json
import sys
import tempfile
from pathlib import Path
import function_dispatch_diff as dispatch
import function_call_diff as calls

original_compile=dispatch.compile_case
def compile_static(*args,**kwargs):
    return original_compile(*args,**kwargs,static_analysis=True,product_pipeline=True)

def reference_static(*args,**kwargs):
    expected=dispatch.base.reference(*args,**kwargs)
    # Unicorn stops before the out-of-region target. V2's dispatcher reports
    # NeedTarget there; static IR reports the committed Transfer itself.
    record=expected[4]
    if record[0]==3 and record[1]==1:
        expected=expected[:4]+((2,)+record[1:],)
    return expected

if __name__=='__main__':
    # call_resume is a V2-specific missing-target/resume scenario; in static
    # mode this fixture tests the same bytes with a declaration supplied at
    # final link time, so no V2 resume request is generated.
    fixture=json.loads((dispatch.base.ROOT/'tests/fixtures/llvm-call-state.json').read_text())
    for case in fixture['cases']:case.pop('initially_missing',None)
    fixture['cases'].append(dict(id='call_memory_clobber',code_hex='8b03e8f90f00008b0b894b04c3',
        undefined_flags=[],stubs=[dict(address=0x2000,code_hex='c7032a000000c3',store=42)]))
    flow=json.loads((dispatch.base.ROOT/'tests/fixtures/llvm-function-flow.json').read_text())
    for case in flow['cases']:
        if case['id'] in ('B','control_store','control_branch','control_loop'):
            if case['id']=='B':case['stubs']=[];case['missing_targets']=[0x3000]
            fixture['cases'].append(case)
    dispatch.compile_case=compile_static
    with tempfile.TemporaryDirectory(prefix='dragon-static-fixtures-') as folder:
        path=Path(folder)/'calls.json';path.write_text(json.dumps(fixture))
        sys.exit(dispatch.main(fixtures_path=path,model_factory=calls.model,stop_before_calls=False,full_guest=True,reference_factory=reference_static))
