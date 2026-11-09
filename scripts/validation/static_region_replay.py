"""Replay frozen Tigress ELFs through explicit static decoded regions.

The unchanged finite_vm_replay oracle executes real Guest megaInit and compares
complete returns on all its modelled fields and 1 MiB of memory. Its --baseline
flag only disables the Graph finite-dispatch Pass. This wrapper selects symbol-
bounded decoded leaders for private machine-block rebuilding, then runs the
usual six LLVM steps followed by the optional control-flow Pass.
It does not infer Graph edges, initialize Guest data, or turn incomplete exits
into matches. Leaders come from existing valid blocks, direct branches,
instructions after terminators, and orphan decoded runs, never CPU observations
or jump-table contents. The native API rebuilds private IGNIL from machine
instructions; it does not reuse user-modified IGNIL for those rebuilt blocks.

Example (run from the owned VM worktree with a pinned library environment):
  python3 scripts/validation/static_region_replay.py --frozen FROZEN --out NEW \
    --tigress TIGRESS --strategy 0
Use --without-region for an actual ordinary-lift baseline, including an older
library/wrapper that has no lift_static_region API. LLVM optimization and all
oracle checks remain the same. --self-test is pure Python and imports no Dragon.
"""

import argparse
import collections
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def canonical_sha(value):
    return digest(json.dumps(value, sort_keys=True, separators=(',', ':')).encode())


def select_blocks(bounds, blocks):
    """Choose whole valid IGNIL blocks within one exact ELF symbol extent."""
    start, end = bounds
    if not 0 <= start < end <= 1 << 64:
        raise ValueError('invalid function symbol extent')
    selected = []
    for block in blocks:
        pcs = block.get('instructions', [])
        if (block['valid'] and block['ignil'] is not None and pcs
                and start <= block['address'] < end
                and all(start <= i['address'] and i['address'] + i['size'] <= end
                        for i in pcs)):
            selected.append(block['address'])
    return sorted(set(selected))


def select_region(bounds, snapshot):
    """Static leaders only; missing direct targets do not create instructions."""
    valid = select_blocks(bounds, snapshot['blocks'])
    start, end = bounds
    decoded = [i for i in snapshot['instructions']
               if start <= i['address'] and i['address'] + i['size'] <= end]
    decoded.sort(key=lambda i: i['address'])
    by_address = {i['address']: i for i in decoded}
    reasons = collections.defaultdict(set)
    for address in valid:
        reasons[address].add('existing-valid-IGNIL-block')
    covered = {i['address'] for b in snapshot['blocks'] if b['address'] in valid
               for i in b['instructions']}
    previous = None
    for ins in decoded:
        flags = ins['flags']
        if flags['is_jump'] and not flags['has_indirect_target']:
            for target in ins['branches']:
                if target in by_address:
                    reasons[target].add('direct-branch-target')
        if flags['is_jump'] or flags['is_return'] or flags['is_trap']:
            following = ins['address'] + ins['size']
            if following in by_address:
                reasons[following].add('decoded-after-machine-terminator')
        if ins['address'] not in covered:
            if (previous is None or previous['address'] in covered
                    or previous['address'] + previous['size'] != ins['address']):
                reasons[ins['address']].add('orphan-decoded-run-start')
        previous = ins
    return dict(selected_starts=sorted(reasons),
                selection_reasons={str(a): sorted(r) for a, r in sorted(reasons.items())},
                existing_valid_blocks=valid,
                decoded_without_original_valid_block=[i['address'] for i in decoded
                                                      if i['address'] not in covered])


