"""Opt-in finite-dispatch recovery vs strict full-state Unicorn executions.

Reuses the unchanged Guest oracle (all modelled fields and all 64 KiB). Both
decoding strategies and V2/static/product modes use the same machine fixtures.
An independent trace checks recovered successors; no observed edge repairs CFG.
Baseline exits before root are incomplete, never semantic successes.
"""
import argparse,collections,hashlib,json,os,sys
from pathlib import Path
if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON'])
    import dragon
import function_guest_diff as guest
from function_static_guest_diff import StaticAdapter
d=guest.base.dragon

def main():
    p=argparse.ArgumentParser(add_help=False)
    p.add_argument('--mode',choices=('v2','static','product'),default='product')
    p.add_argument('--function-entry',action='store_true')
    p.add_argument('--strategy',type=int,choices=(0,1),default=0)
    p.add_argument('--baseline',action='store_true')
    p.add_argument('--prove-leaf-calls',action='store_true')
    p.add_argument('--expect-incomplete',action='store_true',help='require every root attempt to stop incomplete')
    p.add_argument('--fixtures',type=Path,default=guest.base.ROOT/'tests/fixtures/finite-dispatch.json')
    p.add_argument('--control',choices=('result','memory','edge'))
    p.add_argument('--expect-control',action='store_true')
    args,remaining=p.parse_known_args()
    if args.function_entry and args.mode=='v2':p.error('function entry requires static/product mode')
    if bool(args.control)!=args.expect_control or (args.baseline or args.expect_incomplete) and args.control:
        p.error('use matching control/expect-control and no baseline controls')
    out=Path(remaining[remaining.index('--out')+1]).resolve();out.mkdir(parents=True,exist_ok=True)
    sys.argv=[sys.argv[0],*remaining]
    if args.control=='memory':sys.argv.append('--corrupt-output')
    reports=[];observations=[];graphs={};active={}
    original_analyze=d.Disassembler.analyze
    original_config=d.Configuration.__init__
    def configuration(self,*a,**kw):
        kw['strategy']=args.strategy
        original_config(self,*a,**kw)
    def analyze(self,code,address=0,*a,**kw):
        graph=original_analyze(self,code,address,*a,**kw)
        before={i.address:tuple(i.branches) for i in graph.instructions() if i.is_jump and i.has_indirect_target}
        if not args.baseline:
            with d.PassPipeline() as pipeline:
                options={'prove_leaf_calls':1} if args.prove_leaf_calls else {}
                pipeline.register_finite_dispatch(code,address,entry_points=[address]).add('finite-dispatch',**options)
                report=pipeline.run(graph)
                assert report['ok'] and not report['budget_exhausted']
                reports.append(dict(case=active['case'],opt=active['opt'],report=report))
        after={i.address:tuple(i.branches) for i in graph.instructions() if i.is_jump and i.has_indirect_target}
        graphs.setdefault(active['case'],{}).update(after)
        reports.append(dict(case=active['case'],opt=active['opt'],before=before,after=after))
        return graph
    d.Disassembler.analyze=analyze;d.Configuration.__init__=configuration
    def compile_case(f,*a,**kw):
        active.update(case=f['id'],opt=a[1])
        return guest.compile.compile_case(f,*a,**kw,
            static_analysis=args.mode!='v2',product_pipeline=args.mode=='product',
            function_entry=args.function_entry)
    parent=guest.Adapter if args.mode=='v2' else StaticAdapter
    class Adapter(parent):
        def reference(self,f,regs,memory,entry):
            original_uc=guest.Uc;trace=[]
            def tracked(*a,**kw):
                uc=original_uc(*a,**kw)
                def hook(u,pc,size,data):trace.append(u.reg_read(self.pc_reg))
                uc.hook_add(guest.UC_HOOK_CODE,hook)
                return uc
            guest.Uc=tracked
            try:result=super().reference(f,regs,memory,entry)
            finally:guest.Uc=original_uc
            observed=[]
            edges=graphs[f['id']]
            if args.control=='edge':
                edges={site:tuple(x for x in targets if x!=min(f['expected_targets'])) for site,targets in edges.items()}
            for source,target in zip(trace,trace[1:]):
                if source!=f['indirect_site']:continue
                published=edges.get(source,())
                observed.append(target)
                observations.append(dict(case=f['id'],source=source,target=target,
                    resolved=bool(published),missing=bool(published) and target not in published,
                    omitted_control=args.control=='edge' and target==min(f['expected_targets']) and target not in published))
            assert observed, 'reference did not execute the dispatcher'
            assert set(observed).issubset(f['expected_targets']), 'fixture target specification disagrees with Unicorn'
            return result
        def compare(self,f,expected,actual,corrupt):
            if args.control=='result':
                damaged=bytearray(actual[0]);damaged[0]^=1
                actual=(bytes(damaged),)+actual[1:]
            return super().compare(f,expected,actual,corrupt)
    try:
        code=guest.main(fixtures_path=args.fixtures,
                        compile_factory=compile_case,adapter_factory=Adapter)
    finally:
        d.Disassembler.analyze=original_analyze;d.Configuration.__init__=original_config
    raw=json.loads((out/'results.json').read_text())
    rows=raw['results'];summary=raw['summary']
    root=lambda r:r.get('exit',[0])[0]==1 and r['exit'][1]==3 and r['exit'][3]==guest.base.SENTINEL
    complete=sum(root(r) for r in rows)
    incomplete=sum('exit' in r and not root(r) for r in rows)
    errors=[r for r in rows if r['status'] not in ('match','different')]
    wrong=sum(o['missing'] for o in observations)
    fixture_map={f['id']:f for f in json.loads(args.fixtures.read_text())['cases']}
    selected={r['case'] for r in rows}
    coverage={case:sorted({o['target'] for o in observations if o['case']==case})==
                   sorted(fixture_map[case]['expected_targets']) for case in selected}
    precise=all(set(graphs[case].get(fixture_map[case]['indirect_site'],()))==
                set(fixture_map[case]['expected_targets']) for case in selected)
    if args.control=='edge':
        ok=not errors and all(r['status']=='match' and root(r) for r in rows) and wrong>0 and any(o['omitted_control'] for o in observations)
    elif args.control:
        ok=not errors and all(r['status']=='different' and root(r) and len(r['differences'])==1 and
            r['differences'][0]['field']==('memory' if args.control=='memory' else ('EAX' if r['architecture']=='x86_32' else 'RAX')) for r in rows) and wrong==0
    elif args.baseline or args.expect_incomplete:
        ok=not errors and incomplete==len(rows) and complete==0 and wrong==0
    else:
        ok=code==0 and not errors and complete==len(rows) and wrong==0 and precise
    ok=ok and all(coverage.values())
    mapped={str(Path(d._LIB).resolve())}
    mapped.update(line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
        if '/libdragon' in line and line.split()[-1].startswith('/'))
    result=dict(mode=args.mode,strategy=args.strategy,baseline=args.baseline,control=args.control,
        prove_leaf_calls=args.prove_leaf_calls,
        function_entry=args.function_entry,
        expect_incomplete=args.expect_incomplete,
        fixture_sha256=hashlib.sha256(args.fixtures.read_bytes()).hexdigest(),
        harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        complete_root_returns=complete,incomplete_exits=incomplete,
        complete_semantic_differences=sum(root(r) and r['status']=='different' for r in rows),
        infrastructure_errors=len(errors),observed_transfers=len(observations),wrong_edges=wrong,
        unresolved_transfers=sum(not o['resolved'] for o in observations),
        observed_target_coverage=coverage,exact_fixture_target_sets=precise,
        libraries={name:hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in sorted(mapped)},
        exclusions=summary['undefined_flag_fields'],skips=summary['skipped'],
        normalizations=summary['legacy_normalizations'],ok=ok)
    (out/'finite-results.json').write_text(json.dumps(dict(summary=result,reports=reports,observations=observations),indent=2))
    print('FINITE SUMMARY',json.dumps(result))
    return 0 if ok else 1
if __name__=='__main__':raise SystemExit(main())
