"""Static function-entry views vs full Unicorn execution and rejected-PC contracts.

Normal entry executes Guest bytes, including calls/loops/returns. Rejected entry
must preserve every modelled field and all 64 KiB, without executing a byte.
The resumable view is separately checked from real interior PCs. Controls alter
only native output or deliberately substitute resumable IR for a restricted view.
"""
import argparse,collections,hashlib,json,os,re,sys
from pathlib import Path
if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON'])
    import dragon
import function_dispatch_diff as compiler
from function_static_guest_diff import StaticAdapter,fixtures as guest_fixtures
base=compiler.base
ENTRY=0x1000

def fixtures():
    names={arch+'_'+kind for arch in ('x86_32','x86_64','arm64') for kind in
        ('ret','call','nested','loop_calls','recursive_calls','indirect_register','indirect_tail',
         'internal_caller_first','internal_callee_first','redirect','return_zero')}
    names.update(('x86_64_control_loop','x86_64_overlapping_paths','x86_32_overlapping_paths',
                  'x86_64_import_thunk','x86_32_import_thunk','arm64_ret_x9','arm64_ret_xzr'))
    names.update(('x86_64_image_pe','x86_64_image_elf','x86_32_image_elf','arm64_image_elf'))
    names.remove('arm64_redirect')
    selected=[f for f in guest_fixtures()['cases'] if f['id'] in names]
    if {f['id'] for f in selected}!=names:raise RuntimeError('fixture selection drift')
    for arch in ('x86_32','x86_64','arm64'):
        selected.append(dict(id=arch+'_entry_definition',architecture=arch,
            code_hex='e000805201000014600200b9c0035fd6' if arch=='arm64' else 'b807000000eb008903c3',
            interior=0x1008 if arch=='arm64' else 0x1007,undefined_flags=[],stubs=[]))
    if any(f['undefined_flags'] for f in selected):raise RuntimeError('no flag exclusions allowed')
    return selected

