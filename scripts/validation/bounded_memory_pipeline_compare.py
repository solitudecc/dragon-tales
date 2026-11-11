#!/usr/bin/env python3
"""Compare callback/builtin pipeline reports and every saved round bytewise."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

COMMON = ('complete', 'changed', 'blocks_visited', 'guest_loads', 'guest_stores',
          'loads_forwarded', 'loads_reused', 'call_barriers', 'alias_barriers',
          'partial_overlaps', 'phis_inserted', 'join_barriers', 'loop_barriers',
          'budget_exhausted', 'byte_rebuilds')
EXTRA = ('leaf_calls_preserved', 'summary_calls_preserved', 'summary_calls_rejected')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compare_run(old, new):
    if {k: v for k, v in old.items() if k != 'counters'} != {k: v for k, v in new.items() if k != 'counters'}:
        raise RuntimeError('run identity/status/Result.changed/revision differs')
    a, b = old['counters'], new['counters']
    if old['name'] != 'guest-memory':
        if a != b:
            raise RuntimeError('non-memory counters differ')
        return None
    if set(a) != set(COMMON) or set(b) != set(COMMON) | set(EXTRA) | {'available'}:
        raise RuntimeError('unexpected memory counter field coverage')
    if any(type(v) is not int for v in [*a.values(), *b.values()]) or b['available'] != 1:
        raise RuntimeError('malformed or unavailable memory counters')
    if any(a[key] != b[key] for key in COMMON):
        raise RuntimeError('shared memory counters differ')
    return {key: dict(old=None, old_available=False, new=b[key]) for key in EXTRA}


def self_test():
    old = dict(name='guest-memory', iteration=1, revision=4, status=0, changed=True,
               message='', counters={key: 0 for key in COMMON})
    new = copy.deepcopy(old); new['counters'].update(available=1, **{key: 0 for key in EXTRA})
    compare_run(old, new)
    bad = []
    x = copy.deepcopy(new); x['changed'] = False; bad.append(x)
    x = copy.deepcopy(new); x['counters']['changed'] = 1; bad.append(x)
    x = copy.deepcopy(new); del x['counters']['complete']; bad.append(x)
    x = copy.deepcopy(new); x['counters']['available'] = 0; bad.append(x)
    x = copy.deepcopy(new); x['counters']['guest_loads'] = False; bad.append(x)
    for row in bad:
        try:
            compare_run(old, row)
        except RuntimeError:
            continue
        raise RuntimeError('counter comparator missed corrupt control')
    return dict(ok=True, positive=1, rejected_controls=len(bad), new_native_executions=0, new_analysis_runs=0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--baseline', type=Path)
    p.add_argument('--candidate', type=Path)
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    if a.self_test:
        print(json.dumps(self_test())); return
    if any(v is None for v in (a.baseline, a.candidate, a.out)):
        p.error('--baseline --candidate and --out required')
    reports = [json.loads((x / 'bounded-memory-summary.json').read_text()) for x in (a.baseline, a.candidate)]
    old, new = reports
    if not old['ok'] or not new['ok'] or old['mode'] != 'callback' or new['mode'] != 'builtin':
        raise RuntimeError('missing completed callback/builtin groups')
    for key in ('requested_rounds', 'max_pass_runs', 'actual_recipe', 'transformation_recipe'):
        if old[key] != new[key]:raise RuntimeError('pipeline contract mismatch: ' + key)
    rows = []
    left = {x['entry']: x for x in old['rows']}; right = {x['entry']: x for x in new['rows']}
    if len(left) != len(old['rows']) or len(right) != len(new['rows']) or set(left) != set(right) or len(left) != 4:
        raise RuntimeError('module coverage mismatch')
    compared = 0
    for entry, row in left.items():
        other = right[entry]; irs = []
        for name in ('before-pipeline.ll', 'after-pipeline.ll'):
            x, y = Path(row['work']) / name, Path(other['work']) / name
            if x.read_bytes() != y.read_bytes():raise RuntimeError('IR bytes differ: ' + hex(entry) + '/' + name)
            irs.append(dict(name=name, sha256=digest(x)))
        if len(row['rounds']) != len(other['rounds']):raise RuntimeError('round coverage mismatch')
        for index, (x, y) in enumerate(zip(row['rounds'], other['rounds']), 1):
            if x['iteration'] != index or y['iteration'] != index:raise RuntimeError('round order mismatch')
            if x['sha256'] != digest(x['ir']) or y['sha256'] != digest(y['ir']):raise RuntimeError('round file changed')
            if Path(x['ir']).read_bytes() != Path(y['ir']).read_bytes():raise RuntimeError('round IR bytes differ')
            if x['actual_indices'] != y['actual_indices']:raise RuntimeError('round index evidence differs')
            irs.append(dict(name='round-' + str(index), sha256=x['sha256']))
        op, np = row['report'], other['report']
        if {k: v for k, v in op.items() if k != 'runs'} != {k: v for k, v in np.items() if k != 'runs'}:
            raise RuntimeError('whole pipeline results differ')
        if len(op['runs']) != len(np['runs']):raise RuntimeError('run coverage differs')
        extras = []
        for x, y in zip(op['runs'], np['runs']):
            extra = compare_run(x, y); compared += 1
            if extra is not None:extras.append(dict(iteration=x['iteration'], counters=extra))
        rows.append(dict(entry=entry, byte_identical_ir=irs, unavailable_old_counters=extras))
    result = dict(ok=True, rows=rows, runs_compared=compared, common_memory_counter_fields=list(COMMON),
                  old_extra_counters_unavailable=True, no_ir_normalization=True,
                  new_native_executions=0, new_analysis_runs=0)
    a.out.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps(dict(ok=True, modules=4, runs_compared=compared, no_ir_normalization=True)))


if __name__ == '__main__':main()
