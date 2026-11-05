"""Word-level MBA rewrites vs Python specifications and complete Unicorn state.

36 fixed A64/x64 functions, both product execution modes and LLVM O0/O2.
All modelled fields and 64 KiB are compared, without flag exclusions or Legacy
normalizations. Old libraries must fail the simplification gate even when all
semantic comparisons match. Negative controls corrupt only native output.
"""
import argparse,collections,hashlib,json,os,random,sys
from pathlib import Path

if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON'])
    import dragon
import function_guest_diff as guest
import function_dispatch_diff as compiler
from function_static_guest_diff import StaticAdapter
from block_deobf_study import return_cone
base=guest.base

def input_values(width,trial):
    mask=(1<<width)-1
    edges=[0,1,2,mask,1<<(width-1),(1<<(width-1))-1,mask//3,2*(mask//3)]
    values=[edges[(trial+i)%8] if trial<8 else random.Random(42001+trial*3+i).getrandbits(width) for i in (0,3,5)]
    if width==32 and trial&1:values=[v|(0xd34db33f<<32) for v in values]
    return values

def specification(f,a,b,c):
    mask=(1<<f['width'])-1;a&=mask;b&=mask;c&=mask
    return {'add':a+b,'xor':a^b,'or':a|b,'and':a&b,'bitwise':a^b,
        'sub':a-b,'nonlinear':a*a-a,'mask':(a&128)+2*(a&b),
        'alias':a+1,'three':a^b^c}[f['kind']]&mask

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--case',action='append',dest='cases')
    p.add_argument('--trials',type=int,default=8);p.add_argument('--baseline',action='store_true')
    p.add_argument('--expect-no-z3',action='store_true');p.add_argument('--control',choices=('result','memory'))
    p.add_argument('--expect-control',action='store_true')
    p.add_argument('--opt',default='opt-21');p.add_argument('--llc',default='llc-21');p.add_argument('--cc',default='gcc')
    args=p.parse_args()
    if args.trials<1 or bool(args.control)!=args.expect_control:p.error('positive trials and matching control flags required')
    data=json.loads((base.ROOT/'tests/fixtures/llvm-mba-functions.json').read_text());entry=data['entry']
    fixtures=[f for f in data['cases'] if not args.cases or f['id'] in args.cases]
    if not fixtures or args.cases and set(args.cases)!={f['id'] for f in fixtures}:p.error('unknown case')
    if any(f['undefined_flags'] for f in fixtures):raise RuntimeError('MBA campaign permits no flag exclusions')
    args.out.mkdir(parents=True,exist_ok=True);rows=[];modules=[];captures=[]
    proof=getattr(base.dragon.LLVMLifter,'run_mba_simplification',None)
    original=base.dragon.LLVMLifter.optimize
    if proof:
        def capture(self,*a,**kw):
            before=self.ir();s=proof(self,*a,**kw)
            if args.expect_no_z3 and (s.available or s.expressions_simplified or self.ir()!=before):
                raise RuntimeError('disabled MBA stage changed IR or reported available')
            captures.append(vars(s));return s
        base.dragon.LLVMLifter.run_mba_simplification=capture
    else:
        def old(self):
            original(self);captures.append(dict(available=False,expressions_simplified=0))
        base.dragon.LLVMLifter.optimize=old
    try:
        for f in fixtures:
            arch=f['architecture'];adapter_class=guest.Adapter
            for mode in ('v2','static'):
                adapter=(StaticAdapter if mode=='static' else adapter_class)(arch)
                for level in ('O0','O2'):
                    work=args.out/f['id']/mode/level;work.mkdir(parents=True,exist_ok=True);captures.clear()
                    try:
                        exe,bad_ir,sites=compiler.compile_case(f,entry,level,work,args,architecture=arch,
                            static_analysis=mode=='static',product_pipeline=True)
                        if len(captures)!=1:raise RuntimeError('one root product optimizer expected')
                        stats=captures[0]
                        if not args.baseline and stats['available']==args.expect_no_z3:raise RuntimeError('unexpected Z3 availability')
                        ir=(work/'raw.ll').read_text();symbol=f'dragon_{"static" if mode=="static" else "v2"}_{arch}_{entry:x}'
                        cone,ops=return_cone(ir,symbol)
                        positive=mode=='static' and f['kind'] in ('add','xor')
                        target=(3 if arch=='arm64' else 2) if f['width']==32 else 1
                        if positive and cone is None:raise RuntimeError('missing return-value cone')
                        missing=positive and cone>target and not args.expect_no_z3
                        modules.append(dict(case=f['id'],architecture=arch,mode=mode,opt=level,stats=stats,
                            return_cone=cone,return_ops=ops,missing_simplification=missing))
                    except Exception as error:
                        detail=error.stderr.decode(errors='replace') if hasattr(error,'stderr') and isinstance(error.stderr,bytes) else str(error)
                        rows.append(dict(case=f['id'],mode=mode,opt=level,status='infrastructure_error',detail=detail));continue
                    for trial in range(args.trials):
                        row=dict(case=f['id'],architecture=arch,mode=mode,opt=level,trial=trial)
                        regs,memory=adapter.initial(f,trial,entry);a,b,c=input_values(f['width'],trial)
                        offsets=(0,8,16) if arch=='arm64' else (56,48,16)
                        for off,value in zip(offsets,(a,b,c)):base.put(regs,off,value)
                        if f['kind']=='alias':
                            address=base.DATA+[0,1,2,0xffd,0xfff,8,15,31][trial%8]
                            base.put(regs,152 if arch=='arm64' else 24,address);base.put(memory,address,a&0xffffffff,4)
                        try:
                            expected=adapter.reference(f,regs,memory,entry)
                            value=base.get(expected[0],0)
                            want=specification(f,a,b,c)
                            if value!=want:raise RuntimeError(f'Unicorn/Python result disagreement: {value:#x} != {want:#x}')
                        except Exception as error:
                            row.update(status='reference_error',detail=str(error));rows.append(row);continue
                        actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64,regs_size=adapter.regs_size)
                        if error:row.update(status=error['kind'],detail=error)
                        else:
                            if args.control=='result':
                                damage=bytearray(actual[0]);damage[0]^=1;actual=(bytes(damage),)+actual[1:]
                            differences,record=adapter.compare(f,expected,actual,args.control=='memory')
                            if bad_ir:differences.append(dict(field='ir',detail='unreachable in supported fixture'))
                            complete=record[0]==1 and record[1]==3 and record[3]==base.SENTINEL
                            if not complete:differences.append(dict(field='completion',exit=record))
                            row.update(status='different' if differences else 'match',differences=differences,
                                root_return=complete,register_fields_compared=len(adapter.fields),
                                memory_bytes_compared=base.MEM_SIZE,excluded=0,skipped=0)
                        rows.append(row)
                    print(f['id'],mode,level,dict(collections.Counter(r['status'] for r in rows if r['case']==f['id'] and r['mode']==mode and r['opt']==level)),flush=True)
    finally:
        base.dragon.LLVMLifter.optimize=original
        if proof:base.dragon.LLVMLifter.run_mba_simplification=proof
    summary=dict(results=len(rows),planned=len(fixtures)*4*args.trials,
        statuses=dict(collections.Counter(r['status'] for r in rows)),modules=len(modules),
        root_returns=sum(r.get('root_return',False) for r in rows),
        missing_simplifications=sum(m['missing_simplification'] for m in modules),
        control=args.control,baseline=args.baseline,proofs_available=not args.baseline and not args.expect_no_z3,
        exclusions=0,skips=0,normalizations=0)
    mapped={str(Path(base.dragon._LIB).resolve())}
    if Path('/proc/self/maps').exists():
        mapped.update(line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
            if '/libdragon' in line and line.split()[-1].startswith('/'))
    summary['libraries']={name:hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sorted(mapped)}
    summary['harness_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    summary['fixtures_sha256']=hashlib.sha256((base.ROOT/'tests/fixtures/llvm-mba-functions.json').read_bytes()).hexdigest()
    (args.out/'results.json').write_text(json.dumps(dict(summary=summary,modules=modules,results=rows),indent=2))
    print(json.dumps(summary,indent=2))
    wanted='different' if args.control else 'match'
    ok=len(rows)==summary['planned'] and all(r['status']==wanted for r in rows)
    if args.control:
        ok=ok and all(len(r.get('differences',[]))==1 and r['differences'][0]['field']==
            ('memory' if args.control=='memory' else 'X0' if r['architecture']=='arm64' else 'RAX') for r in rows)
    return 0 if ok and summary['missing_simplifications']==0 else 1

if __name__=='__main__':raise SystemExit(main())
