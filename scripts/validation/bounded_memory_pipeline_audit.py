#!/usr/bin/env python3
"""Audit saved round gains and target-switch cases/defaults without executing IR.

This is separate from the unchanged callback/builtin strict counter comparator.
LLVM control-flow may rename/split destination blocks across a whole pipeline;
case address values and the default Exit source PC must remain identical.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import store_hub_real as metric


def switches(ir, entry):
    root = metric.root_body(ir, entry)
    cfg = metric.blocks(ir, entry)
    ptrs = set(re.findall(r'(%[\w.]+) = getelementptr(?: inbounds| nusw| nuw)* i8, ptr %exit, i64 16\b', root))
    defaults = {}
    for name, body in cfg.items():
        values = {int(v) for v, pointer in re.findall(r'store i64 (\d+), ptr (%[\w.]+)', body) if pointer in ptrs}
        if len(values) == 1:
            defaults[name] = next(iter(values))
    definitions = dict(re.findall(r'^\s*(%[\w.]+) = (.*)$', root, re.M))
    def has_load(selector):
        pending, seen = [selector], set()
        while pending and len(seen) < 256:
            value = pending.pop()
            if value in seen: continue
            seen.add(value); rhs = definitions.get(value, '')
            if rhs.startswith('load i64,'): return True
            if rhs.startswith(('phi i64 ', 'freeze i64 ')):
                pending.extend(re.findall(r'%[\w.]+', rhs))
        return False
    rows = []
    for name, body in cfg.items():
        shape = metric.shape(body)
        if shape and shape['default'] in defaults:
            rows.append(dict(block=name, default=shape['default'], source_pc=defaults[shape['default']],
                             cases=sorted(v for v, _ in shape['cases']), selector_has_load=has_load(shape['selector'])))
    return rows


def preserve(before, after):
    if not before: raise RuntimeError('missing original target switch')
    result = []
    for original in before:
        matches = [new for new in after if new['source_pc'] == original['source_pc']
                   and new['cases'] == original['cases'] and new['selector_has_load']]
        if not original['selector_has_load'] or not matches:
            raise RuntimeError('original target cases/default source/LOAD lost')
        result.append(dict(original=original, retained=matches))
    return result


def self_test():
    inherited = metric.self_test()
    ir = '''define void @dragon_static_entry_x86_64_102a0(ptr %state, ptr %memory, ptr %exit) {
entry:
  %guest = load ptr, ptr %memory, align 1
  %p = getelementptr i8, ptr %guest, i64 70032
  %t = load i64, ptr %p, align 1
  switch i64 %t, label %bad [
    i64 123, label %hit
  ]
hit:
  ret void
bad:
  %src = getelementptr i8, ptr %exit, i64 16
  store i64 66264, ptr %src, align 1
  ret void
}
'''
    old = switches(ir, 0x102a0); preserve(old, switches(ir, 0x102a0))
    # A control-flow block rename is valid; cases/default identity stay exact.
    preserve(old, switches(ir.replace('%hit', '%renamed').replace('hit:', 'renamed:'), 0x102a0))
    bad = [ir.replace('i64 123, label', 'i64 124, label'),
           ir.replace('store i64 66264', 'store i64 12345'),
           ir.replace('load i64, ptr %p, align 1', 'add i64 1, 2'),
           ir.replace('switch i64 %t, label %bad [\n    i64 123, label %hit\n  ]', 'br label %hit')]
    for text in bad:
        try: preserve(old, switches(text, 0x102a0))
        except RuntimeError: continue
        raise RuntimeError('switch/default/LOAD control was accepted')
    return dict(ok=True, positive=2, rejected_switch_controls=len(bad), inherited_metric_controls=inherited,
                new_native_executions=0, new_analysis_runs=0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--group', type=Path)
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    if a.self_test: print(json.dumps(self_test())); return
    if a.group is None or a.out is None: p.error('--group and --out required')
    summary = json.loads((a.group / 'bounded-memory-summary.json').read_text())
    if not summary['ok'] or len(summary['rows']) != 4: raise RuntimeError('missing completed four-module group')
    rows = []
    for row in summary['rows']:
        # Relocate archived evidence by its group-relative oracle directory only.
        relative = row['work'].split('/' + a.group.name + '/', 1)[1]
        work = a.group / relative
        before = (work / 'before-pipeline.ll').read_text()
        after = (work / 'after-pipeline.ll').read_text()
        if hashlib.sha256(before.encode()).hexdigest() != row['before_sha256'] or hashlib.sha256(after.encode()).hexdigest() != row['after_sha256']:
            raise RuntimeError('saved whole-pipeline IR hash changed')
        original = switches(before, row['entry'])
        if len(original) != 2: raise RuntimeError('expected original CFF and business target switch')
        retained = preserve(original, switches(after, row['entry']))
        rounds = []
        for checkpoint in row['rounds']:
            ir = (work / ('round-' + str(checkpoint['iteration']) + '.ll')).read_text()
            if hashlib.sha256(ir.encode()).hexdigest() != checkpoint['sha256']: raise RuntimeError('saved round IR changed')
            indices = sorted({x['index'] for x in metric.actual_loads(ir, row['entry'])})
            if indices != checkpoint['actual_indices']: raise RuntimeError('recorded indices differ from actual IR')
            rounds.append(indices)
        if row['entry'] == 0x102a0:
            if len(rounds) != 3 or any(not required.issubset(set(found)) for required, found in zip(
                    ({70032}, {70032, 69760}, {70032, 69760, 69736}), rounds)):
                raise RuntimeError('actual three-round O0 target-load gain gate failed')
        rows.append(dict(entry=row['entry'], rounds=rounds, preserved_original_switches=retained))
    if sum(row['entry'] == 0x102a0 for row in rows) != 1: raise RuntimeError('missing O0 focus root')
    result = dict(ok=True, rows=rows, whole_pipeline_case_addresses_and_default_source_exact=True,
                  block_labels_may_change=True, exclusions=[], skips=[], normalizations=[],
                  new_native_executions=0, new_analysis_runs=0)
    a.out.write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    print(json.dumps(dict(ok=True, modules=len(rows), original_switches_preserved=8)))


if __name__ == '__main__': main()
