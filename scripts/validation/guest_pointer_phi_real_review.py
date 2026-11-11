#!/usr/bin/env python3
"""Review a frozen real replay against its predeclared pointer-PHI precision gate."""
import argparse
import hashlib
import json
from pathlib import Path
import bounded_memory_pipeline_audit as audit
import bounded_memory_pipeline_real as pipeline
import store_hub_real as metric


def precision(manifest, entry, rounds, final):
    old = manifest['baseline_indices'][hex(entry)]
    if len(rounds) != 3 or any(not set(required).issubset(actual)
                             for required, actual in zip(old['rounds'], rounds)):
        raise RuntimeError('baseline per-round target LOAD coverage lost')
    if not set(old['final']).issubset(final):
        raise RuntimeError('baseline final target LOAD coverage lost')
    if hex(entry) == manifest['precision_gate']['combo_entry']:
        candidates = set(manifest['precision_gate']['candidate_state_indices'].values())
        added = set(final) - set(old['final'])
        if not candidates.intersection(added):
            raise RuntimeError('none of the predeclared pointer-PHI states reaches a new actual target LOAD')
    return sorted(set(final) - set(old['final']))


def self_test():
    manifest = dict(baseline_indices={'0x10770': dict(rounds=[[], [90120], [90120]], final=[90120])},
                    precision_gate=dict(combo_entry='0x10770', candidate_state_indices={'14': 90232, '34': 90392, '32': 90376}))
    precision(manifest, 0x10770, [[], [90120], [90120, 90232]], [90120, 90232])
    broken = [([[], [90120], [90120]], [90120]),
              ([[], [90120], [90120, 99999]], [90120, 99999]),
              ([[], [], [90120, 90232]], [90120, 90232]),
              ([[], [90120], [90232]], [90232]),
              ([[], [90120]], [90120, 90232])]
    for rounds, final in broken:
        try:
            precision(manifest, 0x10770, rounds, final)
        except RuntimeError:
            continue
        raise RuntimeError('precision checker missed corrupt control')
    return dict(ok=True, positive=1, rejected_precision_controls=len(broken),
                inherited_switch_and_metric_controls=audit.self_test(),
                new_native_executions=0, new_analysis_runs=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--group', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(self_test(), indent=2))
        return 0
    if any(value is None for value in (args.manifest, args.group, args.out)):
        parser.error('--manifest --group --out required')
    manifest = json.loads(args.manifest.read_text())
    summary = json.loads((args.group / 'bounded-memory-summary.json').read_text())
    semantic = json.loads((args.group / 'summary.json').read_text())
    preflight = json.loads((args.group / 'preflight.json').read_text())
    if not summary['ok'] or not semantic['ok']:
        raise RuntimeError('incomplete real oracle execution')
    if semantic['C_statuses'] != {'match': 16} or semantic['LLVM_statuses'] != {'match': 16}:
        raise RuntimeError('full C/LLVM oracle coverage changed')
    controls = semantic['controls']
    if len(controls) != 16 or not all(x['register_detected'] and x['memory_detected']
            and len(x['exit_fields_detected']) == 7 and all(x['exit_fields_detected']) for x in controls):
        raise RuntimeError('full register/Guest/Exit fault control failed')
    if preflight['inputs'] != manifest['fixture_sha256']:
        raise RuntimeError('frozen fixtures changed')
    if summary['harness_sha256'] != manifest['wrapper_sha256']:
        raise RuntimeError('frozen replay wrapper changed')
    if summary['requested_rounds'] != 3 or summary['max_pass_runs'] != 32:
        raise RuntimeError('bounded iteration contract changed')
    if summary['actual_recipe'] != list(pipeline.ACTUAL_STEPS):
        raise RuntimeError('pipeline order changed')
    entries = [row['entry'] for row in summary['rows']]
    if len(entries) != 4 or set(map(hex, entries)) != set(manifest['baseline_indices']):
        raise RuntimeError('missing/extra real module')
    rows = []
    for row in summary['rows']:
        entry = row['entry']
        relative = row['work'].split('/' + args.group.name + '/', 1)[1]
        work = args.group / relative
        before = (work / 'before-pipeline.ll').read_text()
        after = (work / 'after-pipeline.ll').read_text()
        digest = lambda text: hashlib.sha256(text.encode()).hexdigest()
        if digest(before) != manifest['baseline_indices'][hex(entry)]['before_sha256']:
            raise RuntimeError('lifted input changed')
        if digest(before) != row['before_sha256'] or digest(after) != row['after_sha256']:
            raise RuntimeError('captured IR hashes changed')
        pipeline.validate_report(row['report'], 3)
        if row['report']['iterations'] != 3 or row['original_request'] != list(pipeline.LEGACY_REQUEST):
            raise RuntimeError('oracle request/round count changed')
        original = audit.switches(before, entry)
        if len(original) != 2:
            raise RuntimeError('missing original CFF/business switch')
        retained = audit.preserve(original, audit.switches(after, entry))
        rounds = []
        for checkpoint in row['rounds']:
            text = (work / ('round-' + str(checkpoint['iteration']) + '.ll')).read_text()
            actual = sorted({x['index'] for x in metric.actual_loads(text, entry)})
            if digest(text) != checkpoint['sha256'] or actual != checkpoint['actual_indices']:
                raise RuntimeError('round IR or measured index changed')
            rounds.append(actual)
        final = sorted({x['index'] for x in metric.actual_loads(after, entry)})
        added = precision(manifest, entry, rounds, final)
        rows.append(dict(entry=entry, round_indices=rounds, final_indices=final,
                         added_final_indices=added, retained_original_switches=retained,
                         memory_runs=[x for x in row['report']['runs'] if x['name'] == 'guest-memory'],
                         dispatch_runs=[x for x in row['report']['runs'] if x['name'] == 'dispatch-specialize'],
                         cff_before=row['root_before']['CFF_switches'], cff_after=row['root_after']['CFF_switches']))
    focus = next(row for row in rows if hex(row['entry']) == manifest['precision_gate']['combo_entry'])
    result = dict(ok=True, manifest_sha256=hashlib.sha256(args.manifest.read_bytes()).hexdigest(), rows=rows,
                  candidate_states={state: dict(index=index, actual_final_index_present=index in focus['final_indices'])
                                    for state, index in manifest['precision_gate']['candidate_state_indices'].items()},
                  C_matches=16, LLVM_matches=16, register_controls=16, memory_controls=16, Exit_field_controls=112,
                  original_switches_preserved=8, exclusions=[], skips=[], normalizations=[],
                  new_native_executions=0, new_analysis_runs=0)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps(dict(ok=True, candidates=result['candidate_states'],
                         added={hex(row['entry']): row['added_final_indices'] for row in rows})))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
