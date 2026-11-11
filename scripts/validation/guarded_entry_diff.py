#!/usr/bin/env python3
"""Shared-arena full-state oracle for guarded entry views (no pointer normalization)."""
import argparse
import ctypes
import gzip
import hashlib
import json
import mmap
import os
import resource
import sys
from pathlib import Path
import struct
import subprocess

MODES=('plain','overwrite','partial','call','loop','reg_alias','descriptor_alias','exit_alias','recursive','base_reload')
VARIANTS=('match','register_miss','memory_miss','zero','short','exact','wrong_pc')
VALUES=(0,1,1<<63,(1<<64)-1)
INPUTS=tuple((m,l,v,x) for m in MODES for l in range(4) for v in VARIANTS for x in VALUES)
FAULTS={'constant':'overwrite','drop_store':'overwrite','drop_call':'call','bounds':'plain','entry':'plain'}
MASK=(1<<64)-1
ARENA_BYTES=0x22000
GUEST=0x1000
SNAPSHOT_BYTES=8+ARENA_BYTES
HELPER=r'''
#include <stdint.h>
#include <string.h>
uint64_t guarded_calls;
void guarded_unknown(unsigned char *regs,unsigned char *guest,void *memory,uint64_t index,uint64_t x){
  (void)memory;++guarded_calls;regs[48]^=(unsigned char)x;guest[0x300]^=(unsigned char)(x>>8);
  uint64_t value=x^UINT64_C(0xfaceb00c12345678);memcpy(guest+index,&value,8);
}
'''

