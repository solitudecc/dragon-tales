#!/usr/bin/env python3
"""Full-state replay of an explicit bounded memory/state pipeline.

The historical oracle supplies lifting, inputs, native execution and every
field/error control unchanged. Only its optimization request is substituted.
Callback and builtin modes execute the same declared seven-pass sequence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
STEPS = ('mem2reg', 'instcombine', 'gvn', 'guest-memory',
         'dispatch-specialize', 'control-flow', 'dce')
ACTUAL_STEPS = STEPS + ('bounded-memory-checkpoint',)
LEGACY_REQUEST = ('guest-memory-ssa', 'mem2reg', 'instcombine', 'gvn', 'mba',
                  'predicates', 'dispatch-specialize', 'dispatch-deflatten',
                  'control-flow', 'dce')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def validate_report(report, rounds):
    if not report['ok'] or type(report['iterations']) is not int:
        raise RuntimeError('failed/malformed actual pipeline report')
    iterations = report['iterations']
    if not 1 <= iterations <= rounds or len(report['runs']) != len(ACTUAL_STEPS) * iterations:
        raise RuntimeError('missing/extra actual pipeline runs')
    if iterations < rounds and not report['converged']:
        raise RuntimeError('unexplained early pipeline stop')
    for index, row in enumerate(report['runs']):
        if (row['name'] != ACTUAL_STEPS[index % len(ACTUAL_STEPS)] or type(row['iteration']) is not int
                or row['iteration'] != index // len(ACTUAL_STEPS) + 1):
            raise RuntimeError('wrong pass order/iteration')


def self_test():
    import copy
    good = dict(ok=True, iterations=3, converged=False,
                runs=[dict(name=name, iteration=i) for i in (1, 2, 3) for name in ACTUAL_STEPS])
    validate_report(good, 3)
    broken = []
    x = copy.deepcopy(good); x['runs'].pop(); broken.append(x)
    x = copy.deepcopy(good); x['runs'].append(x['runs'][0]); broken.append(x)
    x = copy.deepcopy(good); x['runs'][0]['name'] = 'dce'; broken.append(x)
    x = copy.deepcopy(good); x['runs'][8]['iteration'] = True; broken.append(x)
    x = copy.deepcopy(good); x['iterations'] = True; broken.append(x)
    x = copy.deepcopy(good); x['iterations'] = 1; x['runs'] = x['runs'][:8]; broken.append(x)
    for x in broken:
        try:
            validate_report(x, 3)
        except RuntimeError:
            continue
        raise RuntimeError('coverage checker accepted corrupt control')
    x = copy.deepcopy(good); x['iterations'] = 1; x['runs'] = x['runs'][:8]; x['converged'] = True
    validate_report(x, 3)
    return dict(ok=True, positive=2, rejected_controls=len(broken),
                new_native_executions=0, new_analysis_runs=0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--self-test', action='store_true')
    p.add_argument('--frozen', type=Path)
    p.add_argument('--out', type=Path)
    p.add_argument('--tigress')
    p.add_argument('--memory-mode', choices=('callback', 'builtin'), default='builtin')
    p.add_argument('--rounds', type=int, choices=(1, 3), default=3)
    a = p.parse_args()
    if a.self_test:
        print(json.dumps(self_test()))
        return 0
    if any(value is None for value in (a.frozen, a.out, a.tigress)):
        p.error('--frozen --out and --tigress are required')
    a.out = a.out.resolve()
    import dragon as d
    import guest_memory_checked_edges_real as backend
    import store_hub_real as metric
    original_pipeline = d.PassPipeline
    captures = []

    class PipelineRequest:
        """Intercept exactly the old oracle's request, not arbitrary clients."""
        def __init__(self, *args, **kwargs):
            if args or kwargs:
                raise RuntimeError('unexpected oracle pipeline construction')
            self.requested = []
            self.registered = []

        def __enter__(self):
            return self

        def __exit__(self, *_):
            return None

        def register(self, name, callback, **options):
            if name != 'guest-memory-ssa' or options or self.registered:
                raise RuntimeError('unexpected oracle callback registration')
            self.registered.append(name)
            return self

        def add(self, name, **options):
            if options:
                raise RuntimeError('unexpected oracle pass options')
            self.requested.append(name)
            return self

        def run(self, target, **options):
            if (tuple(self.requested) != LEGACY_REQUEST or self.registered != ['guest-memory-ssa']
                    or options != dict(max_iterations=1, max_pass_runs=32)):
                raise RuntimeError('oracle optimization contract changed')
            before = target.ir()
            checkpoints = []

            def checkpoint(context, settings):
                if settings:
                    raise RuntimeError('unexpected checkpoint options')
                checkpoints.append(context.target.ir())
                return d.PassResult()

            def memory(context, settings):
                if settings != {'run_mem2reg': 0}:
                    raise RuntimeError('unexpected callback memory options')
                stats = context.target.run_guest_memory_ssa(run_mem2reg=False)
                return d.PassResult(changed=stats.changed,
                    status=(d.PassStatus.UNAVAILABLE if not stats.available else
                            d.PassStatus.SUCCESS if stats.complete else d.PassStatus.INCOMPLETE),
                    counters={key: int(value) for key, value in vars(stats).items() if key != 'available'})

            with original_pipeline() as pipeline:
                pipeline.register('bounded-memory-checkpoint', checkpoint, read_only=True)
                if a.memory_mode == 'callback':
                    pipeline.register('guest-memory', memory)
                for name in ACTUAL_STEPS:
                    settings = ({'run_mem2reg': 0} if name == 'guest-memory' else
                                {'allow_plain_stores': 1} if name == 'dispatch-specialize' else {})
                    pipeline.add(name, **settings)
                report = pipeline.run(target, max_iterations=a.rounds, max_pass_runs=32)
            validate_report(report, a.rounds)
            if len(checkpoints) != report['iterations']:
                raise RuntimeError('missing/extra round checkpoints')
            captures.append(dict(before=before, after=target.ir(), report=report, checkpoints=checkpoints,
                                 original_request=list(self.requested)))
            return report

    previous_argv = sys.argv
    d.PassPipeline = PipelineRequest
    try:
        sys.argv = [backend.__file__, '--frozen', str(a.frozen), '--out', str(a.out), '--tigress', a.tigress]
        status = backend.main()
    finally:
        d.PassPipeline = original_pipeline
        sys.argv = previous_argv
    modules = json.loads((a.out / 'modules.json').read_text())
    semantic = json.loads((a.out / 'summary.json').read_text())
    if len(modules) != 4 or len(captures) != 4:
        raise RuntimeError('missing/extra optimized modules')
    rows = []
    for module, capture in zip(modules, captures):
        work = Path(module['work']); entry = module['kernel']
        if ((work / 'before-pipeline.ll').read_text() != capture['before'] or
                (work / 'after-pipeline.ll').read_text() != capture['after'] or
                module['pipeline'] != capture['report']):
            raise RuntimeError('executed IR/report differs from captured pipeline')
        round_rows = []
        for iteration, ir in enumerate(capture['checkpoints'], 1):
            path = work / ('round-' + str(iteration) + '.ll')
            path.write_text(ir)
            round_rows.append(dict(iteration=iteration, ir=str(path), sha256=digest(path),
                                   actual_indices=sorted({r['index'] for r in metric.actual_loads(ir, entry)})))
        row = dict(rounds=round_rows, entry=entry, work=str(work), before_sha256=digest(work / 'before-pipeline.ll'),
                   after_sha256=digest(work / 'after-pipeline.ll'), report=capture['report'],
                   actual_before=metric.actual_loads(capture['before'], entry),
                   actual_after=metric.actual_loads(capture['after'], entry),
                   root_before=module['root_before'], root_after=module['root_after'],
                   original_request=capture['original_request'])
        rows.append(row)
    ok = status == 0 and semantic['ok']
    result = dict(ok=ok, mode=a.memory_mode, requested_rounds=a.rounds, max_pass_runs=32,
                  transformation_recipe=list(STEPS), actual_recipe=list(ACTUAL_STEPS), semantic_returncode=status,
                  semantic_summary_sha256=digest(a.out / 'summary.json'),
                  harness_sha256=digest(__file__), rows=rows,
                  GuestMemory_incomplete_modules=[row['work'] for row in rows if any(
                      r['name'] == 'guest-memory' and r['status'] != d.PassStatus.SUCCESS
                      for r in row['report']['runs'])],
                  exclusions=[], skips=[], normalizations=[],
                  note='Legacy summary GuestSSA-prefix metrics do not measure guest-memory; use this report. Copied CFF switches are not removal.')
    save(a.out / 'bounded-memory-summary.json', result)
    print(json.dumps(dict(ok=ok, mode=a.memory_mode, rounds=a.rounds,
                         indices={hex(row['entry']): sorted({x['index'] for x in row['actual_after']}) for row in rows}), indent=2))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
