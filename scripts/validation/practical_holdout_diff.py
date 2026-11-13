"""Small, separately reported heldout batch using the unchanged function oracle.

First --prepare-only freezes C sources and eight Guest ELFs. Then --frozen
replays those bytes with static/product O0/O2; it never invokes a Guest producer.
The independent Python specification checks return/out4 across different ELFs.
Complete registers, flags, Guest memory and Exit are compared only against each
ELF's own Unicorn execution. Counts/IR sizes describe residuals, not full CFF
removal or a return-cone proof. This does not change any historical campaign.
"""
import argparse
import collections
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

# Refuse before old imports can fall back to an owned build or a stale RPATH.
_explicit_shim = os.environ.get('DRAGON_C_LIB', '')
if not (_explicit_shim and Path(_explicit_shim).is_absolute()
        and Path(_explicit_shim).is_file()):
    raise RuntimeError('set DRAGON_C_LIB to an existing absolute formal shim before import')
_explicit_ld = os.environ.get('LD_LIBRARY_PATH', '').split(':')
if not all(part and Path(part).is_absolute() for part in _explicit_ld):
    raise RuntimeError('set an explicit absolute LD_LIBRARY_PATH before import')

if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0, os.environ['DRAGON_TEST_PYTHON'])
    import dragon  # Bind the explicitly selected wrapper before old imports.

import practical_holdout_source as heldout
import tigress_function_diff as study
from elftools.elf.elffile import ELFFile
import generated_function_diff


FAMILIES = ('plain', 'manual', 'arithmetic', 'flatten-switch')
SEED = 2026111307
PLANS = [('static-O0', True, False, 'O0'),
         ('static-O2', True, False, 'O2'),
         ('product-O0', True, True, 'O0'),
         ('product-O2', True, True, 'O2')]
PIPELINE = [('mem2reg', {}), ('instcombine', {}), ('gvn', {}),
            ('guest-memory', {}),
            ('mba', dict(timeout_ms=20, max_queries=64,
                         max_expression_nodes=128, resource_limit=100000,
                         max_variables=3)),
            ('predicates', dict(timeout_ms=20, max_queries=64,
                                max_expression_nodes=128,
                                resource_limit=100000)),
            ('dispatch-specialize', {}), ('dispatch-deflatten', {}),
            ('control-flow', {}), ('dce', {})]
GUEST_STEPS = 1000000
SUBPROCESS_SECONDS = 180
START_FREE = 512 * 1024 * 1024
RUN_FREE = 64 * 1024 * 1024
SCHEMA = 'dragon-practical-holdouts-inputs-v1'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def space(path, minimum):
    path = Path(path).resolve()
    while not path.exists():
        path = path.parent
    free = shutil.disk_usage(path).free
    require(free >= minimum, f'free bytes {free} below frozen minimum {minimum}')
    return free


def source_pins():
    modules = [sys.modules[__name__], heldout, study, study.guest,
               study.compiler, study.base, generated_function_diff,
               study.base.dragon]
    paths = {Path(module.__file__).resolve() for module in modules}
    # Include the actual imported repository dependency closure. A later lazy
    # import of another repository module changes this inventory and is refused.
    repo_roots = {study.base.ROOT.resolve(), Path(__file__).resolve().parents[2],
                  Path(study.base.dragon.__file__).resolve().parents[1]}
    for module in list(sys.modules.values()):
        filename = getattr(module, '__file__', None)
        if filename:
            path = Path(filename).resolve()
            if path.suffix == '.py' and any(path.is_relative_to(root) for root in repo_roots):
                paths.add(path)
    paths.update(study.base.ROOT / p for p in (
        'c_shim/dragon_c.h', 'include/dragon/lifters/llvm/Execution.h'))
    return {str(path): digest(path) for path in sorted(paths)}


