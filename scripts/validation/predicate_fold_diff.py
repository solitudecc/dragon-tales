"""Proved LLVM predicates vs complete A64 Unicorn execution, with no exclusions.

Checks both branch polarities and a varying near-miss through V2 and static
product pipelines, O0/O2, all 102 state fields and 64 KiB. --baseline measures
the old pipeline's missing folds; it is not an optimization success. The
result/memory controls corrupt only native output and must be detected.
"""
import argparse,collections,hashlib,json,os,re,sys
from pathlib import Path

if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON'])
    import dragon
import function_guest_diff as guest
import function_dispatch_diff as compiler
from function_static_guest_diff import StaticAdapter
base=guest.base

CASES=[]
for width,even,square in [
    (32,'01040011017c011b2100001261000035e00080d2c0035fd6600c80d2c0035fd6',
        '01040011017c001b2100001261000035e00080d2c0035fd6600c80d2c0035fd6'),
    (64,'01040091017c019b21004092610000b5e00080d2c0035fd6600c80d2c0035fd6',
        '01040091017c009b21004092610000b5e00080d2c0035fd6600c80d2c0035fd6')]:
    opposite=bytearray.fromhex(even);opposite[15]^=1
    for kind,code in [('even',even),('false',opposite.hex()),('square',square)]:
        CASES.append(dict(id=kind+str(width),architecture='arm64',code_hex=code,
            stubs=[],undefined_flags=[],predicate_kind=kind))

