#!/usr/bin/env python3
"""Strict status/IR oracle; no new CPU semantic-execution claim."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

NAMES = ('zero', 'switch_short', 'switch_exact', 'switch_spare', 'branch_short',
         'branch_exact', 'literal_exact', 'unknown_exact', 'unknown_branch',
         'unsupported_root', 'flag_exact', 'flag_short', 'undef_exact', 'shared_short',
         'shared_exact', 'unknown_deep', 'cycle_exact', 'cycle_short', 'default_short',
         'default_exact', 'mixed_continue')
LIMITED = {'switch_short', 'branch_short', 'flag_short', 'shared_short',
           'unknown_deep', 'cycle_short', 'default_short', 'mixed_continue'}
PROVED = {'switch_exact', 'switch_spare', 'branch_exact', 'literal_exact',
          'shared_exact', 'default_exact'}
BRANCHES = {'branch_short', 'branch_exact', 'unknown_branch', 'unsupported_root'}
FIELDS = {'available', 'complete', 'changed', 'visited', 'unique', 'switches',
          'branches_rewritten', 'switches_rewritten', 'unresolved', 'budget', 'invalid'}

def wanted(name, fixed):
    branch = name in BRANCHES
    proves = name in PROVED
    row = dict(available=1, complete=int(not (fixed and name in LIMITED)), changed=int(proves),
               visited=1, unique=int(proves), switches=int(not branch),
               branches_rewritten=int(branch and proves), switches_rewritten=int(not branch and proves),
               unresolved=int(not branch and not proves), budget=int(fixed and name in LIMITED), invalid=0)
    if name == 'zero':
        row.update(complete=0, changed=0, visited=0, unique=0, switches=0,
                   branches_rewritten=0, switches_rewritten=0, unresolved=0, budget=1)
    if name == 'mixed_continue':
        row.update(changed=1, visited=3, unique=1, switches=2, branches_rewritten=1, unresolved=2)
    return row

def load(root):
    records = {}
    for line in (root / 'stats.txt').read_text().splitlines():
        row = dict(field.split('=', 1) for field in line.split())
        name = row.pop('case')
        assert name in NAMES and name not in records, 'bad/duplicate fixture'
        assert set(row) == FIELDS, 'wrong record fields'
        records[name] = {k: int(v) for k, v in row.items()}
    assert set(records) == set(NAMES), 'incomplete fixture set'
    assert {p.name for p in root.glob('*.ll')} == {n + '-' + s + '.ll' for n in NAMES for s in ('before','after')}, 'wrong IR file set'
    return records

def validate(records, fixed):
    for name in NAMES:
        assert records[name] == wanted(name, fixed), (name, records[name], wanted(name, fixed))

def same_ir(left, right):
    assert left == right, "old/new IR bytes changed"

def pin(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--old',type=Path,required=True)
    p.add_argument('--new',type=Path)
    p.add_argument('--require-fixed',action='store_true')
    a=p.parse_args()
    old=load(a.old);validate(old,False)
    result={'ok':True,'cases':len(NAMES),'old_intended_status_failures':sorted(LIMITED),
            'old_stats_sha256':pin(a.old/'stats.txt'),'new_cpu_executions':0}
    if a.new:
        new=load(a.new);validate(new,True)
        equality={}
        for name in NAMES:
            for stage in ('before','after'):
                filename=name+'-'+stage+'.ll'
                same_ir((a.old/filename).read_bytes(), (a.new/filename).read_bytes())
                equality[filename]=pin(a.old/filename)
            left,right=dict(old[name]),dict(new[name])
            for field in ('complete','budget'):left.pop(field);right.pop(field)
            assert left==right, 'non-status behavior changed'
        # Fault controls independently corrupt the status records and valid IR
        # bytes; they do not modify any frozen producer output on disk.
        status_controls=0
        for name in NAMES:
            for field in ('complete','budget','changed'):
                damaged=copy.deepcopy(new);damaged[name][field]^=1
                try:validate(damaged,True)
                except AssertionError:status_controls+=1
                else:raise AssertionError('bad status escaped')
        ir_controls=0
        for name in NAMES:
            before=(a.old/(name+'-after.ll')).read_bytes()
            assert b'ret i64 22' in before
            damaged=before.replace(b'ret i64 22',b'ret i64 23',1)
            try:same_ir(before,damaged)
            except AssertionError:ir_controls+=1
            else:raise AssertionError('damaged IR escaped comparator')
        result.update(new_stats_sha256=pin(a.new/'stats.txt'),identical_ir=equality,
                      status_controls=status_controls,ir_controls=ir_controls,
                      corrected_status_cases=len(LIMITED),nonstatus_fields_equal=True)
    elif a.require_fixed:
        try:validate(old,True)
        except AssertionError as error:
            result['ok']=False
            result['reason']='Fixed-contract validation failed: '+str(error)
        else:
            result['fixed_contract_passed']=True
    print(json.dumps(result,sort_keys=True))
    return 0 if result['ok'] else 1

if __name__=='__main__':raise SystemExit(main())
