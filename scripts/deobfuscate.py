#!/usr/bin/env python3
"""Explicit guarded-view workflow; no automatic target discovery or Guest execution."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time

sys.dont_write_bytecode = True
U64 = (1 << 64) - 1
SCHEMA = 'dragon-deobfuscate-v1'
REGION_LIMITS = {'max_blocks': 4096, 'max_dispatch_cases': 65536}
LOOP_LIMITS = {'max_blocks': 4096, 'max_instructions': 65536, 'max_work': 1000000}


class Incomplete(RuntimeError):
    def __init__(self, message, status='incomplete'):
        super().__init__(message)
        self.status = status


def uint(value, bits, name):
    if isinstance(value, str) and re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)', value):
        value = int(value, 16 if value.lower().startswith('0x') else 10)
    if type(value) is not int or not 0 <= value < (1 << bits):
        raise ValueError(f'{name} must be an unsigned {bits}-bit integer or decimal/hex string')
    return value


def fields(value, allowed, name):
    if not isinstance(value, dict) or set(value) - set(allowed):
        raise ValueError(f'{name} must be an object with only: {", ".join(allowed)}')
    return value


def text(value, name, maximum):
    if not isinstance(value, str) or '\0' in value or not 1 <= len(value.encode('utf-8')) <= maximum:
        raise ValueError(f'{name} must be nonempty UTF-8 without NUL, at most {maximum} bytes')
    return value


def limits(value, caps, name):
    fields(value, caps, name)
    result = dict(caps)
    for key, number in value.items():
        if type(number) is not int or not 1 <= number <= caps[key]:
            raise ValueError(f'{name}.{key} must be 1..{caps[key]}')
        result[key] = number
    return result


def addresses(value, name, maximum):
    if not isinstance(value, list) or len(value) > maximum:
        raise ValueError(f'{name} must be an array of at most {maximum} addresses')
    return [uint(v, 64, name) for v in value]  # Order and repeated slots are significant.


def byte_rows(value, registers):
    name = 'guard.registers' if registers else 'guard.memory'
    if not isinstance(value, list) or len(value) > 256:
        raise ValueError(f'{name} must be an array of at most 256 byte candidates')
    result, seen = [], {}
    for row in value:
        fields(row, ('offset', 'bytes_hex'), name)
        offset = uint(row.get('offset'), 32 if registers else 64, name + '.offset')
        encoded = row.get('bytes_hex')
        if (not isinstance(encoded, str) or len(encoded) > 8192
                or re.fullmatch(r'(?:[0-9a-fA-F]{2})+', encoded) is None):
            raise ValueError(f'{name}.bytes_hex must contain 1..4096 bytes as contiguous hex pairs')
        data = bytes.fromhex(encoded)
        if registers and len(data) not in (1, 2, 4, 8):
            raise ValueError('register candidates must have exactly 1, 2, 4 or 8 bytes')
        if registers and offset > 0xffffffff - len(data):
            raise ValueError('register byte range overflows')
        if not registers and offset > U64 - len(data):
            raise ValueError('Guest byte range overflows')
        for i, byte in enumerate(data):
            if offset + i in seen and seen[offset + i] != byte:
                raise ValueError(f'{name} has conflicting overlapping candidates')
            seen[offset + i] = byte
        result.append({'offset': offset, 'bytes_hex': data.hex()})
    return result


def validate_config(value, base):
    fields(value, ('schema', 'image', 'image_sha256', 'function_entry', 'entries',
                   'extra_blocks', 'strategy', 'guard', 'table', 'region', 'loop'), 'config')
    if value.get('schema', SCHEMA) != SCHEMA:
        raise ValueError('unsupported config schema')
    image = Path(text(value.get('image'), 'image', 32768))
    if not image.is_absolute():
        image = Path(base) / image
    image = image.resolve()
    expected = value.get('image_sha256')
    if expected is not None and (not isinstance(expected, str)
                                 or re.fullmatch('[0-9a-f]{64}', expected) is None):
        raise ValueError('image_sha256 must be a lowercase SHA256 digest')
    entry = uint(value.get('function_entry'), 64, 'function_entry')
    strategy = value.get('strategy', 'linear-sweep')
    if strategy not in ('linear-sweep', 'recursive-traversal'):
        raise ValueError('strategy must be linear-sweep or recursive-traversal')
    region = limits(value.get('region', {}), REGION_LIMITS, 'region')
    loop = limits(value.get('loop', {}), LOOP_LIMITS, 'loop')
    entries = addresses(value.get('entries', []), 'entries', 65536)
    extra = addresses(value.get('extra_blocks', []), 'extra_blocks', region['max_blocks'])
    guard = fields(value.get('guard'), ('context_id', 'provenance', 'registers', 'memory'), 'guard')
    registers = byte_rows(guard.get('registers', []), True)
    memory = byte_rows(guard.get('memory', []), False)
    if len(registers) + len(memory) > 256 or sum(len(bytes.fromhex(x['bytes_hex']))
                                               for x in registers + memory) > 4096:
        raise ValueError('guard exceeds 256 rows or 4096 total candidate bytes')
    context_id = text(guard.get('context_id'), 'guard.context_id', 64)
    if re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', context_id) is None:
        raise ValueError('guard.context_id must match [A-Za-z0-9_.-]{1,64}')
    guard = {'context_id': context_id,
             'provenance': text(guard.get('provenance'), 'guard.provenance', 4096),
             'registers': registers, 'memory': memory}
    table = fields(value.get('table'), ('offset', 'targets'), 'table')
    offset = uint(table.get('offset'), 64, 'table.offset')
    targets = addresses(table.get('targets'), 'table.targets', 512)
    if not targets or len(targets) * 8 > U64 - offset:
        raise ValueError('table needs 1..512 ordered slots and a nonwrapping byte span')
    return {'schema': SCHEMA, 'image': str(image), 'image_sha256': expected,
            'function_entry': entry, 'entries': entries, 'extra_blocks': extra,
            'strategy': strategy, 'guard': guard, 'table': {'offset': offset, 'targets': targets},
            'region': region, 'loop': loop}


def load_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f'duplicate JSON key: {key}')
            result[key] = value
        return result
    def invalid_constant(token):
        raise ValueError(f'nonfinite JSON number: {token}')
    return json.loads(data, object_pairs_hook=pairs, parse_constant=invalid_constant)


class Output:
    def __init__(self, root):
        self.root = root
        self.artifacts = []

    def bytes(self, name, data):
        with (self.root / name).open('xb') as stream:
            stream.write(data)
        self.artifacts.append({'path': name, 'size': len(data),
                               'sha256': hashlib.sha256(data).hexdigest()})

    def text(self, name, data):
        self.bytes(name, data.encode('utf-8'))

    def json(self, name, data):
        self.text(name, json.dumps(data, indent=2, sort_keys=True, ensure_ascii=False) + '\n')


def single_success(report, name):
    if not isinstance(report, dict) or report.get('ok') is not True:
        raise RuntimeError(f'{name}: pipeline did not return ok')
    rows = report.get('runs')
    if (not isinstance(rows, list) or len(rows) != 1 or not isinstance(rows[0], dict)
            or rows[0].get('name') != name):
        raise RuntimeError(f'{name}: expected exactly one invocation of this pass')
    row = rows[0]
    if type(row.get('status')) is not int:
        raise RuntimeError(f'{name}: invalid pass status')
    if row.get('status') in (1, 2):
        raise Incomplete(row.get('message') or name + ' did not complete',
                         'unavailable' if row['status'] == 1 else 'incomplete')
    if row.get('status') != 0:
        raise RuntimeError(row.get('message') or name + ' failed')
    if report.get('budget_exhausted') is not False:
        raise Incomplete(name + ': scheduler budget exhausted or missing budget status')
    return row


def require_closed(report):
    row = single_success(report, 'loop-image-context')
    counters = row.get('counters')
    if not isinstance(counters, dict):
        raise RuntimeError('loop-image-context: missing counter object')
    for name, expected in (('available', 1), ('complete', 1), ('closed', 1),
                           ('budget_exhausted', 0)):
        if type(counters.get(name)) is not int:
            raise RuntimeError('loop-image-context: missing typed counter ' + name)
        if counters[name] != expected:
            raise Incomplete(f'loop-image-context: {name} does not certify complete closure')
    return row


def pipeline_run(dragon, pipeline, view, name, output, report):
    try:
        result = pipeline.run(view, max_iterations=1, max_pass_runs=1, verify=True)
    except dragon.PipelineError as error:
        result = error.report
        report['pipeline_reports'][name] = result
        output.json(name + '-report.json', result)
        try:
            output.text(name + '-error.ll', view.ir())
        except Exception as diagnostic:
            report.setdefault('diagnostic_errors', []).append(str(diagnostic))
        raise
    report['pipeline_reports'][name] = result
    output.json(name + '-report.json', result)
    return result


def workflow(config, output, report, dragon):
    report['runtime'] = {'python_wrapper': getattr(dragon, '__file__', None),
                         'native_library': getattr(dragon, '_LIB', None),
                         'requested_DRAGON_C_LIB': os.environ.get('DRAGON_C_LIB')}
    report['phase'] = 'loader'
    image_bytes = Path(config['image']).read_bytes()
    image_hash = hashlib.sha256(image_bytes).hexdigest()
    report['image'] = {'path': config['image'], 'size': len(image_bytes), 'sha256': image_hash}
    if config['image_sha256'] is not None and config['image_sha256'] != image_hash:
        raise ValueError('image bytes do not match image_sha256')
    with ExitStack() as stack:
        image = stack.enter_context(dragon.LoadedImage.load_bytes(image_bytes))
        strategy = (dragon.Strategy.LINEAR_SWEEP if config['strategy'] == 'linear-sweep'
                    else dragon.Strategy.RECURSIVE_TRAVERSAL)
        options = stack.enter_context(dragon.Configuration(strategy=strategy))
        # Explicit starts are decode seeds, never a claim of complete CFG coverage.
        seeds = list(dict.fromkeys([config['function_entry']] + config['entries']
                                   + config['extra_blocks']))
        hints = stack.enter_context(dragon.AnalysisHints(entry_points=seeds))
        report['phase'] = 'disassembly'
        graph = stack.enter_context(image.disassemble(config=options, hints=hints))
        report['analysis'] = {'architecture': image.architecture, 'strategy': config['strategy'],
                              'hint_entries': seeds, 'instructions': len(graph.instructions()),
                              'functions': len(graph.function_addresses()),
                              'automatic_complete_target_discovery_claim': False}
        report['phase'] = 'lift'
        function = stack.enter_context(graph.get_function(config['function_entry']))
        if not function.is_valid or function.start_address != config['function_entry']:
            raise ValueError('function_entry is not an analyzed function start')
        missing = [address for address in config['extra_blocks']
                   if graph.get_instruction(address) is None]
        if missing:
            report['analysis']['extra_blocks_not_decoded'] = missing
            raise ValueError('some extra_blocks were not decoded even with explicit hint seeds')
        source = stack.enter_context(dragon.LLVMLifter(image.architecture, module_name='deobfuscate',
            execution_mode=dragon.LLVMExecutionMode.STATIC_ANALYSIS,
            static_entry_policy=dragon.LLVMStaticEntryPolicy.FUNCTION_START))
        if not source.lift_static_region(function, graph, config['extra_blocks'],
                                        rebuild_decoded_blocks=True, **config['region']):
            raise RuntimeError('lift_static_region refused; no retry or larger budget was attempted')
        raw_ir = source.ir()
        output.text('00-raw.ll', raw_ir)
        report['region'] = {'additional_blocks': config['extra_blocks'], **config['region'],
                            'rebuild_decoded_blocks': True, 'coverage_complete_claim': False}
        report['phase'] = 'guard'
        registers = [(r['offset'], len(bytes.fromhex(r['bytes_hex'])) * 8,
                      int.from_bytes(bytes.fromhex(r['bytes_hex']), 'little'))
                     for r in config['guard']['registers']]
        memory = [(r['offset'], bytes.fromhex(r['bytes_hex'])) for r in config['guard']['memory']]
        candidates = dragon.FiniteEntryState(config['function_entry'], registers=registers, memory=memory)
        view = stack.enter_context(source.guarded_entry_view(candidates,
            context_id=config['guard']['context_id'], provenance=config['guard']['provenance']))
        manifest = view.guarded_entry_manifest()
        symbol = manifest.get('view_symbol')
        if not isinstance(symbol, str) or not symbol or '\0' in symbol:
            raise RuntimeError('guarded view manifest has no valid view_symbol')
        report['guarded_entry_manifest'] = manifest
        output.json('guarded-entry-manifest.json', manifest)
        output.text('01-guarded.ll', view.ir())
        report['phase'] = 'proof'
        with dragon.PassPipeline() as pipeline:
            pipeline.add_loop_image_context(symbol, config['table']['offset'],
                                            config['table']['targets'], **config['loop'])
            proof = pipeline_run(dragon, pipeline, view, 'loop-image-context', output, report)
        output.text('02-loop.ll', view.ir())
        require_closed(proof)  # ok/converged/changed alone never authorize cleanup.
        report['closed'] = True
        report['phase'] = 'cleanup'
        with dragon.PassPipeline() as pipeline:
            pipeline.add('function-control-flow', function=symbol)
            cleanup = pipeline_run(dragon, pipeline, view, 'function-control-flow', output, report)
        output.text('03-clean.ll', view.ir())
        single_success(cleanup, 'function-control-flow')
        if source.ir() != raw_ir:
            raise RuntimeError('independent source lifter was unexpectedly modified')
        report.update(cleanup_completed=True, selected_view_symbol=symbol,
                      source_lifter_unchanged=True, phase='complete', ok=True,
                      status='proof-closed-cleanup-completed')


def self_test():
    import copy
    base = {'image': 'not-opened.elf', 'function_entry': '0x1000',
            'guard': {'context_id': 'example', 'provenance': 'explicit test candidates',
                      'registers': [{'offset': 32, 'bytes_hex': '00f0000000000000'}], 'memory': []},
            'table': {'offset': '0x800', 'targets': ['0x1010', U64, '0x1010']}}
    value = validate_config(base, Path('/tmp'))
    assert value['table']['targets'] == [0x1010, U64, 0x1010]
    assert int.from_bytes(bytes.fromhex(value['guard']['registers'][0]['bytes_hex']), 'little') == 0xf000
    boundary = copy.deepcopy(base)
    boundary['guard']['context_id'] = 'A_0.-'
    boundary['guard']['memory'] = [{'offset': U64 - 1, 'bytes_hex': 'ab'}]
    assert validate_config(boundary, Path('/tmp'))['guard']['memory'][0]['offset'] == U64 - 1
    rejected = 0
    for change in (
        lambda c: c.update(function_entry=True), lambda c: c.update(unknown=1),
        lambda c: c['table'].update(targets=[]),
        lambda c: c['table'].update(targets=[1 << 64]),
        lambda c: c['table'].update(targets=[True]),
        lambda c: c['table'].update(offset=U64 - 7, targets=[1]),
        lambda c: c.update(loop={'max_work': 1000001}),
        lambda c: c.update(region={'max_blocks': 0}),
        lambda c: c['guard']['registers'][0].update(bytes_hex='000000'),
        lambda c: c['guard']['registers'][0].update(bytes_hex='0x00'),
        lambda c: c['guard']['memory'].append({'offset': U64, 'bytes_hex': '0011'}),
        lambda c: c['guard']['memory'].append({'offset': U64, 'bytes_hex': 'ab'}),
        lambda c: c['guard'].update(context_id='has space'),
        lambda c: c['guard'].update(context_id='上下文'),
        lambda c: c['guard']['registers'].append({'offset': 32, 'bytes_hex': 'ff'}),
    ):
        bad = copy.deepcopy(base)
        change(bad)
        try:
            validate_config(bad, Path('/tmp'))
        except ValueError:
            rejected += 1
        else:
            raise AssertionError('invalid configuration accepted')
    try:
        load_json('{"image":"one","image":"two"}')
    except ValueError:
        rejected += 1
    else:
        raise AssertionError('duplicate JSON key accepted')
    good = {'ok': True, 'budget_exhausted': False, 'runs': [
        {'name': 'loop-image-context', 'status': 0, 'changed': False,
         'counters': {'available': 1, 'complete': 1, 'closed': 1, 'budget_exhausted': 0}}]}
    require_closed(good)
    blocked = 0
    for key, val in [('closed', 0), ('available', 0), ('complete', 0), ('budget_exhausted', 1)]:
        bad = copy.deepcopy(good)
        bad['runs'][0]['counters'][key] = val
        try:
            require_closed(bad)
        except Incomplete:
            blocked += 1
        else:
            raise AssertionError('unproved report authorized cleanup')
    for status in (1, 2):
        bad = copy.deepcopy(good)
        bad['runs'][0]['status'] = status
        try:
            require_closed(bad)
        except Incomplete:
            blocked += 1
        else:
            raise AssertionError('unavailable/incomplete report authorized cleanup')
    import builtins
    from contextlib import redirect_stderr, redirect_stdout
    import io
    import tempfile
    old_import = builtins.__import__
    def no_dragon(name, *args, **kwargs):
        if name == 'dragon':
            raise AssertionError('pure config-only test imported Dragon')
        return old_import(name, *args, **kwargs)
    cli_controls = 0
    try:
        builtins.__import__ = no_dragon
        with tempfile.TemporaryDirectory(prefix='dragon-config-selftest-') as directory:
            root = Path(directory)
            config_file = root / 'job.json'
            config_file.write_text(json.dumps(base))
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                assert main(['--config', str(config_file), '--out', str(root / 'valid'),
                             '--validate-config-only']) == 0
            result = json.loads((root / 'valid/report.json').read_text())
            assert result['status'] == 'config-valid-only' and not result['closed']
            assert not result['cleanup_completed'] and result['Dragon_imported'] is False
            assert not (root / 'valid/00-raw.ll').exists()
            cli_controls += 1
            before = {p.name: p.read_bytes() for p in (root / 'valid').iterdir()}
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                assert main(['--config', str(config_file), '--out', str(root / 'valid'),
                             '--validate-config-only']) == 2
            assert before == {p.name: p.read_bytes() for p in (root / 'valid').iterdir()}
            cli_controls += 1
            config_file.write_text('{"image":true}')
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                assert main(['--config', str(config_file), '--out', str(root / 'invalid'),
                             '--validate-config-only']) == 2
            failed = json.loads((root / 'invalid/report.json').read_text())
            assert failed['phase'] == 'config' and failed['status'] == 'error'
            assert (root / 'invalid/config-input.json').read_text() == '{"image":true}'
            cli_controls += 1
    finally:
        builtins.__import__ = old_import
    assert rejected == 16 and blocked == 6
    print(json.dumps({'self_test': True, 'config_rejections': rejected,
                      'unsafe_cleanup_authorizations_blocked': blocked,
                      'ordered_u64_duplicate_targets_preserved': True,
                      'CLI_config_only_output_controls': cli_controls,
                      'max_legal_Guest_byte_boundary_accepted': True,
                      'Dragon_imports': 0, 'Guest_runs': 0}))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--validate-config-only', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        if args.config or args.out or args.validate_config_only:
            parser.error('--self-test cannot be combined with workflow arguments')
        return self_test()
    if args.config is None or args.out is None:
        parser.error('--config and --out are required')
    out = Path(os.path.abspath(args.out))
    try:
        out.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f'cannot create new output directory: {error}', file=sys.stderr)
        return 2
    output = Output(out)
    report = {'schema': 'dragon-deobfuscate-report-v1', 'ok': False, 'status': 'error',
              'phase': 'config', 'closed': False, 'cleanup_completed': False,
              'semantic_validation': False, 'deobfuscation_complete_claim': False,
              'Guest_runs': 0, 'pipeline_reports': {}}
    started = time.monotonic()
    code = 2
    try:
        data = args.config.read_bytes()
        report['config_sha256'] = hashlib.sha256(data).hexdigest()
        output.bytes('config-input.json', data)
        config = validate_config(load_json(data), args.config.resolve().parent)
        report['config'] = config
        output.json('config-normalized.json', config)
        if args.validate_config_only:
            report.update(ok=True, status='config-valid-only', phase='complete',
                          image_opened=False, Dragon_imported=False)
        else:
            report['phase'] = 'loader'
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
            import dragon
            workflow(config, output, report, dragon)
        code = 0
    except Incomplete as error:
        report.update(status=error.status, error={'type': type(error).__name__, 'message': str(error)})
        code = 3
    except Exception as error:
        report.update(status='error', error={'type': type(error).__name__, 'message': str(error)})
        if hasattr(error, 'report'):
            report['error_pipeline_report'] = error.report
    finally:
        report['seconds'] = time.monotonic() - started
        report['artifacts'] = output.artifacts.copy()
        output.json('report.json', report)
    print(json.dumps({'ok': report['ok'], 'status': report['status'], 'phase': report['phase'],
                      'report': str(out / 'report.json')}, sort_keys=True))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
