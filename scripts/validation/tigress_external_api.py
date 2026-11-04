"""Tigress + real ELF PLT/GOT imports, opaque boundaries and separate Guest DSOs.

Provider bodies are compiled separately and remain unavailable during caller
optimization. No production API models or native address bridges are installed.
Full returns, exact external-boundary snapshots and incomplete dispatches are
different outcomes. This is an execution/IR audit, not universal equivalence.
"""
import argparse
import collections
import hashlib
import io
import json
import platform
import re
import struct
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn import x86_const as U
import tigress_function_diff as common

base = common.base
CASES = ['return', 'write', 'memcpy', 'memcmp', 'strlen', 'indirect', 'callback', 'varargs', 'float', 'tail', 'stop']
PROFILES = ['plain', 'flatten-switch', 'arithmetic-flatten']
INPUTS = common.INPUTS + [0x80000001]  # include memcmp's equal case explicitly
IMPORT = ['api_value', 'api_write', 'memcpy', 'memcmp', 'strlen', None, 'api_invoke', 'api_sum', 'api_fp', 'api_value', 'api_stop']
MASK = 0xffffffff
HOST = r'''
#include <stdio.h>
#include <stdlib.h>
unsigned api_value(unsigned);
unsigned kernel(unsigned,unsigned *,unsigned (*)(unsigned));
int main(int argc,char **argv){
 if(argc!=2)return 2;unsigned out[8],x=(unsigned)strtoul(argv[1],0,0);
 unsigned r=kernel(x,out,api_value);printf("%u",r);
 for(unsigned i=0;i<8;++i)printf(" %u",out[i]);puts("");return 0;
}
'''


def spec(case, x):
    out = [0]*8
    value = MASK if x & 1 else 0
    if case in ('return', 'indirect'):
        out[0] = value;out[1] = 29 if value else 17;r = out[1]
    elif case == 'write':
        before = (x+5)&MASK;out[0] = ((before+x)&MASK)^0xa5a5a5a5
        out[1] = (x+1)&MASK;out[2] = 31 if out[0] == before else 47
        r = out[0]+out[1]+out[2]+bool(x)
    elif case == 'memcpy':
        data = bytearray(32);n = x&15
        data[:n] = bytes((x+i)&255 for i in range(n));data[28:32] = (1).to_bytes(4,'little')
        out = list(struct.unpack('<8I', data));r = data[n-1 if n else 0]+1
    elif case == 'memcmp':
        out[0] = MASK if x < 0x80000001 else 1 if x > 0x80000001 else 0;r = out[0]
    elif case == 'strlen':
        out[0] = x&15;r = 7
    elif case == 'callback':
        v = ((5^x)+13)&MASK;out[:4] = [5^x,17,v,(v+7)&MASK];r = 2*v+7
    elif case == 'varargs':
        out[0] = (x+28)&MASK;r = out[0]^0x13579bdf
    elif case == 'float':
        out[0] = (x&0xffff)+2;out[1] = 1;r = out[0]
    elif case == 'tail':
        r = value
    else:
        raise ValueError('no invented result for a nonreturning API')
    return [int(r)&MASK, *out]


