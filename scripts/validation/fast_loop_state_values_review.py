#!/usr/bin/env python3
"""Independent raw-IR/recorded-byte review for the fresh58 state observer.

This does not replace the production V4 source-to-clone proof. It binds that
frozen inventory to the actual compiler input, then independently checks every
edge which the observer's conditional CFG removes and recomputes its SCCs.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

PROFILE = 'fresh58-state-success-small-set-v1'
SSA = r'%[-a-zA-Z$._0-9]+'
NAME = r'[-a-zA-Z$._0-9]+'
SUCCESS = ('state_success', 'target_success', 'business_success')
FAILURE = ('entry_fallback', 'state_failure', 'target_failure',
           'business_failure', 'central_dispatcher')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def raw_blocks(text, function):
    match = re.search(r'^define [^\n]*@' + re.escape(function) + r'\(', text, re.M)
    assert match, 'selected function absent from actual raw IR'
    body = text[match.end():text.index('\n}', match.end())]
    labels = list(re.finditer(r'^(' + NAME + r'):[^\n]*\n', body, re.M))
    assert labels, 'unsupported unnamed/quoted raw block layout'
    return {m[1]: body[m.end():labels[n + 1].start() if n + 1 < len(labels)
                        else len(body)] for n, m in enumerate(labels)}


def instructions(body):
    result = []
    for line in body.splitlines():
        if not line.strip() or line.lstrip().startswith(';'):
            continue
        if re.match(r'^  \S', line) and not re.match(r'^\s*\]', line):
            result.append(line.strip())
        else:
            assert result and result[-1].startswith('switch '), 'unsupported raw instruction layout'
            result[-1] += '\n' + line
    return result


def bind_inventory(text, function, inventory):
    blocks = raw_blocks(text, function)
    assert inventory['function'] == function
    rows = {r['block']: r['instructions'] for r in inventory['state_value_structure']}
    cfg_rows = {r['name']: r for r in inventory['blocks']}
    assert len(rows) == len(inventory['state_value_structure'])
    assert len(cfg_rows) == len(inventory['blocks'])
    assert set(blocks) == set(rows) == set(cfg_rows), 'actual raw/inventory block coverage differs'
    graph = {}
    for name, body in blocks.items():
        observed = instructions(body)
        expected = [r['instruction'].strip() for r in rows[name]]
        assert observed == expected, ('actual raw/inventory instruction mismatch', name)
        assert observed[-1] == cfg_rows[name]['terminator'].strip()
        graph[name] = re.findall(r'\blabel %(' + NAME + r')', observed[-1])
        assert graph[name] == [e['target'] for e in cfg_rows[name]['successors']], ('actual CFG differs', name)
        assert [e['slot'] for e in cfg_rows[name]['successors']] == list(range(len(graph[name])))
    assert all(child in graph for edges in graph.values() for child in edges)
    # PHI slots are unprojected actual edge slots, including duplicate edges.
    predecessors = {name: Counter() for name in graph}
    for name, edges in graph.items():
        for child in edges:
            predecessors[child][name] += 1
    for name, records in rows.items():
        for record in records:
            if record['opcode'] == 'phi':
                assert Counter(v['block'] for v in record['incoming']) == predecessors[name], 'incomplete PHI edge inventory'
    return blocks, graph


def branch(body):
    match = re.search(r'\bbr i1 (' + SSA + r'), label %(' + NAME + r'), label %(' + NAME + r')', body)
    assert match, 'not an actual conditional branch'
    return match.groups()


def compare(body, operand, width=None):
    match = re.search(re.escape(operand) + r' = icmp eq i([1-9][0-9]*) (' + SSA + r'), (-?[0-9]+)(?:,|\s|$)', body)
    assert match, 'not an actual SSA/literal equality'
    bits, value, number = int(match[1]), match[2], int(match[3])
    assert width is None or bits == width
    return bits, value, number % (1 << bits)


def switch(body):
    match = re.search(r'\bswitch i64 (' + SSA + r'), label %(' + NAME + r') \[(.*?)\]', body, re.S)
    assert match, 'not an actual i64 switch'
    cases = [(int(k) % (1 << 64), b) for k, b in re.findall(r'i64 (-?[0-9]+), label %(' + NAME + r')', match[3])]
    assert len(cases) == len({k for k, _ in cases}), 'duplicate switch value'
    return match[1], match[2], cases


def selected_selector(blocks, name, value, predecessor):
    match = re.search(re.escape(value) + r' = phi i64 (.*)', blocks[name])
    if match:
        values = [v for v, pred in re.findall(r'\[ (' + SSA + r'), %(' + NAME + r') \]', match[1]) if pred == predecessor]
        assert values and len(set(values)) == 1, 'missing/ambiguous actual fallback PHI edge'
        value = values[0]
        assert not any(re.search(re.escape(value) + r' = phi ', body) for body in blocks.values()), 'historical selector PHI refused'
    return value


def local_load(body, value):
    assert re.search(re.escape(value) + r' = load i64, ptr ', body), 'guard is not on its actual local LOAD'


def components(graph, nodes):
    """Iterative Kosaraju, separately implemented from the source auditor."""
    visited, finish = set(), []
    for start in sorted(nodes):
        if start in visited:
            continue
        visited.add(start)
        stack = [(start, iter(graph[start]))]
        while stack:
            try:
                child = next(stack[-1][1])
            except StopIteration:
                finish.append(stack.pop()[0])
                continue
            if child in nodes and child not in visited:
                visited.add(child)
                stack.append((child, iter(graph[child])))
    reverse = {n: [] for n in nodes}
    for name in nodes:
        for child in graph[name]:
            if child in nodes:
                reverse[child].append(name)
    visited, groups = set(), []
    for start in reversed(finish):
        if start in visited:
            continue
        todo, group = [start], set()
        visited.add(start)
        while todo:
            name = todo.pop()
            group.add(name)
            for child in reverse[name]:
                if child not in visited:
                    visited.add(child)
                    todo.append(child)
        groups.append(group)
    return groups


def classify_central(anchor, inventory, manifest):
    """Source Exit-PC families, independently derived; never trust site lists."""
    def pc_map(inv):
        result = {}
        rows = {b['name']: b for b in inv['blocks']}
        for sw in inv['switches']:
            values = [int(s['value'].split()[1]) for s in rows[sw['default']]['stores']
                      if re.search(r'getelementptr i8, ptr %exit, i64 16$', s['pointer'])
                      and re.fullmatch(r'i64 [0-9]+', s['value'])]
            if values:
                assert len(values) == 1
                result[sw['default']] = values[0]
        return result
    pcs = pc_map(anchor)
    for name, value in pc_map(inventory).items():
        assert name not in pcs or pcs[name] == value
        pcs[name] = value
    def signature(sw):
        return (tuple(sorted(int(c['value'].split()[1]) for c in sw['cases'])),
                ('pc', pcs[sw['default']]) if sw['default'] in pcs else ('block', sw['default']))
    ranges = [(int(r['offset'], 16), int(r['offset'], 16) + len(bytes.fromhex(r['bytes'])))
              for r in manifest['memory']]
    by_fallback = {g['fallback']: g for g in anchor['same_value_guards']}
    groups = set()
    for sw in anchor['switches']:
        anchored = sw.get('same_block_load', False)
        if sw['block'] in by_fallback:
            guard = by_fallback[sw['block']]
            anchored = (all(guard[k] for k in ('same_value_full_switch', 'fallback_has_candidate_case', 'fallback_candidate_matches_success'))
                        and sw.get('load') == guard['load'] and sw.get('load_block') == guard['block']
                        and sw['selector'] == guard['load'].strip().split(' = ', 1)[0])
        if anchored and 'address_decimal' in sw and any(lo <= int(sw['address_decimal']) and int(sw['address_decimal']) + 8 <= hi for lo, hi in ranges):
            groups.add(signature(sw))
    assert len(groups) == 1, 'source CFF family not uniquely anchored'
    group = next(iter(groups))
    assert group[1][0] == 'pc'
    for sw in inventory['switches']:
        if signature(sw)[0] == group[0]:
            assert sw['default'] in pcs, 'unknown source Exit PC on a full CFF selector'
    return sorted(sw['block'] for sw in inventory['switches'] if signature(sw) == group), group[1][1], pcs


def review_structure(plan, text, inventory, before, anchor, manifest):
    assert plan['schema'] == 'final-ir-loop-observation-v1'
    assert plan['observer_profile'] == PROFILE and plan['recipe_steps'] == 58
    assert plan['conditional_closure']
    blocks, graph = bind_inventory(text, plan['function'], inventory)
    source_rows = {r['block']: r['instructions'] for r in before['state_value_structure']}
    source_cfg = {b['name']: [e['target'] for e in b['successors']] for b in before['blocks']}
    source_switches = {s['block']: s for s in before['switches']}
    central, central_pc, pcs = classify_central(anchor, inventory, manifest)
    assert central == sorted(plan['central_blocks']), 'central dispatcher coverage differs from source family'
    sites = plan['sites']
    assert [s['id'] for s in sites] == list(range(len(sites)))
    identities = [(s['block'], s['kind'], s['label'], s.get('slot'), s.get('member')) for s in sites]
    assert len(set(identities)) == len(identities), 'duplicate observation identity'
    assert all(s['block'] in blocks for s in sites)
    assert all(s['label'] in SUCCESS + FAILURE + ('root_entry', 'entry_admission') for s in sites)
    projected = {n: list(edges) for n, edges in graph.items()}
    metric = plan['set_aware_state_success_metric']
    states = {w['seed']: w for w in metric['state_guard_witnesses']}
    assert len(states) == plan['state_guard_count'] == len(metric['state_guard_witnesses'])
    targets = {g['block'] for g in inventory['same_value_guards']}
    targets.update(w['target_guard'] for w in states.values())
    memberships = {s['block']: s for s in metric['validated_sets']}
    assert len(memberships) == len(metric['validated_sets'])
    assert not set(states) & targets and not targets & set(memberships)
    expected = Counter()
    def expect(n, kind, label, **kwargs):
        expected[(n, kind, label, kwargs.get('slot'), kwargs.get('member'))] += 1
    entry = plan['admission_check']
    assert graph[entry['block']] == [entry['success'], entry['failure']]
    expect(next(iter(blocks)), 'visit', 'root_entry')
    expect(entry['block'], 'branch', 'entry_admission', slot=0)
    expect(entry['failure'], 'visit', 'entry_fallback')
    for name, witness in states.items():
        condition, success, failure = branch(blocks[name])
        freeze = re.search(re.escape(condition) + r' = freeze i1 (' + SSA + r')(?:,|\s|$)', blocks[name])
        assert freeze, 'state guard lacks the single actual frozen i1 equality'
        _, value, number = compare(blocks[name], freeze[1], witness['width'])
        assert value == witness['state'] and number == witness['prediction'], 'state guard reread/value differs'
        assert condition == witness['frozen_i1']
        assert (success, failure) == (witness['state_success'], witness['state_fallback'])
        original = witness['original_predicate'].strip()
        assert original in instructions(blocks[name]), 'original predicate not retained'
        original_condition = original.split(' = ', 1)[0]
        assert branch(blocks[failure])[0] == original_condition, 'state fallback no longer uses original predicate'
        assert graph[failure] == source_cfg[name], 'original state fallback direction/slots changed'
        assert instructions(blocks[name])[:-3] == [i['instruction'].strip() for i in source_rows[name][:-1]], 'state seed effects/SSA changed'
        projected[name] = [success]
        expect(name, 'branch', 'state_success', slot=0)
        expect(name, 'branch', 'state_failure', slot=1)
    for name in targets:
        condition, success, failure = branch(blocks[name])
        bits, value, candidate = compare(blocks[name], condition, 64)
        assert bits == 64
        local_load(blocks[name], value)
        selected, _, cases = switch(blocks[failure])
        assert selected_selector(blocks, failure, selected, name) == value, 'target fallback rereads selector'
        assert dict(cases).get(candidate) == success, 'target fallback candidate changed'
        projected[name] = [success]
        expect(name, 'branch', 'target_success', slot=0)
        expect(name, 'branch', 'target_failure', slot=1)
    small_nodes = set()
    for name, witness in memberships.items():
        value, fallback, cases = switch(blocks[name])
        local_load(blocks[name], value)
        selected, full_default, full_cases = switch(blocks[fallback])
        assert selected_selector(blocks, fallback, selected, name) == value, 'small-switch fallback rereads selector'
        assert all(dict(full_cases).get(k) == target for k, target in cases), 'small-switch/full mappings differ'
        assert fallback == witness['fallback'] and value == witness['selector']
        assert cases == [(int(c['value'].split()[1]) % (1 << 64), c['target']) for c in witness['cases']]
        assert any(s['cases'] == next(s for s in inventory['switches'] if s['block'] == fallback)['cases']
                   and s['default'] == full_default for s in before['switches']), 'small-switch full fallback differs from original'
        projected[name] = [target for _, target in cases]
        pair = [s for s in sites if s['block'] == name and s['kind'] == 'membership']
        assert len(pair) == 2 and {s['label'].split('_')[0] for s in pair} <= {'target', 'business'}
        assert len({s['label'].split('_')[0] for s in pair}) == 1
        prefix = pair[0]['label'].split('_')[0]
        assert full_default in pcs and prefix == ('target' if pcs[full_default] == central_pc else 'business'), 'small-switch CFF/business family mislabelled'
        for member in (True, False):
            label = prefix + ('_success' if member else '_failure')
            record = next(s for s in pair if s['label'] == label)
            assert record['member'] is member and record['validated_small_switch']
            assert record['values'] == [k for k, _ in cases]
            expect(name, 'membership', label, member=member)
        small_nodes.add(name)
    for witness in states.values():
        previous = witness['target_guard']
        _, _, current = branch(blocks[previous])
        _, value, _ = compare(blocks[previous], branch(blocks[previous])[0], 64)
        for row in witness['target_fallback_chain']:
            assert current == row['block']
            selected, default, cases = switch(blocks[current])
            assert selected_selector(blocks, current, selected, previous) == value, 'recursive target fallback rereads selector'
            actual = next(s for s in inventory['switches'] if s['block'] == current)
            assert any(s['cases'] == actual['cases'] and s['default'] == actual['default'] for s in source_switches.values()), 'recursive target fallback cases/default changed'
            assert len(cases) == row['cases'] and default == row['default']
            previous, current = current, default
    assert sorted(plan['central_blocks']) == sorted(metric['central_blocks'])
    assert len(set(plan['central_blocks'])) == len(plan['central_blocks'])
    for name in plan['central_blocks']:
        assert name in blocks
        expect(name, 'visit', 'central_dispatcher')
    assert Counter(identities) == expected, 'omitted, extra or wrong-direction observation site'
    seen, todo = {entry['success']}, [entry['success']]
    while todo:
        for child in projected[todo.pop()]:
            if child not in seen:
                seen.add(child)
                todo.append(child)
    assert not set(plan['central_blocks']) & seen, 'actual projected CFG reenters a central dispatcher'
    guards = set(states) | targets | small_nodes
    cycles = [group for group in components(projected, seen)
              if (len(group) > 1 or next(iter(group)) in projected[next(iter(group))]) and group & guards]
    assert {frozenset(c) for c in cycles} == {frozenset(c) for c in plan['fast_sccs']}, 'actual fast SCC differs'
    fast = set().union(*cycles) if cycles else set()
    assert small_nodes & fast, 'no actual validated small-switch guard in a fast SCC'
    for record in sites:
        if record['label'] in SUCCESS + ('state_failure', 'target_failure', 'business_failure'):
            assert record['in_fast_scc'] == (record['block'] in fast)
    return dict(actual_cfg_blocks=len(blocks), projected=projected, fast_cycles=cycles,
                fast_nodes=fast, small_switch_nodes=small_nodes)


def review_counts(plan, structure, counts):
    assert len(counts) == len(plan['sites'])
    assert all(isinstance(n, int) and 0 <= n < (1 << 64) for n in counts)
    totals = {label: sum(counts[s['id']] for s in plan['sites'] if s['label'] == label)
              for label in ('root_entry', 'entry_admission') + FAILURE + SUCCESS}
    assert totals['root_entry'] == totals['entry_admission'] == 1, 'entry/admission count changed'
    assert all(totals[name] == 0 for name in FAILURE), 'an actual fallback/central edge executed'
    witnesses = [dict(id=s['id'], block=s['block'], count=counts[s['id']])
                 for s in plan['sites'] if s['kind'] == 'membership'
                 and s['label'] in ('target_success', 'business_success')
                 and s['block'] in structure['fast_nodes']
                 and s['block'] in structure['small_switch_nodes'] and counts[s['id']] >= 2]
    return dict(totals=totals, repeated_same_guard_sites=witnesses,
                loop_witness=bool(witnesses))


def read_counts(path, number):
    raw = Path(path).read_bytes()
    assert len(raw) >= 8
    count, = struct.unpack('<Q', raw[:8])
    assert count == number and len(raw) == 8 * (count + 1), 'counter stream length/EOF differs'
    return list(struct.unpack('<' + 'Q' * count, raw[8:]))


def main(root):
    frozen, run = root / 'frozen', root / 'run'
    pre = json.loads((root / 'PREDECLARED.json').read_text())
    plan = json.loads((frozen / 'PLAN.json').read_text())
    assert all(sha(frozen / n) == h for n, h in pre['files'].items())
    assert all(sha(n) == h for n, h in pre['external_pins'].items())
    for name, record in plan['source_pins'].items():
        assert sha(frozen / name) == record['sha256']
    assert sha(frozen / 'raw.ll') == plan['input_ir_sha256']
    # This namespace must name the compiler's real original input and exact
    # compile argv. A component-tail file cannot masquerade as native-final.
    binding = json.loads((frozen / 'native-bindings.json').read_text())
    assert binding['recipe_steps'] == 58
    assert binding['raw_ir']['sha256'] == plan['input_ir_sha256']
    assert binding['final_ir']['sha256'] == plan['final_ir_sha256']
    assert binding['raw_ir']['path'] in binding['compile_command']['argv']
    inventory = json.loads((frozen / 'module.after.json').read_text())
    before = json.loads((frozen / 'module.before.json').read_text())
    anchor = json.loads((frozen / 'anchor.json').read_text())
    manifest = json.loads((frozen / 'manifest.json').read_text())
    result = review_structure(plan, (frozen / 'raw.ll').read_text(), inventory, before, anchor, manifest)
    assert [r['index'] for r in plan['native_inputs']] == list(range(4))
    for row in plan['native_inputs']:
        assert sha(frozen / f"input-{row['index']}.bin") == row['sha256']
        assert sha(frozen / f"output-{row['index']}.bin") == row['expected_output_sha256']
    observations = json.loads((run / 'observations.json').read_text())
    assert len(observations) == 8 and {(r['mode'], r['input']) for r in observations} == {(m, i) for m in ('normal', 'double-count') for i in range(4)}
    rows, normal, witnesses = [], {}, []
    for mode in ('normal', 'double-count'):
        for i in range(4):
            counts = read_counts(run / f'{mode}-{i}.counts', len(plan['sites']))
            observed = next(r for r in observations if r['mode'] == mode and r['input'] == i)
            assert counts == observed['counts']
            assert (run / f'{mode}-{i}.state').read_bytes() == (frozen / f'output-{i}.bin').read_bytes(), 'complete native state differs'
            if mode == 'normal':
                checked = review_counts(plan, result, counts)
                normal[i] = counts
                if checked['loop_witness']:
                    witnesses.append(i)
            else:
                assert counts == [2 * value for value in normal[i]] and counts != normal[i]
                try:
                    review_counts(plan, result, counts)
                except AssertionError:
                    pass
                else:
                    raise AssertionError('double counts were not detected')
                checked = {'double_count_control_detected': True}
            rows.append(dict(mode=mode, input=i, counts=counts, review=checked,
                             full_state_bytes=(run / f'{mode}-{i}.state').stat().st_size))
    assert witnesses, 'no frozen input repeated the same actual small-switch fast-SCC guard'
    for mode in ('normal', 'double-count', 'omit-central'):
        report = json.loads((run / (mode + '.json')).read_text())
        assert report['input_ir_sha256'] == plan['input_ir_sha256']
        assert report['plan_sha256'] == sha(frozen / 'PLAN.json')
        assert report['output_ir_sha256'] == sha(run / (mode + '.ll'))
        emitted = [s['id'] for s in report['emitted_sites']]
        expected = list(range(len(plan['sites'])))
        omitted = []
        if mode == 'omit-central':
            omitted = [next(s['id'] for s in plan['sites'] if s['label'] == 'central_dispatcher')]
            expected.remove(omitted[0])
        assert emitted == expected and report['omitted_sites'] == omitted
        assert report['emitted_sites'] == [{k: s[k] for k in ('id', 'block', 'kind', 'label')}
                                           for s in plan['sites'] if s['id'] in expected]
        assert report['declared_sites'] == len(plan['sites'])
        assert report['guest_loads_added'] == 0 and report['cfg_changed'] is False
        assert report['optimized_after_instrumentation'] is False
    canonical = root / 'review-canonical-before.ll'
    assert not canonical.exists()
    command = ['opt-21', '-passes=verify', '-S', str(frozen / 'raw.ll'), '-o', str(canonical)]
    verify = subprocess.run(command, capture_output=True, timeout=180)
    (root / 'review-opt-command.json').write_text(json.dumps(command) + '\n')
    (root / 'review-opt.stdout').write_bytes(verify.stdout)
    (root / 'review-opt.stderr').write_bytes(verify.stderr)
    assert verify.returncode == 0, verify.stderr.decode(errors='replace')
    attrs = lambda text: [line.strip() for line in text.splitlines()
                          if (line.startswith('declare ') and '@llvm.' in line) or line.startswith('attributes #')]
    original_attrs = attrs(canonical.read_text())
    assert all(attrs((run / (mode + '.ll')).read_text()) == original_attrs
               for mode in ('normal', 'double-count', 'omit-central'))
    output = root / 'STATE-VALUES-INDEPENDENT-REVIEW.json'
    assert not output.exists()
    summary = dict(profile=PROFILE, case=plan['case'], input_ir_sha256=plan['input_ir_sha256'],
                   actual_cfg_blocks=result['actual_cfg_blocks'],
                   recomputed_fast_scc_sizes=sorted(len(c) for c in result['fast_cycles']),
                   normal_full_state_matches=4, controlled_full_state_matches=4,
                   double_count_controls_detected=4, omitted_central_emission_detected=1,
                   loop_witness_inputs=witnesses, all_four_loop_witnesses=len(witnesses) == 4,
                   intrinsic_attributes_preserved=True, rows=rows,
                   scope='Actual fresh58 compiler-input view only; 4 fixed inputs, complete states; conditional success-edge projection is not arbitrary-input success or a claim about recursive ordinary callees.')
    output.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: v for k, v in summary.items() if k != 'rows'}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    main(parser.parse_args().root)