def entries(ir,symbol):
    header=re.search(r'define void @'+symbol+r'\([^\n]*\) #(\d+)',ir)
    attrs=re.search(r'^attributes #'+header[1]+r' = \{([^\n]*)',ir,re.M) if header else None
    found=re.search(r'"dragon.entrypoints"="([^"]*)"',attrs[1]) if attrs else None
    if not found:raise RuntimeError('missing entry metadata')
    return [int(s,16) for s in found[1].split(',')]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--case',action='append',dest='cases')
    p.add_argument('--trials',type=int,default=8);p.add_argument('--baseline',action='store_true')
    p.add_argument('--control',choices=('result','memory','entry'));p.add_argument('--expect-control',action='store_true')
    for command in ('opt','llc','cc'):p.add_argument('--'+command,default=command+'-21' if command!='cc' else 'gcc')
    args=p.parse_args()
    if args.trials<1 or bool(args.control)!=args.expect_control:p.error('positive trials and matching control flags required')
    selected=[f for f in fixtures() if not args.cases or f['id'] in args.cases]
    if not selected or args.cases and {f['id'] for f in selected}!=set(args.cases):p.error('unknown case')
    if args.control=='entry' and any('interior' not in f for f in selected):p.error('entry control requires entry_definition cases')
    args.out.mkdir(parents=True,exist_ok=True);rows=[];modules=[]
    for f in selected:
        adapter=StaticAdapter(f['architecture'])
        for restricted in (False,True):
            for level in ('O0','O2'):
                view='entry' if restricted else 'resumable';work=args.out/f['id']/view/level;work.mkdir(parents=True,exist_ok=True)
                specialized=restricted and not args.baseline and args.control!='entry'
                try:
                    exe,bad_ir,sites=compiler.compile_case(f,ENTRY,level,work,args,architecture=f['architecture'],
                        static_analysis=True,product_pipeline=True,function_entry=specialized)
                    ir=(work/'raw.ll').read_text();symbol=f'dragon_static_{"entry_" if specialized else ""}{f["architecture"]}_1000'
                    accepted=entries(ir,symbol)
                    modules.append(dict(case=f['id'],view=view,opt=level,entries=accepted,
                        missing_entry_contract=restricted and accepted!=[ENTRY]))
                    if bad_ir:raise RuntimeError('unexpected unreachable')
                except Exception as error:
                    rows.append(dict(case=f['id'],view=view,opt=level,status='infrastructure_error',detail=str(error)));continue
                # Every valid entry uses an independent machine execution.
                for trial in range(args.trials):
                    pcs=[ENTRY]+([f['interior']] if 'interior' in f and not restricted else [])
                    if restricted:
                        pcs += ([f['interior']] if args.control=='entry' else sorted(
                            {0,ENTRY+1,(1<<64)-1}|({f['interior']} if 'interior' in f else set())))
                    for pc in pcs:
                        reject=restricted and pc!=ENTRY
                        row=dict(case=f['id'],architecture=f['architecture'],view=view,opt=level,trial=trial,pc=pc,rejected_entry=reject)
                        regs,memory=adapter.initial(f,trial,ENTRY);base.put(regs,adapter.pc_offset,pc)
                        if reject:expected=(bytes(regs),bytes(memory),collections.Counter(),(5,0,pc,pc,1,0,5))
                        else:
                            try:expected=adapter.reference(f,regs,memory,pc)
                            except Exception as error:
                                row.update(status='reference_error',detail=str(error));rows.append(row);continue
                        actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64,regs_size=adapter.regs_size)
                        if error:row.update(status=error['kind'],detail=error)
                        else:
                            if args.control=='result':
                                damaged=bytearray(actual[0]);damaged[0]^=1;actual=(bytes(damaged),)+actual[1:]
                            differences,record=adapter.compare(f,expected,actual,args.control=='memory')
                            row.update(status='different' if differences else 'match',differences=differences,
                                root_return=record[0]==1 and record[1]==3 and record[3]==base.SENTINEL,
                                register_fields_compared=len(adapter.fields),memory_bytes_compared=base.MEM_SIZE)
                        rows.append(row)
                print(f['id'],view,level,dict(collections.Counter(r['status'] for r in rows if
                    r['case']==f['id'] and r['view']==view and r['opt']==level)),flush=True)
    summary=dict(cases=len(selected),modules=len(modules),results=len(rows),
        statuses=dict(collections.Counter(r['status'] for r in rows)),
        valid_entry_checks=sum(not r.get('rejected_entry',False) for r in rows),
        rejected_entry_checks=sum(r.get('rejected_entry',False) for r in rows),
        root_returns=sum(r.get('root_return',False) for r in rows),
        missing_entry_contracts=sum(m['missing_entry_contract'] for m in modules),
        exclusions=0,skips=0,normalizations=0,baseline=args.baseline,control=args.control)
    mapped={str(Path(base.dragon._LIB).resolve())}
    if Path('/proc/self/maps').exists():mapped.update(line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
        if '/libdragon' in line and line.split()[-1].startswith('/'))
    summary['libraries']={name:hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sorted(mapped)}
    summary['harness_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (args.out/'results.json').write_text(json.dumps(dict(summary=summary,modules=modules,results=rows),indent=2))
    print(json.dumps(summary,indent=2))
    if not args.control:ok=all(r['status']=='match' for r in rows) and not summary['missing_entry_contracts']
    elif args.control=='entry':
        ok=all((r['status']=='different' and r.get('root_return',False) and
                any(d['field']=='exit' for d in r.get('differences',[]))) if r['rejected_entry']
               else r['status']=='match' for r in rows)
    else:ok=all(r['status']=='different' and len(r.get('differences',[]))==1 and r['differences'][0]['field']==
        ('memory' if args.control=='memory' else 'X0' if r['architecture']=='arm64' else 'EAX' if r['architecture']=='x86_32' else 'RAX') for r in rows)
    return 0 if ok and len(modules)==len(selected)*4 else 1

if __name__=='__main__':raise SystemExit(main())
