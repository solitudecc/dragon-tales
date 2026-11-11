#!/usr/bin/env python3
"""One explicit entry-prefix pass before the frozen guarded SP real recipe.

The old guarded_entry_real.py and its C/LLVM oracle are imported unchanged.
The production builtin and budgets are explicit; unsupported paths retain their IR.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys

import guarded_entry_real as guarded
from guest_edge_worklist_real import validate_activation

FROZEN_GUARDED_SHA256 = '812c7862616804db32a40ace8984da38634362017a64f172180d752443239b0e'
PREFIX_INTERFACE_CONFIRMED = True
PREFIX_NAME = 'entry-prefix'
PREFIX_OPTIONS = dict(max_blocks=32, max_instructions=2048)
PREFIX_RUN_OPTIONS = dict(max_iterations=1, max_pass_runs=1)
CASE_NAMES = {0x102a0: ('f-cff', 'O0'), 0x10080: ('f-cff', 'O2'),
              0x10770: ('f-combo', 'O0'), 0x10090: ('f-combo', 'O2')}


def digest(data):
    return hashlib.sha256(data.encode()).hexdigest()


def validate_prefix_report(report, before, after):
    if report.get('ok') is not True or type(report.get('iterations')) is not int or report['iterations'] != 1:
        raise RuntimeError('entry-prefix did not execute exactly one iteration')
    if len(report.get('runs', [])) != 1:
        raise RuntimeError('entry-prefix must have exactly one run record')
    run = report['runs'][0]
    if (run.get('name') != PREFIX_NAME or type(run.get('iteration')) is not int or
            run['iteration'] != 1 or type(run.get('changed')) is not bool or
            type(run.get('status')) is not int or run['status'] not in (0, 1, 2)):
        raise RuntimeError('malformed entry-prefix run')
    if run['changed'] != (before != after):
        raise RuntimeError('entry-prefix mutation report disagrees with exact IR bytes')
    if run['status'] == 1 and before != after:
        raise RuntimeError('unavailable entry-prefix mutated its input')
    counters = run.get('counters', {})
    required = {'available','complete','changed','candidate_edges','peeled_edges','cloned_blocks',
                'cloned_instructions','original_exits','rejected','budget_exhausted',
                'skipped_single_entry','internal_edges','backedges','terminal_edges'}
    if set(counters) != required or any(type(value) is not int or value < 0 for value in counters.values()):
        raise RuntimeError('entry-prefix counters differ from the pinned interface')
    expected_status = 1 if not counters['available'] else 0 if counters['complete'] else 2
    if run['status'] != expected_status or counters['changed'] != int(run['changed']):
        raise RuntimeError('entry-prefix status/counters disagree')
    normal_skip = (run['status'] == 0 and not run['changed'] and not counters['rejected'] and
                   not counters['budget_exhausted'] and
                   (counters['candidate_edges'] == 0 or counters['skipped_single_entry'] > 0))
    if before == after and not normal_skip and not run.get('message'):
        raise RuntimeError('rejected entry-prefix must explain its unsupported condition')
    return dict(applied=run['changed'], unsupported_or_no_change=not run['changed'],
                native_status=run['status'], native_message=run.get('message', ''))


def ordinary_functions(ir, guarded_symbol):
    found = list(re.finditer(r'^define [^\n]*@([^\s(]+)\([^\n]*\).*?^}', ir, re.M | re.S))
    result = {match[1]: match[0] for match in found if match[1] != guarded_symbol}
    if len(result) != len(found) - 1:
        raise RuntimeError('guarded module identity is ambiguous')
    return result


def self_test():
    counters={key:0 for key in ('available','complete','changed','candidate_edges','peeled_edges',
        'cloned_blocks','cloned_instructions','original_exits','rejected','budget_exhausted',
        'skipped_single_entry','internal_edges','backedges','terminal_edges')}
    counters.update(available=1,complete=1,changed=1,candidate_edges=1,peeled_edges=1,cloned_blocks=21,cloned_instructions=748)
    positive = dict(ok=True, iterations=1, runs=[dict(name=PREFIX_NAME, iteration=1,
                    changed=True, status=0, message='', counters=counters)])
    assert validate_prefix_report(positive, 'before', 'after')['applied']
    rejected = copy.deepcopy(positive)
    rejected['runs'][0].update(changed=False, status=2, message='unsupported live-out')
    rejected['runs'][0]['counters'].update(changed=0,complete=0,peeled_edges=0,cloned_blocks=0,cloned_instructions=0,rejected=1)
    assert validate_prefix_report(rejected, 'before', 'before')['unsupported_or_no_change']
    skipped=copy.deepcopy(rejected)
    skipped['runs'][0].update(status=0,message='')
    skipped['runs'][0]['counters'].update(complete=1,rejected=0,skipped_single_entry=1)
    assert validate_prefix_report(skipped,'before','before')['unsupported_or_no_change']
    bad = []
    x=copy.deepcopy(positive);x['runs'].append(x['runs'][0]);bad.append((x,'before','after'))
    x=copy.deepcopy(positive);x['iterations']=2;bad.append((x,'before','after'))
    x=copy.deepcopy(positive);x['runs'][0]['name']='dispatch-specialize';bad.append((x,'before','after'))
    x=copy.deepcopy(positive);x['runs'][0]['changed']=False;bad.append((x,'before','after'))
    x=copy.deepcopy(positive);x['runs'][0]['status']=1;bad.append((x,'before','after'))
    x=copy.deepcopy(rejected);x['runs'][0]['message']='';bad.append((x,'before','before'))
    x=copy.deepcopy(positive);x['runs'][0]['iteration']=True;bad.append((x,'before','after'))
    x=copy.deepcopy(positive);x['runs'][0]['counters'].pop('terminal_edges');bad.append((x,'before','after'))
    x=copy.deepcopy(positive);x['runs'][0]['counters']['available']=0;bad.append((x,'before','after'))
    for report, before, after in bad:
        try: validate_prefix_report(report,before,after)
        except RuntimeError: continue
        raise RuntimeError('prefix report control accepted')
    return dict(ok=True, positives=3, rejected_controls=len(bad), native_runs=0,
                prefix_interface_confirmed=PREFIX_INTERFACE_CONFIRMED)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test',action='store_true')
    for name in ('facts','source-pins','frozen','out'):
        parser.add_argument('--'+name,type=Path)
    parser.add_argument('--tigress')
    args=parser.parse_args()
    if args.self_test:
        print(json.dumps(self_test(),indent=2));return 0
    if not PREFIX_INTERFACE_CONFIRMED:
        parser.error('production entry-prefix interface has not been pinned; native execution disabled')
    if any(getattr(args,key) is None for key in ('facts','source_pins','frozen','out','tigress')):
        parser.error('--facts --source-pins --frozen --out --tigress required')
    if hashlib.sha256(Path(guarded.__file__).read_bytes()).hexdigest()!=FROZEN_GUARDED_SHA256:
        raise RuntimeError('frozen previous real driver changed')
    import dragon as d
    saved_pipeline,saved_ir,saved_argv=d.PassPipeline,d.LLVMLifter.ir,sys.argv
    records=[];seen=[];status=None

    class PrefixBeforeLegacy:
        def __init__(self,*a,**kw):
            if a or kw:raise RuntimeError('unexpected actual pipeline construction')
            self.inner=saved_pipeline();self.steps=[]
        def __enter__(self):self.inner.__enter__();return self
        def __exit__(self,*a):return self.inner.__exit__(*a)
        def register(self,*a,**kw):self.inner.register(*a,**kw);return self
        def add(self,name,**options):
            self.steps.append(dict(name=name,options=options));self.inner.add(name,**options);return self
        def run(self,view,**options):
            validate_activation([dict(steps=self.steps,run_options=options)],True,expected_count=1)
            if any(view is old for old in seen):raise RuntimeError('entry-prefix would run twice on one view')
            seen.append(view)
            manifest=view.guarded_entry_manifest();entry=int(manifest['entry'],16)
            if (manifest.get('guarded') is not True or manifest.get('context_id')!='real_sp' or
                    manifest.get('registers')!=[dict(offset='0x20',bytes='f8c7000000000000')] or entry not in CASE_NAMES):
                raise RuntimeError('prefix target is not the frozen SP-only guarded view')
            profile,level=CASE_NAMES[entry]
            directory=args.out/'entry-prefix-views'/profile/level;directory.mkdir(parents=True,exist_ok=False)
            before=saved_ir(view);before_bound=view.ir()
            baseline=args.out/'guarded-views'/profile/level
            if (baseline/'before-raw.ll').read_text()!=before or (baseline/'before-bound.ll').read_text()!=before_bound:
                raise RuntimeError('prefix did not run immediately after raw view creation')
            # Reuse the already saved exact before snapshots; no duplicate large files.
            receipt=dict(entry=entry,view_symbol=manifest['view_symbol'],prefix_options=PREFIX_OPTIONS,
                prefix_run_options=PREFIX_RUN_OPTIONS,before_raw=str(baseline/'before-raw.ll'),
                before_bound=str(baseline/'before-bound.ll'),before_raw_sha256=digest(before),
                before_bound_sha256=digest(before_bound),state='started')
            guarded.original.save(directory/'prefix-report.json',receipt)
            with saved_pipeline() as prefix:
                prefix.add(PREFIX_NAME,**PREFIX_OPTIONS)
                report=prefix.run(view,**PREFIX_RUN_OPTIONS)
            after=saved_ir(view);after_bound=view.ir()
            (directory/'after-raw.ll').write_text(after);(directory/'after-bound.ll').write_text(after_bound)
            outcome=validate_prefix_report(report,before,after)
            if ordinary_functions(before,manifest['view_symbol'])!=ordinary_functions(after,manifest['view_symbol']):
                raise RuntimeError('prefix modified an ordinary root or recursive callee')
            receipt.update(state='prefix-finished',report=report,outcome=outcome,
                after_raw_sha256=digest(after),after_bound_sha256=digest(after_bound),ordinary_roots_unchanged=True)
            guarded.original.save(directory/'prefix-report.json',receipt);records.append(receipt)
            legacy=self.inner.run(view,**options)
            receipt.update(state='legacy-finished',legacy_report=legacy,
                actual_total_pass_runs=1+len(legacy['runs']),actual_prefix_runs=1,
                actual_legacy_iterations=legacy['iterations'])
            guarded.original.save(directory/'prefix-report.json',receipt)
            return legacy
    try:
        d.PassPipeline=PrefixBeforeLegacy
        sys.argv=[guarded.__file__,'--facts',str(args.facts),'--source-pins',str(args.source_pins),
            '--recipe','sp','--frozen',str(args.frozen),'--out',str(args.out),'--tigress',args.tigress]
        status=guarded.main()
        if len(records)!=4:raise RuntimeError('missing entry-prefix reports')
    finally:
        d.PassPipeline,sys.argv=saved_pipeline,saved_argv
        if args.out.exists():
            guarded.original.save(args.out/'entry-prefix-activation.json',dict(status=status,records=records,
                driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                frozen_guarded_driver_sha256=FROZEN_GUARDED_SHA256,
                recipe='one explicit entry-prefix then unchanged SP-only three-round bounded pipeline',
                prefix_name=PREFIX_NAME,prefix_options=PREFIX_OPTIONS,production_interface_confirmed=PREFIX_INTERFACE_CONFIRMED))
    return status


if __name__=='__main__':
    raise SystemExit(main())
