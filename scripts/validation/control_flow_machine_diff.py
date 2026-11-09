"""Execute exported state-machine IR against an independent arithmetic oracle.

First export the real production Pass output with DRAGON_CONTROL_FLOW_FIXTURE_DIR
set while running dragon_tests '[control-flow]'. Both before/after IR and O0/O2
are checked, including external states, defined defaults, overflow and all 64 KiB
of memory. This is an LLVM state-machine test, not a machine lifter/Unicorn test.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import platform
import subprocess

INPUTS=(0,1,2,3,7,8,15,16,0x7fffffff,0x80000000,0xfffffffe,0xffffffff)
STATES=(0,10,20,30,40,999,0xffffffff)
DRIVER=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern uint32_t machine(uint32_t, uint32_t *, uint32_t);
static _Alignas(8) unsigned char memory[65536];
int main(int argc,char **argv) {
  if(argc!=4)return 90;
  uint32_t x=strtoull(argv[1],0,10),state=strtoull(argv[2],0,10),salt=strtoull(argv[3],0,10);
  for(unsigned i=0;i<sizeof memory;++i)memory[i]=(i*37+salt)&255;
  uint32_t result=machine(x,(uint32_t *)(memory+16),state);
  if(fwrite(&result,1,4,stdout)!=4 || fwrite(memory,1,sizeof memory,stdout)!=sizeof memory)return 91;
  return 0;
}
'''

def expected(x,state,salt):
    memory=bytearray((i*37+salt)&255 for i in range(65536))
    limit=x&3
    if state in (10,20,30):
        iterations=max(1,limit) if state==30 else limit
        result=(x+iterations)&0xffffffff
        if state==10 or iterations:
            memory[16:20]=result.to_bytes(4,'little')
    else:
        result=x if state==40 else 0xbad
    return result.to_bytes(4,'little')+memory

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixtures',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--control',choices=('result','memory'));p.add_argument('--expect-control',action='store_true')
    p.add_argument('--opt',default='opt-21');p.add_argument('--llc',default='llc-21');p.add_argument('--cc',default='gcc')
    args=p.parse_args()
    if bool(args.control)!=args.expect_control:p.error('control and expect-control must be used together')
    triple={'aarch64':'aarch64-linux-gnu','x86_64':'x86_64-linux-gnu'}.get(platform.machine())
    if not triple:raise RuntimeError('Linux aarch64/x86_64 required')
    args.out.mkdir(parents=True,exist_ok=True);rows=[];modules=[]
    for kind in ('known','external'):
        for variant in ('before','after'):
            source=args.fixtures/(kind+'-'+variant+'.ll')
            text=source.read_text();switches=text.count('switch i32 ')
            if kind=='known' and switches!=(1 if variant=='before' else 0):
                raise RuntimeError('missing measured dispatcher transformation: '+str(source))
            for level in ('O0','O2'):
                work=args.out/(kind+'-'+variant)/level;work.mkdir(parents=True,exist_ok=True)
                subprocess.run([args.opt,'-passes=verify','-disable-output',str(source)],check=True,capture_output=True)
                selected=source
                if level=='O2':
                    selected=work/'optimized.ll'
                    subprocess.run([args.opt,'-S','-passes=default<O2>',str(source),'-o',str(selected)],check=True,capture_output=True)
                subprocess.run([args.opt,'-passes=verify','-disable-output',str(selected)],check=True,capture_output=True)
                obj=work/'module.o';exe=work/'native';driver=work/'driver.c';driver.write_text(DRIVER)
                subprocess.run([args.llc,'-O0','-filetype=obj','-mtriple='+triple,str(selected),'-o',str(obj)],check=True,capture_output=True)
                subprocess.run([args.cc,'-O0',str(driver),str(obj),'-o',str(exe)],check=True,capture_output=True)
                modules.append(dict(kind=kind,variant=variant,opt=level,switches=switches,
                    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                    selected_sha256=hashlib.sha256(selected.read_bytes()).hexdigest()))
                for trial,x in enumerate(INPUTS):
                    for state in STATES if kind=='external' else (10,):
                        salt=(trial*17+state)&255
                        want=expected(x,state,salt)
                        completed=subprocess.run([str(exe),str(x),str(state),str(salt)],check=True,capture_output=True)
                        actual=bytearray(completed.stdout)
                        if len(actual)!=65540:raise RuntimeError('truncated native snapshot')
                        stem=work/f'trial-{trial}-state-{state}'
                        native=stem.with_suffix('.native.bin');reference=stem.with_suffix('.expected.bin')
                        native.write_bytes(completed.stdout);reference.write_bytes(want)
                        if args.control:actual[0 if args.control=='result' else 4+1024]^=1
                        compared=stem.with_suffix('.compared.bin') if args.control else native
                        if args.control:compared.write_bytes(actual)
                        differences=[]
                        if actual[:4]!=want[:4]:differences.append('result')
                        if actual[4:]!=want[4:]:differences.append('memory')
                        rows.append(dict(kind=kind,variant=variant,opt=level,x=x,state=state,
                            status='different' if differences else 'match',differences=differences,
                            snapshots={key:dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                                for key,path in (('native',native),('expected',reference),('compared',compared))}))
    summary=dict(results=len(rows),statuses=dict(collections.Counter(r['status'] for r in rows)),
        modules=len(modules),memory_bytes_compared=65536,exclusions=0,skips=0,normalizations=0,control=args.control,
        harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (args.out/'results.json').write_text(json.dumps(dict(summary=summary,modules=modules,results=rows),indent=2)+'\n')
    print(json.dumps(summary,indent=2))
    wanted='different' if args.control else 'match'
    return 0 if len(rows)==384 and all(r['status']==wanted and r['differences']==
        ([args.control] if args.control else []) for r in rows) else 1

if __name__=='__main__':raise SystemExit(main())
