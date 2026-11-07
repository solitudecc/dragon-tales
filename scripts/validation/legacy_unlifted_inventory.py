"""Record actual Legacy acceptance and compare an unlifted-refusal update.

This checks API acceptance and byte-identical accepted IR, not program semantics.
Run record in separate processes for the frozen old and final libraries, then
compare the reports. Every recovered valid function is attempted; no sampling.
"""
import argparse
import collections
import hashlib
import json
import os
import re
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def coverage(report):
    errors = []
    sha = lambda value: isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None
    integer = lambda value: type(value) is int and value >= 0
    if report.get('schema') != 1 or not sha(report.get('driver_sha256')) or not sha(report.get('python_sha256')):
        errors.append('missing schema or source hashes')
    inputs = [r['path'] for r in report['inputs']]
    graph_keys = [(r['input'], r['strategy']) for r in report['graphs']]
    wanted_graphs = {(p, s) for p in inputs for s in (0, 1)}
    function_keys = [(r['input'], r['strategy'], r['entry']) for r in report['functions']]
    wanted_functions = {(r['input'], r['strategy'], e) for r in report['graphs'] for e in r['entries']}
    if report.get('complete') is not True or not inputs or len(inputs) != len(set(inputs)):
        errors.append('incomplete or duplicate input manifest')
    if len(graph_keys) != len(set(graph_keys)) or set(graph_keys) != wanted_graphs:
        errors.append('graph coverage does not match input manifest and both strategies')
    if any(len(r['entries']) != len(set(r['entries'])) for r in report['graphs']):
        errors.append('duplicate graph entry')
    if any(type(r['strategy']) is not int or r['strategy'] not in (0, 1) or
           not integer(r['instructions']) or
           any(not integer(e) for k in ('blocks', 'entries') for e in r[k]) for r in report['graphs']):
        errors.append('invalid graph field type')
    if len(function_keys) != len(set(function_keys)) or set(function_keys) != wanted_functions:
        errors.append('function coverage does not match declared graph entries')
    if any(not sha(r.get('sha256')) for r in report['inputs']):
        errors.append('invalid input hash')
    for row in report['functions']:
        if (any(type(row.get(k)) is not bool for k in ('valid', 'accepted', 'module_preserved')) or
            type(row.get('strategy')) is not int or row['strategy'] not in (0, 1) or
            not integer(row.get('entry')) or not integer(row.get('instructions')) or
            any(not integer(value) for k in ('blocks', 'markers') for value in row[k]) or
            not sha(row.get('ir_sha256'))):
            errors.append('invalid function field type or hash')
    return errors


def compare(old, new, expected_inputs):
    errors = [f'{label}: {error}' for label, report in (('old', old), ('new', new)) for error in coverage(report)]
    counts = collections.Counter()
    if old['inputs'] != new['inputs']:
        errors.append('input manifest differs')
    if old['inputs'] != expected_inputs or new['inputs'] != expected_inputs:
        errors.append('input manifest differs from independent requested plan')
    if any(old.get(k) != new.get(k) for k in ('driver_sha256', 'python_sha256')):
        errors.append('validation source differs')
    left = {(r['input'], r['strategy'], r['entry']): r for r in old['functions']}
    right = {(r['input'], r['strategy'], r['entry']): r for r in new['functions']}
    if len(left) != len(old['functions']) or len(right) != len(new['functions']):
        errors.append('duplicate function record')
    if left.keys() != right.keys():
        errors.append('function coverage differs')
    if old['graphs'] != new['graphs']:
        errors.append('graph structure differs')
    for key in left.keys() & right.keys():
        a, b = left[key], right[key]
        if any(a[k] != b[k] for k in ('valid', 'blocks', 'instructions', 'markers')):
            errors.append(f'{key}: function structure differs')
        if b['accepted']:
            if not a['accepted'] or not b['valid'] or b['markers'] or a['ir_sha256'] != b['ir_sha256']:
                errors.append(f'{key}: unexpected acceptance or changed accepted IR')
            else:
                counts['accepted_unchanged'] += 1
        elif a['accepted']:
            if not b['markers'] or not b['module_preserved']:
                errors.append(f'{key}: new refusal lacks marker or mutated module')
            else:
                counts['new_marker_refusals'] += 1
        else:
            counts['already_refused'] += 1
        if not b['accepted'] and b['markers'] and not b['module_preserved']:
            errors.append(f'{key}: marker refusal mutated module')
    return dict(ok=not errors, errors=errors, counts=dict(counts), functions=len(right),
                valid_functions=sum(r['valid'] for r in right.values()),
                invalid_candidates=sum(not r['valid'] for r in right.values()),
                semantic_proof=False)


def mapped_libraries(d, args):
    lines = [line for line in Path('/proc/self/maps').read_text().splitlines() if '/libdragon' in line]
    if not lines or any('(deleted)' in line for line in lines):
        raise RuntimeError('missing or deleted Dragon mapping')
    paths = {Path(line.split()[-1]).resolve() for line in lines}
    wanted = {'libdragon.so': args.backend_sha256, 'libdragon_c.so': args.shim_sha256}
    if len(paths) != 2 or {p.name: digest(p) for p in paths} != wanted:
        raise RuntimeError(f'wrong or duplicate mapped libraries: {paths}')
    if Path(d._LIB).resolve() not in paths:
        raise RuntimeError('Python shim differs from mapped shim')
    return dict(libraries={str(p): digest(p) for p in sorted(paths)}, maps=lines)


