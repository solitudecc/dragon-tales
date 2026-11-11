#!/usr/bin/env python3
"""Read-only batch-context review of already frozen real replay IR."""
import argparse,json
from pathlib import Path
import loop_carried_context_real as measure

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before',type=Path,required=True);p.add_argument('--after',type=Path,required=True)
    p.add_argument('--final',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--require-batching',action='store_true')
    a=p.parse_args()
    result=measure.batch_contexts(a.before.read_text(),a.after.read_text(),a.final.read_text())
    ok=measure.batching_ok(result) if a.require_batching else measure.baseline_ok(result)
    row=dict(ok=bool(ok),native_executions=0,analysis_runs=0,kind='saved-IR-only review',
             hashes={str(path):measure.digest(path) for path in (a.before,a.after,a.final)},
             measurement_sha256=measure.digest(Path(measure.__file__)),result=result)
    measure.save(a.out,row);print(json.dumps(dict(ok=row['ok'],contexts=len(result['contexts']),states=result['distinct_states'],native_executions=0)))
    return 0 if ok else 1
if __name__=='__main__':raise SystemExit(main())