def graph_snapshot(graph):
    """Include semantic operands, machine bytes, markers, and membership."""
    instructions = []
    for ins in sorted(graph.instructions(), key=lambda i: i.address):
        instructions.append(dict(address=ins.address, size=ins.size,
            bytes=ins.bytes.hex(), disassembly=ins.disassembly, edges=ins.edges,
            flags={name: getattr(ins, name) for name in (
                'is_block_start', 'is_function_start', 'is_prologue', 'is_call',
                'is_return', 'is_jump', 'is_conditional', 'is_trap',
                'has_indirect_target')}, branches=sorted(ins.branches),
            callees=sorted(ins.callees)))
    blocks = []
    for address in sorted(graph.block_addresses()):
        with graph.get_block(address) as block:
            row = dict(address=address, valid=block.is_valid, ignil=None)
            if block.is_valid:
                row.update(terminator=block.terminator_address,
                    byte_size=block.byte_size, successors=sorted(block.successors()),
                    instructions=[dict(address=i.address, size=i.size)
                                  for i in block.instructions()])
            ignil = graph.get_ignil_block(address)
            if ignil is not None:
                with ignil:
                    row['ignil'] = dict(text=ignil.to_string(),
                        ops=ignil.ops(), unlifted=list(ignil.unlifted()))
            blocks.append(row)
    functions = []
    for address in sorted(graph.function_addresses()):
        with graph.get_function(address) as function:
            row = dict(address=address, valid=function.is_valid)
            if function.is_valid:
                row.update(blocks=sorted(function.block_addresses()),
                    byte_size=function.byte_size, edges=function.edges,
                    instructions=function.num_instructions)
            functions.append(row)
    return dict(instructions=instructions, blocks=blocks, functions=functions)


def function_symbols(data, elf_class):
    """Require a positive STT_FUNC range in one file-backed executable section."""
    elf = elf_class(io.BytesIO(data))
    table = elf.get_section_by_name('.symtab')
    if table is None:
        raise ValueError('frozen replay needs an ELF symbol table')
    result = collections.defaultdict(list)
    for symbol in table.iter_symbols():
        section_index = symbol['st_shndx']
        if (symbol['st_info']['type'] != 'STT_FUNC'
                or type(section_index) is not int or not symbol['st_size']):
            continue
        start, size = int(symbol['st_value']), int(symbol['st_size'])
        section = elf.get_section(section_index)
        lower = int(section['sh_addr'])
        upper = lower + int(section['sh_size'])
        if (not section['sh_flags'] & 4 or section['sh_type'] == 'SHT_NOBITS'
                or not lower <= start < start + size <= upper):
            raise ValueError(f'function symbol outside executable bytes: {symbol.name}')
        result[start].append(dict(name=symbol.name, start=start, end=start + size,
                                  size=size, section=section.name))
    return dict(result)


def mapped_libraries(dragon):
    result = {}
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) != 6 or '/libdragon' not in fields[5]:
            continue
        if fields[5].endswith(' (deleted)') or not fields[5].startswith('/'):
            raise RuntimeError('invalid mapped Dragon library path')
        path = str(Path(fields[5]).resolve())
        result[path] = file_sha(path)
    shim = str(Path(dragon._LIB).resolve())
    if shim not in result or sum(Path(p).name == 'libdragon.so' for p in result) != 1:
        raise RuntimeError('actual backend/shim maps are not unambiguous')
    return result


def mapped_unicorn():
    paths = set()
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and '/libunicorn' in fields[5]:
            if fields[5].endswith(' (deleted)') or not fields[5].startswith('/'):
                raise RuntimeError('invalid mapped Unicorn library path')
            paths.add(str(Path(fields[5]).resolve()))
    if len(paths) != 1:
        raise RuntimeError('actual Unicorn reference library is not unambiguous')
    return {p: file_sha(p) for p in sorted(paths)}