def elf_data(path):
    raw = path.read_bytes();elf = ELFFile(io.BytesIO(raw));regions=[];symbols={};relocations=[]
    for s in elf.iter_sections():
        if s['sh_flags']&2 and s['sh_size']:
            regions.append((s['sh_addr'],bytes(s['sh_size']) if s['sh_type']=='SHT_NOBITS' else s.data()))
        if s.name == '.symtab':
            symbols = {x.name:x['st_value'] for x in s.iter_symbols()
                       if x['st_info']['type']=='STT_FUNC' and x['st_shndx']!='SHN_UNDEF'}
        if s['sh_type']=='SHT_RELA':
            table = elf.get_section(s['sh_link'])
            for r in s.iter_relocations():
                sym = table.get_symbol(r['r_info_sym']) if r['r_info_sym'] else None
                relocations.append(dict(address=r['r_offset'],kind=r['r_info_type'],addend=r['r_addend'],
                                        name=sym.name if sym else '',defined=sym['st_shndx']!='SHN_UNDEF' if sym else False,
                                        value=sym['st_value'] if sym else 0))
    plt=elf.get_section_by_name('.plt')
    # x86-64 lld's conventional PLT: resolver + fixed 16-byte entries. These
    # linker thunks have no STT_FUNC and are reported separately from C entries.
    thunks=set()
    if plt:
        assert plt['sh_size']%16==0
        thunks=set(range(plt['sh_addr'],plt['sh_addr']+plt['sh_size'],16))
        for offset in range(16,plt['sh_size'],16):
            assert plt.data()[offset:offset+2]==b'\xff\x25'
    return dict(raw=raw,regions=regions,symbols=symbols,relocations=relocations,thunks=thunks)


def layout(start, entry=False):
    return ("ENTRY(oracle_entry)\n" if entry else '') + '''SECTIONS {
 . = START;
 .text : { *(.text.entry) *(.text .text.*) }
 .plt : { *(.plt .plt.*) }
 . = ALIGN(0x100);
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.*) }
 .bss : { *(.bss .bss.*) *(COMMON) }
 .dynsym : { *(.dynsym) } .dynstr : { *(.dynstr) }
 .hash : { *(.hash) } .gnu.hash : { *(.gnu.hash) }
 .rela.dyn : { *(.rela.dyn) } .rela.plt : { *(.rela.plt) }
 .dynamic : { *(.dynamic) } .got : { *(.got) } .got.plt : { *(.got.plt) }
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
'''.replace('START', hex(start))


def build(caller, provider, work, level, form, args):
    work.mkdir(parents=True,exist_ok=True)
    flags=['--target=x86_64-linux-gnu','-ffreestanding','-fno-builtin','-fno-stack-protector',
           '-fPIC','-fno-asynchronous-unwind-tables','-mno-red-zone','-ffunction-sections','-fdata-sections','-'+level]
    (work/'provider.ld').write_text(layout(0x6000));(work/'caller.ld').write_text(layout(0x1000,True))
    (work/'wrapper.S').write_text(common.WRAPPER)
    common.run([args.clang,*flags,'-c',provider,'-o','provider.o'],work,'provider-compile')
    common.run([args.ld,'-shared','-Bsymbolic','--soname=libfixture-api.so','-T','provider.ld','provider.o','-o','libfixture-api.so'],work,'provider-link')
    common.run([args.clang,*flags,*(['-fno-plt'] if form=='got' else []),'-c',caller,'-o','caller.o'],work,'caller-compile')
    common.run([args.clang,'--target=x86_64-linux-gnu','-c','wrapper.S','-o','wrapper.o'],work,'wrapper-compile')
    common.run([args.ld,'-shared','-Bsymbolic','--gc-sections','-T','caller.ld','wrapper.o','caller.o','-L.','-lfixture-api','-o','caller.elf'],work,'caller-link')
    main,api=elf_data(work/'caller.elf'),elf_data(work/'libfixture-api.so')
    for low,high,obj in ((0x1000,0x5000,main),(0x6000,0x8000,api)):
        assert all(low<=a and a+len(b)<high for a,b in obj['regions']), 'allocated image region overlap'
    assert main['symbols']['oracle_entry']==0x1000
    return main,api


def initial(adapter, main, api, x, trial, known):
    f=dict(code_hex='',stubs=[],initial_sp=base.STACK+8,set_gprs={'RDI':x,'RSI':base.DATA,'RDX':api['symbols']['api_value']})
    regs,memory=adapter.initial(f,trial,0x1000);regs[142]=0
    for obj in ([main,api] if known else [main]):
        for address,data in obj['regions']:memory[address:address+len(data)]=data
    if not known:
        # Decodeable observation markers only; both oracles stop before any
        # marker executes. No unknown API implementation/effect is supplied.
        for address in api['symbols'].values():memory[address]=0x90
    for r in main['relocations']+api['relocations']:
        if r['kind']==8:value=r['addend']
        elif r['kind'] in (1,6,7):
            value=(r['value'] if r['defined'] else api['symbols'][r['name']])+r['addend']
        else:raise RuntimeError(f'unhandled real ELF relocation: {r}')
        base.put(memory,r['address'],value)
    return regs,memory


