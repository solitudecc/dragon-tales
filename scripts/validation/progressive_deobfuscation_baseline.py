"""One frozen ELF, ordinary StaticAnalysis IR baseline; no native/reference runs.

The pipeline and limits are the practical-holdout ten-step recipe.  This does
not invoke the three opt-in checked components or supply Guest memory facts.
The small textual CFG reader refuses unsupported syntax; it is a diagnostic,
not a semantic oracle or a certificate of dispatcher removal.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
PIPELINE = [('mem2reg', {}), ('instcombine', {}), ('gvn', {}),
            ('guest-memory', {}),
            ('mba', dict(timeout_ms=20, max_queries=64,
                         max_expression_nodes=128, resource_limit=100000,
                         max_variables=3)),
            ('predicates', dict(timeout_ms=20, max_queries=64,
                                max_expression_nodes=128, resource_limit=100000)),
            ('dispatch-specialize', {}), ('dispatch-deflatten', {}),
            ('control-flow', {}), ('dce', {})]
START, FLOOR, SECONDS = 512 * 1024**2, 64 * 1024**2, 180
NAME = r'(?:"(?:[^"\\]|\\.)*"|[-a-zA-Z$._0-9]+)'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def cyclic_components(nodes):
    """Iterative Kosaraju, with duplicate successor slots retained in nodes."""
    reverse = {n: [] for n in nodes}
    for n, row in nodes.items():
        for target in row['successors']:
            require(target in nodes, f'unknown CFG target {target}')
            reverse[target].append(n)
    seen, order = set(), []
    for start in nodes:
        stack = [(start, False)]
        while stack:
            node, done = stack.pop()
            if done:
                order.append(node)
            elif node not in seen:
                seen.add(node)
                stack.append((node, True))
                stack.extend((x, False) for x in reversed(nodes[node]['successors']))
    seen, components = set(), []
    for start in reversed(order):
        if start in seen:
            continue
        group, stack = [], [start]
        while stack:
            node = stack.pop()
            if node in seen:
                continue
            seen.add(node)
            group.append(node)
            stack.extend(reverse[node])
        if len(group) > 1 or group[0] in nodes[group[0]]['successors']:
            components.append(sorted(group))
    return sorted(components)


def ir_cfg(text):
    """Only the generated single-line-header br/switch/ret dialect is accepted."""
    functions, current, block, switch = {}, None, None, False
    for line in text.splitlines():
        if line.startswith('define '):
            match = re.search('@(' + NAME + r')\(', line)
            require(current is None and match and line.rstrip().endswith('{'),
                    'unsupported function header')
            current, block = match[1], None
            require(current not in functions, 'duplicate function')
            functions[current] = {}
        elif current is not None:
            if line == '}':
                require(not switch, 'unterminated switch')
                current, block = None, None
                continue
            label = re.match('^(' + NAME + r'):', line)
            if label:
                block = label[1]
                require(block not in functions[current], 'duplicate block')
                functions[current][block] = dict(successors=[], terminator=None,
                                                  operations={})
                continue
            stripped = line.strip()
            if not stripped or stripped.startswith(';'):
                continue
            if block is None:
                block = '<implicit-entry>'
                functions[current][block] = dict(successors=[], terminator=None,
                                                  operations={})
            row = functions[current][block]
            if switch:
                if stripped.startswith(']'):
                    switch = False
                else:
                    require(re.fullmatch(r'i\d+ -?\d+, label %' + NAME, stripped),
                            'unsupported switch case syntax')
                    row['successors'].extend(re.findall(r'label %(' + NAME + ')', stripped))
                continue
            require(row['terminator'] is None, 'instruction follows terminator')
            inst = re.sub('^%' + NAME + r' =\s*', '', stripped)
            inst = re.sub(r'^(?:musttail|tail|notail)\s+', '', inst)
            opcode = inst.split(maxsplit=1)[0]
            require(re.fullmatch('[a-z]+', opcode), 'unsupported instruction syntax')
            row['operations'][opcode] = row['operations'].get(opcode, 0) + 1
            if opcode in ('br', 'switch', 'ret', 'unreachable'):
                row['terminator'] = opcode
                row['successors'].extend(re.findall(r'label %(' + NAME + ')', inst))
                switch = opcode == 'switch'
                require(opcode != 'switch' or inst.rstrip().endswith('['),
                        'unsupported switch header')
            require(opcode not in ('indirectbr', 'invoke', 'callbr', 'resume',
                                   'catchswitch', 'catchret', 'cleanupret'),
                    'unsupported CFG terminator: ' + opcode)
    require(current is None, 'unterminated function')
    result = {}
    for name, nodes in functions.items():
        require(nodes and all(r['terminator'] for r in nodes.values()),
                'missing terminator')
        groups = cyclic_components(nodes)
        cyclic = {node for group in groups for node in group}
        operations = collections.Counter()
        for row in nodes.values():
            operations.update(row['operations'])
        result[name] = dict(blocks=nodes, block_count=len(nodes),
                            successor_slots=sum(len(r['successors']) for r in nodes.values()),
                            cyclic_components=groups,
                            cyclic_switches=[n for n in nodes if n in cyclic
                                             and nodes[n]['terminator'] == 'switch'],
                            operation_counts=dict(operations))
    return dict(scope='syntactic CFG of verified generated IR; no semantic/CFF claim',
                functions=result)


def graph_snapshot(graph):
    return dict(functions=[dict(address=pc, valid=graph.get_function(pc).is_valid,
                               blocks=graph.get_function(pc).block_addresses())
                           for pc in sorted(graph.function_addresses())],
                blocks=[dict(address=pc, successors=graph.get_block(pc).successors(),
                             instructions=[i.address for i in graph.get_block(pc).instructions()])
                        for pc in sorted(graph.block_addresses())],
                instructions=[dict(address=i.address, bytes=i.bytes.hex(),
                                   disassembly=i.disassembly, branches=i.branches,
                                   callees=i.callees) for i in graph.instructions()])


def worker(args):
    out, root = args.out, args.frozen.resolve()
    report = dict(schema='dragon-progressive-ordinary-baseline-v1', ok=False,
                  semantic_validation=False, native_runs=0, c_runs=0, unicorn_runs=0,
                  checked_component_calls=0, pipeline=PIPELINE, failures=[])
    inputs = {}
    try:
        manifest = root / 'FROZEN.json'
        require(sha(manifest) == args.frozen_sha256, 'frozen manifest hash mismatch')
        frozen = json.loads(manifest.read_text())
        item = frozen['families'][args.family]['images'][args.c_level]
        elf = root / item['elf']
        require(elf.resolve().is_relative_to(root), 'ELF outside frozen root')
        require(sha(elf) == item['sha256'] == frozen['files'][item['elf']], 'ELF hash mismatch')
        required_entries = sorted(set(item['symbols'].values()))
        for symbol in (args.entry_symbol, args.kernel_symbol):
            require(symbol in item['symbols'], 'requested symbol absent: ' + symbol)
        wrapper = args.python_root.resolve()
        inputs = {str(p): sha(p) for p in (Path(__file__).resolve(), manifest, elf,
                  wrapper / 'python/dragon.py', wrapper / 'c_shim/dragon_c.h',
                  wrapper / 'include/dragon/lifters/llvm/Execution.h', args.backend, args.shim,
                  args.opt)}
        require(inputs[str(args.backend)] == args.backend_sha256, 'backend hash mismatch')
        require(inputs[str(args.shim)] == args.shim_sha256, 'shim hash mismatch')
        require(os.environ.get('DRAGON_C_LIB') == str(args.shim), 'explicit shim env required')
        require(os.environ.get('LD_LIBRARY_PATH') == str(args.backend.parent),
                'explicit single backend LD_LIBRARY_PATH required')
        require(not os.environ.get('LD_PRELOAD'), 'LD_PRELOAD must be empty')
        sys.path.insert(0, str(wrapper / 'python'))
        import dragon as d
        mapped = {str(Path(line.split(maxsplit=5)[5]).resolve())
                  for line in Path('/proc/self/maps').read_text().splitlines()
                  if len(line.split(maxsplit=5)) == 6 and
                  Path(line.split(maxsplit=5)[5]).name == 'libdragon.so'}
        require(mapped == {str(args.backend.resolve())}, 'actual backend mapping mismatch')
        report.update(inputs=inputs, runtime_maps=sorted(mapped),
                      required_entries=required_entries, elf_symbols=item['symbols'],
                      strategy=frozen['strategy'], calls=dict(lifts=0, pipeline=0, passes=0))
        with d.LoadedImage.load_bytes(elf.read_bytes()) as image:
            with image.disassemble(d.Configuration(strategy=frozen['strategy'])) as graph:
                before_graph = graph_snapshot(graph)
                save(out / 'graph.before.json', before_graph)
                entries = sorted(pc for pc in graph.function_addresses()
                                 if graph.get_function(pc).is_valid)
                require(set(required_entries).issubset(entries), 'symbol entry coverage failed')
                unlifted = [(pc, addr) for pc in graph.block_addresses()
                            for addr in (graph.get_ignil_block(pc).unlifted()
                                         if graph.get_ignil_block(pc) else [])]
                report['unlifted'] = unlifted
                report['entries'] = entries
                with d.LLVMLifter(d.Architecture.X86_64, 'lifted', execution_mode=2,
                                 static_entry_policy=1) as lifter:
                    try:
                        for pc in entries:
                            report['calls']['lifts'] += 1
                            require(lifter.lift(graph.get_function(pc), graph), 'lift rejected ' + hex(pc))
                    finally:
                        (out / 'before.ll').write_text(lifter.ir())
                    verify(args, out / 'before.ll', 'before')
                    save(out / 'before.cfg.json', ir_cfg((out / 'before.ll').read_text()))
                    try:
                        with d.PassPipeline(PIPELINE) as pipeline:
                            report['calls']['pipeline'] += 1
                            result = pipeline.run(lifter, max_iterations=1, max_pass_runs=16, verify=True)
                    except d.PipelineError as error:
                        report['pass_report'] = error.report
                        report['calls']['passes'] = len(error.report['runs'])
                        save(out / 'pass-report.json', error.report)
                        raise
                    finally:
                        (out / 'after.ll').write_text(lifter.ir())
                    report['pass_report'] = result
                    report['calls']['passes'] = len(result['runs'])
                    save(out / 'pass-report.json', result)
                    require(result['ok'], 'pipeline error; retain target as failed')
                    require([r['name'] for r in result['runs']] == [n for n, _ in PIPELINE],
                            'pipeline step sequence changed')
                    verify(args, out / 'after.ll', 'after')
                    save(out / 'after.cfg.json', ir_cfg((out / 'after.ll').read_text()))
                after_graph = graph_snapshot(graph)
                save(out / 'graph.after.json', after_graph)
                require(after_graph == before_graph, 'LLVM pipeline changed source Graph')
        require(all(sha(Path(p)) == digest for p, digest in inputs.items()), 'input pin changed')
        report.update(ok=True, verified=True, source_graph_exact=True,
                      precision_complete=None,
                      semantic_scope='IR baseline only; native equivalence pending')
        return 0
    except BaseException as error:
        report['failures'].append(dict(type=type(error).__name__, message=str(error)))
        report['inputs_unchanged'] = all(Path(p).is_file() and sha(p) == h for p, h in inputs.items())
        raise
    finally:
        save(out / 'REPORT.json', report)


def verify(args, ir, tag):
    command = [str(args.opt), '-passes=verify', '-disable-output', str(ir)]
    with (args.out / (tag + '.verify.stdout')).open('wb') as stdout, \
            (args.out / (tag + '.verify.stderr')).open('wb') as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=SECONDS)
    require(result.returncode == 0, 'LLVM verify failed: ' + tag)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--frozen', type=Path, required=True)
    p.add_argument('--frozen-sha256', required=True)
    p.add_argument('--family', required=True)
    p.add_argument('--c-level', choices=['O0', 'O2'], default='O2')
    p.add_argument('--entry-symbol', default='oracle_entry')
    p.add_argument('--kernel-symbol', default='kernel')
    p.add_argument('--python-root', type=Path, required=True)
    p.add_argument('--backend', type=Path, required=True)
    p.add_argument('--backend-sha256', required=True)
    p.add_argument('--shim', type=Path, required=True)
    p.add_argument('--shim-sha256', required=True)
    p.add_argument('--opt', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = p.parse_args()
    for name in ('backend', 'shim', 'opt', 'python_root', 'frozen', 'out'):
        require(getattr(args, name).is_absolute(), name + ' must be absolute')
    if args.worker:
        return worker(args)
    require(not args.out.exists(), 'new output namespace required')
    require(shutil.disk_usage(args.out.parent).free >= START, '512 MiB startup gate failed')
    args.out.mkdir()
    command = [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], '--worker']
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', LD_DEBUG='libs')
    start, failure = time.monotonic(), None
    with (args.out / 'worker.stdout').open('wb') as stdout, \
            (args.out / 'loader.stderr').open('wb') as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                   env=environment, start_new_session=True)
        while process.poll() is None:
            if time.monotonic() - start > SECONDS:
                failure = '180 second deadline'
            elif shutil.disk_usage(args.out).free < FLOOR:
                failure = '64 MiB running floor'
            if failure:
                os.killpg(process.pid, signal.SIGKILL)
                break
            time.sleep(0.1)
        code = process.wait()
    save(args.out / 'GUARD.json', dict(argv=command, returncode=code, failure=failure,
         elapsed_seconds=time.monotonic() - start,
         limits=dict(start_bytes=START, floor_bytes=FLOOR, seconds=SECONDS),
         free_after=shutil.disk_usage(args.out).free))
    return 1 if failure else code


if __name__ == '__main__':
    raise SystemExit(main())
