"""CET landing-pad boundary recovery vs unchanged full-state Unicorn oracle.

Only the declared root is lifted, exposing false independent functions rather
than using their accidental registration as runtime resume implementations.
All modelled state and 64 KiB are checked; incomplete old exits are separate.
"""
import argparse,hashlib,json,os,sys
from pathlib import Path
if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON']);import dragon
import function_guest_diff as guest
from function_static_guest_diff import StaticAdapter
d=guest.base.dragon

def main():
    p=argparse.ArgumentParser(add_help=False)
    p.add_argument('--mode',choices=('v2','static','product'),default='product')
    p.add_argument('--strategy',type=int,choices=(0,1),default=0)
    p.add_argument('--baseline',action='store_true')
    p.add_argument('--control',choices=('result','memory'))
    p.add_argument('--expect-control',action='store_true')
    args,remaining=p.parse_known_args()
    if bool(args.control)!=args.expect_control or args.baseline and args.control:p.error('matching control/expect-control required')
    out=Path(remaining[remaining.index('--out')+1]).resolve();out.mkdir(parents=True,exist_ok=True)
    sys.argv=[sys.argv[0],*remaining]
    if args.control=='memory':sys.argv.append('--corrupt-output')
    graphs=[];analyze=d.Disassembler.analyze;configuration=d.Configuration.__init__
    def config(self,*a,**kw):kw['strategy']=args.strategy;configuration(self,*a,**kw)
    def analyze_with_pass(self,code,address=0,*a,**kw):
        g=analyze(self,code,address,*a,**kw)
        with d.PassPipeline() as pipeline:
            pipeline.register_finite_dispatch(code,address,entry_points=[address]).add('finite-dispatch')
            report=pipeline.run(g);assert report['ok']
        graphs.append(dict(functions=g.function_addresses(),root_blocks=g.get_function(address).block_addresses(),report=report))
        return g
    def compile_case(f,*a,**kw):return guest.compile.compile_case(f,*a,**kw,static_analysis=args.mode!='v2',product_pipeline=args.mode=='product')
    parent=guest.Adapter if args.mode=='v2' else StaticAdapter
    class Adapter(parent):
        def compare(self,f,expected,actual,corrupt):
            if args.control=='result':
                damaged=bytearray(actual[0]);damaged[0]^=1;actual=(bytes(damaged),)+actual[1:]
            return super().compare(f,expected,actual,corrupt)
    d.Configuration.__init__=config;d.Disassembler.analyze=analyze_with_pass
    fixture=guest.base.ROOT/'tests/fixtures/cet-boundaries.json'
    try:code=guest.main(fixtures_path=fixture,compile_factory=compile_case,adapter_factory=Adapter)
    finally:d.Configuration.__init__=configuration;d.Disassembler.analyze=analyze
    r=json.loads((out/'results.json').read_text());rows=r['results'];s=r['summary']
    root=lambda x:x.get('exit',[0])[0]==1 and x['exit'][1]==3 and x['exit'][3]==guest.base.SENTINEL
    complete=sum(root(x) for x in rows);incomplete=sum('exit' in x and not root(x) for x in rows)
    errors=sum(x['status'] not in ('match','different') for x in rows)
    if args.baseline:ok=errors==0 and incomplete==len(rows) and complete==0
    elif args.control:ok=errors==0 and complete==len(rows) and all(x['status']=='different' and len(x['differences'])==1 and x['differences'][0]['field']==('memory' if args.control=='memory' else 'RAX') for x in rows)
    else:ok=code==0 and complete==len(rows) and all(x['functions']==[0x1000] for x in graphs)
    mapped={str(Path(d._LIB).resolve())}
    mapped.update(line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines() if '/libdragon' in line and line.split()[-1].startswith('/'))
    summary=dict(ok=ok,complete=complete,incomplete=incomplete,errors=errors,control=args.control,
        full_semantic_differences=sum(root(x) and x['status']=='different' for x in rows),
        exclusions=s['undefined_flag_fields'],skips=s['skipped'],normalizations=s['legacy_normalizations'],
        libraries={x:hashlib.sha256(Path(x).read_bytes()).hexdigest() for x in sorted(mapped)},
        fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (out/'cet-results.json').write_text(json.dumps(dict(summary=summary,graphs=graphs),indent=2))
    print(json.dumps(summary,indent=2));return 0 if ok else 1
if __name__=='__main__':raise SystemExit(main())
