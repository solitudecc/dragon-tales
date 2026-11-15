"""Replay frozen large Tigress combinations through public LLVM passes.

Generation and Unicorn references are separate inputs. Original machine bytes,
all modelled fields, complete Guest memory and Exit stay unchanged. An early
Transfer/unsupported exit is incomplete, never a complete semantic match.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import struct
import time
from types import SimpleNamespace

import tigress_function_diff as tf
import static_region_replay as regions

PIPELINE = [('mem2reg', {}), ('instcombine', {}), ('gvn', {}),
            ('guest-memory', {}),
            ('mba', dict(timeout_ms=20, max_queries=64, max_expression_nodes=128,
                         resource_limit=100000, max_variables=3)),
            ('predicates', dict(timeout_ms=20, max_queries=64,
                                max_expression_nodes=128, resource_limit=100000)),
            ('dispatch-specialize', {}), ('dispatch-deflatten', {}),
            ('control-flow', {}), ('dce', {})]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def mapped_libraries():
    result = {}
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and Path(fields[5]).name in ['libdragon.so', 'libdragon_c.so']:
            path = Path(fields[5]).resolve()
            result[str(path)] = sha(path)
    assert len(result) == 2 and str(Path(tf.base.dragon._LIB).resolve()) in result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frozen', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--level', action='append', choices=['O0', 'O2'])
    args = parser.parse_args()
    args.out.mkdir(exist_ok=False)
    frozen = json.loads((args.frozen / 'REFERENCE.json').read_text())
    assert frozen['summary'] == {'match': 64}, 'all source/reference checks must pass first'
    assert frozen['source_pins_stable'] and frozen['skipped'] == 0
    d = tf.base.dragon
    adapter = tf.guest.Adapter('x86_64')
    runtime = mapped_libraries()
    sources = {str(Path(m.__file__).resolve()): sha(m.__file__)
               for m in [tf, tf.guest, tf.compiler, tf.base, d, regions]}
    sources[str(Path(__file__).resolve())] = sha(__file__)
    inputs = {str(p.resolve()): sha(p) for p in args.frozen.rglob('*') if p.is_file()}
    save(args.out / 'PLAN.json', dict(runtime=runtime, source_pins=sources,
         input_pins=inputs, levels=args.level or ['O0', 'O2'], pipeline=PIPELINE,
         max_iterations=1, max_pass_runs=16, guest_step_limit=1000000,
         reference_generated_here=False, exclusion=0, normalization=0))
    old_lift, old_optimize = d.LLVMLifter.lift, d.LLVMLifter.optimize
    active = None
    rows, modules = [], []

    def lift(lifter, function, graph):
        if active['graph'] is None:
            active['graph'] = graph
            active['before'] = regions.graph_snapshot(graph)
            save(active['work'] / 'graph-before.json', active['before'])
        assert graph is active['graph']
        address = function.start_address
        choices = active['symbols'].get(address, [])
        bounds = {(x['start'], x['end']) for x in choices}
        if len(bounds) != 1:
            return old_lift(lifter, function, graph)
        selection = regions.select_region(next(iter(bounds)), active['before'])
        extra = sorted(set(selection['selected_starts']) - set(function.block_addresses()))
        active['lifts'].append(dict(entry=address, extra=extra, **selection))
        return lifter.lift_static_region(function, graph, extra, max_blocks=4096,
                 max_dispatch_cases=65536, rebuild_decoded_blocks=True)

    def optimize(lifter, pipeline=None):
        assert pipeline is None
        with d.PassPipeline(PIPELINE) as selected:
            report = selected.run(lifter, max_iterations=1, max_pass_runs=16, verify=True)
        save(active['work'] / 'pass-report.json', report)
        assert report['ok'], 'public pass pipeline error'
        assert [r['name'] for r in report['runs']] == [n for n, _ in PIPELINE]
        return report

    d.LLVMLifter.lift, d.LLVMLifter.optimize = lift, optimize
    options = SimpleNamespace(strategy=0, function_entry=True, control=None,
            opt='opt-21', llc='llc-21', cc='gcc')
    try:
        for c_level in args.level or ['O0', 'O2']:
            record = frozen['images']['combined-' + c_level]
            image = Path(record['elf'])
            assert sha(image) == record['sha256']
            data = image.read_bytes()
            for product in [False, True]:
                for llvm_level in ['O0', 'O2']:
                    name = f'C-{c_level}-' + ('product-' if product else 'raw-') + llvm_level
                    work = args.out / name
                    work.mkdir()
                    active = dict(work=work, graph=None, before=None, lifts=[],
                           symbols=regions.function_symbols(data, tf.ELFFile))
                    module = dict(name=name)
                    start = time.monotonic()
                    try:
                        exe, analysis = tf.compile_image(data, record['symbols'], work,
                           True, product, llvm_level, options, budget=0)
                        module.update(analysis)
                        for trial in range(16):
                            initial = (image.parent / f'input-{trial}.bin').read_bytes()
                            expected = (image.parent / f'expected-{trial}.bin').read_bytes()
                            assert len(initial) == 512 + 65536 and len(expected) == 512 + 65536 + 64
                            want = (expected[:512], expected[512:512+65536],
                                    collections.Counter(), struct.unpack('<8Q', expected[-64:])[:7])
                            actual, error = tf.base.native(exe, initial[:512], initial[512:], work,
                                                         trial, extra_bytes=64)
                            row = dict(name=name, trial=trial)
                            if error:
                                row.update(status=error['kind'], detail=error)
                            else:
                                diffs, exit_record = adapter.compare(dict(undefined_flags=[]), want, actual, False)
                                root = exit_record[0] == 1 and exit_record[1] == 3 and exit_record[3] == tf.base.SENTINEL and exit_record[4] == 1
                                row.update(exit=exit_record, root_return=root)
                                if root:
                                    row.update(status='different' if diffs else 'match', differences=diffs,
                                          register_fields=len(adapter.fields), memory_bytes=65536,
                                          complete_register_bytes_equal=actual[0] == want[0],
                                          complete_frame_equal=actual[0]+actual[1]+actual[3] == expected)
                                    controls = []
                                    for kind in ['register', 'memory', 'exit']:
                                        bad = list(actual)
                                        index, offset = {'register': (0,0), 'memory': (1,tf.base.DATA), 'exit': (3,0)}[kind]
                                        damaged = bytearray(bad[index]); damaged[offset] ^= 1; bad[index] = bytes(damaged)
                                        controls.append(bool(adapter.compare(dict(undefined_flags=[]), want, tuple(bad), False)[0]))
                                    row['three_controls_detected'] = all(controls)
                                    assert all(controls)
                                else:
                                    row.update(status='incomplete', differences_not_compared=True)
                            rows.append(row)
                    except Exception as error:
                        module.update(error=type(error).__name__ + ': ' + str(error))
                    finally:
                        if active['graph'] is not None:
                            after = regions.graph_snapshot(active['graph'])
                            module['Graph_unchanged'] = after == active['before']
                            assert module['Graph_unchanged']
                        module.update(seconds=time.monotonic()-start, lifts=active['lifts'])
                        modules.append(module)
                        save(args.out / 'PROGRESS.json', dict(rows=rows, modules=modules))
                        print(name, dict(collections.Counter(r['status'] for r in rows if r['name']==name)), module.get('error',''), flush=True)
    finally:
        d.LLVMLifter.lift, d.LLVMLifter.optimize = old_lift, old_optimize
    stable = (mapped_libraries() == runtime and
              all(sha(p) == h for p,h in sources.items()) and
              all(sha(p) == h for p,h in inputs.items()))
    save(args.out / 'RESULT.json', dict(rows=rows, modules=modules,
         summary=dict(collections.Counter(r['status'] for r in rows)),
         source_runtime_input_pins_stable=stable, skipped=0, exclusion=0, normalization=0,
         planned_native=16*4*len(args.level or ['O0','O2']),
         complete_frame_matches=sum(r.get('complete_frame_equal',False) for r in rows),
         comparator_controls_detected=3*sum(r.get('three_controls_detected',False) for r in rows)))
    assert stable


if __name__ == '__main__':
    main()
