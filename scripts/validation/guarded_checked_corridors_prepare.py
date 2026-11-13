#!/usr/bin/env python3
"""Strict public-API and actual cyclic-PHI/fallback gates; no native execution."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

import guarded_checked_corridors_oracle as model


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def blocks(text):
    match = re.search(r'^define [^\n]*@oracle\(', text, re.M)
    assert match
    body = text[match.end():text.index('\n}', match.end())]
    labels = list(re.finditer(r'^([-a-zA-Z$._0-9]+):[^\n]*\n', body, re.M))
    return {m[1]: body[m.end():labels[n+1].start() if n+1 < len(labels) else len(body)]
            for n, m in enumerate(labels)}


def switch(body):
    match = re.search(r'switch i64 (%[-a-zA-Z$._0-9]+), label %([-a-zA-Z$._0-9]+) \[(.*?)\]', body, re.S)
    assert match
    cases = [(int(k), target) for k, target in re.findall(r'i64 (-?[0-9]+), label %([-a-zA-Z$._0-9]+)', match[3])]
    assert len(cases) == len({k for k, _ in cases})
    return match[1], match[2], cases


def validate_ir(before, after, candidates, expected_contexts, expected_backedges):
    source, actual = blocks(before), blocks(after)
    graph = {name: re.findall(r'\blabel %([-a-zA-Z$._0-9]+)', body.strip().splitlines()[-1])
             for name, body in actual.items()}
    # Switches span multiple lines. Their source edge order is default, cases.
    for name, body in actual.items():
        if re.search(r'\bswitch ', body):
            _, fallback, cases = switch(body)
            graph[name] = [fallback] + [target for _, target in cases]
    assert all(child in actual for edges in graph.values() for child in edges)
    incoming = {n: Counter() for n in actual}
    for name, edges in graph.items():
        for child in edges:
            incoming[child][name] += 1
    phis = {}
    for name, body in actual.items():
        for value, operands in re.findall(r'^\s*(%[-a-zA-Z$._0-9]+) = phi i64 (.*)$', body, re.M):
            pairs = re.findall(r'\[ ([^,]+), %([-a-zA-Z$._0-9]+) \]', operands)
            assert Counter(pred for _, pred in pairs) == incoming[name], ('incomplete PHI actual slots', name, value)
            phis[value] = (name, pairs)
    assert not re.search(r'\b(?:undef|poison)\b', after), 'defined fixture acquired an unproved value'
    assert switch(actual['dispatch']) == switch(source['dispatch']), 'original full switch changed'
    for callee in ('prefix_unknown', 'side_unknown'):
        assert len(re.findall(r'call void @' + callee + r'\(', after)) == len(re.findall(r'call void @' + callee + r'\(', before)), 'CALL copied or removed'
    source_full = switch(source['dispatch'])
    contexts = backedges = 0
    products = []
    for name, body in actual.items():
        if name in source or not re.search(r'\bswitch ', body):
            continue
        value, fallback, cases = switch(body)
        if fallback not in actual or not re.search(r'\bswitch ', actual[fallback]):
            continue
        full_value, full_default, full_cases = switch(actual[fallback])
        assert (full_default, full_cases) == source_full[1:]
        assert value == full_value, 'fallback consumes another selector'
        assert re.search(re.escape(value) + r' = load i64, ptr ', body), 'fast selector not its same-block original LOAD'
        assert {k for k, _ in cases} == set(candidates)
        loops = [(k, target) for k, target in cases if target not in source]
        for k, target in cases:
            original_target = dict(source_full[2])[k]
            assert (target.startswith('head.transition') and original_target == 'head') or target == original_target, 'wrong fast target mapping'
        if loops:
            assert len({target for _, target in loops}) == 1
            head = loops[0][1]
            contexts += 1
            backedges += len(loops)
            roots = [(phi_name, pairs) for phi_name, (parent, pairs) in phis.items() if parent == head]
            assert len(roots) == 7, 'copied root PHIs disappeared'
            for phi_name, pairs in roots:
                fresh = [v for v, pred in pairs if pred == name]
                assert len(fresh) == len(loops) and len(pairs) == len(loops)+1
                original_name = phi_name.split('.transition', 1)[0]
                original_phi = re.search(re.escape(original_name) + r' = phi i64 (.*)', source['head'])
                assert original_phi
                old_fresh = next(v for v, pred in re.findall(r'\[ ([^,]+), %([-a-zA-Z$._0-9]+) \]', original_phi[1]) if pred == 'dispatch')
                assert all(v.startswith(old_fresh + '.transition') if old_fresh.startswith('%') else v == old_fresh for v in fresh), ('wrong fresh loop PHI', phi_name, old_fresh, fresh)
                old_initial = next(v for v, pred in re.findall(r'\[ ([^,]+), %([-a-zA-Z$._0-9]+) \]', original_phi[1]) if pred == 'seed')
                initial = next(v for v, pred in pairs if pred != name)
                # A real initial snapshot may contain edge-complete SSA repair
                # PHIs selecting the previous original or copied execution.
                # It cannot be a guessed constant or this root's fresh PHI.
                assert initial == old_initial or not (old_initial.startswith('%') and initial.startswith(old_initial + '.transition')), 'initial root directly uses its fresh definition'
                leaves, seen, todo = set(), set(), [initial]
                while todo:
                    item = todo.pop()
                    if item == old_initial:
                        leaves.add(item)
                        continue
                    if old_initial.startswith('%') and item.startswith(old_initial + '.transition'):
                        leaves.add(item)
                        continue
                    if item in seen:
                        continue
                    seen.add(item)
                    assert len(seen) <= 128, 'snapshot PHI inspection budget exceeded'
                    if item in phis:
                        parent, incoming_values = phis[item]
                        assert parent != head, 'initial snapshot traverses its current clone root'
                        todo.extend(v for v, _ in incoming_values)
                    else:
                        assert old_initial.startswith('%') and item.startswith(old_initial + '.transition'), 'initial snapshot acquired another value'
                        leaves.add(item)
                assert old_initial in leaves, 'initial snapshot lost the original seed observation'
        products.append(dict(block=name, selector=value, fallback=fallback, fast_cases=cases, loop_slots=len(loops)))
    assert products and contexts == expected_contexts and backedges == expected_backedges
    return dict(actual_fast_products=products, loop_contexts=contexts, loop_backedges=backedges,
                phi_values=len(phis), actual_cfg_blocks=len(actual))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('label')
    parser.add_argument('runtime', type=Path)
    args = parser.parse_args()
    root, out = args.root, args.root / args.label
    out.mkdir(exist_ok=False)
    env = dict(os.environ, LD_LIBRARY_PATH=str(args.runtime))
    files = [root/'driver', *sorted((root/'source').glob('*')), *sorted((root/'before').glob('*')), *sorted(args.runtime.glob('*.so'))]
    pins = {str(p): sha(p) for p in files if p.is_file()}
    (out/'pins-before.json').write_text(json.dumps(pins, indent=2)+'\n')
    (out/'ldd.txt').write_text(subprocess.check_output(['ldd', str(root/'driver')], env=env, text=True))
    rows = []
    recipes = [(mode, 'normal') for mode in model.MODES]
    recipes += [('loop', recipe) for recipe in ('wrong', 'straight', 'resumed', 'bad', 'budget', 'zero', 'old', 'oldfinite', 'oldstore')]
    recipes += [('duplicate', 'resumed')]
    for mode, recipe in recipes:
        destination = out/recipe
        destination.mkdir(exist_ok=True)
        output, report = destination/(mode+'.ll'), destination/(mode+'.json')
        command = [str(root/'driver'), str(root/'before'/(mode+'.ll')), str(output), str(report), recipe]
        (destination/(mode+'.command.json')).write_text(json.dumps(command, indent=2)+'\n')
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        (destination/(mode+'.log')).write_bytes(result.stdout)
        (destination/(mode+'.exit')).write_text(str(result.returncode)+'\n')
        record = json.loads(report.read_text()) if report.exists() else None
        row = dict(mode=mode, recipe=recipe, exit=result.returncode, report=record)
        rows.append(row)
        (out/'summary.json').write_text(json.dumps(rows, indent=2)+'\n')
        assert result.returncode == 0 and record and all(record[k] for k in ('option_accepted', 'verified_output', 'identity_preserved', 'metadata_preserved')), row
        rounds = record['rounds']
        assert len(rounds) == (3 if recipe == 'resumed' else 1) and all(r['ok'] for r in rounds)
        counters = [r['runs'][0]['counters'] for r in rounds]
        rewrites = [c.get('rewritten_edges', 0) for c in counters]
        expected = recipe in ('normal', 'wrong', 'straight', 'resumed') and mode != 'call_inside'
        if recipe.startswith('old'):
            # Compatibility/control observations remain explicit; this is not
            # an assumption that every legacy analysis must have no candidate.
            continue
        if not expected:
            assert not sum(rewrites) and record['unchanged']
            assert output.read_bytes() == Path(str(output)+'.parsed-before.ll').read_bytes()
            continue
        assert not record['unchanged'] and sum(rewrites) == (2 if mode == 'duplicate' else 1), row
        if recipe == 'resumed':
            assert rewrites == ([1, 1, 0] if mode == 'duplicate' else [1, 0, 0])
            assert output.read_bytes() == (out/'normal'/(mode+'.ll')).read_bytes()
        assert all(c.get('complete') == 0 for c in counters if c.get('rewritten_edges', 0))
        assert all(r['runs'][0]['status'] == 2 for r, c in zip(rounds, counters) if c.get('rewritten_edges', 0))
        contexts = sum(c.get('loop_contexts', 0) for c in counters)
        backedges = sum(c.get('loop_backedges', 0) for c in counters)
        assert contexts == (0 if recipe in ('wrong', 'straight') else 2 if mode == 'duplicate' else 1)
        assert backedges == (contexts*2 if mode == 'duplicate' else contexts)
        row['actual_structure'] = validate_ir(Path(str(output)+'.parsed-before.ll').read_text(), output.read_text(),
                                               (777,) if recipe == 'wrong' else (44,) if recipe == 'straight' else (43, 44), contexts, backedges)
        (out/'summary.json').write_text(json.dumps(rows, indent=2)+'\n')
    assert pins == {p: sha(Path(p)) for p in pins}
    (out/'pins-after.json').write_text(json.dumps(pins, indent=2)+'\n')
    print(json.dumps(dict(label=args.label, recipes=len(rows), candidate_native_executions=0,
                          source_pins_stable=True)))


if __name__ == '__main__':
    main()