def producer_pins(args):
    paths = {Path(args.tigress).resolve()}
    for tool in ('clang', 'ld'):
        resolved = shutil.which(getattr(args, tool))
        require(resolved, f'missing {tool}')
        paths.add(Path(resolved).resolve())
    # The existing wrapper delegates to these siblings; a wrapper hash alone
    # does not identify the actual Tigress/CIL producer.
    for name in ('tigress', 'cilly.byte.exe', 'cilly.asm.exe'):
        path = Path(args.tigress).resolve().parent / name
        if path.is_file():
            paths.add(path)
    return {str(path): digest(path) for path in sorted(paths)}


def runtime_pins():
    configured = os.environ.get('DRAGON_C_LIB', '')
    require(configured and Path(configured).is_absolute(),
            'DRAGON_C_LIB must explicitly name an absolute formal shim')
    shim = Path(study.base.dragon._LIB).resolve()
    require(shim == Path(configured).resolve() and shim.is_file(),
            'imported shim differs from explicit DRAGON_C_LIB')
    ld_dirs = os.environ.get('LD_LIBRARY_PATH', '').split(':')
    require(all(part and Path(part).is_absolute() for part in ld_dirs),
            'LD_LIBRARY_PATH must explicitly contain absolute runtime directories')
    resolved_dirs = {Path(part).resolve() for part in ld_dirs}
    backends = set()
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and Path(fields[5]).name == 'libdragon.so':
            backends.add(Path(fields[5]).resolve())
    require(backends, 'no mapped libdragon.so found for explicit backend pin')
    require(all(path.parent in resolved_dirs for path in backends),
            'mapped backend is outside explicit LD_LIBRARY_PATH')
    return dict(shim=dict(path=str(shim), sha256=digest(shim)),
                backends=[dict(path=str(path), sha256=digest(path))
                          for path in sorted(backends)],
                LD_LIBRARY_PATH=os.environ['LD_LIBRARY_PATH'],
                DRAGON_C_LIB=configured,
                DRAGON_TEST_PYTHON=os.environ.get('DRAGON_TEST_PYTHON'))


def elf_parts(data):
    elf = ELFFile(io.BytesIO(data))
    regions = []
    for section in elf.iter_sections():
        if section['sh_flags'] & 2 and section['sh_size']:
            start, size = section['sh_addr'], section['sh_size']
            require(study.ENTRY <= start and start + size < 0x7000,
                    'fixture exceeds unchanged reserved low image region')
            regions.append((start, bytes(size) if section['sh_type'] == 'SHT_NOBITS'
                            else section.data()))
    symbols = {symbol.name: symbol['st_value']
               for symbol in elf.get_section_by_name('.symtab').iter_symbols()
               if symbol['st_info']['type'] == 'STT_FUNC'
               and symbol['st_shndx'] != 'SHN_UNDEF'}
    require(symbols.get('oracle_entry') == study.ENTRY and 'kernel' in symbols,
            'frozen ELF lacks independently known entry symbols')
    return data, regions, symbols


def frozen_file(root, relative, manifest):
    require(relative in manifest['files'], 'file absent from frozen manifest')
    path = root / relative
    require(not path.is_symlink() and path.is_file()
            and path.resolve().is_relative_to(root), 'unsafe frozen file')
    require(digest(path) == manifest['files'][relative],
            f'frozen file hash mismatch: {relative}')
    return path


def verify_frozen(root, manifest):
    require(manifest['schema'] == SCHEMA and manifest['seed'] == SEED,
            'unexpected frozen batch schema/seed')
    require(manifest['inputs'] == list(heldout.INPUTS)
            and len(manifest['inputs']) == 16, 'frozen input set changed')
    require(manifest['source_pins'] == source_pins(), 'oracle/source pins changed')
    require(manifest['plans'] == [list(plan) for plan in PLANS]
            and manifest['pipeline'] == [[name, options] for name, options in PIPELINE],
            'frozen plan changed')
    require(manifest['guest_steps'] == GUEST_STEPS
            and manifest['subprocess_seconds'] == SUBPROCESS_SECONDS,
            'frozen execution limits changed')
    for relative in manifest['files']:
        frozen_file(root, relative, manifest)


