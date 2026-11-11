#!/usr/bin/env python3
"""Recheck sealed real IR with the strict context gate, without native execution."""
import argparse
import json
from pathlib import Path
import state_context_real as context

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--saved',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    group=p.add_mutually_exclusive_group(required=True);group.add_argument('--baseline',action='store_true');group.add_argument('--require-context',action='store_true')
    a=p.parse_args();a.saved=a.saved.resolve();a.out=a.out.resolve()
    if a.out.exists():p.error('--out must be new')
    semantic=json.loads((a.saved/'summary.json').read_text())
    original=json.loads((a.saved/'context-summary.json').read_text())
    modules=json.loads((a.saved/'modules.json').read_text());rows=[]
    for module,seal in zip(modules,original['rows']):
        work=Path(module['work']);before=work/'before-context.ll';after=work/'after-context.ll'
        if context.digest(before)!=seal['before_sha256'] or context.digest(after)!=seal['after_sha256']:
            raise RuntimeError('sealed IR source changed')
        row=dict(kernel=module['kernel'],work=str(work),before_sha256=context.digest(before),after_sha256=context.digest(after))
        if module['kernel']==context.ENTRY:
            row['context']=context.context_shape(before.read_text(),after.read_text(),
                (work/'before-pipeline.ll').read_text(),(work/'after-pipeline.ll').read_text())
        rows.append(row)
    if len(modules)!=4 or len(original['rows'])!=4:raise RuntimeError('inexact module coverage')
    focus=[row for row in rows if row['kernel']==context.ENTRY]
    valid=len(focus)==1 and bool(focus[0]['context']['valid'])
    report=dict(kind='strict-saved-IR-context-review',executions=0,
        ok=semantic['ok'] and (not valid if a.baseline else valid),rows=rows,
        native_execution_summary_sha256=context.digest(a.saved/'summary.json'),
        prior_context_summary_sha256=context.digest(a.saved/'context-summary.json'),
        measurement_source_sha256=context.digest(context.__file__),
        source_sha256=context.digest(__file__),dispatch_specialize=semantic['dispatch_specialize_runs'],
        GuestSSA_incomplete_modules=semantic['GuestSSA_incomplete_modules'])
    context.save(a.out,report);print(json.dumps(dict(ok=report['ok'],executions=0,contexts=[r['context'] for r in focus]),indent=2))
    return 0 if report['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