def is_call(code):
    i=0
    while code[i] in (0x66,0x67,0x26,0x2e,0x36,0x3e,0x64,0x65,0xf2,0xf3) or 0x40<=code[i]<=0x4f:i+=1
    return code[i]==0xe8 or (code[i]==0xff and (code[i+1]>>3)&7==2)


def reference(adapter, regs, memory, api_addresses, known, static):
    # Reuse the full x64 state seeding/serialization oracle, adding only a
    # read-only observation boundary. No API effects or return values are faked.
    original=common.guest.Uc;last=[None,0];stop=[]
    def observed(*args):
        uc=original(*args)
        def hook(u,pc,size,_):
            if pc in api_addresses:
                code=bytes(u.mem_read(last[0],last[1]));call=is_call(code)
                if not known or (static and not call):
                    stop.extend([3 if not static or call else 2,2 if call else 1,last[0],pc,1,last[0]+last[1] if call else 0,0])
                    u.emu_stop();return
            last[:]=[pc,size]
        uc.hook_add(UC_HOOK_CODE,hook)
        return uc
    common.guest.Uc=observed
    try:
        # Adapter's own boundary support constructs the same complete snapshot.
        # If our protocol-specific boundary fires, temporarily present its PC as
        # a declared endpoint to the serializer, with independently decoded kind.
        fixture=dict(stubs=[],missing_targets=[],undefined_flags=[])
        try:return adapter.reference(fixture,regs,memory,0x1000)
        except RuntimeError as error:
            if not stop:raise
            # A separate pass stops using Adapter's existing missing-target
            # mechanism. It never executes the external target's first byte.
            common.guest.Uc=original
            f=dict(stubs=[],missing_targets=[stop[3]],boundary_kind=stop[1],undefined_flags=[])
            snapshot=adapter.reference(f,regs,memory,0x1000)
            return snapshot[:3]+(tuple(stop),)
    finally:common.guest.Uc=original


