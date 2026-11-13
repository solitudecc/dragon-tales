#!/usr/bin/env python3
"""Synthetic checker controls only. No undef execution or real58 result claim."""
import copy
import json
from pathlib import Path
import re
import tempfile

import fast_loop_state_values_review as review
from fast_loop_state_values_real import gate


def fixture():
    # The state is deliberately a loop PHI/expression, not a table LOAD. CALL
    # and STORE remain in the original seed exactly once, unlike target guards.
    rows = {
        'entry': ['br label %admit'],
        'admit': ['%admission = icmp eq i32 %x, 2', 'br i1 %admission, label %seed, label %entryfail'],
        'entryfail': ['ret void'],
        'seed': ['%state = phi i32 [ %x, %admit ], [ %next, %body ]',
                 'call void @unknown(ptr %p)', 'store i32 %state, ptr %p, align 1',
                 '%original = icmp ugt i32 %state, 50', '%state.eq = icmp eq i32 %state, 2',
                 '%safe.eq = freeze i1 %state.eq', 'br i1 %safe.eq, label %gate, label %old.branch'],
        'old.branch': ['br i1 %original, label %left, label %right'],
        'left': ['br label %central'],
        'right': ['br label %central'],
        'gate': ['br label %target'],
        'target': ['%target.read = load i64, ptr %table, align 1',
                   '%target.eq = icmp eq i64 %target.read, 43',
                   'br i1 %target.eq, label %body, label %target.full'],
        'target.full': ['switch i64 %target.read, label %cexit [\n    i64 43, label %body\n    i64 44, label %central\n  ]'],
        'body': ['%next = add i32 %state, 0', 'br i1 %again, label %seed, label %small'],
        'small': ['%small.read = load i64, ptr %table, align 1',
                  'switch i64 %small.read, label %small.full [\n    i64 43, label %body\n  ]'],
        'small.full': ['switch i64 %small.read, label %cexit [\n    i64 43, label %body\n    i64 44, label %central\n  ]'],
        'central': ['%central.read = load i64, ptr %table, align 1',
                    'switch i64 %central.read, label %cexit [\n    i64 43, label %body\n    i64 44, label %body\n  ]'],
        'cexit': ['%ep = getelementptr i8, ptr %exit, i64 16',
                  'store i64 111, ptr %ep, align 1', 'ret void'],
    }
    def inventory(records):
        blocks, structures, switches = [], [], []
        for name, instructions in records.items():
            items = []
            for instruction in instructions:
                rhs = instruction.split(' = ', 1)[-1]
                opcode = rhs.split()[0]
                operand = instruction.split(' = ', 1)[0] if ' = ' in instruction else ''
                item = dict(opcode=opcode, operand=operand, instruction='  ' + instruction)
                if opcode == 'phi':
                    item['incoming'] = [dict(value=v, block=b) for v, b in re.findall(r'\[ (%[^ ,]+), %([^ ]+) \]', instruction)]
                items.append(item)
            term = items[-1]['instruction']
            blocks.append(dict(name=name, successors=[dict(slot=n, target=t) for n, t in enumerate(re.findall(r'label %([-a-zA-Z$._0-9]+)', term))], terminator=term, stores=[], calls=[]))
            structures.append(dict(block=name, instructions=items))
            if items[-1]['opcode'] == 'switch':
                selector, default, cases = review.switch(term)
                switches.append(dict(block=name, selector=selector, default=default,
                                     cases=[dict(value='i64 ' + str(k), target=t) for k, t in cases]))
        return dict(function='view', blocks=blocks, state_value_structure=structures, switches=switches,
                    same_value_guards=[])
    def raw(records):
        return ('declare void @unknown(ptr)\n'
                'define void @view(i32 %x, i1 %again, ptr %p, ptr %table, ptr %exit) {\n' +
                '\n'.join(name + ':\n' + '\n'.join('  ' + i for i in items) for name, items in records.items()) + '\n}\n')
    before_rows = copy.deepcopy(rows)
    before_rows['seed'] = before_rows['seed'][:-3] + ['br i1 %original, label %left, label %right']
    for name in ('gate', 'old.branch', 'target', 'target.full'):
        del before_rows[name]
    # Retain the original complete source fallback independently of new names.
    before_rows['source.full'] = ['%source.read = load i64, ptr %table, align 1',
                                 'switch i64 %source.read, label %cexit [\n    i64 43, label %body\n    i64 44, label %central\n  ]']
    before = inventory(before_rows)
    after = inventory(rows)
    for inv in (before, after):
        next(b for b in inv['blocks'] if b['name'] == 'cexit')['stores'] = [
            dict(value='i64 111', pointer='%ep = getelementptr i8, ptr %exit, i64 16')]
    anchor = copy.deepcopy(before)
    source = next(s for s in anchor['switches'] if s['block'] == 'central')
    source.update(same_block_load=True, address_decimal='16')
    manifest = dict(memory=[dict(offset='0x10', bytes='2b00000000000000')])
    witness = dict(seed='seed', state='%state', width=32, prediction=2,
                   original_predicate='  %original = icmp ugt i32 %state, 50',
                   state_success='gate', state_fallback='old.branch',
                   target_guard='target', target_success='body', frozen_i1='%safe.eq',
                   target_fallback_chain=[dict(block='target.full', cases=2, default='cexit')])
    small = dict(block='small', selector='%small.read', fallback='small.full',
                 cases=[dict(value='i64 43', target='body')])
    central = ['central', 'small.full', 'target.full']
    fast = ['seed', 'gate', 'target', 'body', 'small']
    metric = dict(state_guard_witnesses=[witness], validated_sets=[small], central_blocks=central)
    sites = []
    def add(block, kind, label, **kwargs):
        sites.append(dict(id=len(sites), block=block, kind=kind, label=label, **kwargs))
    add('entry', 'visit', 'root_entry')
    add('admit', 'branch', 'entry_admission', slot=0)
    add('entryfail', 'visit', 'entry_fallback')
    for label, slot in (('state_success', 0), ('state_failure', 1)):
        add('seed', 'branch', label, slot=slot, in_fast_scc=True)
    for label, slot in (('target_success', 0), ('target_failure', 1)):
        add('target', 'branch', label, slot=slot, in_fast_scc=True)
    for member in (True, False):
        add('small', 'membership', 'target_success' if member else 'target_failure',
            values=[43], member=member, in_fast_scc=True, validated_small_switch=True)
    for name in central:
        add(name, 'visit', 'central_dispatcher')
    plan = dict(schema='final-ir-loop-observation-v1', observer_profile=review.PROFILE,
                recipe_steps=58, function='view', conditional_closure=True,
                sites=sites, central_blocks=central, fast_sccs=[fast],
                state_guard_count=1, set_aware_state_success_metric=metric,
                admission_check=dict(block='admit', success='seed', failure='entryfail'))
    return rows, inventory, raw, plan, after, before, anchor, manifest