def self_test():
    def block(address, size=1, valid=True, ignil=True):
        return dict(address=address, valid=valid,
                    ignil={'text': 'COPY r0:64 <- 7'} if ignil else None,
                    instructions=[dict(address=address, size=size)])
    blocks = [block(0x10), block(0x18, 9), block(0x20),
              block(0x12, valid=False), block(0x14, ignil=False)]
    if select_blocks((0x10, 0x20), blocks) != [0x10]:
        raise AssertionError('symbol boundary/valid-IGNIL selection control failed')
    leader_input = dict(blocks=[block(0x10)], instructions=[
        dict(address=a, size=1, branches=targets,
             flags=dict(is_jump=jump, has_indirect_target=indirect,
                        is_return=ret, is_trap=False))
        for a, jump, indirect, ret, targets in (
            (0x10, True, True, False, []), (0x11, False, False, False, []),
            (0x12, True, False, False, [0x16, 0x18]),
            (0x13, False, False, True, []), (0x14, False, False, False, []),
            (0x16, False, False, False, []))])
    leaders = select_region((0x10, 0x18), leader_input)
    if leaders['selected_starts'] != [0x10, 0x11, 0x13, 0x14, 0x16]:
        raise AssertionError('static leader selection failed')
    rejected = 0
    for extent in ((1, 1), (2, 1), (-1, 4), (0, (1 << 64) + 1)):
        try:
            select_blocks(extent, blocks)
        except ValueError:
            rejected += 1
    original = {'instructions': [dict(bytes='90', flags={'is_block_start': True})],
                'blocks': [block(0x10)], 'functions': [dict(blocks=[0x10])]}
    controls = []
    for kind in ('operand', 'bytes', 'flags', 'members'):
        changed = json.loads(json.dumps(original))
        if kind == 'operand':
            changed['blocks'][0]['ignil']['text'] = 'COPY r0:64 <- 8'
        elif kind == 'bytes':
            changed['instructions'][0]['bytes'] = 'cc'
        elif kind == 'flags':
            changed['instructions'][0]['flags']['is_block_start'] = False
        else:
            changed['functions'][0]['blocks'] = []
        controls.append(canonical_sha(original) != canonical_sha(changed))
    if rejected != 4 or not all(controls):
        raise AssertionError('invalid-extent or semantic fingerprint control failed')
    print(json.dumps(dict(pure_python=True, Dragon_CPU_Graph_LLVM_runs=0,
                          positive=2, rejected_invalid_extents=rejected,
                          mutation_controls_detected=sum(controls))))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--frozen', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--tigress')
    parser.add_argument('--strategy', type=int, choices=(0, 1), default=0)
    parser.add_argument('--profile', action='append', choices=('f-mba', 'f-combo'))
    parser.add_argument('--plans', default='product-O0,product-O2')
    parser.add_argument('--trials', type=int, default=8)
    parser.add_argument('--without-region', action='store_true')
    parser.add_argument('--max-blocks', type=int, default=4096)
    parser.add_argument('--max-dispatch-cases', type=int, default=65536)
    parser.add_argument('--control', choices=('llvm-register', 'llvm-memory'))
    for name, default in (('clang', 'clang-21'), ('ld', 'ld.lld-21'),
                          ('opt', 'opt-21'), ('llc', 'llc-21'), ('cc', 'gcc')):
        parser.add_argument('--' + name, default=default)
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.frozen or not args.out or not args.tigress:
        parser.error('--frozen, --out and --tigress are required')
    if (args.trials < 8 or not 1 <= args.max_blocks <= 4096
            or not 1 <= args.max_dispatch_cases <= 65536):
        parser.error('need at least eight trials and positive limits within hard caps')
    plans = args.plans.split(',')
    if not plans or len(plans) != len(set(plans)) or not set(plans) <= {'product-O0', 'product-O2'}:
        parser.error('this replay supports product-O0 and product-O2 only')
    profiles = args.profile or ['f-mba', 'f-combo']
    if len(profiles) != len(set(profiles)):
        parser.error('duplicate profiles')
    out = args.out.expanduser().resolve()
    if out.exists():
        parser.error('--out must be a fresh directory; failed runs are retained')
    frozen = args.frozen.expanduser().resolve()
    inputs = {}
    for profile in profiles:
        for name, path in (('elf', frozen / profile / 'O0' / 'fixture.elf'),
                           ('source', frozen / profile / 'transformed.c')):
            inputs[str(path)] = dict(profile=profile, role=name, size=path.stat().st_size,
                                     sha256=file_sha(path))
    if os.environ.get('DRAGON_TEST_PYTHON'):
        sys.path.insert(0, os.environ['DRAGON_TEST_PYTHON'])
    import finite_vm_replay as replay
    d, study = replay.d, replay.study
    tf = study.tf
    out.mkdir(parents=True)
    libraries = mapped_libraries(d)
    unicorn_libraries = mapped_unicorn()
    dependencies = [Path(__file__), Path(d.__file__), Path(replay.__file__),
                    Path(study.__file__), Path(tf.__file__),
                    Path(study.guest.__file__), Path(tf.compiler.__file__),
                    Path(study.base.__file__),
                    Path(d.__file__).resolve().parents[1] / 'c_shim' / 'dragon_c.h',
                    Path(d.__file__).resolve().parents[1] / 'include/dragon/lifters/llvm/Execution.h',
                    Path(__file__).resolve().parents[2] / 'include/dragon/lifters/llvm/Execution.hpp']
    validation_dir = Path(__file__).resolve().parent
    dependencies.extend(Path(module.__file__) for module in tuple(sys.modules.values())
                        if getattr(module, '__file__', None)
                        and Path(module.__file__).resolve().parent == validation_dir)
    sources = {str(p.resolve()): file_sha(p) for p in dependencies}
    save(out / 'preflight.json', dict(schema=1, strategy=args.strategy,
        without_region=args.without_region, libraries=libraries,
        unicorn_libraries=unicorn_libraries, sources=sources,
        inputs=inputs, memory_bytes=study.MEM_SIZE,
        region_source='ELF STT_FUNC bounds; valid blocks, direct branches, after-terminators, orphan-run starts',
        private_machine_IGNIL_rebuild=not args.without_region,
        Graph_finite_dispatch=False, real_Guest_megaInit=True, injected_memory=False))
    compile_original = tf.compile_image
    lift_original = d.LLVMLifter.lift
    optimize_original = d.LLVMLifter.optimize
    modules = []
    active = None

    def lift(lifter, function, graph):
        if active is None:
            raise RuntimeError('lift outside the compile-image context')
        if active['graph'] is None:
            active['graph'] = graph
            active['before'] = graph_snapshot(graph)
            save(active['work'] / 'graph-before.json', active['before'])
        if graph is not active['graph']:
            raise RuntimeError('compile_image changed Graph during one module')
        address = function.start_address
        symbols = active['symbols'].get(address, [])
        ranges = {(s['start'], s['end']) for s in symbols}
        row = dict(entry=address, symbols=symbols, original_blocks=sorted(function.block_addresses()))
        if args.without_region or len(ranges) != 1:
            row.update(method='ordinary-lift', reason='explicit-baseline' if args.without_region
                       else 'no-unique-positive-executable-symbol-extent')
        else:
            bounds = next(iter(ranges))
            selection = select_region(bounds, active['before'])
            extra = sorted(set(selection['selected_starts']) - set(row['original_blocks']))
            row.update(method='lift-static-region', bounds=list(bounds),
                       additional_blocks=extra, **selection)
        active['lifts'].append(row)
        try:
            if row['method'] == 'ordinary-lift':
                ok = lift_original(lifter, function, graph)
            else:
                # No fallback: a missing new symbol on an old library is an error.
                ok = lifter.lift_static_region(function, graph, extra,
                    max_blocks=args.max_blocks, max_dispatch_cases=args.max_dispatch_cases,
                    rebuild_decoded_blocks=True)
            row['accepted'] = bool(ok)
        except Exception as error:
            row['error'] = dict(type=type(error).__name__, detail=str(error))
            raise
        finally:
            save(active['work'] / 'region-bounds.json', active['lifts'])
        return ok

    def optimize(lifter, *a, **kw):
        if active is None:
            raise RuntimeError('optimize outside the compile-image context')
        result = optimize_original(lifter, *a, **kw)
        before = lifter.ir()
        (active['work'] / 'before-control-flow.ll').write_text(before)
        with d.PassPipeline(['control-flow']) as pipeline:
            report = pipeline.run(lifter)
        after = lifter.ir()
        (active['work'] / 'after-control-flow.ll').write_text(after)
        record = dict(report=report, before_sha256=digest(before.encode()),
                      after_sha256=digest(after.encode()))
        save(active['work'] / 'control-flow-report.json', record)
        active['control_flow'] = record
        if not report['ok']:
            raise RuntimeError('control-flow pipeline failed')
        return result

    def compile_image(data, symbols, work, static, product, level, options, budget=10000):
        nonlocal active
        if active is not None or not static or not product or not options.function_entry:
            raise RuntimeError('expected sequential StaticAnalysis FunctionStart product compilation')
        active = dict(work=Path(work), symbols=function_symbols(data, study.ELFFile),
                      graph=None, before=None, lifts=[], elf_sha256=digest(data), level=level)
        current = active
        started = time.monotonic()
        try:
            return compile_original(data, symbols, work, static, product, level, options, budget=budget)
        finally:
            try:
                if current['graph'] is not None:
                    after = graph_snapshot(current['graph'])
                    save(current['work'] / 'graph-after.json', after)
                    before_sha, after_sha = canonical_sha(current['before']), canonical_sha(after)
                    current['Graph_unchanged'] = before_sha == after_sha
                    current['Graph_before_sha256'] = before_sha
                    current['Graph_after_sha256'] = after_sha
                    if before_sha != after_sha:
                        raise RuntimeError('LLVM region lifting changed the source Graph')
            finally:
                record = {k: v for k, v in current.items() if k not in ('graph', 'before', 'symbols')}
                record['work'] = str(record['work'])
                record['elapsed_seconds'] = time.monotonic() - started
                modules.append(record)
                save(current['work'] / 'static-region-module.json', record)
                active = None

    argv = [str(Path(replay.__file__).resolve()), '--baseline', '--frozen', str(frozen),
            '--out', str(out / 'oracle'), '--tigress', args.tigress, '--function-level',
            '--function-entry', '--c-level', 'O0', '--plans', args.plans,
            '--trials', str(args.trials), '--strategy', str(args.strategy)]
    for profile in profiles:
        argv += ['--profile', profile]
    if args.control:
        argv += ['--control', args.control]
    for name in ('clang', 'ld', 'opt', 'llc', 'cc'):
        argv += ['--' + name, getattr(args, name)]
    save(out / 'invocation.json', dict(argv=argv, cwd=str(Path.cwd()), started_unix=time.time()))
    previous_argv = sys.argv
    tf.compile_image = compile_image
    d.LLVMLifter.lift = lift
    d.LLVMLifter.optimize = optimize
    status = None
    try:
        sys.argv = argv
        status = replay.main()
    finally:
        sys.argv = previous_argv
        tf.compile_image = compile_original
        d.LLVMLifter.lift = lift_original
        d.LLVMLifter.optimize = optimize_original
        save(out / 'static-region-modules.json', modules)
        stable = (mapped_libraries(d) == libraries and mapped_unicorn() == unicorn_libraries
                  and all(file_sha(p) == h for p, h in sources.items())
                  and all(file_sha(p) == item['sha256'] for p, item in inputs.items()))
        save(out / 'postflight.json', dict(oracle_returncode=status, runtime_sources_inputs_unchanged=stable))
        if not stable:
            raise RuntimeError('runtime, input, or validation source changed during replay')
    raw = json.loads((out / 'oracle' / 'results.json').read_bytes())
    rows = raw['results']
    summary = dict(schema=1, oracle_returncode=status, strategy=args.strategy,
        without_region=args.without_region, libraries=libraries,
        C_statuses=raw['summary']['c_statuses'], LLVM_statuses=raw['summary']['llvm_statuses'],
        complete_root_comparisons=sum(r['status'] in ('match', 'different') for r in rows),
        complete_root_differences=sum(r['status'] == 'different' for r in rows),
        incomplete=sum(r['status'] == 'incomplete' for r in rows),
        other_LLVM_errors=sum(r['status'] not in ('match', 'different', 'incomplete') for r in rows),
        CFG_wrong_sites=raw['summary']['cfg_wrong_sites'],
        executed_undecoded=sum(a.get('executed_undecoded', 0) for a in raw['analyses']),
        unlifted=sum(a.get('unlifted', 0) for a in raw['analyses']),
        modules=len(modules), Graph_unchanged=bool(modules) and all(m.get('Graph_unchanged') is True for m in modules),
        memory_bytes=study.MEM_SIZE, real_Guest_megaInit=True, injected_memory=False,
        Graph_finite_dispatch=False,
        semantic_complete=(bool(rows) and all(r['status'] == 'match' for r in rows)
                           and all(r['status'] == 'match' for r in raw['c_results'])
                           and raw['summary']['cfg_wrong_sites'] == 0))
    save(out / 'static-region-summary.json', summary)
    print(json.dumps(summary, sort_keys=True), flush=True)
    return status


if __name__ == '__main__':
    raise SystemExit(main())
