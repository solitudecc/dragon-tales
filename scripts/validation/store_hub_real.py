#!/usr/bin/env python3
"""Explicit STORE contexts and PHI hubs around the unchanged full-state replay.

Measure actual constant-index Guest LOADs used by the CFF target switch.
Copied switches remain switches; this harness never calls them CFF removal.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
CFF_PC = {0x102a0: 0x102d8, 0x10080: 0x100b6, 0x10770: 0x10adc, 0x10090: 0x101f6}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def root_body(ir, entry):
    match = re.search(r'^define [^\n]*@dragon_static_entry_x86_64_' + format(entry, 'x')
                      + r'\([^\n]*\).*?^}', ir, re.M | re.S)
    if not match:
        raise RuntimeError('missing exact frozen root: ' + hex(entry))
    return match[0]


def blocks(ir, entry):
    return dict(re.findall(r'^([\w.]+):[^\n]*\n(.*?)(?=^[\w.]+:|^})', root_body(ir, entry), re.M | re.S))


def shape(body):
    sw = re.search(r'^\s*switch i64 (%[\w.]+), label %([\w.]+) \[\n(.*?)^\s*\]', body, re.M | re.S)
    if not sw:
        return None
    return dict(selector=sw[1], default=sw[2],
                cases=[(int(value), target) for value, target in re.findall(r'i64 (-?\d+), label %([\w.]+)', sw[3])])


def uses_load(root, selector, loaded):
    definitions = dict(re.findall(r'^\s*(%[\w.]+) = (.*)$', root, re.M))
    pending, seen = [selector], set()
    while pending and len(seen) < 256:
        value = pending.pop()
        if value == loaded:
            return True
        if value in seen:
            continue
        seen.add(value)
        rhs = definitions.get(value, '')
        if rhs.startswith(('phi i64 ', 'freeze i64 ')):
            pending.extend(re.findall(r'%[\w.]+', rhs))
    return False


def actual_loads(ir, entry):
    root = root_body(ir, entry)
    cfg = blocks(ir, entry)
    source_pointers = set(re.findall(r'(%[\w.]+) = getelementptr(?: inbounds| nusw| nuw)* i8, ptr %exit, i64 16\b', root))
    cff_defaults = set()
    for name, body in cfg.items():
        values = {int(v) for v, pointer in re.findall(r'store i64 (\d+), ptr (%[\w.]+)', body)
                  if pointer in source_pointers}
        if values == {CFF_PC[entry]}:
            cff_defaults.add(name)
    reachable, pending = set(), [next(iter(cfg))]
    while pending:
        name = pending.pop()
        if name in reachable or name not in cfg:
            continue
        reachable.add(name)
        literal = re.search(r'br i1 (true|false), label %([\w.]+), label %([\w.]+)', cfg[name])
        pending.extend([literal[2] if literal[1] == 'true' else literal[3]] if literal
                       else re.findall(r'label %([\w.]+)', cfg[name]))
    load_owners = {value: name for name, body in cfg.items()
                   for value in re.findall(r'(%[\w.]+) = load i64, ptr ', body)}
    descriptors = {'%memory'}
    for _ in range(4):
        for dest, base in re.findall(r'(%[\w.]+) = getelementptr(?: inbounds| nusw| nuw)* i8, ptr (%[\w.]+), i64 0\b', root):
            if base in descriptors:
                descriptors.add(dest)
    bases = {dest for dest, address in re.findall(r'(%[\w.]+) = load ptr, ptr (%[\w.]+),', root)
             if address in descriptors}
    pointers = {dest: int(offset) for dest, base, offset in re.findall(
        r'(%[\w.]+) = getelementptr(?: inbounds| nusw| nuw)* i8, ptr (%[\w.]+), i64 (-?\d+)\b', root)
        if base in bases}
    result = []
    for value, pointer in re.findall(r'(%[\w.]+) = load i64, ptr (%[\w.]+),', root):
        if pointer not in pointers or load_owners.get(value) not in reachable:
            continue
        consumers = []
        for name, body in cfg.items():
            sw = shape(body)
            if name in reachable and sw and sw['default'] in cff_defaults and uses_load(root, sw['selector'], value):
                consumers.append(dict(block=name, cases=sw['cases'], default=sw['default']))
        if consumers:
            result.append(dict(value=value, pointer=pointer, index=pointers[pointer], consumers=consumers))
    return result


def preserved_original_switches(before, after, entry):
    old, new = blocks(before, entry), blocks(after, entry)
    rows = []
    for name, body in old.items():
        sw = shape(body)
        if not sw:
            continue
        root = root_body(before, entry)
        loads = re.findall(r'(%[\w.]+) = load i64, ptr ', root)
        if not any(uses_load(root, sw['selector'], value) for value in loads):
            continue
        retained = shape(new.get(name, ''))
        rows.append(dict(block=name, preserved=bool(retained and retained['cases'] == sw['cases']
                                                  and retained['default'] == sw['default']),
                         cases=len(sw['cases']), default=sw['default']))
    return rows


def self_test():
    header = 'define void @dragon_static_entry_x86_64_102a0(ptr %state, ptr %memory, ptr %exit, i64 %other) {\n'
    body = '''entry:
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
    good = header + body
    assert [x['index'] for x in actual_loads(good, 0x102a0)] == [70032]
    false = [good.replace('switch i64 %t', 'switch i64 %other'),
             good.replace('ptr %guest, i64 70032', 'ptr %state, i64 70032'),
             good.replace('store i64 66264', 'store i64 12345'),
             good.replace('store i64 66264', 'store i64 65718'),
             good.replace('entry:\n', 'entry:\n  br label %hit\ndead:\n'),
             good.replace('entry:\n', 'entry:\n  br i1 false, label %dead, label %hit\ndead:\n')]
    for text in false:
        assert not actual_loads(text, 0x102a0)
    wrong_index = good.replace('i64 70032', 'i64 70040')
    assert not any(x['index'] == 70032 for x in actual_loads(wrong_index, 0x102a0))
    assert preserved_original_switches(good, good, 0x102a0)[0]['preserved']
    lost = good.replace('switch i64 %t, label %bad [\n    i64 123, label %hit\n  ]', 'br label %hit')
    assert not preserved_original_switches(good, lost, 0x102a0)[0]['preserved']
    changed = good.replace('i64 123, label %hit', 'i64 124, label %hit')
    assert not preserved_original_switches(good, changed, 0x102a0)[0]['preserved']
    return dict(ok=True,positive=1,false_gain_controls=len(false)+1,switch_preservation_controls=2,
                new_native_executions=0,new_analysis_runs=0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--frozen', type=Path)
    p.add_argument('--out', type=Path)
    p.add_argument('--tigress')
    p.add_argument('--ssa-after-gvn', action='store_true')
    p.add_argument('--allow-plain-stores', type=int, choices=(0, 1), default=1)
    p.add_argument('--require-o0-gain', action='store_true')
    a = p.parse_args()
    if a.self_test:
        print(json.dumps(self_test()))
        return 0
    if any(v is None for v in (a.frozen, a.out, a.tigress)):
        p.error('--frozen, --out and --tigress required')
    import dragon as d
    import guest_memory_checked_edges_real as backend
    original_add = d.PassPipeline.add
    dispatch_captures, memory_captures = [], []
    active = {}
    ordinal = 0

    def capture(kind, phase, token):
        def record(context, _options):
            if phase == 'before':
                if token in active:
                    raise RuntimeError('nested capture token')
                active[token] = dict(kind=kind, before=context.target.ir())
            else:
                row = active.pop(token)
                row['after'] = context.target.ir()
                (dispatch_captures if kind == 'dispatch' else memory_captures).append(row)
            return d.PassResult()
        return record

    def add(pipeline, name, **options):
        nonlocal ordinal
        if name != 'dispatch-specialize' and not name.startswith('guest-memory-ssa'):
            return original_add(pipeline, name, **options)
        kind = 'dispatch' if name == 'dispatch-specialize' else 'memory'
        ordinal += 1
        token = str(ordinal)
        before, after = 'store-hub-before-' + token, 'store-hub-after-' + token
        pipeline.register(before, capture(kind, 'before', token), read_only=True)
        pipeline.register(after, capture(kind, 'after', token), read_only=True)
        original_add(pipeline, before)
        if kind == 'dispatch':
            options['allow_plain_stores'] = a.allow_plain_stores
        original_add(pipeline, name, **options)
        return original_add(pipeline, after)

    prior_argv = sys.argv
    d.PassPipeline.add = add
    try:
        sys.argv = [str(Path(backend.__file__)), '--frozen', str(a.frozen),
                    '--out', str(a.out), '--tigress', a.tigress]
        if a.ssa_after_gvn:
            sys.argv.append('--ssa-after-gvn')
        status = backend.main()
    finally:
        sys.argv = prior_argv
        d.PassPipeline.add = original_add
    modules = json.loads((a.out / 'modules.json').read_text())
    if len(modules) != 4 or len(dispatch_captures) != 4 or active:
        raise RuntimeError('missing or extra module/pass captures')
    expected_memory = 8 if a.ssa_after_gvn else 4
    if len(memory_captures) != expected_memory:
        raise RuntimeError('missing or extra memory-stage captures')
    rows = []
    for module, captured in zip(modules, dispatch_captures):
        entry, work = module['kernel'], Path(module['work'])
        before, after = captured['before'], captured['after']
        (work / 'before-store-context.ll').write_text(before)
        (work / 'after-store-context.ll').write_text(after)
        final = (work / 'after-pipeline.ll').read_text()
        original = preserved_original_switches(before, after, entry)
        if not all(r['preserved'] for r in original):
            raise RuntimeError('original target switch/default was lost')
        rows.append(dict(entry=entry, work=str(work), actual_before=actual_loads(before, entry),
                         actual_after=actual_loads(final, entry), original_switches=original,
                         before_sha256=digest(work / 'before-store-context.ll'),
                         after_sha256=digest(work / 'after-store-context.ll')))
    for i, captured in enumerate(memory_captures):
        module = modules[i // 2 if a.ssa_after_gvn else i]
        work = Path(module['work'])
        label = str(i % 2) if a.ssa_after_gvn else '0'
        (work / ('before-memory-stage-' + label + '.ll')).write_text(captured['before'])
        (work / ('after-memory-stage-' + label + '.ll')).write_text(captured['after'])
    focus = [row for row in rows if row['entry'] == 0x102a0]
    if len(focus) != 1:
        raise RuntimeError('missing frozen f-cff O0 root')
    seed_gain = any(row['index'] == 70032 for row in focus[0]['actual_after'])
    semantic = json.loads((a.out / 'summary.json').read_text())
    ok = status == 0 and semantic['ok'] and (seed_gain or not a.require_o0_gain)
    report = dict(ok=ok, semantic_returncode=status, require_o0_gain=a.require_o0_gain,
                  allow_plain_stores=a.allow_plain_stores, o0_actual_index70032=seed_gain,
                  rows=rows, memory_stage_captures=len(memory_captures),
                  semantic_summary_sha256=digest(a.out / 'summary.json'),
                  harness_sha256=digest(__file__),
                  copied_switches_are_not_CFF_removal=True, exclusions=[], skips=[], normalizations=[])
    save(a.out / 'store-hub-summary.json', report)
    print(json.dumps(dict(ok=ok, o0_actual_index70032=seed_gain,
                          actual_indices={hex(row['entry']): sorted({v['index'] for v in row['actual_after']}) for row in rows}), indent=2))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
