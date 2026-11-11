#!/usr/bin/env python3
"""Explicit edge_worklist activation around the unchanged bounded real oracle.

Only PassPipeline.add('guest-memory', ...) receives one additional numeric
option in enabled mode. Lifting, inputs, pass order, rounds and comparators
remain owned by bounded_memory_pipeline_real.py and its original backend.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import bounded_memory_pipeline_real as original


def validate_activation(records, enabled, expected_count=4):
    expected = [dict(name=name, options=(
        dict(run_mem2reg=0, **({'edge_worklist': 1} if enabled else {}))
        if name == 'guest-memory' else
        {'allow_plain_stores': 1} if name == 'dispatch-specialize' else {}))
        for name in original.ACTUAL_STEPS]
    if len(records) != expected_count or any(
            row != dict(steps=expected, run_options=dict(max_iterations=3, max_pass_runs=32))
            for row in records):
        raise RuntimeError('activation changed options/order/rounds or lost a module')


def adapter(factory, enabled, records):
    class ActivatedPipeline:
        def __init__(self, *args, **kwargs):
            if args or kwargs:
                raise RuntimeError('unexpected actual pipeline construction')
            self.inner = factory()
            self.steps = []

        def __enter__(self):
            self.inner.__enter__()
            return self

        def __exit__(self, *args):
            return self.inner.__exit__(*args)

        def register(self, *args, **kwargs):
            self.inner.register(*args, **kwargs)
            return self

        def add(self, name, **options):
            if name == 'guest-memory':
                if options != {'run_mem2reg': 0}:
                    raise RuntimeError('unexpected baseline guest-memory options')
                if enabled:
                    options = dict(options, edge_worklist=1)
            self.steps.append(dict(name=name, options=dict(options)))
            self.inner.add(name, **options)
            return self

        def run(self, target, **options):
            record = dict(steps=self.steps, run_options=options)
            validate_activation([record], enabled, expected_count=1)
            records.append(record)
            return self.inner.run(target, **options)
    return ActivatedPipeline


def self_test():
    import copy
    class Fake:
        def __enter__(self): return self
        def __exit__(self, *_): return None
        def add(self, name, **options): return self
        def run(self, target, **options): return {'sentinel': target}
    rejected = 0
    for enabled in (False, True):
        records = []
        for _ in range(4):
            with adapter(Fake, enabled, records)() as pipeline:
                for name in original.ACTUAL_STEPS:
                    options = ({'run_mem2reg': 0} if name == 'guest-memory' else
                               {'allow_plain_stores': 1} if name == 'dispatch-specialize' else {})
                    pipeline.add(name, **options)
                assert pipeline.run(17, max_iterations=3, max_pass_runs=32) == {'sentinel': 17}
        validate_activation(records, enabled)
        bad = []
        x = copy.deepcopy(records); x.pop(); bad.append(x)
        x = copy.deepcopy(records); x[0]['run_options']['max_iterations'] = 4; bad.append(x)
        x = copy.deepcopy(records); x[0]['steps'][3]['options']['edge_worklist'] = 0; bad.append(x)
        x = copy.deepcopy(records); x[0]['steps'][0]['name'] = 'dce'; bad.append(x)
        for value in bad:
            try: validate_activation(value, enabled)
            except RuntimeError: rejected += 1
            else: raise RuntimeError('activation fault control was accepted')
    return dict(ok=True, positive_modes=2, rejected_controls=rejected,
                new_native_executions=0, new_analysis_runs=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--edge-worklist', type=int, choices=(0, 1), required=False)
    parser.add_argument('--frozen', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--tigress')
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(self_test(), indent=2))
        return 0
    if any(value is None for value in (args.edge_worklist, args.frozen, args.out, args.tigress)):
        parser.error('--edge-worklist --frozen --out --tigress required')
    import dragon
    records = []
    saved_pipeline, saved_argv = dragon.PassPipeline, sys.argv
    status = None
    try:
        dragon.PassPipeline = adapter(saved_pipeline, bool(args.edge_worklist), records)
        sys.argv = [original.__file__, '--frozen', str(args.frozen), '--out', str(args.out),
                    '--tigress', args.tigress, '--memory-mode', 'builtin', '--rounds', '3']
        status = original.main()
        validate_activation(records, bool(args.edge_worklist))
    finally:
        dragon.PassPipeline, sys.argv = saved_pipeline, saved_argv
        if args.out.exists():
            receipt = dict(enabled=bool(args.edge_worklist), records=records,
                           backend_status=status, driver_sha256=original.digest(__file__),
                           original_wrapper_sha256=original.digest(original.__file__))
            original.save(args.out / 'edge-worklist-activation.json', receipt)
    return status


if __name__ == '__main__':
    raise SystemExit(main())
