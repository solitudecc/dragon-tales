"""Segment fallback contracts vs the unchanged full-state Guest oracle.

Unsupported cases stop Unicorn before the segment instruction, after a real
prefix with SP/register/memory effects. These are explicit boundary checks,
not complete-function successes or hardware segment-value equivalence tests.
Positive controls execute fully: ignored LEA/long-mode segment prefixes and
0x64/0x65 bytes in immediates/displacements. The independent nonzero-base
Unicorn oracle is separate. Defaults retain the existing eight edge-scheduled
register trials with deterministic random memory/vector/flag state.
"""
import argparse,collections,hashlib,json,os,sys
from pathlib import Path

def check_plan(rows,reports,summary,expected_cases,trials):
    """Audit the requested plan independently of the observed rows/reports."""
    failures=[];levels=('O0','O2');cases=set(expected_cases)
    expected_rows={(case,level,trial) for case in cases for level in levels for trial in range(trials)}
    expected_reports={(case,level) for case in cases for level in levels}
    row_keys=[];report_keys=[];statuses={}
    for row in rows:
        if not isinstance(row,dict) or type(row.get('trial')) is not int:
            failures.append('row missing an integer trial');continue
        row_keys.append((row.get('case'),row.get('opt'),row['trial']))
        status=row.get('status')
        if not isinstance(status,str):failures.append('row missing status')
        else:statuses[status]=statuses.get(status,0)+1
    for report in reports:
        if not isinstance(report,dict):failures.append('invalid report');continue
        report_keys.append((report.get('case'),report.get('opt')))
    if len(row_keys)!=len(expected_rows) or len(set(row_keys))!=len(row_keys) or set(row_keys)!=expected_rows:
        failures.append('row keys differ from requested case/optimization/trial plan')
    if len(report_keys)!=len(expected_reports) or len(set(report_keys))!=len(report_keys) or set(report_keys)!=expected_reports:
        failures.append('report keys differ from requested case/optimization plan')
    for field,value in (('cases',len(cases)),('trials_per_opt',trials),('results',len(rows))):
        if type(summary.get(field)) is not int or summary[field]!=value:
            failures.append('summary '+field+' disagrees with requested plan/rows')
    if summary.get('statuses')!=statuses:failures.append('summary statuses disagree with rows')
    for field in ('register_fields_compared','memory_bytes_compared','undefined_flag_fields'):
        total=sum(row.get(field,0) for row in rows)
        if type(summary.get(field)) is not int or summary[field]!=total:
            failures.append('summary '+field+' disagrees with rows')
    return dict(ok=not failures,failures=failures,expected_cases=sorted(cases),
        expected_rows=len(expected_rows),expected_reports=len(expected_reports))

def control_difference(row,control):
    differences=row.get('differences',[])
    field='memory' if control=='memory' else ('EAX' if row.get('architecture')=='x86_32' else 'RAX')
    return (row.get('status')=='different' and len(differences)==1 and differences[0].get('field')==field
        and (control!='memory' or type(differences[0].get('count')) is int and differences[0]['count']==1))

def self_test():
    """Pure-Python controls: no native library, compilation or execution."""
    import copy
    cases=('a','b');trials=2
    rows=[dict(case=case,opt=level,trial=trial,status='match')
        for case in cases for level in ('O0','O2') for trial in range(trials)]
    reports=[dict(case=case,opt=level) for case in cases for level in ('O0','O2')]
    def summary_for(rs):
        return dict(cases=len({r['case'] for r in rs}),trials_per_opt=trials,results=len(rs),
            statuses=dict(collections.Counter(r['status'] for r in rs)),
            register_fields_compared=0,memory_bytes_compared=0,undefined_flag_fields=0)
    checks={}
    checks['valid_plan']=check_plan(rows,reports,summary_for(rows),cases,trials)['ok']
    missing=[r for r in rows if r['case']!='b'];missing_reports=[r for r in reports if r['case']!='b']
    checks['missing_entire_case_rejected']=not check_plan(missing,missing_reports,summary_for(missing),cases,trials)['ok']
    duplicated=copy.deepcopy(rows);duplicated[1]=dict(duplicated[0])
    checks['duplicate_trial_rejected']=not check_plan(duplicated,reports,summary_for(duplicated),cases,trials)['ok']
    duplicated_reports=copy.deepcopy(reports);duplicated_reports[1]=dict(duplicated_reports[0])
    checks['duplicate_report_rejected']=not check_plan(rows,duplicated_reports,summary_for(rows),cases,trials)['ok']
    bad_summary=summary_for(rows);bad_summary['statuses']={'match':len(rows)-1}
    checks['summary_mismatch_rejected']=not check_plan(rows,reports,bad_summary,cases,trials)['ok']
    damaged=dict(status='different',architecture='x86_64',differences=[dict(field='memory',count=1)])
    checks['single_memory_byte_accepted']=control_difference(damaged,'memory')
    damaged['differences'][0]['count']=2
    checks['multiple_memory_bytes_rejected']=not control_difference(damaged,'memory')
    return dict(checks=checks,passed=sum(checks.values()),total=len(checks),ok=all(checks.values()))

