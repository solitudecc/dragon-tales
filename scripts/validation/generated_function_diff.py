"""Seeded x64 function semantics against Unicorn, with generated CFG truth.

Each original/NOP variant is compared to its own machine code over all modelled
registers, flags, PC, and all 64 KiB memory. No cross-layout equivalence claim:
NOP insertion changes code bytes and saved return addresses. Controls compile
an intentionally wrong branch or callee while reference/input bytes stay intact.
Only test code is generated/executed; no arbitrary input executable is run.
"""
import argparse,collections,copy,hashlib,json,random,struct
from pathlib import Path
import function_dispatch_diff as compiler
import function_guest_diff as guest

base=guest.base
ENTRY=0x1000
INPUTS=[0,1,2,0x7fffffff,0x80000000,0xffffffff,0x100000000,
        0x100000001,0x8000000000000000,0xffffffffffffffff]
PLANS=[('v2-O0',False,False,'O0'),('v2-O2',False,False,'O2'),
       ('static-O0',True,False,'O0'),('static-O2',True,False,'O2'),
       ('product-O0',True,True,'O0'),('product-O2',True,True,'O2')]

class Emitter:
    def __init__(self,assembler,nops,start=ENTRY):
        self.asm=assembler;self.nops=nops;self.start=start;self.code=bytearray()
        self.labels={};self.patches=[];self.edges=[];self.sites={};self.listing=[];self.current=None
    @property
    def pc(self):return self.start+len(self.code)
    def label(self,name):self.labels[name]=self.pc;self.current=name
    def insn(self,text):
        if self.nops:self.code.extend(b'\x90')
        self.listing.append(dict(pc=self.pc,asm=text));self.code.extend(self.asm.assemble(text))
    def jump(self,target,fall=None):
        site=self.pc;opcode=b'\x0f\x84' if fall else b'\xe9'
        self.code.extend(opcode+b'\0'*4);self.patches.append((site,len(opcode),target))
        self.sites[site]=self.current;self.edges.append((self.current,target))
        if fall:self.edges.append((self.current,fall))
    def call(self,target):
        site=self.pc;self.code.extend(b'\xe8'+(target-site-5).to_bytes(4,'little',signed=True))
    def finish(self):
        for site,size,target in self.patches:
            offset=site-self.start+size
            self.code[offset:offset+4]=(self.labels[target]-site-size-4).to_bytes(4,'little',signed=True)
        return self.code.hex()

def generate(seed,family,nops,assembler):
    rng=random.Random(seed);e=Emitter(assembler,nops);e.label('entry');stubs=[]
    a=rng.randrange(1,0x10000);b=a+1+rng.randrange(0x10000)
    if family=='diamond':
        e.insn('mov eax, edi');e.insn(f'cmp edi, {rng.choice([0,1,0x80000000,0xffffffff])}')
        e.jump('taken','fall');e.label('fall');e.insn(f'add eax, {a}');e.jump('join')
        e.label('taken');e.insn(f'add eax, {b}');e.jump('join');e.label('join')
    elif family=='loop':
        e.insn('mov eax, edi');e.insn('mov ecx, edi');e.insn('and ecx, 7');e.jump('guard')
        e.label('guard');e.insn('test ecx, ecx');e.jump('done','body')
        e.label('body');e.insn(f'add eax, {a}');e.insn('mov dword ptr [rbx+4], eax');e.insn('dec ecx');e.jump('guard')
        e.label('done')
    elif family=='alias':
        e.insn(f'mov rax, {hex(rng.getrandbits(64))}');e.insn(f'mov ah, {rng.randrange(256)}')
        e.insn('mov cx, ax');e.insn('mov al, cl');e.insn(f'add eax, {a}')
        e.insn('mov qword ptr [rbx+8], rax')
    elif family=='call':
        e.insn(f'mov ecx, {a}');e.insn('xor r15d, r15d');e.call(0x2000)
        e.insn('mov qword ptr [rbx+8], r15')
        def callee(value):
            c=Emitter(assembler,nops,0x2000);c.label('callee')
            for text in ['mov eax, edi','add eax, ecx',f'mov r15, {hex(value)}',
                         'mov dword ptr [rbx+16], eax','cmp eax, ecx','ret']:c.insn(text)
            return c.finish()
        value=rng.getrandbits(64)|1
        stubs=[dict(address=0x2000,code_hex=callee(value))]
        wrong_stub=callee(0)
    else:raise ValueError(family)
    e.insn('mov dword ptr [rbx], eax');e.insn('cmp eax, edi');e.insn('ret')
    code=e.finish()
    f=dict(id=f'{family}-{seed}-'+('nop' if nops else 'plain'),architecture='x86_64',
           code_hex=code,undefined_flags=[],stubs=stubs,set_gprs={'RDI':INPUTS},
           family=family,seed=seed,nops=nops,listing=e.listing,labels=e.labels,
           expected_edges=[[e.labels[x],e.labels[y]] for x,y in e.edges],
           branch_sites={str(pc):e.labels[label] for pc,label in e.sites.items()})
    if family=='diamond':f['invert_offset']=next(iter(e.sites))-ENTRY+1
    if family=='call':f['wrong_stub']=wrong_stub
    return f