def prepare(args, families, source_before, runtime_before):
    require(args.control in (None, 'c-result'),
            'LLVM controls replay ordinary frozen inputs; they need no prepare')
    if args.control:
        require(families == ['plain'], 'C-result control prepares only plain')
    original = args.out / 'original.c'
    original.write_text(heldout.source(SEED))
    records = {}
    timings = []
    producers_before = producer_pins(args)
    original_run = study.run

    def timed_run(command, work, tag, *a, **kw):
        space(args.out, RUN_FREE)
        start = time.monotonic()
        try:
            kw['timeout'] = SUBPROCESS_SECONDS
            return original_run(command, work, tag, *a, **kw)
        finally:
            timings.append(dict(tag=tag, work=str(Path(work).relative_to(args.out)),
                                seconds=time.monotonic() - start))

    study.run = timed_run
    try:
        require(Path(args.tigress).is_absolute() and Path(args.tigress).is_file(),
                'use the explicit existing Tigress wrapper path')
        for tool in ('clang', 'ld'):
            study.run([getattr(args, tool), '--version'], args.out,
                      tool + '-version')
        for family in families:
            work = args.out / family
            work.mkdir()
            input_c, transformed = work / 'input.c', work / 'transformed.c'
            input_c.write_text(heldout.source(SEED, wrong=args.control == 'c-result',
                                              manual=family == 'manual'))
            if family in ('plain', 'manual'):
                transformed.write_bytes(input_c.read_bytes())
            else:
                study.run([args.tigress, '--Environment=x86_64:Linux:Gcc:4.6',
                           '--Seed=' + str(SEED), *study.transforms(family, args.version),
                           '--out=' + str(transformed), '-c', str(input_c)], work,
                          'tigress')
            images = {}
            for level in ('O0', 'O2'):
                image_work = work / level
                data, _, symbols = study.build_image(transformed, image_work, level, args)
                images[level] = dict(elf=str((image_work / 'fixture.elf').relative_to(args.out)),
                                     sha256=hashlib.sha256(data).hexdigest(), symbols=symbols)
            records[family] = dict(manual=family == 'manual',
                                   profile='plain' if family in ('plain', 'manual') else family,
                                   input_c=str(input_c.relative_to(args.out)),
                                   transformed_c=str(transformed.relative_to(args.out)),
                                   images=images)
        # Reuse the actual plain ELF as the original calibration, when present.
        # A C-result control instead needs separate correct original ELF bytes.
        calibration = {}
        if 'plain' in records and args.control is None:
            calibration = records['plain']['images']
        else:
            for level in ('O0', 'O2'):
                work = args.out / 'calibration' / level
                data, _, symbols = study.build_image(original, work, level, args)
                calibration[level] = dict(elf=str((work / 'fixture.elf').relative_to(args.out)),
                                          sha256=hashlib.sha256(data).hexdigest(), symbols=symbols)
    finally:
        study.run = original_run
    files = {str(path.relative_to(args.out)): digest(path)
             for path in sorted(args.out.rglob('*')) if path.is_file()}
    manifest = dict(schema=SCHEMA, seed=SEED, inputs=list(heldout.INPUTS),
                    fixture=dict(bounds=heldout.BOUNDS,
                                 business_branch=heldout.BUSINESS_BRANCH,
                                 opaque_predicates=heldout.OPAQUE_PREDICATES),
                    families=records, calibration=calibration,
                    original_c='original.c', control=args.control, files=files,
                    plans=PLANS, pipeline=PIPELINE, guest_steps=GUEST_STEPS,
                    subprocess_seconds=SUBPROCESS_SECONDS,
                    static_entry_policy='function-start', strategy=args.strategy,
                    tigress=dict(path=str(Path(args.tigress).resolve()), version=args.version,
                                 sha256=digest(args.tigress)),
                    source_pins=source_before, runtime_pins=runtime_before,
                    producer_pins=producers_before,
                    timings=timings, skipped=0, undefined_exclusions=0,
                    calibration_elfs=2, batch_elfs=2 * len(families),
                    planned_c_checks=32 * len(families),
                    planned_llvm_comparisons=128 * len(families),
                    producer_only=True, product_pass_runs=0, target_native_runs=0)
    require(source_pins() == source_before and runtime_pins() == runtime_before,
            'source/runtime changed while preparing')
    require(producer_pins(args) == producers_before, 'producer tools changed while preparing')
    write_json(args.out / 'FROZEN.json', manifest)
    return 0