def compile_module(obj, work, static, args):
    work.mkdir(parents=True,exist_ok=True)
    image=base.dragon.LoadedImage.load_bytes(obj['raw']);graph=image.disassemble(base.dragon.Configuration(strategy=args.strategy))
    entries=sorted(pc for pc in graph.function_addresses() if graph.get_function(pc).is_valid)
    unlifted=[pc for a in graph.block_addresses() for pc in (graph.get_ignil_block(a).unlifted() if graph.get_ignil_block(a) else [])]
    lifter=base.dragon.LLVMLifter(2,execution_mode=2 if static else 1)
    for pc in entries:
        if not lifter.lift(graph.get_function(pc),graph):raise RuntimeError('function lift rejected')
    (work/'unoptimized.ll').write_text(lifter.ir())
    if static:lifter.optimize()
    raw=work/'raw.ll';raw.write_text(lifter.ir())
    common.run([args.opt,'-passes=verify','-disable-output',raw],work,'verify-raw')
    selected=raw
    if static:
        selected=work/'optimized.ll';common.run([args.opt,'-S','-passes=default<O2>',raw,'-o',selected],work,'optimize')
    common.run([args.opt,'-passes=verify','-disable-output',selected],work,'verify-final')
    common.run([args.llc,'-O0','-filetype=obj','-mtriple='+('aarch64-linux-gnu' if platform.machine()=='aarch64' else 'x86_64-linux-gnu'),selected,'-o','module.o'],work,'llc')
    prefix='dragon_static' if static else 'dragon_v2';names=[]
    for pc in entries:
        symbol=f'{prefix}_x86_64_{pc:x}'
        header=re.search(r'define void @'+symbol+r'\([^\n]*\) #(\d+)',raw.read_text())
        attrs=re.search(r'^attributes #'+header[1]+r' = \{([^\n]*)',raw.read_text(),re.M) if header else None
        resume=re.search(r'"dragon.entrypoints"="([^"]*)"',attrs[1]) if attrs else None
        names.extend((int(x,16),symbol) for x in resume[1].split(',')) if resume else names.append((pc,symbol))
    optimized=selected.read_text();forbidden=[]
    sites=[line for line in optimized.splitlines() if '@dragon_static_external(' in line]
    for site in sites:
        group=re.search(r'#(\d+)',site);attributes=re.search(r'^attributes #'+group[1]+r' = \{([^\n]*)',optimized,re.M) if group else None
        text=site+(attributes[1] if attributes else '')
        forbidden.extend(x for x in ('memory(none)','memory(read)','readnone','readonly','willreturn','nounwind') if x in text)
    detail=dict(entries=entries,missing_entries=sorted(set(obj['symbols'].values())-set(entries)),
                extra_entries=sorted(set(entries)-set(obj['symbols'].values())-obj['thunks']),
                synthetic_thunks=sorted(set(entries)&obj['thunks']),unlifted=unlifted,
                unresolved=[i.address for i in graph.instructions() if i.is_jump and i.has_indirect_target and not i.branches],
                external_calls=len(re.findall(r'call void @dragon_static_external\(',optimized)),
                forbidden_attributes=forbidden,unexpected_unreachable='unreachable' in optimized,
                imported_symbols=sorted({s.name for s in image.symbols() if s.imported}),
                import_names_in_ir=sorted({s.name for s in image.symbols() if s.imported and ('!"'+s.name+'"') in optimized}))
    (work/'analysis.json').write_text(json.dumps(detail,indent=2))
    return work/'module.o',names,detail


