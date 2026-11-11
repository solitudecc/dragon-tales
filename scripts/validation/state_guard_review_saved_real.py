#!/usr/bin/env python3
"""Read-only re-audit of saved real-IR measurements, never execution evidence."""
import argparse, ast, hashlib, json, re
from pathlib import Path

def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    group=p.add_mutually_exclusive_group(required=True)
    group.add_argument('--baseline',action='store_true');group.add_argument('--require-gains',action='store_true')
    a=p.parse_args();source=Path(__file__).with_name('guest_memory_checked_edges_real.py')
    tree=ast.parse(source.read_text());namespace={'re':re}
    names={'state_guard_edges','state_guard_gain_valid'}
    nodes=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names]
    for n in tree.body:
        if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='CFF_GUARDS' for t in n.targets):
            namespace['CFF_GUARDS']=ast.literal_eval(n.value)
    exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),namespace)
    modules=json.loads((a.run/'modules.json').read_text());rows=[];pins={}
    for m in modules:
        directory=Path(m['work']);before=directory/'before-dispatch.ll';after=directory/'after-dispatch.ll'
        pins[str(before)]=digest(before);pins[str(after)]=digest(after)
        record=namespace['state_guard_edges'](before.read_text(),after.read_text(),m['kernel'])
        rows.append(dict(entry=m['kernel'],work=str(directory),measurement=record))
    focus=[r['measurement'] for r in rows if r['entry']==0x10080]
    complete=(len(rows)==4 and len(focus)==1 and all(r['measurement'].get('guard_present') for r in rows))
    if a.baseline:
        good=complete and not any(r['measurement']['redirects'] for r in rows)
        c=focus[0].get('condition',{}) if focus else {}
        good=good and c.get('predicate')=='ugt' and c.get('width')==64 and c.get('rhs')==50 and c.get('state_phi','').startswith('%RCX.')
    else:good=complete and namespace['state_guard_gain_valid'](focus[0])
    report=dict(ok=bool(good),run=str(a.run),kind='saved-IR-read-only-review',executions=0,
                evaluator_sha256=digest(source),reviewer_sha256=digest(__file__),
                source_IR_sha256=pins,modules=rows)
    report['dispatch_specialize_runs']=[dict(work=m['work'],kernel=m['kernel'],
        status=r['status'],complete=r['status']==0,
        budget_exhausted=r['counters'].get('budget_exhausted',0),counters=r['counters'])
        for m in modules for r in m.get('pipeline',{}).get('runs',[]) if r['name']=='dispatch-specialize']
    report['dispatch_specialize_incomplete_modules']=[r['work'] for r in report['dispatch_specialize_runs']
        if r['status']==2 or r['budget_exhausted']]
    saved=json.loads((a.run/'summary.json').read_text())
    report['saved_execution_summary']=dict(C_statuses=saved['C_statuses'],LLVM_statuses=saved['LLVM_statuses'],
        GuestSSA_incomplete_modules=saved['GuestSSA_incomplete_modules'],
        original_summary_sha256=digest(a.run/'summary.json'))
    with a.out.open('x') as stream:json.dump(report,stream,indent=2);stream.write('\n')
    print(json.dumps(dict(ok=report['ok'],kind=report['kind'],executions=0,modules=len(rows))))
    return 0 if report['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