def main():
    rows, inventory, raw, plan, after, before, anchor, manifest = fixture()
    original = raw(rows)
    checked = review.review_structure(plan, original, after, before, anchor, manifest)
    assert len(checked['fast_cycles']) == 1 and len(checked['fast_cycles'][0]) == 5
    # Generated switches print their metadata on the closing-bracket line.
    # It remains part of the same SwitchInst, rather than a new instruction.
    annotated_rows = copy.deepcopy(rows)
    for records in annotated_rows.values():
        for n, instruction in enumerate(records):
            if instruction.startswith('switch '):
                records[n] = instruction + ', !dragon.guarded-transitions.generated !0'
    annotated_inventory = inventory(annotated_rows)
    next(b for b in annotated_inventory['blocks'] if b['name'] == 'cexit')['stores'] = copy.deepcopy(next(b for b in after['blocks'] if b['name'] == 'cexit')['stores'])
    review.review_structure(plan, raw(annotated_rows) + '!0 = !{}\n', annotated_inventory, before, anchor, manifest)
    controls = []
    def reject(label, callback):
        try:
            callback()
        except (AssertionError, KeyError, ValueError):
            controls.append(label)
        else:
            raise AssertionError('undetected ' + label)
    def changed_ir(label, edit):
        bad = copy.deepcopy(rows)
        edit(bad)
        inv = inventory(bad)
        next(b for b in inv['blocks'] if b['name'] == 'cexit')['stores'] = copy.deepcopy(next(b for b in after['blocks'] if b['name'] == 'cexit')['stores'])
        reject(label, lambda: review.review_structure(plan, raw(bad), inv, before, anchor, manifest))
    changed_ir('unfrozen-state-EQ', lambda b: b['seed'].__setitem__(-2, '%safe.eq = xor i1 %state.eq, false'))
    changed_ir('wrong-state-SSA', lambda b: b['seed'].__setitem__(-3, '%state.eq = icmp eq i32 %next, 2'))
    changed_ir('wrong-state-value', lambda b: b['seed'].__setitem__(-3, '%state.eq = icmp eq i32 %state, 3'))
    changed_ir('freeze-state-instead-of-condition', lambda b: b['seed'].__setitem__(-2, '%safe.eq = freeze i32 %state'))
    changed_ir('changed-original-comparison', lambda b: b['seed'].__setitem__(-4, '%original = icmp ugt i32 %state, 49'))
    changed_ir('reversed-original-fallback', lambda b: b['old.branch'].__setitem__(-1, 'br i1 %original, label %right, label %left'))
    changed_ir('duplicated-seed-CALL', lambda b: b['seed'].insert(2, 'call void @unknown(ptr %p)'))
    changed_ir('omitted-seed-STORE', lambda b: b['seed'].pop(2))
    changed_ir('target-fallback-reread', lambda b: b['target.full'].__setitem__(0, b['target.full'][0].replace('%target.read', '%small.read')))
    changed_ir('small-switch-wrong-case-target', lambda b: b['small'].__setitem__(-1, b['small'][-1].replace('label %body', 'label %cexit')))
    changed_ir('ordinary-sideexit-still-reaches-central', lambda b: b['body'].__setitem__(-1, 'br i1 %again, label %seed, label %central'))
    reject('raw-inventory-different-byte', lambda: review.review_structure(plan, original.replace('add i32 %state, 0', 'add i32 %state, 1'), after, before, anchor, manifest))
    for label, edit in (
        ('missing-state-failure-observer', lambda p: p['sites'].pop(4)),
        ('wrong-state-observer-direction', lambda p: p['sites'][3].update(slot=1)),
        ('extra-unproved-ordinary-projection', lambda p: p['sites'].append(dict(id=len(p['sites']), block='body', kind='branch', label='business_success', slot=0))),
        ('forged-fast-SCC', lambda p: p['fast_sccs'][0].append('central')),
        ('wrong-small-set-members', lambda p: p['sites'][7].update(values=[44])),
        ('wrong-membership-family', lambda p: [s.update(label='business_' + s['label'].split('_')[1]) for s in p['sites'] if s['kind']=='membership']),
        ('wrong-small-fast-flag', lambda p: p['sites'][7].update(in_fast_scc=False)),
    ):
        bad = copy.deepcopy(plan)
        edit(bad)
        # IDs alone do not stand in for observation identity coverage.
        for i, site in enumerate(bad['sites']):
            site['id'] = i
        reject(label, lambda b=bad: review.review_structure(b, original, after, before, anchor, manifest))
    bad = copy.deepcopy(plan)
    bad['central_blocks'] = ['central']
    bad['set_aware_state_success_metric']['central_blocks'] = ['central']
    bad['sites'] = [s for s in bad['sites'] if s['label'] != 'central_dispatcher' or s['block'] == 'central']
    for i, site in enumerate(bad['sites']):
        site['id'] = i
    reject('omitted-central-in-plan-and-cached-metric', lambda: review.review_structure(bad, original, after, before, anchor, manifest))
    normal = [0] * len(plan['sites'])
    for site in plan['sites']:
        if site['label'] in ('root_entry', 'entry_admission'):
            normal[site['id']] = 1
        elif site['label'] in review.SUCCESS:
            normal[site['id']] = 3
    assert review.review_counts(plan, checked, normal)['loop_witness']
    assert gate(plan, normal)['loop_witness']
    for label in ('root_entry', 'entry_admission', 'entry_fallback', 'state_failure', 'target_failure', 'central_dispatcher'):
        bad = list(normal)
        site = next(s for s in plan['sites'] if s['label'] == label)
        bad[site['id']] += 1
        reject('count-' + label, lambda b=bad: review.review_counts(plan, checked, b))
        assert not gate(plan, bad)['entry_and_fallback_gate']
    reject('all-double-counts', lambda: review.review_counts(plan, checked, [2*n for n in normal]))
    scalar_only = list(normal)
    scalar_only[7] = 1
    assert not review.review_counts(plan, checked, scalar_only)['loop_witness']
    assert not gate(plan, scalar_only)['loop_witness']
    controls.append('repeated-scalar-EQ-is-not-small-switch-loop-witness')
    with tempfile.TemporaryDirectory() as directory:
        p = Path(directory) / 'counts'
        for label, data in (('truncated-count-header', b'\0'*7),
                            ('missing-counts', len(normal).to_bytes(8, 'little')),
                            ('extra-count-byte', len(normal).to_bytes(8, 'little') + b'\0'*(8*len(normal)+1))):
            p.write_bytes(data)
            reject(label, lambda: review.read_counts(p, len(normal)))
    print(json.dumps(dict(synthetic_valid_raw_inventory=2, annotated_switch_closing_line=True, valid_phi_state_and_seed_effects=True,
                          controls_detected=len(controls), controls=controls,
                          native_executions=0, real58_executions=0), indent=2))


if __name__ == '__main__':
    main()
