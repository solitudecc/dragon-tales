"""Emit exact old-driver callee adaptation and a separate host-counter sidecar helper.

Preparation only: no compiler, product pass, C oracle, Unicorn or Guest call.
Requires an independently sealed ready roles file; the accompanying example
is deliberately ready=false. This script is not a closure certificate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

DRIVER_SHA='e0fb71e08b117ddb6c929dc60ff66af938073695ced0fe68b6f50aa9c69c75bb'
OBSERVER_HELPER=r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern uint64_t dragon_loop_native_accepted;
extern uint64_t dragon_loop_native_fallback;
__attribute__((destructor)) static void save_loop_observer(void) {
  const char *path=getenv("DRAGON_LOOP_OBSERVER_OUTPUT");
  const uint16_t endian=1;
  if(!path || *(const unsigned char*)&endian!=1)_Exit(96);
  FILE *file=fopen(path,"wb");if(!file)_Exit(97);
  const unsigned char magic[8]={'D','T','L','O','B','S','1',0};
  if(fwrite(magic,1,8,file)!=8 ||
     fwrite(&dragon_loop_native_accepted,8,1,file)!=1 ||
     fwrite(&dragon_loop_native_fallback,8,1,file)!=1 || fclose(file))_Exit(98);
}
'''

def digest(raw):
    return hashlib.sha256(raw).hexdigest()

def once(text,old,new):
    if text.count(old)!=1:raise RuntimeError('exact driver binding absent or ambiguous: '+old)
    return text.replace(old,new,1)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--driver',type=Path,required=True)
    ap.add_argument('--roles',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args()
    original=a.driver.read_bytes();roles_bytes=a.roles.read_bytes();roles=json.loads(roles_bytes)
    if (roles.get('schema')!='loop-real-native-adapter-roles-v1' or roles.get('ready') is not True or
        roles.get('eligible_actual_closed_result') is not True):
        raise RuntimeError('unsealed roles: preparation cannot emit executable adapters')
    if digest(original)!=DRIVER_SHA:raise RuntimeError('original driver SHA differs; never rewrite a different driver')
    if (roles['wrapper']!='dragon_static_entry_x86_64_1000' or
        roles['ordinary_kernel']!='dragon_static_entry_x86_64_1010' or roles['kernel_pc']!=4112):
        raise RuntimeError('driver domain differs from the frozen wrapper ABI')
    view=roles['view']
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.$]*',view):raise RuntimeError('unsupported C asm symbol spelling')
    before=original.decode()
    declaration='void dragon_static_entry_x86_64_1010(S*,M*,C*,E*);'
    alias='void dragon_loop_native_view(S*,M*,C*,E*) __asm__("'+view+'");'
    external='if(pc==4112ULL){dragon_static_entry_x86_64_1010(s,m,c,e);return;}'
    target='{4112ULL,dragon_static_entry_x86_64_1010}'
    adapted=once(before,declaration,declaration+'\n'+alias)
    adapted=once(adapted,external,'if(pc==4112ULL){dragon_loop_native_view(s,m,c,e);return;}')
    adapted=once(adapted,target,'{4112ULL,dragon_loop_native_view}')
    recovered=once(adapted,declaration+'\n'+alias,declaration)
    recovered=once(recovered,'if(pc==4112ULL){dragon_loop_native_view(s,m,c,e);return;}',external)
    recovered=once(recovered,'{4112ULL,dragon_loop_native_view}',target)
    if recovered.encode()!=original:raise RuntimeError('unauthorized driver difference')
    if a.out.exists():raise RuntimeError('new output namespace required')
    a.out.mkdir(parents=True)
    (a.out/'driver.original.c').write_bytes(original)
    (a.out/'driver.adapter.c').write_text(adapted)
    (a.out/'observer-sidecar.c').write_text(OBSERVER_HELPER)
    if a.driver.read_bytes()!=original or a.roles.read_bytes()!=roles_bytes:raise RuntimeError('input changed')
    report=dict(schema='loop-real-native-driver-emission-v1',original_driver_sha256=DRIVER_SHA,
      roles_sha256=digest(roles_bytes),view=view,source_inputs_unchanged=True,
      changes=['one C asm alias declaration','one4112 external handler callee','one4112 target-table callee'],
      reverse_difference_reproduces_original=True,ordinary1000_initial_call_unchanged=True,
      observer_sidecar=dict(bytes=24,magic='DTLOBS1\\0',fields=['accepted_u64_le','fallback_u64_le'],
                            separate_from_full66136byte_output=True),
      payload={p.name:dict(size=p.stat().st_size,sha256=digest(p.read_bytes())) for p in sorted(a.out.iterdir())},
      compiled=False,Guest_native_calls=0,C_oracle_calls=0,Unicorn_calls=0,closure_proved_by_emitter=False)
    (a.out/'REPORT.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')

if __name__=='__main__':main()