def conditional_count(ir):return len(re.findall(r'\bbr i1\b',ir))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--case',action='append',dest='cases')
    parser.add_argument('--trials',type=int,default=8)
    parser.add_argument('--control',choices=('result','memory'))
    parser.add_argument('--expect-control',action='store_true')
    parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--expect-no-z3',action='store_true')
    parser.add_argument('--llc',default='llc-21');parser.add_argument('--opt',default='opt-21');parser.add_argument('--cc',default='gcc')
    args=parser.parse_args()
    if args.trials<1 or bool(args.control)!=args.expect_control:parser.error('positive trials and control/expect-control together required')
    fixtures=[f for f in CASES if not args.cases or f['id'] in args.cases]
    if not fixtures or args.cases and set(args.cases)!={f['id'] for f in fixtures}:parser.error('unknown case')
    args.out.mkdir(parents=True,exist_ok=True)
    rows=[];modules=[];captures=[]
    original_optimize=base.dragon.LLVMLifter.optimize
    proof=getattr(base.dragon.LLVMLifter,'run_predicate_folding',None)
    if proof:
        def capture(self,*a,**kw):
            before=self.ir();stats=proof(self,*a,**kw);after=self.ir()
            captures.append(dict(stats=vars(stats),before=conditional_count(before),after=conditional_count(after)))
            return stats
        base.dragon.LLVMLifter.run_predicate_folding=capture
    else:
        def capture_old(self):
            original_optimize(self)
            captures.append(dict(stats=dict(available=False),before=conditional_count(self.ir()),after=conditional_count(self.ir())))
        base.dragon.LLVMLifter.optimize=capture_old
    try:
        for fixture in fixtures:
            for mode in ('v2','static'):
                adapter=(StaticAdapter if mode=='static' else guest.Adapter)('arm64')
                for level in ('O0','O2'):
                    work=args.out/fixture['id']/mode/level;work.mkdir(parents=True,exist_ok=True)
                    captures.clear()
                    try:
                        exe,bad_ir,sites=compiler.compile_case(fixture,0x1000,level,work,args,
                            architecture='arm64',static_analysis=mode=='static',product_pipeline=True)
                        if len(captures)!=1:raise RuntimeError('one root product optimizer expected')
                        measurement=captures[0]
                        constant=fixture['predicate_kind']!='square'
                        available=measurement['stats']['available']
                        if available==args.expect_no_z3 and not args.baseline:raise RuntimeError('unexpected Z3 availability')
                        shape_ok=(measurement['after']==(0 if available and constant else 1)) if mode=='static' else True
                        if not shape_ok:raise RuntimeError('wrong predicate CFG shape: '+str(measurement))
                        modules.append(dict(case=fixture['id'],mode=mode,opt=level,**measurement,
                            missing_fold=mode=='static' and constant and not available))
                    except Exception as error:
                        detail=error.stderr.decode(errors='replace') if hasattr(error,'stderr') and isinstance(error.stderr,bytes) else str(error)
                        rows.append(dict(case=fixture['id'],mode=mode,opt=level,status='infrastructure_error',detail=detail));continue
                    observed=set()
                    for trial in range(args.trials):
                        row=dict(case=fixture['id'],mode=mode,opt=level,trial=trial)
                        regs,memory=adapter.initial(fixture,trial,0x1000)
                        value=base.EDGES[trial%len(base.EDGES)] if trial<len(base.EDGES) else (trial*0x9123ab97)&((1<<64)-1)
                        base.put(regs,0,value)
                        try:
                            expected=adapter.reference(fixture,regs,memory,0x1000)
                            low=value&1 if fixture['predicate_kind']=='square' else 0
                            result=99 if fixture['predicate_kind']=='false' or low else 7
                            if base.get(expected[0],0)!=result or base.get(expected[0],8)!=low:raise RuntimeError('Unicorn disagrees with independent parity specification')
                            observed.add(result)
                        except Exception as error:row.update(status='reference_error',detail=str(error));rows.append(row);continue
                        actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64,regs_size=adapter.regs_size)
                        if error:row.update(status=error['kind'],detail=error)
                        else:
                            if args.control=='result':
                                damaged=bytearray(actual[0]);damaged[0]^=1;actual=(bytes(damaged),)+actual[1:]
                            differences,record=adapter.compare(fixture,expected,actual,args.control=='memory')
                            if bad_ir:differences.append(dict(field='ir',detail='unreachable in supported fixture'))
                            if record[0]!=1 or record[1]!=3 or record[3]!=base.SENTINEL:differences.append(dict(field='completion',exit=record))
                            row.update(status='different' if differences else 'match',differences=differences,
                                root_return=record[0]==1 and record[1]==3 and record[3]==base.SENTINEL,
                                register_fields_compared=len(adapter.fields),memory_bytes_compared=base.MEM_SIZE,
                                excluded=0,skipped=0)
                        rows.append(row)
                    if fixture['predicate_kind']=='square' and args.trials>=2 and observed!={7,99}:
                        rows.append(dict(case=fixture['id'],mode=mode,opt=level,status='reference_error',detail='varying predicate must take both paths'))
                    print(fixture['id'],mode,level,dict(collections.Counter(r['status'] for r in rows if r['case']==fixture['id'] and r['mode']==mode and r['opt']==level)),flush=True)
    finally:
        base.dragon.LLVMLifter.optimize=original_optimize
        if proof:base.dragon.LLVMLifter.run_predicate_folding=proof
    wanted='different' if args.control else 'match'
    summary=dict(results=len(rows),statuses=dict(collections.Counter(r['status'] for r in rows)),
        planned=len(fixtures)*4*args.trials,root_returns=sum(r.get('root_return',False) for r in rows),
        modules=len(modules),missing_folds=sum(m['missing_fold'] for m in modules),control=args.control,
        baseline=args.baseline,proofs_available=not args.baseline and not args.expect_no_z3,
        exclusions=0,skips=0,normalizations=0)
    mapped={str(Path(base.dragon._LIB).resolve())}
    maps=Path('/proc/self/maps')
    if maps.exists():
        mapped.update(line.split()[-1] for line in maps.read_text().splitlines()
            if '/libdragon' in line and line.split()[-1].startswith('/'))
    summary['libraries']={name:hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sorted(mapped)}
    summary['harness_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (args.out/'results.json').write_text(json.dumps(dict(summary=summary,modules=modules,results=rows),indent=2))
    print(json.dumps(summary,indent=2))
    ok=len(rows)==summary['planned'] and all(r['status']==wanted for r in rows)
    if args.control:
        expected_field='X0' if args.control=='result' else 'memory'
        ok=ok and all(len(r.get('differences',[]))==1 and
            r['differences'][0]['field']==expected_field for r in rows)
    return 0 if ok and (not args.baseline or summary['missing_folds']==0) else 1

if __name__=='__main__':raise SystemExit(main())
