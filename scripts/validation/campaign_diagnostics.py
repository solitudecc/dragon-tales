"""Optional diagnostics and exact coverage accounting for Tigress campaigns.

An unavailable symbolic diagnostic never substitutes for a semantic comparison.
Failures from an available implementation are allowed to propagate to the caller.
This module has no Dragon, Unicorn or Z3 import dependency.
"""
from collections import Counter


def branch_diagnostic(library, graph, blocks, run_branches):
    """Probe the optional C symbol before invoking the existing branch oracle."""
    try:
        library.dragon_symexec_create
    except AttributeError:
        return dict(status='unavailable', available=False,
                    reason='dragon_symexec_create is unavailable',
                    branches_requested=len(blocks), branches_decided_single_edge=None,
                    branches_both_edges=None, branches_symbolic_error=None)
    edges = run_branches(graph, blocks)
    if (set(edges) != set(blocks) or any(type(key) is not int for key in edges)
            or any(v is not None and (type(v) is not int or v not in (1, 2)) for v in edges.values())):
        raise ValueError('branch diagnostic returned an incomplete or invalid site map')
    errors = sum(v is None for v in edges.values())
    return dict(status='incomplete' if errors else 'complete', available=True,
                branches_requested=len(blocks),
                branches_decided_single_edge=sum(v == 1 for v in edges.values()),
                branches_both_edges=sum(v == 2 for v in edges.values()),
                branches_symbolic_error=errors)


def _coverage(records, fields, expected):
    actual, invalid = Counter(), 0
    for row in records:
        key = tuple(row.get(field) for field in fields)
        if any(type(value) is not (int if field == 'trial' else str)
               for field, value in zip(fields, key)):
            invalid += 1
        else:
            actual[key] += 1
    return dict(expected=len(expected), observed=len(records),
                missing=len(expected - actual.keys()), unexpected=len(actual.keys() - expected),
                duplicates=sum(count - 1 for count in actual.values()), invalid=invalid)


def campaign_summary(analyses, c_rows, llvm_rows, cases, plans, trials,
                     require_diagnostics=False):
    """A complete campaign contains every requested case, plan and trial once."""
    if (not cases or not plans or type(trials) is not int or trials < 1 or len(set(cases)) != len(cases)
            or len(set(plans)) != len(plans)):
        raise ValueError('campaign needs unique cases/plans and a positive trial count')
    coverage = dict(
        analyses=_coverage(analyses, ('case',), {(case,) for case in cases}),
        C=_coverage(c_rows, ('case', 'trial'),
                    {(case, trial) for case in cases for trial in range(trials)}),
        LLVM=_coverage(llvm_rows, ('case', 'plan', 'trial'),
                       {(case, plan, trial) for case in cases
                        for plan in plans for trial in range(trials)}))
    covered = all(not any(record[key] for key in ('missing', 'unexpected', 'duplicates', 'invalid'))
                  for record in coverage.values())
    diagnostic_statuses = Counter(a.get('branch_diagnostic', {}).get('status', 'missing')
                                  for a in analyses)
    diagnostics_complete = covered and diagnostic_statuses == {'complete': len(cases)}
    diagnostics_valid = (sum(diagnostic_statuses.values()) == len(cases)
                         and set(diagnostic_statuses) <= {'complete', 'incomplete', 'unavailable'})
    cfg_counts_valid = all(type(a.get('wrong_sites')) is int and a['wrong_sites'] >= 0 for a in analyses)
    cfg_wrong_sites = sum(a['wrong_sites'] for a in analyses) if cfg_counts_valid else None
    semantic_complete = (covered and all(a.get('status') == 'analysed' for a in analyses)
                         and cfg_wrong_sites == 0
                         and all(row.get('status') == 'match' for row in c_rows)
                         and all(row.get('status') == 'match' for row in llvm_rows))
    complete = (semantic_complete and diagnostics_valid
                and (not require_diagnostics or diagnostics_complete))
    return dict(c_statuses=dict(Counter(r['status'] for r in c_rows)),
                llvm_statuses=dict(Counter(r['status'] for r in llvm_rows)),
                cfg_wrong_sites=cfg_wrong_sites,
                coverage=coverage, semantic_complete=semantic_complete,
                diagnostic_statuses=dict(diagnostic_statuses),
                diagnostics_complete=diagnostics_complete,
                require_diagnostics=require_diagnostics, complete=complete)
