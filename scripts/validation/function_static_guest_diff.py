"""Three-architecture static IR oracle: full function or declared static boundary.

Uses the unchanged Unicorn state/memory comparison, with explicit observation
boundaries where static functions propagate transfers/altered returns instead
of invoking V2's dispatcher. No register/memory normalization or new exclusions.
The native external implementation is test-only and linked after optimization.
"""
import copy,json,tempfile
from pathlib import Path
import function_guest_diff as guest
import function_dispatch_diff as dispatch

class StaticAdapter(guest.Adapter):
    def reference(self,f,*args):
        regs,memory,events,record=super().reference(f,*args)
        if record[0]==3 and record[1] in (1,3):
            record=(2 if record[1]==1 else 1,)+record[1:]
        return regs,memory,events,record

def compile_static(f,*args,**kwargs):
    result=dispatch.compile_case(f,*args,**kwargs,static_analysis=True)
    if '_internal_' in f['id']:
        raw=(args[2]/'raw.ll').read_text()
        if f'call void @dragon_static_{f["architecture"]}_2000(' not in raw:
            raise RuntimeError('internal target was not emitted as a direct static call')
    return result

def fixtures():
    data=json.loads((guest.base.ROOT/'tests/fixtures/llvm-guest-flow.json').read_text())
    internals=[]
    for f in data['cases']:
        if f['id'] in ('x86_64_call','x86_32_call','arm64_call'):
            for caller_first in (True,False):
                variant=copy.deepcopy(f);callee=variant['stubs'].pop()
                variant['id']=f['architecture']+'_internal_'+('caller_first' if caller_first else 'callee_first')
                variant['local_functions']=[callee];variant['callee_first']=not caller_first
                root=bytes.fromhex(variant['code_hex'])
                variant['code_hex']=(root+bytes(callee['address']-0x1000-len(root))+bytes.fromhex(callee['code_hex'])).hex()
                internals.append(variant)
    data['cases'].extend(internals)
    for f in data['cases']:
        name=f['id'];boundary=None
        if name.endswith('_redirect'):boundary=(0x3000,3)
        elif name.endswith('_import_thunk'):boundary=(0x3000,1)
        elif name.endswith('_indirect_tail') or '_image_' in name:boundary=(0x2000,1)
        elif name=='x86_64_B':boundary=(0x3000,1)
        if boundary:
            f['missing_targets']=[boundary[0]];f['boundary_kind']=boundary[1]
    return data

if __name__=='__main__':
    with tempfile.TemporaryDirectory(prefix='dragon-static-guests-') as folder:
        path=Path(folder)/'fixtures.json';path.write_text(json.dumps(fixtures()))
        raise SystemExit(guest.main(fixtures_path=path,compile_factory=compile_static,adapter_factory=StaticAdapter))
