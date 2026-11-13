"""Recipe B: replay the same heldout bytes with explicit decoded regions.

--adopt-frozen OLD --out NEW copies an A input freeze without a producer.
--frozen NEW --out RESULTS [the usual A options] runs A's unchanged oracle and
ten LLVM passes. Every lifted function uses its unique positive ELF STT_FUNC
extent and static_region_replay's decoded-leader selection. Actual Guest LOADs
and unknown-target exits remain; there are no trusted memory facts, Graph
finite passes, target observations, or full-unflattening claims.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import time

import practical_holdout_diff as A
import static_region_replay as regions


BASE_SCHEMA = 'dragon-practical-holdouts-inputs-v1'
SCHEMA = 'dragon-practical-holdouts-region-inputs-v1'
BASE_MANIFEST_SHA256 = '4ae488b0b0c0a236f077ce2bbb7d4ec7cdba40fca719ac2b0713bee3ca0cb0f7'
BASE_RUNNER_SHA256 = '09ee1a3be3696d521b57b8bf7580da9e146482d831c3d992fc1ada9fd480ae91'
BASE_SOURCE_SHA256 = '6ce5d1417bd1ada1c12e4b4a4adf32e35233f60445868c9ffdbde004245bd5b5'
BASE_MANIFEST_NAME = 'BASE-A-FROZEN.json'
RECIPE = dict(name='B-decoded-region', max_blocks=4096, max_dispatch_cases=65536,
              rebuild_decoded_blocks=True, static_entry_policy='function-start',
              selection='unique positive executable STT_FUNC extent; existing select_region',
              graph_finite_dispatch=False, known_memory=False,
              image_immutability=False, oracle_and_llvm_pipeline='unchanged A')


def canonical_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True,
                                    separators=(',', ':')).encode()).hexdigest()


def verify_base(root, manifest, original_verify):
    """Use A's exact checks, plus its actual old source-pin subset.

    B and the existing selection helper are new audited dependencies; they are
    pinned in B, rather than silently removed from B's imported-source closure.
    """
    A.require(A.digest(A.__file__) == BASE_RUNNER_SHA256, 'A runner source changed')
    A.require(A.digest(A.heldout.__file__) == BASE_SOURCE_SHA256, 'A fixture source changed')
    current = A.source_pins()
    old = manifest['source_pins']
    A.require(all(current.get(path) == sha for path, sha in old.items()),
              'actual A imported source pins differ from the original freeze')
    extra = set(current) - set(old)
    allowed = {str(Path(__file__).resolve()), str(Path(regions.__file__).resolve())}
    A.require(extra == allowed, 'unexpected additional repository dependency')
    source_pins, schema = A.source_pins, A.SCHEMA
    try:
        A.source_pins = lambda: old
        A.SCHEMA = BASE_SCHEMA
        original_verify(root, manifest)
    finally:
        A.source_pins, A.SCHEMA = source_pins, schema


def adopt(source, out, expected, original_verify):
    source, out = source.resolve(), out.resolve()
    A.require(not out.exists(), 'adopt output namespace must not exist')
    A.require(not out.is_relative_to(source) and not source.is_relative_to(out),
              'adopt input/output namespaces must be disjoint')
    initial_free = A.space(out, A.START_FREE)
    source_before, runtime_before = A.source_pins(), A.runtime_pins()
    manifest_bytes = (source / 'FROZEN.json').read_bytes()
    A.require(hashlib.sha256(manifest_bytes).hexdigest() == expected,
              'original A manifest hash differs from explicit expected pin')
    base = json.loads(manifest_bytes)
    verify_base(source, base, original_verify)
    A.require({BASE_MANIFEST_NAME, 'ADOPTION.json', 'FAILURE.json'}.isdisjoint(base['files']),
              'A payload collides with recipe B evidence names')
    out.mkdir(parents=True)
    try:
        for relative in base['files']:
            A.space(out, A.RUN_FREE)
            src = A.frozen_file(source, relative, base)
            dst = out / relative
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            A.require(A.digest(dst) == base['files'][relative], 'adopted bytes differ')
        (out / BASE_MANIFEST_NAME).write_bytes(manifest_bytes)
        derived = copy.deepcopy(base)
        derived.update(schema=SCHEMA, source_pins=source_before,
                       runtime_pins=runtime_before, recipe=RECIPE,
                       base_A_manifest_sha256=expected,
                       base_A_manifest_file=BASE_MANIFEST_NAME,
                       base_A_manifest_path=str(source / 'FROZEN.json'),
                       adopt_only=True, producer_only=False,
                       producer_invocations=0, product_pass_runs=0,
                       target_native_runs=0)
        A.write_json(out / 'ADOPTION.json', dict(argv=sys.argv, initial_free=initial_free,
                     source_pins=source_before, runtime_pins=runtime_before,
                     base_A_manifest_sha256=expected, copied_A_files=len(base['files']),
                     recipe=RECIPE, producer_invocations=0, product_pass_runs=0,
                     target_native_runs=0))
        derived['files'][BASE_MANIFEST_NAME] = expected
        derived['files']['ADOPTION.json'] = A.digest(out / 'ADOPTION.json')
        verify_base(source, base, original_verify)
        A.require(A.digest(source / 'FROZEN.json') == expected, 'original A manifest changed')
        A.require(A.source_pins() == source_before and A.runtime_pins() == runtime_before,
                  'source/runtime changed while adopting')
        A.write_json(out / 'FROZEN.json', derived)
    except BaseException as error:
        A.write_json(out / 'FAILURE.json', dict(type=type(error).__name__, detail=str(error)))
        raise
    return 0


def region_attributes(lifter, entry):
    symbol = f'dragon_static_entry_x86_64_{entry:x}'
    text = lifter.ir()
    header = re.search(r'^define void @' + re.escape(symbol)
                       + r'\([^\n]*\) #(\d+)', text, re.M)
    A.require(header is not None, 'accepted region function missing from LLVM IR')
    attrs = re.search(r'^attributes #' + header[1] + r' = (\{[^\n]*\})', text, re.M)
    A.require(attrs is not None, 'region function attributes missing')
    values = dict(re.findall(r'"(dragon\.[^"]+)"="([^"]*)"', attrs[1]))
    A.require(values.get('dragon.static-coverage') == 'decoded-region'
              and values.get('dragon.coverage-source') == 'decoded-machine'
              and values.get('dragon.static-entry-policy') == 'function-start',
              'accepted function lacks the requested decoded-region contract')
    return dict(symbol=symbol, attribute_group=int(header[1]),
                attribute_text=attrs[1], dragon_attributes=values)


def replay(remaining, expected, original_verify):
    A.require('--prepare-only' not in remaining, 'recipe B cannot run a producer')
    A.require('--frozen' in remaining and '--out' in remaining,
              'use --adopt-frozen first, then the usual --frozen/--out options')
    out = Path(remaining[remaining.index('--out') + 1]).resolve()
    modules, active = [], None
    d, study = A.study.base.dragon, A.study
    compile_original, lift_original = study.compile_image, d.LLVMLifter.lift
    old_schema, old_verify, old_argv = A.SCHEMA, A.verify_frozen, sys.argv

    def verify(root, manifest):
        original_verify(root, manifest)
        A.require(manifest.get('recipe') == RECIPE, 'recipe B settings changed')
        A.require(manifest.get('base_A_manifest_file') == BASE_MANIFEST_NAME
                  and manifest.get('base_A_manifest_sha256') == expected,
                  'recipe B lineage differs from explicit A manifest pin')
        base_path = A.frozen_file(root, BASE_MANIFEST_NAME, manifest)
        A.require(A.digest(base_path) == expected, 'copied A manifest changed')
        base = json.loads(base_path.read_bytes())
        verify_base(root, base, original_verify)
        keys = ('families', 'calibration', 'original_c', 'control', 'seed', 'inputs',
                'plans', 'pipeline', 'guest_steps', 'subprocess_seconds', 'strategy',
                'static_entry_policy', 'fixture')
        A.require(all(manifest[key] == base[key] for key in keys),
                  'recipe B changed frozen ELF/source/input/oracle/pipeline records')
        A.require(all(manifest['files'].get(path) == sha
                      for path, sha in base['files'].items()), 'A payload hash list changed')

    def lift(lifter, function, graph):
        A.require(active is not None, 'region lift outside sequential compile context')
        if active['graph'] is None:
            active['graph'] = graph
            start = time.monotonic()
            active['before'] = regions.graph_snapshot(graph)
            A.write_json(active['work'] / 'graph-before.json', active['before'])
            active['graph_audit_seconds'] += time.monotonic() - start
        A.require(graph is active['graph'], 'Graph changed during one module')
        entry = function.start_address
        symbols = active['symbols'].get(entry, [])
        ranges = {(item['start'], item['end']) for item in symbols}
        A.require(len(ranges) == 1, 'no unique positive executable STT_FUNC extent')
        bounds = next(iter(ranges))
        start = time.monotonic()
        selection = regions.select_region(bounds, active['before'])
        original = sorted(function.block_addresses())
        extra = sorted(set(selection['selected_starts']) - set(original))
        row = dict(entry=entry, symbols=symbols, bounds=list(bounds),
                   original_blocks=original, additional_blocks=extra, **selection)
        row['selection_seconds'] = time.monotonic() - start
        active['lifts'].append(row)
        start = time.monotonic()
        try:
            ok = lifter.lift_static_region(function, graph, extra,
                max_blocks=4096, max_dispatch_cases=65536, rebuild_decoded_blocks=True)
            row['region_native_lift_seconds'] = time.monotonic() - start
            row['accepted'] = bool(ok)
            if ok:
                row['actual_region_attributes'] = region_attributes(lifter, entry)
            return ok
        except BaseException as error:
            row['error'] = dict(type=type(error).__name__, detail=str(error))
            raise
        finally:
            A.write_json(active['work'] / 'region-selection.json', active['lifts'])

    def compile_image(data, symbols, work, static, product, level, options, budget=10000):
        nonlocal active
        A.require(active is None and static and options.function_entry,
                  'expected sequential StaticAnalysis FunctionStart compilation')
        active = dict(work=Path(work), symbols=regions.function_symbols(data, A.ELFFile),
                      graph=None, before=None, lifts=[], graph_audit_seconds=0.0,
                      elf_sha256=hashlib.sha256(data).hexdigest(), product=product, level=level)
        current = active
        started = time.monotonic()
        try:
            return compile_original(data, symbols, work, static, product, level, options, budget=budget)
        finally:
            try:
                if current['graph'] is not None:
                    start = time.monotonic()
                    after = regions.graph_snapshot(current['graph'])
                    A.write_json(current['work'] / 'graph-after.json', after)
                    current['graph_audit_seconds'] += time.monotonic() - start
                    current['Graph_unchanged'] = after == current['before']
                    current['Graph_before_sha256'] = canonical_sha(current['before'])
                    current['Graph_after_sha256'] = canonical_sha(after)
                    A.require(current['Graph_unchanged'], 'recipe B changed source Graph')
            finally:
                record = {k: v for k, v in current.items() if k not in ('graph', 'before', 'symbols')}
                record.update(work=str(current['work']), elapsed_seconds=time.monotonic() - started)
                modules.append(record)
                A.write_json(current['work'] / 'region-module.json', record)
                active = None

    A.SCHEMA, A.verify_frozen = SCHEMA, verify
    study.compile_image, d.LLVMLifter.lift = compile_image, lift
    status = None
    try:
        sys.argv = [old_argv[0], *remaining]
        status = A.main()
        return status
    finally:
        sys.argv = old_argv
        A.SCHEMA, A.verify_frozen = old_schema, old_verify
        study.compile_image, d.LLVMLifter.lift = compile_original, lift_original
        if out.is_dir():
            A.write_json(out / 'region-results.json', dict(recipe=RECIPE, oracle_returncode=status,
                         base_A_manifest_sha256=expected, modules=modules,
                         Graph_unchanged=bool(modules) and all(m.get('Graph_unchanged') for m in modules),
                         metric_scope='A lift envelope includes B selection/audits; native lift and audit times are separately recorded',
                         scope='decoded execution coverage; no Graph recovery, memory contents, cone or full-CFF proof'))


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--adopt-frozen', type=Path)
    parser.add_argument('--base-manifest-sha256', default=BASE_MANIFEST_SHA256)
    options, remaining = parser.parse_known_args()
    A.require(re.fullmatch(r'[0-9a-f]{64}', options.base_manifest_sha256), 'invalid A manifest pin')
    original_verify = A.verify_frozen
    if options.adopt_frozen is not None:
        adopt_parser = argparse.ArgumentParser(description=__doc__)
        adopt_parser.add_argument('--out', type=Path, required=True)
        args = adopt_parser.parse_args(remaining)
        return adopt(options.adopt_frozen, args.out, options.base_manifest_sha256, original_verify)
    return replay(remaining, options.base_manifest_sha256, original_verify)


if __name__ == '__main__':
    raise SystemExit(main())