def link(main_module, provider_module, work, static, known, api, args):
    work.mkdir(parents=True,exist_ok=True)
    obj,names,_=main_module;objects=[obj];names=list(names);declarations=[];wrappers=[]
    if known:
        other,api_names,_=provider_module;objects.append(other)
        for pc,symbol in api_names:
            wrapper='test_api_'+format(pc,'x')
            damage=''
            if args.control=='stale-register':damage='put64(s->regs_bytes,0);'
            elif args.control=='stale-memory':damage='memcpy(m->data+0x8000,saved,32);'
            if damage:
                declarations.append(f'void {symbol}(S*,M*,C*,E*);')
                wrappers.append(f'void {wrapper}(S*s,M*m,C*c,E*e){{unsigned char saved[32];memcpy(saved,m->data+0x8000,32);{symbol}(s,m,c,e);if(e->reason==DRAGON_EXIT_RETURN){{{damage}}}}}')
                names.append((pc,wrapper))
            else:names.append((pc,symbol))
    declarations += [f'void {symbol}(S*,M*,C*,E*);' for symbol in sorted({s for _,s in names})]
    driver=common.compiler.DRIVER.replace('DECLARATIONS','\n'.join(declarations+wrappers))
    driver=driver.replace('ENTRIES',','.join(f'{{{pc}ULL,{symbol}}}' for pc,symbol in names)).replace('INITIAL_COUNT',str(len(names))).replace('INITIAL_BUDGET','1000000').replace('RESUME_CODE','')
    if static:
        external='void dragon_static_external(S*s,M*m,C*c,E*e){uint64_t pc=get64(s->regs_bytes+128);'
        external+=''.join(f'if(pc=={pc}ULL){{{symbol}(s,m,c,e);return;}}' for pc,symbol in names)
        external+='e->reason=DRAGON_EXIT_NEED_TARGET;}\n'
        driver=driver.replace('int main(',external+'int main(').replace('dragon_llvm_execute_v2(&s,&m,&c,&e,0xf000);','dragon_static_x86_64_1000(&s,&m,&c,&e);')
        driver=driver.replace('}; E e={0};','}; E e={0}; c.budget=0;').replace('  f=fopen(argv[2]','  if(c.budget!=0)return 95;\n  f=fopen(argv[2]')
    (work/'driver.c').write_text(driver)
    runtime=Path(base.dragon._LIB).resolve()
    common.run([args.cc,'-O0','-I'+str(base.ROOT/'include'),'-I'+str(base.ROOT/'c_shim'),'driver.c',*objects,
                *([] if static else [runtime,'-Wl,-rpath,'+str(runtime.parent)]),'-o','native'],work,'native-link')
    return work/'native'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--tigress')
    p.add_argument('--case',action='append',choices=CASES);p.add_argument('--profile',action='append',choices=PROFILES)
    p.add_argument('--form',action='append',choices=['plt','got']);p.add_argument('--c-level',action='append',choices=['O0','O2'])
    p.add_argument('--strategy',action='append',type=int,choices=[0,1]);p.add_argument('--trials',type=int,default=17)
    p.add_argument('--control',choices=['c-result','stale-register','stale-memory']);p.add_argument('--expect-control',action='store_true')
    for name,default in [('clang','clang-21'),('ld','ld.lld-21'),('opt','opt-21'),('llc','llc-21'),('cc','gcc')]:p.add_argument('--'+name,default=default)
    args=p.parse_args();cases=args.case or CASES;profiles=args.profile or PROFILES;forms=args.form or ['plt','got'];levels=args.c_level or ['O0','O2'];strategies=args.strategy or [0,1]
    if args.trials<16:p.error('at least 16 boundary inputs required')
    if profiles!=['plain'] and not args.tigress:p.error('Tigress required; absence is not a successful skip')
    if args.expect_control and not args.control:p.error('control required')
    if args.control and cases!=(['write'] if args.control=='stale-memory' else ['return']):p.error('control must select its matching scenario only')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    source=(base.ROOT/'tests/fixtures/tigress-external-caller.c').read_text()
    library=(base.ROOT/'tests/fixtures/tigress-external-provider.c').read_text()
    if args.control=='c-result':library=library.replace('return x&1u?0xffffffffu:0u;','return (x&1u?0xffffffffu:0u)^1u;')
    provider=out/'provider.c';provider.write_text(library);(out/'host.c').write_text(HOST)
    version=common.run([args.tigress,'--version'],out,'tigress-version') if args.tigress else 'not used'
    if args.tigress and 'Linux-x86_64-2.1' not in version:p.error('campaign pinned to Tigress 2.1')
    metadata=dict(cases=cases,profiles=profiles,forms=forms,c_levels=levels,strategies=strategies,trials=args.trials,
                  tigress=version,control=args.control,plans=['v2-O0','product-O2'],unicorn_version=common.unicorn_version,
                  harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  llvm_lifter_sha256=hashlib.sha256((base.ROOT/'lib/LLVMLifter.cpp').read_bytes()).hexdigest(),
                  source_sha256=hashlib.sha256(source.encode()).hexdigest(),provider_sha256=hashlib.sha256(library.encode()).hexdigest(),
                  backend=[dict(path=s,sha256=hashlib.sha256(Path(s).read_bytes()).hexdigest()) for s in sorted({line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines() if line.split()[-1].endswith(('/libdragon.so','/libdragon_c.so'))})])
    (out/'metadata.json').write_text(json.dumps(metadata,indent=2))
    rows=[];hosts=[];c_checks=[];analyses=[];adapter=common.guest.Adapter('x86_64');provider_cache={};provider_hashes={}
    def save():
        planned=sum(1 if c=='stop' else 2 for c in cases)*len(profiles)*len(forms)*len(levels)*len(strategies)*2*args.trials
        s=dict(metadata,planned=planned,executions=len(rows),statuses=dict(collections.Counter(r['status'] for r in rows)),
               root_returns=sum(r.get('root_return',False) for r in rows),api_boundaries=sum(r.get('api_boundary',False) for r in rows),
               unexecuted=planned-sum('trial' in r for r in rows),host_checks=len(hosts),host_statuses=dict(collections.Counter(r['status'] for r in hosts)),
               c_checks=len(c_checks),c_statuses=dict(collections.Counter(r['status'] for r in c_checks)),
               analyses=len(analyses),entry_failures=sum(bool(a['missing_entries'] or a['extra_entries']) for a in analyses),
               loader_import_failures=sum(not a['loader_import_valid'] for a in analyses),
               missing_import_provenance=sum(not a['ir_import_valid'] for a in analyses),
               provider_modules=len(provider_cache),provider_unlifted=sum(len(m[2]['unlifted']) for m in provider_cache.values()),
               unlifted=sum(len(a['unlifted']) for a in analyses),forbidden_attributes=sum(len(a['forbidden_attributes']) for a in analyses),
               fields_compared=sum(r.get('fields',0) for r in rows),memory_bytes_compared=sum(r.get('memory_bytes',0) for r in rows),skipped=0,undefined_exclusions=0)
        (out/'results.json').write_text(json.dumps(dict(summary=s,results=rows,host=hosts,c=c_checks,analyses=analyses),indent=2));return s
    for case in cases:
      for profile in profiles:
        w=out/case/profile;w.mkdir(parents=True,exist_ok=True);input_c=w/'input.c';transformed=w/'transformed.c'
        input_c.write_text('#define CASE '+str(CASES.index(case))+'\n'+source)
        if profile=='plain':transformed.write_bytes(input_c.read_bytes())
        else:
            commands=[]
            if profile=='arithmetic-flatten':commands+=['--Transform=EncodeArithmetic','--Functions=kernel']
            commands+=['--Transform=Flatten','--Functions=kernel','--FlattenDispatch=switch','--FlattenRandomizeBlocks=true','--FlattenSplitBasicBlocks=true']
            # Transform a translation unit with unresolved imports; -c prevents
            # Tigress's compiler driver from trying to link an executable.
            common.run([args.tigress,'-c','--Environment=x86_64:Linux:Gcc:4.6','--Seed=81901901',*commands,'--out='+str(transformed),input_c],w,'tigress')
        for level in levels:
          h=w/level;h.mkdir(exist_ok=True)
          if case!='stop':
            common.run([args.cc,'-'+level,'-ffreestanding','-fno-builtin','-Dmain=tigress_unused_main','-c',transformed,'-o',h/'host-caller.o'],out,case+'-'+profile+'-'+level+'-host-caller')
            for system in (False,True):
                suffix='system' if system else 'fixture'
                common.run([args.cc,'-'+level,'-ffreestanding','-fno-builtin',*(['-DUSE_SYSTEM_LIBC'] if system else []),h/'host-caller.o',provider,out/'host.c','-o',h/('host-'+suffix)],out,case+'-'+profile+'-'+level+'-host-'+suffix)
                for trial in range(args.trials):
                    x=INPUTS[trial%len(INPUTS)];r=subprocess.run([h/('host-'+suffix),str(x)],capture_output=True,timeout=5)
                    got=[int(x) for x in r.stdout.split()] if r.returncode==0 else []
                    hosts.append(dict(case=case,profile=profile,level=level,system_libc=system,trial=trial,status='match' if got==spec(case,x) else 'different',got=got,expected=spec(case,x)))
          for form in forms:
            b=h/form;main_obj,api=build(transformed,provider,b,level,form,args)
            digest=hashlib.sha256(api['raw']).hexdigest()
            if level in provider_hashes:assert provider_hashes[level]==digest,'provider changed across scenarios'
            provider_hashes[level]=digest
            for strategy in strategies:
              args.strategy=strategy
              for static,plan in ((False,'v2-O0'),(True,'product-O2')):
                m=compile_module(main_obj,b/str(strategy)/plan/'caller',static,args)
                expected_import=IMPORT[CASES.index(case)]
                m[2]['expected_import']=expected_import
                m[2]['loader_import_valid']=expected_import in m[2]['imported_symbols'] if expected_import else not m[2]['imported_symbols']
                m[2]['ir_import_valid']=expected_import in m[2]['import_names_in_ir'] if expected_import else not m[2]['import_names_in_ir']
                analyses.append(dict(case=case,profile=profile,level=level,form=form,strategy=strategy,plan=plan,**m[2]))
                key=level,strategy,static
                if key not in provider_cache:provider_cache[key]=compile_module(api,out/'provider-modules'/level/str(strategy)/plan,static,args)
                for known in ([False] if case=='stop' else [False,True]):
                  native_work=b/str(strategy)/plan/('available' if known else 'opaque')
                  exe=link(m,provider_cache[key],native_work,static,known,api,args)
                  for trial in range(args.trials):
                    x=INPUTS[trial%len(INPUTS)];regs,memory=initial(adapter,main_obj,api,x,trial,known)
                    row=dict(case=case,profile=profile,level=level,form=form,strategy=strategy,plan=plan,available=known,trial=trial,x=x)
                    try:expected=reference(adapter,regs,memory,set(api['symbols'].values()),known,static)
                    except Exception as error:row.update(status='reference_error',detail=str(error));rows.append(row);continue
                    if known and expected[3][0]==1:
                        got=[base.get(expected[0],0,4),*struct.unpack('<8I',expected[1][base.DATA:base.DATA+32])]
                        c_checks.append(dict(case=case,profile=profile,level=level,form=form,trial=trial,status='match' if got==spec(case,x) else 'different',got=got,expected=spec(case,x)))
                    actual,error=base.native(exe,regs,memory,native_work,trial,extra_bytes=64)
                    if error:row.update(status=error['kind'],detail=error)
                    else:
                        record=struct.unpack('<8Q',actual[3]);root=record[0]==1 and record[1]==3 and record[3]==base.SENTINEL and record[4]==1
                        boundary=record[0] in (2,3) and record[3] in set(api['symbols'].values()) and bool(record[4])
                        if root or boundary:
                            diffs,_=adapter.compare(dict(undefined_flags=[]),expected,actual,False)
                            if m[2]['unexpected_unreachable']:diffs.append(dict(field='ir',detail='unreachable'))
                            row.update(status='different' if diffs else 'root_match' if root else 'api_boundary_match',differences=diffs,fields=len(adapter.fields),memory_bytes=base.MEM_SIZE)
                        else:row.update(status='incomplete',detail='stopped before expected root/API boundary')
                        row.update(exit=record[:7],expected_exit=expected[3],root_return=root,api_boundary=boundary)
                    rows.append(row)
                print(case,profile,level,form,strategy,plan,dict(collections.Counter(r['status'] for r in rows if r['case']==case and r['profile']==profile and r['level']==level and r['form']==form and r['strategy']==strategy and r['plan']==plan)),flush=True);save()
    summary=save();print(json.dumps(summary,indent=2))
    c_status='different' if args.expect_control and args.control=='c-result' else 'match'
    correct=all(r['status']==c_status for r in hosts+c_checks) and not summary['unexecuted'] and not summary['entry_failures'] and not summary['unlifted'] and not summary['provider_unlifted'] and not summary['forbidden_attributes'] and not summary['loader_import_failures'] and not summary['missing_import_provenance']
    for r in rows:
        wanted='api_boundary_match' if not r['available'] or r.get('expected_exit',[0])[0]!=1 else 'root_match'
        if args.expect_control and r['available'] and wanted=='root_match':
            if args.control=='stale-memory' or args.control=='stale-register' and r['x']&1:wanted='different'
        correct &= r['status']==wanted
    return 0 if correct else 1


if __name__=='__main__':raise SystemExit(main())