def ir_description(path):
    """Textual counts only; no CFG, semantic cone or full-unflattening claim."""
    text = path.read_bytes().decode()
    operations = collections.Counter()
    switches, entry_switches, function, block = 0, 0, None, None
    opcodes = set(('ret br switch indirectbr invoke callbr resume catchswitch catchret '
                   'cleanupret unreachable fneg add fadd sub fsub mul fmul udiv sdiv '
                   'fdiv urem srem frem shl lshr ashr and or xor alloca load store '
                   'fence cmpxchg atomicrmw getelementptr trunc zext sext fptrunc fpext '
                   'fptoui fptosi uitofp sitofp ptrtoint inttoptr bitcast addrspacecast '
                   'icmp fcmp phi select freeze call va_arg landingpad catchpad '
                   'cleanuppad extractelement insertelement shufflevector extractvalue '
                   'insertvalue').split())
    for line in text.splitlines():
        if line.startswith('define '):
            function, block = line, 'entry'
        elif line == '}':
            function = None
        elif function:
            label = re.match(r'^([A-Za-z$._][\w$.-]*|\d+):', line)
            if label:
                block = label[1]
                continue
            instruction = re.sub(r'^\s*%[^=]+ =\s*', '', line.strip())
            instruction = re.sub(r'^(?:tail|musttail|notail)\s+', '', instruction)
            opcode = instruction.split(maxsplit=1)[0] if instruction else ''
            if opcode in opcodes:
                operations[opcode] += 1
                if opcode == 'switch':
                    switches += 1
                    entry_switches += block == 'entry'
    return dict(sha256=digest(path), bytes=len(path.read_bytes()),
                operation_counts=dict(operations), total_operations=sum(operations.values()),
                switches=switches, switches_in_exact_entry_block=entry_switches,
                non_entry_switches=switches - entry_switches,
                scope='textual opcounts; entry means exact block label entry; no cone/central-CFF proof')


