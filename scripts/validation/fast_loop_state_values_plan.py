#!/usr/bin/env python3
"""Prepare a new fresh58-only observation plan; this does not execute native IR."""
import argparse
import hashlib
import json
from pathlib import Path

from fast_loop_state_values_review import PROFILE, classify_central, review_structure


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ('anchor', 'before', 'after', 'manifest', 'hints', 'raw',
                 'final-ir', 'native-bindings', 'inputs', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--case', required=True)
    args = parser.parse_args()
    assert not args.output.exists(), 'refuse to overwrite a frozen plan'
    # The frozen V4 source must be supplied on PYTHONPATH; older un-frozen-EQ
    # auditors are deliberately not used as a compatibility fallback.
    import guarded_state_values_set_cycles_v4_audit as audit
    load = lambda p: json.loads(p.read_text())
    anchor, before, after, manifest, hints = [load(p) for p in
        (args.anchor, args.before, args.after, args.manifest, args.hints)]
    hints = [] if hints is None else hints['hints']
    binding = load(args.native_bindings)
    assert binding['recipe_steps'] == 58
    assert Path(binding['raw_ir']['path']).resolve() == args.raw.resolve()
    assert binding['raw_ir']['sha256'] == sha(args.raw)
    assert binding['raw_ir']['path'] in binding['compile_command']['argv']
    assert Path(binding['final_ir']['path']).resolve() == args.final_ir.resolve()
    assert binding['final_ir']['sha256'] == sha(args.final_ir)
    metric = audit.metric(anchor, before, after, manifest, hints)
    assert metric['set_aware_guard_success_entry_closure'] and not metric['remaining_central_blocks'], 'fresh58 final conditional closure is incomplete'
    old, old_failure = None, None
    try:
        old = audit.family.evaluate(anchor, after, manifest)
    except (AssertionError, ValueError) as error:
        old_failure = str(error) or type(error).__name__
    central, central_pc, pcs = classify_central(anchor, after, manifest)
    assert central == metric['central_blocks']
    fast = {name for component in metric['fast_cycles'] for name in component['blocks']}
    entry = audit.closure.admission(after, manifest)
    sites = []
    def add(block, kind, label, **kwargs):
        sites.append(dict(id=len(sites), block=block, kind=kind, label=label, **kwargs))
    add(after['blocks'][0]['name'], 'visit', 'root_entry')
    add(entry['block'], 'branch', 'entry_admission', slot=0)
    add(entry['failure'], 'visit', 'entry_fallback')
    states = set()
    for witness in metric['state_guard_witnesses']:
        name = witness['seed']
        assert name not in states
        states.add(name)
        add(name, 'branch', 'state_success', slot=0, in_fast_scc=name in fast)
        add(name, 'branch', 'state_failure', slot=1, in_fast_scc=name in fast)
    targets = {guard['block'] for guard in after['same_value_guards']}
    targets.update(w['target_guard'] for w in metric['state_guard_witnesses'])
    for name in sorted(targets):
        add(name, 'branch', 'target_success', slot=0, in_fast_scc=name in fast)
        add(name, 'branch', 'target_failure', slot=1, in_fast_scc=name in fast)
    switches = {s['block']: s for s in after['switches']}
    for group in metric['validated_sets']:
        full = switches[group['fallback']]
        assert full['default'] in pcs
        label = 'target' if pcs[full['default']] == central_pc else 'business'
        values = [audit.closure.int_value(c['value']) % (1 << 64) for c in group['cases']]
        for member in (True, False):
            add(group['block'], 'membership', label + ('_success' if member else '_failure'),
                values=values, member=member, in_fast_scc=group['block'] in fast,
                validated_small_switch=True)
    for name in central:
        add(name, 'visit', 'central_dispatcher')
    sources = {
        'anchor.json': args.anchor, 'module.before.json': args.before,
        'module.after.json': args.after, 'manifest.json': args.manifest,
        'hints.json': args.hints, 'raw.ll': args.raw,
        'native-bindings.json': args.native_bindings,
    }
    modules = (audit, audit.state, audit.family, audit.family.audit, audit.closure)
    audit_pins = {Path(module.__file__).name: sha(Path(module.__file__)) for module in modules}
    plan = dict(
        schema='final-ir-loop-observation-v1', observer_profile=PROFILE,
        recipe_steps=58, case=args.case, function=manifest['view_symbol'],
        root_symbol=manifest['root_symbol'], input_ir_sha256=sha(args.raw),
        final_ir_sha256=sha(args.final_ir), sites=sites,
        admission_check=entry, central_blocks=central, central_source_pc=central_pc,
        fast_sccs=[c['blocks'] for c in metric['fast_cycles']],
        conditional_closure=True, state_guard_count=len(states),
        set_aware_state_success_metric=metric,
        frozen_target_only_EQ_cycle_metric=old, frozen_metric_failure=old_failure,
        source_pins={name: dict(original_path=str(path.resolve()), sha256=sha(path))
                     for name, path in sources.items()},
        audit_source_pins=audit_pins,
        native_inputs=[dict(index=i, sha256=sha(args.inputs / f'input-{i}.bin'),
                            expected_output_sha256=sha(args.inputs / f'output-{i}.bin'))
                       for i in range(4)],
        scope='Actual fresh58 compiler-input view; conditional on observed state and selector checks. Every ordinary condition, business case and side exit is retained. At least one fixed input must repeat the same validated small-switch guard in an actual fast SCC; every normal input must preserve all bytes, entry1 and failure/central0. This is not arbitrary-input guard success or a claim about recursive ordinary callees.',
    )
    structure = review_structure(plan, args.raw.read_text(), after, before, anchor, manifest)
    args.output.write_text(json.dumps(plan, indent=2) + '\n')
    print(json.dumps(dict(case=args.case, sites=len(sites), state_guards=len(states),
                          actual_cfg_blocks=structure['actual_cfg_blocks'],
                          fast_scc_sizes=sorted(len(c) for c in structure['fast_cycles']),
                          plan_sha256=sha(args.output))))


if __name__ == '__main__':
    main()
