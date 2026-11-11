#!/usr/bin/env python3
"""Guarded entry view around the unchanged four-ELF full-state oracle.

Only the wrapper's one direct kernel call is rebound in exported IR. Original
roots, recursive calls, inputs, oracle comparisons and pass limits stay intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

import bounded_memory_pipeline_real as original
from guest_edge_worklist_real import validate_activation


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def function_span(ir, symbol):
    hits = list(re.finditer(r'^define void @' + re.escape(symbol) +
                           r'\([^\n]*\).*?^}', ir, re.M | re.S))
    if len(hits) != 1:
        raise RuntimeError('expected exactly one defined function: ' + symbol)
    return hits[0]


def bind_wrapper(ir, root, view, entry):
    """Change a single call token; reject ambiguous or unproven adapters."""
    old, new = function_span(ir, root), function_span(ir, view)
    for match in (old, new):
        args = match[0].split('\n', 1)[0].split('(', 1)[1].split(')', 1)[0]
        if len(args.split(',')) != 4 or any(not arg.strip().startswith('ptr ') for arg in args.split(',')):
            raise RuntimeError('root/view does not use the four-pointer void ABI')
    wrapper = function_span(ir, 'dragon_static_entry_x86_64_1000')
    calls = list(re.finditer(r'^.*\bcall void @' + re.escape(root) + r'\([^\n]*$', wrapper[0], re.M))
    if len(calls) != 1:
        raise RuntimeError('wrapper must contain exactly one direct kernel call')
    call = calls[0]
    metadata = re.search(r'!dragon\.guest\.call\.target !(\d+)\b', call[0])
    if not metadata or not re.search(r'^!' + metadata[1] + r' = !\{i64 ' + str(entry) + r'\}$', ir, re.M):
        raise RuntimeError('wrapper call metadata does not match the requested kernel')
    token = '@' + root + '('
    at = wrapper.start() + call.start() + call[0].index(token)
    result = ir[:at] + '@' + view + '(' + ir[at + len(token):]
    if result.count('@' + root + '(') != ir.count('@' + root + '(') - 1:
        raise RuntimeError('binding changed unexpected root uses')
    return result, dict(root_symbol=root, view_symbol=view, entry=entry,
                        changed_call_count=1, raw_sha256=sha(ir), bound_sha256=sha(result),
                        original_call=call[0], bound_call=call[0].replace(token, '@' + view + '(', 1))


def self_test():
    root, view = 'kernel', 'kernel.guarded.ctx'
    ir = ('define void @kernel(ptr %s, ptr %m, ptr %c, ptr %e) {\n'
          '  call void @kernel(ptr %s, ptr %m, ptr %c, ptr %e)\n  ret void\n}\n'
          'define void @kernel.guarded.ctx(ptr %s, ptr %m, ptr %c, ptr %e) {\n'
          '  call void @kernel(ptr %s, ptr %m, ptr %c, ptr %e)\n  ret void\n}\n'
          'define void @dragon_static_entry_x86_64_1000(ptr %s, ptr %m, ptr %c, ptr %e) {\n'
          '  call void @kernel(ptr %s, ptr %m, ptr %c, ptr %e), !dragon.guest.call.target !0\n'
          '  ret void\n}\n!0 = !{i64 66208}\n')
    bound, receipt = bind_wrapper(ir, root, view, 66208)
    assert function_span(bound, root)[0] == function_span(ir, root)[0]
    assert function_span(bound, view)[0] == function_span(ir, view)[0]
    assert receipt['changed_call_count'] == 1
    bad = [ir.replace('!{i64 66208}', '!{i64 1}'),
           ir.replace('define void @kernel.guarded.ctx', 'define void @absent'),
           ir.replace(receipt['original_call'], receipt['original_call'] + '\n' + receipt['original_call']),
           ir.replace('!dragon.guest.call.target', '!untrusted'),
           ir.replace('@dragon_static_entry_x86_64_1000(', '@other_wrapper('), bound]
    rejected = 0
    for value in bad:
        try:
            bind_wrapper(value, root, view, 66208)
        except RuntimeError:
            rejected += 1
        else:
            raise RuntimeError('binding control incorrectly accepted')
    return dict(ok=True, unchanged_recursive_and_fallback_calls=True,
                rejected_controls=rejected, native_executions=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--facts', type=Path)
    parser.add_argument('--source-pins', type=Path, help='previous PREDECLARED.json with before IR hashes')
    parser.add_argument('--recipe', choices=('sp', 'sp-output'))
    parser.add_argument('--frozen', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--tigress')
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(self_test(), indent=2)); return 0
    if any(getattr(args, key) is None for key in ('facts', 'source_pins', 'recipe', 'frozen', 'out', 'tigress')):
        parser.error('--facts --source-pins --recipe --frozen --out --tigress required')
    import dragon as d
    facts = json.loads(args.facts.read_text())
    pins = json.loads(args.source_pins.read_text())['baseline_indices']
    records, activation, live = [], [], []
    saved_pipeline, saved_ir, saved_argv = d.PassPipeline, d.LLVMLifter.ir, sys.argv
    redirects = {}
    status = None

    def exported(target):
        state = redirects.get(target)
        if state is None:
            return saved_ir(target)
        raw = saved_ir(state['view'])
        result, receipt = bind_wrapper(raw, state['root'], state['symbol'], state['entry'])
        state['exports'].append(receipt)
        return result

    class GuardedPipeline:
        def __init__(self, *a, **kw):
            if a or kw: raise RuntimeError('unexpected pipeline construction')
            self.inner, self.steps = saved_pipeline(), []
        def __enter__(self):
            self.inner.__enter__(); return self
        def __exit__(self, *a): return self.inner.__exit__(*a)
        def register(self, name, callback, **options):
            if name != 'bounded-memory-checkpoint' or options != {'read_only': True}:
                raise RuntimeError('unexpected actual callback')
            def checkpoint(context, settings):
                state = redirects[context.target]
                raw = saved_ir(context.target)
                bound, receipt = bind_wrapper(raw, state['root'], state['symbol'], state['entry'])
                n = len(state['rounds']) + 1
                (state['directory'] / f'round-{n}-raw.ll').write_text(raw)
                (state['directory'] / f'round-{n}-bound.ll').write_text(bound)
                state['rounds'].append(receipt)
                return callback(context, settings)
            self.inner.register(name, checkpoint, **options); return self
        def add(self, name, **options):
            if name == 'guest-memory':
                if options != {'run_mem2reg': 0}: raise RuntimeError('unexpected memory options')
                options = dict(options, edge_worklist=1)
            self.steps.append(dict(name=name, options=options))
            self.inner.add(name, **options); return self
        def run(self, source, **options):
            active = dict(steps=self.steps, run_options=options)
            validate_activation([active], True, expected_count=1); activation.append(active)
            before = saved_ir(source)
            matches = [case for case in facts['cases'] if pins[case['kernel']['PC_hex']]['before_sha256'] == sha(before)]
            if len(matches) != 1: raise RuntimeError('source IR differs from frozen raw module')
            case = matches[0]; entry = case['kernel']['PC']; root = case['kernel']['registered_symbol']
            table = case['CFF_table']; data = bytes.fromhex(table['bytes_hex'])
            if hashlib.sha256(data).hexdigest() != table['bytes_sha256'] or len(data) != 408:
                raise RuntimeError('table fact bytes changed')
            registers = [(32, 64, case['first_kernel_entry']['RSP'])]
            if args.recipe == 'sp-output': registers.append((48, 64, facts['oracle_entry']['RSI']))
            context_id = 'real_' + args.recipe.replace('-', '_')
            view = source.guarded_entry_view(d.FiniteEntryState(entry, registers=registers,
                memory=[(table['base'], data)]), context_id=context_id,
                provenance='Frozen ELF ' + case['ELF_sha256'] + '; runtime checked bytes; initial wrapper invocation')
            live.append(view)
            manifest = view.guarded_entry_manifest()
            symbol = root + '.guarded.' + context_id
            expected_registers = [dict(offset=hex(offset), bytes=value.to_bytes(width // 8, 'little').hex())
                                  for offset, width, value in registers]
            if (manifest.get('root_symbol') != root or manifest.get('view_symbol') != symbol or
                    manifest.get('entry') != hex(entry) or manifest.get('check_kind') != 'runtime-byte-equality' or
                    manifest.get('guarded') is not True or manifest.get('context_id') != context_id or
                    manifest.get('registers') != expected_registers or
                    manifest.get('memory') != [dict(offset=hex(table['base']), bytes=data.hex())] or
                    manifest.get('minimum_guest_bytes') != str(table['base'] + len(data)) or
                    manifest.get('minimum_register_bytes') != str(max(offset + width // 8 for offset, width, _ in registers))):
                raise RuntimeError('guarded manifest identity or requested checks changed')
            directory = args.out / 'guarded-views' / case['profile'] / case['level']
            directory.mkdir(parents=True, exist_ok=False)
            state = dict(view=view, root=root, symbol=symbol, entry=entry, directory=directory, exports=[], rounds=[])
            redirects[source] = redirects[view] = state
            raw = saved_ir(view); bound, binding = bind_wrapper(raw, root, symbol, entry)
            (directory / 'before-raw.ll').write_text(raw)
            (directory / 'before-bound.ll').write_text(bound)
            original.save(directory / 'manifest.json', manifest)
            report = self.inner.run(view, **options)
            source_after = saved_ir(source)
            if source_after != before: raise RuntimeError('guarded optimization mutated source module')
            raw = saved_ir(view); bound, final = bind_wrapper(raw, root, symbol, entry)
            (directory / 'after-raw.ll').write_text(raw)
            (directory / 'after-bound.ll').write_text(bound)
            records.append(dict(entry=entry, profile=case['profile'], level=case['level'],
                source_before_sha256=sha(before), source_after_sha256=sha(source_after),
                manifest=manifest, initial_binding=binding, final_binding=final,
                rounds=state['rounds'], exports=state['exports'], directory=str(directory)))
            return report
    try:
        d.PassPipeline, d.LLVMLifter.ir = GuardedPipeline, exported
        sys.argv = [original.__file__, '--frozen', str(args.frozen), '--out', str(args.out),
                    '--tigress', args.tigress, '--memory-mode', 'builtin', '--rounds', '3']
        status = original.main()
        validate_activation(activation, True)
        if len(records) != 4: raise RuntimeError('missing guarded modules')
    finally:
        d.PassPipeline, d.LLVMLifter.ir, sys.argv = saved_pipeline, saved_ir, saved_argv
        if args.out.exists():
            original.save(args.out / 'guarded-entry-activation.json', dict(recipe=args.recipe,
                status=status, records=records, activation=activation, facts_sha256=original.digest(args.facts),
                source_pins_sha256=original.digest(args.source_pins), driver_sha256=original.digest(__file__),
                no_production_symbol_renaming=True, binding='exported wrapper direct kernel call only',
                unmodified_oracle_sha256=original.digest(original.__file__)))
        for view in live: view.__exit__()
    return status


if __name__ == '__main__':
    raise SystemExit(main())