def shape_errors(f,nodes,edges,entries):
    errors=[]
    if entries!={ENTRY}:errors.append(dict(field='function_entries',got=sorted(entries),expected=[ENTRY]))
    # CALL can be represented with a continuation split or within a block.
    # Full block-graph truth is checked for the three no-CALL families only.
    if f['family']!='call':
        expected_nodes=set(f['labels'].values());expected_edges={tuple(x) for x in f['expected_edges']}
        if nodes!=expected_nodes:errors.append(dict(field='cfg_nodes',got=sorted(nodes),expected=sorted(expected_nodes)))
        if edges!=expected_edges:errors.append(dict(field='cfg_edges',got=sorted(edges),expected=sorted(expected_edges)))
    return errors

def check_shape(f,strategy):
    graph=base.dragon.Disassembler(2,base.dragon.Configuration(strategy=strategy)).analyze(bytes.fromhex(f['code_hex']),ENTRY)
    root=graph.get_function(ENTRY)
    if not root or not root.is_valid:return [dict(field='root',detail='missing or invalid')]
    nodes=set(root.block_addresses());edges={(pc,t) for pc in nodes for t in graph.get_block(pc).successors()}
    entries={pc for pc in graph.function_addresses() if graph.get_function(pc) and graph.get_function(pc).is_valid}
    return shape_errors(f,nodes,edges,entries)