def record(args):
    # Loading the public binding happens only after the caller selected a DSO.
    sys.path.insert(0, str(ROOT / 'python'))
    import dragon as d
    mapped = mapped_libraries(d, args)
    paths = [Path(p).resolve() for p in args.image]
    if len(paths) != len(set(paths)):
        raise RuntimeError('duplicate input path')
    result = dict(schema=1, inputs=[dict(path=str(p), sha256=digest(p)) for p in paths],
                  complete=False, graphs=[], functions=[], mapped_libraries=mapped,
                  driver_sha256=digest(__file__), python_sha256=digest(d.__file__), semantic_proof=False)
    args.out.write_text(json.dumps(result, indent=2))
    for path, manifest in zip(paths, result['inputs']):
        before = manifest['sha256']
        with d.LoadedImage.load_file(str(path)) as image:
            for strategy in (d.Strategy.LINEAR_SWEEP, d.Strategy.RECURSIVE_TRAVERSAL):
                with image.disassemble(d.Configuration(strategy=strategy)) as graph:
                    entries = sorted(graph.function_addresses())
                    result['graphs'].append(dict(input=str(path), strategy=strategy,
                        instructions=len(graph), blocks=sorted(graph.block_addresses()), entries=entries))
                    for entry in entries:
                        with graph.get_function(entry) as function:
                            blocks = sorted(function.block_addresses())
                            markers = []
                            calls = set()
                            for address in blocks:
                                with graph.get_block(address) as machine_block:
                                    calls.update(i.address for i in machine_block.instructions() if i.is_call)
                                block = graph.get_ignil_block(address)
                                if block is not None:
                                    with block:
                                        markers.extend(block.unlifted())
                            row = dict(input=str(path), strategy=strategy, entry=entry,
                                valid=function.is_valid, blocks=blocks,
                                instructions=function.num_instructions, markers=sorted(set(markers)),
                                call_sites=sorted(calls))
                            with d.LLVMLifter(image.architecture, 'acceptance') as lifter:
                                empty = lifter.ir()
                                row['accepted'] = lifter.lift(function, graph)
                                ir = lifter.ir()
                                row['module_preserved'] = ir == empty
                                row['ir_sha256'] = hashlib.sha256(ir.encode()).hexdigest()
                            result['functions'].append(row)
                    print(path.name, strategy, len(entries), flush=True)
                    args.out.write_text(json.dumps(result, indent=2))
        if digest(path) != before:
            raise RuntimeError(f'input changed while reading: {path}')
        if mapped_libraries(d, args)['libraries'] != mapped['libraries']:
            raise RuntimeError('mapped library bytes changed during inventory')
    result['mapped_libraries_after'] = mapped_libraries(d, args)
    result['complete'] = True
    if coverage(result):
        raise RuntimeError(coverage(result))
    return result


def self_test():
    row = dict(input='a', strategy=0, entry=1, valid=True, blocks=[1], instructions=1,
               markers=[1], accepted=True, module_preserved=False, ir_sha256='a' * 64)
    old = dict(schema=1, complete=True, driver_sha256='b' * 64, python_sha256='c' * 64,
               inputs=[dict(path='a', sha256='d' * 64)],
               graphs=[dict(input='a', strategy=0, instructions=1, blocks=[1], entries=[1]),
                       dict(input='a', strategy=1, instructions=0, blocks=[], entries=[])],
               functions=[row])
    new = dict(old, functions=[dict(row, accepted=False, module_preserved=True)])
    wanted = old['inputs']
    assert compare(old, new, wanted)['ok']
    for bad in (dict(new, functions=[]), dict(new, functions=new['functions'] * 2),
                dict(new, functions=[dict(new['functions'][0], module_preserved=False)]),
                dict(new, functions=[dict(new['functions'][0], markers=[])]),
                dict(new, functions=[dict(row, ir_sha256='e' * 64)]),
                dict(new, complete=False), dict(new, graphs=[]),
                dict(new, functions=[dict(row, accepted='not-a-bool')]),
                dict(new, driver_sha256='f' * 64)):
        assert not compare(old, bad, wanted)['ok']
    assert not compare(dict(old, functions=[]), dict(new, functions=[]), wanted)['ok']
    assert not compare(old, new, wanted + [dict(path='omitted', sha256='e' * 64)])['ok']
    good = dict(old, functions=[dict(row, markers=[])])
    assert compare(good, good, wanted)['ok']
    print('inventory controls: 11 corrupt reports rejected; unchanged supported IR accepted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--image', action='append')
    parser.add_argument('--backend-sha256')
    parser.add_argument('--shim-sha256')
    parser.add_argument('--compare', nargs=2, type=Path)
    parser.add_argument('--manifest', type=Path, help='independent requested input plan; required for comparison')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if sys.flags.optimize or os.environ.get('PYTHONOPTIMIZE'):
        parser.error('validation requires Python assertions enabled')
    if args.self_test:
        self_test()
        return 0
    if not args.out:
        parser.error('--out required')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    if args.compare:
        if args.manifest is None:
            parser.error('comparison requires --manifest independent requested input plan')
        result = compare(*(json.loads(p.read_text()) for p in args.compare),
                         json.loads(args.manifest.read_text())['inputs'])
    else:
        if not args.image or not args.backend_sha256 or not args.shim_sha256:
            parser.error('record requires --image and both mapped library hashes')
        result = record(args)
    args.out.write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k not in ('graphs', 'inputs') and
                      not (k == 'functions' and isinstance(v, list))}, indent=2))
    return 0 if result.get('ok', True) else 1


if __name__ == '__main__':
    raise SystemExit(main())