def replay(args, families, source_before, runtime_before):
    frozen = args.frozen.resolve()
    manifest_bytes = (frozen / 'FROZEN.json').read_bytes()
    manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
    manifest = json.loads(manifest_bytes)
    verify_frozen(frozen, manifest)
    require(all(family in manifest['families'] for family in families),
            'requested family is not frozen')
    require(manifest['strategy'] == args.strategy, 'frozen decode strategy changed')
    require(manifest['control'] == ('c-result' if args.control == 'c-result' else None),
            'C-result needs its own frozen erroneous C/ELFs')
    require(not args.control or families == ['plain'], 'controls select only plain')
    d = study.base.dragon
    originals = dict(source=study.source, specification=study.specification,
                     build_image=study.build_image, run=study.run,
                     compile_image=study.compile_image, inputs=study.INPUTS,
                     plans=study.PLANS, disassemble=d.LoadedImage.disassemble,
                     lift=d.LLVMLifter.lift, optimize=d.LLVMLifter.optimize,
                     native=study.base.native, reference=study.guest.Adapter.reference,
                     subprocess_run=subprocess.run, argv=sys.argv)
    timings, passes, descriptions, summaries, statuses = [], [], [], [], []
    active = {}
    current = {}

    def event(kind, start, **extra):
        timings.append(dict(kind=kind, seconds=time.monotonic() - start,
                            case=active.get('case'), family=current.get('family'), **extra))

    def adapted_source(seed, *a, **kw):
        require(seed == SEED, 'unexpected seed')
        # tf.main calls original source(seed), then input source(seed, wrong=...).
        if not a and 'wrong' not in kw:
            return heldout.source(seed)
        wrong = a[0] if a else kw.get('wrong', False)
        return heldout.source(seed, wrong=wrong, manual=current['family'] == 'manual')

    def frozen_build(c_source, work, level, configuration):
        space(args.out, RUN_FREE)
        active['case'] = str(Path(work).relative_to(args.out))
        original = Path(work).name.startswith('original-')
        record = manifest['families'][current['family']]
        image = manifest['calibration'][level] if original else record['images'][level]
        source_rel = manifest['original_c'] if original else record['transformed_c']
        source_path = frozen_file(frozen, source_rel, manifest)
        require(Path(c_source).read_bytes() == source_path.read_bytes(),
                'generated source differs from exact frozen C')
        elf_path = frozen_file(frozen, image['elf'], manifest)
        data = elf_path.read_bytes()
        require(hashlib.sha256(data).hexdigest() == image['sha256'], 'ELF record changed')
        parts = elf_parts(data)
        require(parts[2] == image['symbols'], 'ELF symbols differ from frozen record')
        Path(work).mkdir(parents=True, exist_ok=True)
        (Path(work) / 'fixture.elf').write_bytes(data)
        write_json(Path(work) / 'frozen-input.json', dict(source=source_rel,
                   elf=image['elf'], sha256=image['sha256'], manual=record['manual']))
        return parts

    def timed_run(command, work, tag, *a, **kw):
        space(args.out, RUN_FREE)
        if tag == 'tigress':
            record = manifest['families'][current['family']]
            target = Path(work) / 'transformed.c'
            target.write_bytes(frozen_file(frozen, record['transformed_c'], manifest).read_bytes())
            (Path(work) / 'frozen-transform.txt').write_text(record['transformed_c'] + '\n')
            return 'Exact frozen transformed C replay; producer not invoked\n'
        start = time.monotonic()
        try:
            kw['timeout'] = SUBPROCESS_SECONDS
            return originals['run'](command, work, tag, *a, **kw)
        finally:
            event('llvm_stock_opt' if tag == 'optimize' else
                  'native_compile' if tag in ('llc', 'native-link') else 'subprocess',
                  start, tag=tag, work=str(Path(work).relative_to(args.out)))

    def disassemble(image, *a, **kw):
        space(args.out, RUN_FREE)
        start = time.monotonic()
        try:
            return originals['disassemble'](image, *a, **kw)
        finally:
            event('decode_analysis', start)

    def lift(lifter, *a, **kw):
        start = time.monotonic()
        try:
            return originals['lift'](lifter, *a, **kw)
        finally:
            event('lift', start)

    def optimize(lifter, pipeline=None):
        require(pipeline is None, 'only the frozen public pipeline is permitted')
        start = time.monotonic()
        with d.PassPipeline(PIPELINE) as selected:
            report = selected.run(lifter, max_iterations=1, max_pass_runs=16, verify=True)
        event('product_pipeline', start)
        passes.append(dict(case=active.get('case'), family=current['family'], report=report))
        require(report['ok'], 'public pipeline reported an error; discard this target')
        require([row['name'] for row in report['runs']] == [name for name, _ in PIPELINE],
                'public pipeline did not execute the frozen ten steps')
        return report

    def compile_image(data, symbols, work, *a, **kw):
        active['case'] = str(Path(work).relative_to(args.out))
        result = originals['compile_image'](data, symbols, work, *a, **kw)
        metrics = {}
        for filename in ('unoptimized.ll', 'raw.ll', 'optimized.ll'):
            path = Path(work) / filename
            if path.exists():
                metrics[filename] = ir_description(path)
        product = a[1] if len(a) > 1 else kw['product']
        if product:
            shutil.copyfile(Path(work) / 'raw.ll', Path(work) / 'product.ll')
        if (Path(work) / 'optimized.ll').exists():
            shutil.copyfile(Path(work) / 'optimized.ll', Path(work) / 'stock.ll')
        descriptions.append(dict(case=active['case'], family=current['family'], ir=metrics))
        return result

    def reference(adapter, fixture, *a, **kw):
        space(args.out, RUN_FREE)
        fixture = dict(fixture, max_instructions=GUEST_STEPS)
        start = time.monotonic()
        try:
            return originals['reference'](adapter, fixture, *a, **kw)
        finally:
            event('unicorn_reference', start)

    def native(*a, **kw):
        space(args.out, RUN_FREE)
        start = time.monotonic()
        try:
            return originals['native'](*a, **kw)
        finally:
            event('target_native', start)

    def subprocess_run(*a, **kw):
        # base.native's existing protocol/comparator stays intact. Only this
        # new batch's child deadline changes from its small-kernel default.
        kw['timeout'] = SUBPROCESS_SECONDS
        return originals['subprocess_run'](*a, **kw)

    study.source, study.specification = adapted_source, heldout.specification
    study.build_image, study.run, study.compile_image = frozen_build, timed_run, compile_image
    study.INPUTS, study.PLANS = list(heldout.INPUTS), PLANS
    d.LoadedImage.disassemble, d.LLVMLifter.lift = disassemble, lift
    d.LLVMLifter.optimize = optimize
    study.base.native, study.guest.Adapter.reference = native, reference
    subprocess.run = subprocess_run
    try:
        for family in families:
            current['family'] = family
            active.clear()
            family_out = args.out / family
            record = manifest['families'][family]
            argv = [originals['argv'][0], '--out', str(family_out),
                    '--profile', record['profile'], '--seeds', '1',
                    '--first-seed', str(SEED), '--trials', '16',
                    '--strategy', str(args.strategy), '--function-entry',
                    '--tigress', manifest['tigress']['path'],
                    '--version', manifest['tigress']['version']]
            for tool in ('clang', 'ld', 'opt', 'llc', 'cc'):
                argv += ['--' + tool, getattr(args, tool)]
            if args.control:
                argv += ['--control', args.control]
            if args.expect_control:
                argv += ['--expect-control']
            sys.argv = argv
            status = study.main()
            result_path = family_out / 'results.json'
            result = json.loads(result_path.read_bytes())
            summary = result['summary']
            checked = [analysis for analysis in result['analyses']
                       if analysis['plan'] == 'static-O0']
            summary.update(function_entry_checks=len(checked),
                           function_entry_failures=sum(bool(a['missing_symbol_entries']
                                                       or a['extra_function_entries']) for a in checked),
                           missing_symbol_entries=sum(len(a['missing_symbol_entries']) for a in checked),
                           extra_function_entries=sum(len(a['extra_function_entries']) for a in checked),
                           unlifted_instructions=sum(len(a['unlifted']) for a in checked),
                           manual=record['manual'], heldout_family=family)
            # tf's historical summary selects v2-O0; it must not silently turn
            # into a successful empty set after this adapter selects four plans.
            if len(checked) != 2 or summary['function_entry_failures']:
                status = 1
            write_json(result_path, result)
            summaries.append(summary)
            statuses.append(dict(family=family, returncode=status,
                                 results=str(result_path.relative_to(args.out))))
    finally:
        study.source, study.specification = originals['source'], originals['specification']
        study.build_image, study.run = originals['build_image'], originals['run']
        study.compile_image = originals['compile_image']
        study.INPUTS, study.PLANS = originals['inputs'], originals['plans']
        d.LoadedImage.disassemble, d.LLVMLifter.lift = originals['disassemble'], originals['lift']
        d.LLVMLifter.optimize = originals['optimize']
        study.base.native, study.guest.Adapter.reference = originals['native'], originals['reference']
        subprocess.run, sys.argv = originals['subprocess_run'], originals['argv']
        write_json(args.out / 'practical-progress.json',
                   dict(families=statuses, timings=timings, pass_reports=passes,
                        ir_descriptions=descriptions))
    verify_frozen(frozen, manifest)
    require(digest(frozen / 'FROZEN.json') == manifest_hash, 'frozen manifest changed during replay')
    require(source_pins() == source_before and runtime_pins() == runtime_before,
            'source/runtime pins changed during replay')
    calibration_checks = sum(summary['original_specification_checks'] for summary in summaries)
    report = dict(schema='dragon-practical-holdouts-results-v1',
                  frozen=str(frozen), frozen_manifest_sha256=manifest_hash,
                  source_pins=source_before, runtime_pins=runtime_before,
                  seed=SEED, inputs=list(heldout.INPUTS), families=statuses,
                  summaries=summaries, manual_is_obfuscated=True,
                  planned_c_checks=32 * len(families),
                  planned_llvm_comparisons=128 * len(families),
                  c_checks=sum(s['c_checks'] for s in summaries),
                  llvm_comparisons=sum(s['comparisons'] for s in summaries),
                  original_specification_checks_executed=calibration_checks,
                  original_specification_distinct_inputs=16,
                  original_specification_distinct_elf_input_pairs=len({
                      (image['sha256'], value)
                      for image in manifest['calibration'].values()
                      for value in heldout.INPUTS}),
                  guest_steps=GUEST_STEPS, subprocess_seconds=SUBPROCESS_SECONDS,
                  plans=PLANS, pipeline=PIPELINE, max_iterations=1, max_pass_runs=16,
                  timings=timings, pass_reports=passes, ir_descriptions=descriptions,
                  skipped=0, undefined_exclusions=0,
                  scope='heldout semantic/residual/cost report; no full-CFF or cone proof; old gates unchanged')
    write_json(args.out / 'practical-results.json', report)
    return 0 if len(statuses) == len(families) and all(row['returncode'] == 0 for row in statuses) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--prepare-only', action='store_true')
    mode.add_argument('--frozen', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--family', action='append', choices=FAMILIES)
    parser.add_argument('--tigress', default='/home/cc/bench/tools/tigress-2.1-github/run-tigress')
    parser.add_argument('--version', default='2.1')
    parser.add_argument('--strategy', type=int, choices=(0, 1), default=0)
    parser.add_argument('--control', choices=('c-result', 'llvm-register', 'llvm-memory'))
    parser.add_argument('--expect-control', action='store_true')
    for name, default in [('clang', 'clang-21'), ('ld', 'ld.lld-21'),
                          ('opt', 'opt-21'), ('llc', 'llc-21'), ('cc', 'gcc')]:
        parser.add_argument('--' + name, default=default)
    args = parser.parse_args()
    families = args.family or list(FAMILIES)
    require(len(families) == len(set(families)), 'duplicate family')
    require(heldout.SEED == SEED and len(heldout.INPUTS) == 16
            and heldout.GUEST_STEP_LIMIT == GUEST_STEPS,
            'heldout source seed/input contract changed')
    require(not args.expect_control or args.control, '--expect-control requires --control')
    require(not args.control or families == ['plain'], 'controls require --family plain')
    args.out = args.out.resolve()
    require(not args.out.exists(), 'output namespace must not exist')
    initial_free = space(args.out, START_FREE)
    source_before, runtime_before = source_pins(), runtime_pins()
    args.out.mkdir(parents=True)
    write_json(args.out / 'INVOCATION.json', dict(argv=sys.argv, initial_free=initial_free,
               source_pins=source_before, runtime_pins=runtime_before,
               guest_steps=GUEST_STEPS, subprocess_seconds=SUBPROCESS_SECONDS,
               start_free=START_FREE, running_free=RUN_FREE))
    os.environ.setdefault('CILLY_NATIVE_CC', 'x86_64-linux-gnu-gcc')
    try:
        return prepare(args, families, source_before, runtime_before) if args.prepare_only else replay(
            args, families, source_before, runtime_before)
    except BaseException as error:
        write_json(args.out / 'FAILURE.json', dict(type=type(error).__name__, detail=str(error)))
        raise


if __name__ == '__main__':
    raise SystemExit(main())