def reference(f,regs,memory):
    edges=set();previous=[None];original=guest.Uc
    def observed(*args):
        uc=original(*args)
        def hook(u,pc,size,data):
            pc=u.reg_read(guest.X.UC_X86_REG_RIP)
            if str(previous[0]) in f['branch_sites']:edges.add((f['branch_sites'][str(previous[0])],pc))
            previous[0]=pc
        uc.hook_add(guest.UC_HOOK_CODE,hook)
        return uc
    guest.Uc=observed
    try:result=guest.Adapter('x86_64').reference(f,regs,memory,ENTRY)
    finally:guest.Uc=original
    return result,edges

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--seeds',type=int,default=4);p.add_argument('--first-seed',type=int,default=1901)
    p.add_argument('--trials',type=int,default=16);p.add_argument('--family',action='append',choices=['diamond','alias','loop','call'])
    p.add_argument('--control',choices=['invert-branch','stale-call']);p.add_argument('--expect-control',action='store_true')
    p.add_argument('--opt',default='opt-21');p.add_argument('--llc',default='llc-21');p.add_argument('--cc',default='gcc');args=p.parse_args()
    if args.seeds<1 or args.trials<8:p.error('positive seeds and at least 8 boundary inputs required')
    if args.expect_control and not args.control:p.error('--expect-control requires an explicit control')
    families=args.family or (['diamond'] if args.control=='invert-branch' else ['call'] if args.control else ['diamond','alias','loop','call'])
    if args.control and families!=(['diamond'] if args.control=='invert-branch' else ['call']):p.error('control must select only its matching family')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    asm=base.dragon.Assembler(2)
    fixtures=[generate(seed,family,nops,asm) for seed in range(args.first_seed,args.first_seed+args.seeds) for family in families for nops in (False,True)]
    (out/'fixtures.json').write_text(json.dumps(dict(schema=1,entry=ENTRY,cases=fixtures),indent=2))
    # Negative controls for CFG reporting itself: omitted edge, extra entry.
    probe=generate(args.first_seed,'diamond',False,asm);nodes=set(probe['labels'].values());edges={tuple(x) for x in probe['expected_edges']}
    assert not shape_errors(probe,nodes,edges,{ENTRY})
    assert shape_errors(probe,nodes,edges-{next(iter(edges))},{ENTRY})
    assert shape_errors(probe,nodes,edges,{ENTRY,0x9999})
    rows=[];shapes=[];coverage=[];adapter=guest.Adapter('x86_64')
    for f in fixtures:
        for strategy in (0,1):shapes.append(dict(case=f['id'],strategy=strategy,scope='entry_only' if f['family']=='call' else 'entry_and_cfg',errors=check_shape(f,strategy)))
        for plan,static,product,level in PLANS:
            work=out/f['id']/plan;work.mkdir(parents=True,exist_ok=True)
            native_fixture=copy.deepcopy(f)
            if args.control=='invert-branch':
                code=bytearray.fromhex(f['code_hex']);code[f['invert_offset']]^=1;native_fixture['code_hex']=code.hex()
            if args.control=='stale-call':native_fixture['stubs'][0]['code_hex']=f['wrong_stub']
            (work/'native-fixture.json').write_text(json.dumps(native_fixture,indent=2))
            observed=set()
            try:
                exe,bad_ir,_=compiler.compile_case(native_fixture,ENTRY,level,work,args,static_analysis=static,product_pipeline=product)
                for trial in range(args.trials):
                    regs,memory=adapter.initial(f,trial,ENTRY);expected,trace=reference(f,regs,memory);observed.update(trace)
                    actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64)
                    row=dict(case=f['id'],plan=plan,trial=trial)
                    if error:row.update(status=error['kind'],detail=error)
                    else:
                        differences,record=adapter.compare(f,expected,actual,False)
                        if bad_ir:differences.append(dict(field='ir',detail='unexpected unreachable'))
                        if record[0]!=1 or record[1]!=3 or record[3]!=base.SENTINEL or record[4]!=1:differences.append(dict(field='root_return',got=record[:7]))
                        row.update(status='different' if differences else 'match',differences=differences,exit=record[:7],
                                   register_fields_compared=len(adapter.fields),memory_bytes_compared=base.MEM_SIZE)
                    rows.append(row)
            except Exception as error:
                detail=getattr(error,'stderr',None);rows.append(dict(case=f['id'],plan=plan,status='infrastructure_error',detail=detail.decode(errors='replace') if isinstance(detail,bytes) else str(error)))
            wanted={tuple(x) for x in f['expected_edges']};coverage.append(dict(case=f['id'],plan=plan,expected=sorted(wanted),observed=sorted(observed),complete=observed==wanted))
        print(f['id'],dict(collections.Counter(r['status'] for r in rows if r['case']==f['id'])),flush=True)
    summary=dict(cases=len(fixtures),plans=len(PLANS),results=len(rows),statuses=dict(collections.Counter(r['status'] for r in rows)),
                 function_entry_checks=len(shapes),cfg_checks=sum(x['scope']=='entry_and_cfg' for x in shapes),cfg_failures=sum(bool(x['errors']) for x in shapes),
                 edge_coverage_checks=sum(bool(x['expected']) for x in coverage),edge_coverage_complete=sum(x['complete'] and bool(x['expected']) for x in coverage),
                 checker_negative_controls=2,skipped=0,undefined_exclusions=0,control=args.control,
                 root_returns=sum(r.get('exit',[0])[0]==1 and r['exit'][3]==base.SENTINEL for r in rows),
                 register_fields_compared=sum(r.get('register_fields_compared',0) for r in rows),memory_bytes_compared=sum(r.get('memory_bytes_compared',0) for r in rows),
                 scope='x64 generated functions and sampled paths; not universal equivalence proof',
                 source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                 llvm_lifter_sha256=hashlib.sha256((base.ROOT/'lib/LLVMLifter.cpp').read_bytes()).hexdigest())
    (out/'results.json').write_text(json.dumps(dict(summary=summary,cfg=shapes,coverage=coverage,results=rows),indent=2));print(json.dumps(summary,indent=2))
    wanted='different' if args.expect_control else 'match'
    correct=len(rows)==len(fixtures)*len(PLANS)*args.trials and all(r['status']==wanted for r in rows) and not summary['cfg_failures'] and all(x['complete'] for x in coverage)
    for row in rows:
        if row['status']!=wanted:print('FAIL',json.dumps(row))
    for row in shapes:
        if row['errors']:print('CFG FAIL',json.dumps(row))
    return 0 if correct else 1

if __name__=='__main__':raise SystemExit(main())