def put(a,at,value):a[at:at+8]=struct.pack('<Q',value&MASK)
def get(a,at):return struct.unpack_from('<Q',a,at)[0]
def layout(n):return (GUEST+256 if n==1 else 0x100,GUEST+64 if n==2 else 0x60,GUEST+240 if n==3 else 0x300)
def initial(key,address):
    mode,n,variant,x=key;regs,desc,ex=layout(n);pc=0x1000+MODES.index(mode)*0x100
    a=bytearray(bytes((i*37+(x&255))&255 for i in range(256))*(ARENA_BYTES//256))
    for k in range(7):put(a,ex+k*8,0xcafe000000000000+k)
    put(a,GUEST+256,34 if variant=='memory_miss' else 14)
    put(a,GUEST+512,34);put(a,GUEST+4096+256,51);put(a,GUEST+4096+512,67)
    put(a,regs+8,x);put(a,regs+16,x&3);put(a,regs+24,512 if variant=='register_miss' else 256)
    put(a,regs+32,768);put(a,regs+128,pc+1 if variant=='wrong_pc' else pc)
    for off,value in ((0,2),(8,2),(16,512),(24,address+regs)):put(a,0x20+off,value)
    put(a,desc,address+GUEST);put(a,desc+8,0 if variant=='zero' else 263 if variant=='short' else 264 if variant=='exact' else 65536)
    a[0x400:0x428]=bytes(40)
    return a

def model(key,address,fault=None):
    mode,n,variant,x=key;regs,desc,ex=layout(n);pc=0x1000+MODES.index(mode)*0x100
    a=initial(key,address);calls=0
    def execute(depth=0):
        nonlocal calls
        assert depth<=1
        # The canonical entry prologue snapshots these architectural registers.
        values={off:get(a,regs+off) for off in (0,8,16,24,32,128)}
        if values[128]!=pc and fault!='entry':
            for off,v in values.items():put(a,regs+off,v)
            fields=(5,0,values[128],values[128],1,0,5)
            for k,v in enumerate(fields):put(a,ex+k*8,v)
            return
        index,payload,limit=values[24],values[8],values[16]
        base=get(a,desc)-address
        iteration=0
        while True:
            current=base
            if mode=='overwrite' and fault!='drop_store':put(a,base+index,payload^34)
            if mode=='partial':a[base+index+1]=0xa5
            if mode=='call' and fault!='drop_call':
                calls+=1;a[regs+48]^=payload&255;a[base+0x300]^=(payload>>8)&255
                put(a,base+index,payload^0xfaceb00c12345678)
            if mode=='reg_alias':put(a,regs,payload)
            if mode=='descriptor_alias':put(a,base+72,7)
            if mode=='exit_alias':put(a,ex+16,payload)
            if mode=='base_reload':put(a,desc,address+base+4096);current=get(a,desc)-address
            if mode=='recursive' and limit:
                put(a,regs+16,0);put(a,base+index,34);execute(depth+1)
            size=get(a,desc+8)
            success=fault=='bounds' or size>=8 and index<=size-8
            if not success:
                result=index^0xbad0000000000000;break
            value=14 if fault=='constant' else get(a,current+index)
            if mode=='loop' and iteration<(limit&3):
                iteration+=1;put(a,base+index,34+iteration);continue
            result=value^payload;break
        put(a,regs,result);put(a,regs+16,iteration);put(a,regs+128,0xdead if success else pc)
        fields=(1 if success else 5,0,pc,0xdead if success else pc,int(success),0,0 if success else 3)
        for k,v in enumerate(fields):put(a,ex+k*8,v)
    execute()
    return struct.pack('<Q',calls)+a

def invoke(library,key,arena,symbol):
    address=ctypes.addressof(arena);contents=initial(key,address);ctypes.memmove(address,bytes(contents),len(contents))
    _,n,_,_=key;regs,desc,ex=layout(n)
    count=ctypes.c_uint64.in_dll(library,'guarded_calls');count.value=0
    fn=getattr(library,symbol);fn.argtypes=[ctypes.c_void_p]*4;fn.restype=None
    fn(address+0x20,address+desc,address+0x400,address+ex)
    return struct.pack('<Q',count.value)+bytes(arena)

def compare(actual,expected):
    if len(actual)!=SNAPSHOT_BYTES or len(expected)!=SNAPSHOT_BYTES:raise AssertionError('snapshot length')
    return actual==expected

def self_test():
    address=0x400000000;controls=0
    for key in INPUTS:
        expected=model(key,address);assert compare(expected,expected)
        regs,desc,ex=layout(key[1])
        for at in (0,8,8+regs,8+GUEST,8+0x20,8+desc,*[8+ex+n*8 for n in range(7)],8+ARENA_BYTES-1):
            damaged=bytearray(expected);damaged[at]^=1;assert not compare(damaged,expected);controls+=1
    for size in (0,SNAPSHOT_BYTES-1,SNAPSHOT_BYTES+1):
        try:compare(bytes(size),model(INPUTS[0],address))
        except AssertionError:controls+=1
        else:raise AssertionError('length damage escaped')
    return controls

def guard_page_child(library_path,stage,kind):
    # The valid mapping ends before the requested table cell. Original plain
    # IR must take its Memory/Entry exit without touching the protected page.
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    libc=ctypes.CDLL(None);libc.mprotect.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int]
    page=mmap.PAGESIZE;mapping=mmap.mmap(-1,page*2,prot=mmap.PROT_READ|mmap.PROT_WRITE)
    backing=(ctypes.c_ubyte*(page*2)).from_buffer(mapping);base=ctypes.addressof(backing)
    assert libc.mprotect(base+page,page,0)==0
    data=base+page-256;ctypes.memset(data,0xab,256)
    if kind=='bad_read':
        ctypes.string_at(data+256,8)
        raise AssertionError('protected-page read unexpectedly returned')
    class State(ctypes.Structure):_fields_=[('abi',ctypes.c_uint64),('arch',ctypes.c_uint64),('size',ctypes.c_uint64),('regs',ctypes.c_void_p)]
    class Memory(ctypes.Structure):_fields_=[('data',ctypes.c_void_p),('size',ctypes.c_uint64)]
    r=bytearray((i*13)&255 for i in range(512));pc=0x1001 if kind=='wrong_pc' else 0x1000
    for off,v in ((8,1),(16,0),(24,256),(32,768),(128,pc)):put(r,off,v)
    regs=(ctypes.c_ubyte*512).from_buffer_copy(r);state=State(2,2,512,ctypes.addressof(regs))
    memory=Memory(None if kind=='null_zero' else data,0 if kind=='null_zero' else 255)
    ex=(ctypes.c_uint64*7)(*[0xcafe000000000000+i for i in range(7)]);runtime=(ctypes.c_ubyte*40)()
    initial_state,initial_descriptor=bytes(state),bytes(memory)
    lib=ctypes.CDLL(library_path);fn=getattr(lib,'dragon_static_entry_x86_64_1000'+('.guarded.oracle' if stage!='original' else ''))
    fn.argtypes=[ctypes.c_void_p]*4;fn.restype=None
    calls=ctypes.c_uint64.in_dll(lib,'guarded_calls');calls.value=0
    fn(ctypes.byref(state),ctypes.byref(memory),runtime,ex)
    if kind=='wrong_pc':fields=(5,0,pc,pc,1,0,5)
    else:
        put(r,0,0xbad0000000000100);put(r,16,0);put(r,128,pc);fields=(5,0,pc,pc,0,0,3)
    actual=struct.pack('<Q',calls.value)+bytes(regs)+ctypes.string_at(data,256)+bytes(ex)+bytes(state)+bytes(memory)
    expected=bytes(8)+r+bytes([0xab])*256+struct.pack('<7Q',*fields)+initial_state+initial_descriptor
    assert actual==expected,'guard-page complete state mismatch'
    print(json.dumps({'kind':kind,'stage':stage,'ok':True,'bytes':len(actual),'snapshot':actual.hex(),'sha256':hashlib.sha256(actual).hexdigest()}))
    return 0

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--self-test',action='store_true')
    p.add_argument('--guard-page-child',nargs=3);p.add_argument('--ir',type=Path);p.add_argument('--out',type=Path);p.add_argument('--guarded',action='store_true');p.add_argument('--clang',default='clang-21')
    a=p.parse_args()
    if a.guard_page_child:return guard_page_child(*a.guard_page_child)
    if a.self_test:print(json.dumps({'inputs':len(INPUTS),'comparator_controls':self_test(),'ok':True}));return 0
    if a.ir is None or a.out is None:p.error('--ir and --out required')
    if a.out.exists():p.error('--out must be new')
    a.out.mkdir(parents=True);(a.out/'helper.c').write_text(HELPER)
    arena=(ctypes.c_ubyte*ARENA_BYTES)();address=ctypes.addressof(arena)
    report={'ok':False,'inputs':len(INPUTS),'arena_address':address,'arena_bytes':ARENA_BYTES,'snapshot_bytes':SNAPSHOT_BYTES,'guarded':a.guarded,'rows':[],
            'normal':0,'mismatches':0,'fault_observations':0,'fault_required':0,'fault_detected':0,'commands':[],'errors':[], 'excluded':0,'skipped':0,'normalized':0}
    def compile(label,files):
        so=(a.out/(label+'.so')).resolve();argv=[a.clang,'-'+label.split('-')[-1],'-shared','-fPIC','-Wno-override-module']+[str(f) for f in files]+[str(a.out/'helper.c'),'-o',str(so)]
        r=subprocess.run(argv,capture_output=True,timeout=240);(a.out/(label+'.stdout')).write_bytes(r.stdout);(a.out/(label+'.stderr')).write_bytes(r.stderr)
        report['commands'].append({'argv':argv,'rc':r.returncode});assert r.returncode==0,label
        return ctypes.CDLL(str(so))
    try:
        report['comparator_controls']=self_test()
        with gzip.open(a.out/'expected.bin.gz','wb',compresslevel=1) as out:
            for key in INPUTS:out.write(model(key,address))
        for level in ('O0','O2'):
            for stage in (('original','guarded','optimized') if a.guarded else ('original',)):
                label=stage+'-'+level;lib=compile(label,[a.ir/(n+'-'+stage+'.ll') for n in MODES])
                for kind in ('short','wrong_pc','null_zero'):
                    probe=subprocess.run([sys.executable,str(Path(__file__).resolve()),'--guard-page-child',str((a.out/(label+'.so')).resolve()),stage,kind],capture_output=True,timeout=15)
                    (a.out/(label+'-'+kind+'.stdout')).write_bytes(probe.stdout);(a.out/(label+'-'+kind+'.stderr')).write_bytes(probe.stderr)
                    report.setdefault('guard_pages',[]).append({'stage':stage,'level':level,'kind':kind,'rc':probe.returncode})
                    assert probe.returncode==0,('guard page',label,kind)
                with gzip.open(a.out/(label+'.bin.gz'),'wb',compresslevel=1) as out:
                    for key in INPUTS:
                        pc=0x1000+MODES.index(key[0])*0x100;symbol=f'dragon_static_entry_x86_64_{pc:x}'+('.guarded.oracle' if stage!='original' else '')
                        actual=invoke(lib,key,arena,symbol);expected=model(key,address);out.write(actual);ok=compare(actual,expected)
                        report['normal']+=1;report['mismatches']+=not ok
                        report['rows'].append({'key':key,'stage':stage,'level':level,'match':ok,'sha256':hashlib.sha256(actual).hexdigest()})
            for fault,target in FAULTS.items():
                label='control-'+fault+'-'+level;lib=compile(label,[a.ir/('control_'+fault+'.ll')])
                with gzip.open(a.out/(label+'.bin.gz'),'wb',compresslevel=1) as out:
                    for key in INPUTS:
                        if key[0]!=target:continue
                        actual=invoke(lib,key,arena,'control_'+fault);bad=model(key,address,fault);out.write(actual)
                        assert compare(actual,bad),(fault,key,'faulty full snapshot')
                        wanted=not compare(bad,model(key,address));detected=not compare(actual,model(key,address))
                        report['fault_observations']+=1;report['fault_required']+=wanted;report['fault_detected']+=detected
                        report['rows'].append({'key':key,'fault':fault,'level':level,'required':wanted,'detected':detected,'sha256':hashlib.sha256(actual).hexdigest()})
                        assert wanted==detected
        probe=subprocess.run([sys.executable,str(Path(__file__).resolve()),'--guard-page-child',str((a.out/'original-O0.so').resolve()),'original','bad_read'],capture_output=True,timeout=15)
        (a.out/'guard-page-control.stdout').write_bytes(probe.stdout);(a.out/'guard-page-control.stderr').write_bytes(probe.stderr)
        report['guard_page_fault_control_rc']=probe.returncode
        assert probe.returncode in (-11,-7),'guard-page fault control escaped'
        report['ok']=report['mismatches']==0 and report['normal']==len(INPUTS)*(6 if a.guarded else 2) and report['fault_required']==report['fault_detected']
    except Exception as e:report['errors'].append(type(e).__name__+': '+str(e))
    report['artifact_sha256']={str(f.relative_to(a.out)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(a.out.rglob('*')) if f.is_file()}
    (a.out/'report.json').write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('inputs','normal','mismatches','fault_observations','fault_required','fault_detected','errors','ok')}))
    return 0 if report['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
