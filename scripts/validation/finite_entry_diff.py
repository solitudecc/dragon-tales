"""Independent CALL-entry CFG oracle; no target repairs or ABI summaries.

A recovered branch reveals a CALL to a function also reached by a tail jump
following a STORE. The CALL input memory is independent from that STORE.
Unicorn executes both initial pointer alternatives and both CMOV alternatives.
Pre-hardening finite-dispatch falsely published only the tail's stored target.
"""
from pathlib import Path
import argparse,hashlib,json,os,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,os.environ.get('DRAGON_TEST_PYTHON',str(ROOT/'python')))
import dragon as d
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_MODE_64,UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_RSP,UC_X86_REG_EDI,UC_X86_REG_RDI

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--expect-wrong',action='store_true')
    p.add_argument('--control',action='store_true',help='omit a proved root target in the checker copy')
    args=p.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    if args.control and not args.expect_wrong:p.error('control requires --expect-wrong')
    fixture=ROOT/'tests/fixtures/finite-dispatch-entries.json';rows=[];reports=[];wrong=executions=0
    for f in json.loads(fixture.read_text()):
        x64=f['architecture']=='x86_64';width=8 if x64 else 4
        for strategy in (0,1):
            for explicit in (False,True):
                code=bytes.fromhex(f['code_hex'])
                graph=d.Disassembler(2 if x64 else 1,d.Configuration(strategy=strategy)).analyze(code,f['base'])
                with d.PassPipeline() as pipeline:
                    pipeline.register_finite_dispatch(code,f['base'],entry_points=[f['base']] if explicit else []).add('finite-dispatch')
                    report=pipeline.run(graph)
                assert report['ok'] and not report['budget_exhausted']
                sites={site:set(graph.get_instruction(site).branches) for site in (f['root_site'],f['callee_site'])}
                assert sites[f['root_site']]=={0x1100,0x1120}
                if args.control:sites[f['root_site']].remove(0x1120)
                observed={site:set() for site in sites}
                for initial in (0x1200,0x1210):
                    for value in (0,1,0x80000000,0xffffffff):
                        uc=Uc(UC_ARCH_X86,UC_MODE_64 if x64 else UC_MODE_32);uc.mem_map(0x1000,0x9000)
                        uc.mem_write(f['base'],code);uc.mem_write(0x3000,initial.to_bytes(width,'little'))
                        uc.reg_write(UC_X86_REG_RSP if x64 else UC_X86_REG_ESP,0x8000)
                        uc.reg_write(UC_X86_REG_RDI if x64 else UC_X86_REG_EDI,value)
                        uc.mem_write(0x8000,(0x9000).to_bytes(width,'little'));trace=[]
                        uc.hook_add(UC_HOOK_CODE,lambda u,pc,size,data:trace.append(pc))
                        uc.emu_start(f['base'],0x9000,count=1000)
                        assert trace[-1] in (0x1120,0x1200)
                        for source,target in zip(trace,trace[1:]):
                            if source not in sites:continue
                            observed[source].add(target)
                            missing=bool(sites[source]) and target not in sites[source]
                            wrong+=missing
                            rows.append(dict(architecture=f['architecture'],strategy=strategy,explicit=explicit,
                                initial=initial,input=value,source=source,target=target,wrong=missing))
                        executions+=1
                assert observed[f['root_site']]=={0x1100,0x1120}
                assert observed[f['callee_site']]=={0x1200,0x1210}
                if not args.expect_wrong:assert not sites[f['callee_site']], 'callee input was constrained by the tail STORE'
                reports.append(dict(architecture=f['architecture'],strategy=strategy,explicit=explicit,
                                    sites={hex(k):sorted(v) for k,v in sites.items()},report=report))
    mapped={str(Path(d._LIB).resolve())}
    mapped.update(line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
                  if '/libdragon' in line and line.split()[-1].startswith('/'))
    summary=dict(executions=executions,observed_transfers=len(rows),wrong_edges=wrong,
                 exclusions=0,skips=0,normalizations=0,control=args.control,
                 libraries={s:hashlib.sha256(Path(s).read_bytes()).hexdigest() for s in sorted(mapped)},
                 fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                 harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (args.out/'results.json').write_text(json.dumps(dict(summary=summary,observations=rows,reports=reports),indent=2))
    print(json.dumps(summary,indent=2))
    assert executions==64 and (wrong>0 if args.expect_wrong else wrong==0)
if __name__=='__main__':main()
