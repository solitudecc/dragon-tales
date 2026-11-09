"""Guard optional diagnostics and reject missing semantic comparisons."""
import copy
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts/validation'))
from campaign_diagnostics import branch_diagnostic, campaign_summary


class CampaignDiagnostics(unittest.TestCase):
    def setUp(self):
        self.cases = ['a-O0', 'b-O0']
        self.plans = ['product-O0', 'product-O2']
        self.analyses = [dict(case=c, status='analysed', wrong_sites=0,
                              branch_diagnostic=dict(status='complete')) for c in self.cases]
        self.c_rows = [dict(case=c, trial=t, status='match') for c in self.cases for t in range(2)]
        self.llvm_rows = [dict(case=c, plan=p, trial=t, status='match')
                          for c in self.cases for p in self.plans for t in range(2)]

    def summary(self, strict=False):
        return campaign_summary(self.analyses, self.c_rows, self.llvm_rows,
                                self.cases, self.plans, 2, strict)

    def test_complete_campaign(self):
        self.assertTrue(self.summary(True)['complete'])
        self.assertTrue(self.summary()['semantic_complete'])

    def test_unavailable_diagnostic_does_not_execute_or_claim_zero_proofs(self):
        def forbidden(*args):
            raise AssertionError('unavailable diagnostic executed')
        report = branch_diagnostic(SimpleNamespace(), object(), [1, 2], forbidden)
        self.assertEqual(report['status'], 'unavailable')
        self.assertFalse(report['available'])
        self.assertIsNone(report['branches_decided_single_edge'])
        for analysis in self.analyses:
            analysis['branch_diagnostic'] = report
        self.assertTrue(self.summary()['complete'])
        self.assertFalse(self.summary()['diagnostics_complete'])
        self.assertFalse(self.summary(True)['complete'])

    def test_available_diagnostic_runs(self):
        graph = object()
        def run(g, blocks):
            self.assertIs(g, graph)
            self.assertEqual(blocks, [1, 2])
            return {1: 1, 2: 2}
        report = branch_diagnostic(SimpleNamespace(dragon_symexec_create=object()), graph, [1, 2], run)
        self.assertEqual(report['status'], 'complete')
        self.assertEqual((report['branches_decided_single_edge'], report['branches_both_edges']), (1, 1))

    def test_available_failure_is_not_unavailable(self):
        for error in (AttributeError('broken dependency'), RuntimeError('solver failure')):
            with self.subTest(error=error):
                def broken(*args):
                    raise error
                with self.assertRaisesRegex(type(error), str(error)):
                    branch_diagnostic(SimpleNamespace(dragon_symexec_create=object()), None, [1], broken)

    def test_incomplete_and_malformed_diagnostic(self):
        library = SimpleNamespace(dragon_symexec_create=object())
        report = branch_diagnostic(library, None, [1], lambda *a: {1: None})
        self.assertEqual(report['status'], 'incomplete')
        self.assertEqual(report['branches_symbolic_error'], 1)
        for edges in ({}, {1: 1, 2: 1}, {1: 0}, {1: 3}, {1: True}, {1: 1.0}, {True: 1}):
            with self.subTest(edges=edges), self.assertRaises(ValueError):
                branch_diagnostic(library, None, [1], lambda *a: edges)

    def test_empty_diagnostic_still_checks_capability(self):
        self.assertEqual(branch_diagnostic(SimpleNamespace(), None, [], None)['status'], 'unavailable')
        self.assertEqual(branch_diagnostic(SimpleNamespace(dragon_symexec_create=object()), None, [],
                                          lambda *a: {})['status'], 'complete')

    def test_missing_entire_case_is_not_success(self):
        self.analyses.pop()
        self.c_rows = [r for r in self.c_rows if r['case'] == 'a-O0']
        self.llvm_rows = [r for r in self.llvm_rows if r['case'] == 'a-O0']
        self.assertFalse(self.summary()['complete'])
        self.assertEqual(self.summary()['coverage']['LLVM']['missing'], 4)

    def test_failed_build_is_not_success_of_other_case(self):
        self.analyses[-1] = dict(case='b-O0', status='build_error')
        self.c_rows = [r for r in self.c_rows if r['case'] == 'a-O0']
        self.llvm_rows = [r for r in self.llvm_rows if r['case'] == 'a-O0']
        self.assertFalse(self.summary()['complete'])

    def test_missing_trial_missing_plan_duplicates_and_unexpected_rows(self):
        original = copy.deepcopy(self.llvm_rows)
        for changed in (original[:-1], [r for r in original if r['plan'] != 'product-O2'],
                        original + [original[0]], original + [dict(case='other', plan='product-O0', trial=0, status='match')]):
            self.llvm_rows = changed
            with self.subTest(rows=changed):
                self.assertFalse(self.summary()['complete'])

    def test_semantic_failure_cannot_be_hidden_by_optional_diagnostic(self):
        self.analyses[0]['branch_diagnostic'] = dict(status='unavailable')
        for key in ('different', 'incomplete', 'infrastructure_error'):
            self.llvm_rows[0]['status'] = key
            self.assertFalse(self.summary()['complete'])
        self.llvm_rows[0]['status'] = 'match'
        self.c_rows[0]['status'] = 'different'
        self.assertFalse(self.summary()['complete'])

    def test_cfg_wrong_and_missing_diagnostics_reject_success(self):
        self.analyses[0]['wrong_sites'] = 1
        self.assertFalse(self.summary()['semantic_complete'])
        self.analyses[0]['wrong_sites'] = 0
        for status in ('missing', 'error', 'invented'):
            self.analyses[0]['branch_diagnostic'] = dict(status=status)
            self.assertFalse(self.summary()['complete'])

    def test_report_keys_and_cfg_counts_have_exact_types(self):
        for invalid in (False, 0.0, '0', None, []):
            self.llvm_rows[0]['trial'] = invalid
            self.assertFalse(self.summary()['complete'])
            self.assertEqual(self.summary()['coverage']['LLVM']['invalid'], 1)
        self.llvm_rows[0]['trial'] = 0
        for invalid in (False, 0.0, '0', None):
            self.analyses[0]['wrong_sites'] = invalid
            self.assertFalse(self.summary()['complete'])

    def test_invalid_expected_matrix(self):
        for cases, plans, trials in (([], ['p'], 2), (['c'], [], 2),
                                     (['c', 'c'], ['p'], 2), (['c'], ['p', 'p'], 2), (['c'], ['p'], 0),
                                     (['c'], ['p'], True), (['c'], ['p'], 1.0)):
            with self.subTest(cases=cases, plans=plans, trials=trials), self.assertRaises(ValueError):
                campaign_summary([], [], [], cases, plans, trials)


if __name__ == '__main__':
    unittest.main()