def main():
    parser=argparse.ArgumentParser(description=__doc__,add_help=False)
    parser.add_argument('--mode',choices=('v2','static','product'),default='product')
    parser.add_argument('--strategy',type=int,choices=(0,1),default=0)
    parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--control',choices=('result','memory'))
    parser.add_argument('--expect-control',action='store_true')
    parser.add_argument('--fixtures',type=Path,default=Path(__file__).resolve().parents[2]/'tests/fixtures/x86-segments.json')
    parser.add_argument('--out',type=Path)
    parser.add_argument('--self-test',action='store_true')
    parser.add_argument('--help',action='store_true')
    args,remaining=parser.parse_known_args()
    if args.help:
        parser.print_help();print('\nGuest oracle options: --trials N (default 8), --case ID, --opt, --llc, --cc');return 0
    if args.self_test:
        result=self_test();print(json.dumps(result,sort_keys=True));return 0 if result['ok'] else 1
    if args.out is None:parser.error('--out DIR is required')
    if bool(args.control)!=args.expect_control or args.baseline and args.control:
        parser.error('use matching --control/--expect-control and no baseline controls')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    fixture_data=json.loads(args.fixtures.read_text());ids=[f['id'] for f in fixture_data['cases']]
    if not ids or any(not isinstance(i,str) or not i for i in ids) or len(ids)!=len(set(ids)):
        parser.error('fixture IDs must be nonempty and unique')
    fixtures={f['id']:f for f in fixture_data['cases']}
    if any(f.get('expectation') not in ('complete','unsupported') for f in fixtures.values()):
        parser.error('fixtures must declare complete or unsupported expectation')
    selection=argparse.ArgumentParser(add_help=False)
    selection.add_argument('--case',action='append',dest='cases')
    selection.add_argument('--trials',type=int,default=8)
    requested,_=selection.parse_known_args(remaining)
    selected=set(requested.cases) if requested.cases else set(fixtures);trials=requested.trials
    if not selected or not selected<=set(fixtures):parser.error('unknown or empty case selection')
    if trials<1:parser.error('positive trials required')
    if os.environ.get('DRAGON_TEST_PYTHON'):
        sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON'])
        import dragon
    import function_guest_diff as guest
    from function_static_guest_diff import StaticAdapter
    d=guest.base.dragon
    sys.argv=[sys.argv[0],*remaining,'--out',str(out)]
    if args.control=='memory':sys.argv.append('--corrupt-output')
    reports=[];active={};original_analyze=d.Disassembler.analyze;original_config=d.Configuration.__init__
    def configuration(self,*a,**kw):
        kw['strategy']=args.strategy;original_config(self,*a,**kw)
    def analyze(self,code,address=0,*a,**kw):
        graph=original_analyze(self,code,address,*a,**kw)
        if address==fixture_data['entry']:
            unlifted=set()
            for at in graph.block_addresses():
                block=graph.get_ignil_block(at)
                if block:unlifted.update(block.unlifted())
            f=fixtures[active['case']];expected=set(f.get('unsupported_before',[]))
            reports.append(dict(case=f['id'],opt=active['opt'],unlifted=sorted(unlifted),
                expected_unlifted=sorted(expected),missing_unlifted=sorted(expected-unlifted),
                unexpected_unlifted=sorted(unlifted-expected)))
        return graph
    def compile_case(f,*a,**kw):
        active.update(case=f['id'],opt=a[1])
        return guest.compile.compile_case(f,*a,**kw,static_analysis=args.mode!='v2',
            product_pipeline=args.mode=='product',function_entry=args.mode!='v2')
    parent=guest.Adapter if args.mode=='v2' else StaticAdapter
    class Adapter(parent):
        def reference(self,f,*a,**kw):
            result=super().reference(f,*a,**kw)
            if 'expected_rax' in f:
                assert int.from_bytes(result[0][:8],'little')==f['expected_rax'], 'Unicorn disagrees with the fixed LEA wrap oracle'
            return result
        def compare(self,f,expected,actual,corrupt):
            if args.control=='result':
                damaged=bytearray(actual[0]);damaged[0]^=1;actual=(bytes(damaged),)+actual[1:]
            return super().compare(f,expected,actual,corrupt)
    d.Disassembler.analyze=analyze;d.Configuration.__init__=configuration
    try:
        oracle_code=guest.main(fixtures_path=args.fixtures,compile_factory=compile_case,adapter_factory=Adapter)
    finally:
        d.Disassembler.analyze=original_analyze;d.Configuration.__init__=original_config
    data=json.loads((out/'results.json').read_text());rows=data['results'];summary=data['summary']
    def root(row):
        e=row.get('exit',[]);return len(e)==7 and e[0]==1 and e[1]==3 and e[3]==guest.base.SENTINEL and e[4]==1
    def boundary(row):
        f=fixtures[row['case']];pcs=f.get('unsupported_before',[])
        return len(pcs)==1 and row.get('exit')==[5,0,pcs[0],pcs[0],1,0,2]
    errors=[r for r in rows if r['status'] not in ('match','different')]
    complete=[r for r in rows if fixtures.get(r.get('case'),{}).get('expectation')=='complete']
    unsupported=[r for r in rows if fixtures.get(r.get('case'),{}).get('expectation')=='unsupported']
    matches=lambda rs:sum(r['status']=='match' for r in rs)
    contract_differences=[r for r in unsupported if r['status']=='different' and not boundary(r)]
    old_semantic_differences=[r for r in complete if fixtures[r['case']].get('baseline_different') and
        r['status']=='different' and root(r) and len(r['differences'])==1 and r['differences'][0]['field']=='RAX']
    missing_notes=sum(bool(r['missing_unlifted']) for r in reports)
    unexpected_notes=sum(bool(r['unexpected_unlifted']) for r in reports)
    plan=check_plan(rows,reports,summary,selected,trials);plans_ok=plan['ok']
    if args.control:
        ok=all(control_difference(r,args.control) for r in rows)
        ok=ok and all(root(r) for r in complete) and all(boundary(r) for r in unsupported)
    elif args.baseline:
        # Old code continuing past the requested unsupported PC is a contract
        # difference, not a proof that the old entire CPU implementation is wrong.
        ok=all((r in old_semantic_differences) if fixtures[r['case']].get('baseline_different') else
               (r['status']=='match' and root(r)) for r in complete)
        ok=ok and all((r['status']=='match' and boundary(r)) or r in contract_differences for r in unsupported)
    else:
        ok=oracle_code==0 and all(r['status']=='match' and root(r) for r in complete)
        ok=ok and all(r['status']=='match' and boundary(r) for r in unsupported)
    if not args.baseline:ok=ok and missing_notes==unexpected_notes==0
    mapped={str(Path(d._LIB).resolve())}
    mapped.update(line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
        if '/libdragon' in line and line.split()[-1].startswith('/'))
    exclusions=summary['undefined_flag_fields'];skips=summary['skipped'];normalizations=summary['legacy_normalizations']
    ok=bool(ok and plans_ok and not errors and exclusions==skips==normalizations==0)
    result=dict(mode=args.mode,strategy=args.strategy,baseline=args.baseline,control=args.control,
        comparisons=len(rows),trials_per_opt=trials,cases=len(selected),oracle_returncode=oracle_code,
        complete_root_returns=sum(root(r) for r in complete),complete_root_matches=matches(complete),
        unsupported_boundary_exits=sum(boundary(r) for r in unsupported),unsupported_boundary_matches=matches(unsupported),
        state_or_exit_differences=sum(r['status']=='different' for r in rows),
        baseline_not_rejected=len(contract_differences),baseline_already_rejected=sum(r['status']=='match' and boundary(r) for r in unsupported),
        baseline_complete_semantic_differences=len(old_semantic_differences) if args.baseline else 0,
        missing_unlifted_modules=missing_notes,unexpected_unlifted_modules=unexpected_notes,
        errors=len(errors),exclusions=exclusions,skips=skips,normalizations=normalizations,
        register_fields_compared=summary['register_fields_compared'],memory_bytes_compared=summary['memory_bytes_compared'],
        statuses=dict(collections.Counter(r['status'] for r in rows)),plans_complete=plans_ok,plan_audit=plan,
        control_detected_rows=sum(control_difference(r,args.control) for r in rows) if args.control else 0,
        control_single_memory_byte_rows=sum(control_difference(r,'memory') for r in rows) if args.control=='memory' else 0,
        libraries={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in sorted(mapped)},
        fixture_sha256=hashlib.sha256(args.fixtures.read_bytes()).hexdigest(),
        harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        semantics='Complete positive controls and explicit pre-instruction Unsupported boundaries; no full-function or hardware segment-equivalence claim for boundary cases',ok=ok)
    (out/'segments-results.json').write_text(json.dumps(dict(summary=result,reports=reports),indent=2))
    print('SEGMENTS SUMMARY',json.dumps(result,sort_keys=True))
    return 0 if ok else 1
if __name__=='__main__':raise SystemExit(main())
