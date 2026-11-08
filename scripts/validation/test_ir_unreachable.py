"""Pure regression controls for the structural checker; no LLVM/CPU/Dragon."""
import unittest

from ir_unreachable import Limits, audit_unreachable, has_unproved_unreachable


def fixture(*, width=32, mask=63, base=4352, cases=None, flag="disjoint", before="", selector=None,
            header=None, prefix="", other="", dead_body="unreachable"):
    if cases is None: cases = range(base, base + 64)
    if header is None: header = f"define void @check(i{width} %x, i{width} %y) {{"
    if selector is None:
        selector = (f"%difference = xor i{width} %x, %y\n"
                    f"%masked = and i{width} %difference, {mask}\n"
                    f"%selector = or {flag + ' ' if flag else ''}i{width} %masked, {base}")
    return (header + "\nentry:\n" + prefix + before + selector +
            f"\nswitch i{width} %selector, label %dead [\n" +
            "\n".join(f"i{width} {value}, label %returned" for value in cases) +
            "\n]\ndead:\n" + dead_body + "\nreturned:\nret void\n" + other + "\n}\n")


class DeadSwitchDefaults(unittest.TestCase):
    def accepted(self, ir):
        result = audit_unreachable(ir)
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["unreachable_count"], result["proved_dead_count"])
        self.assertFalse(result["violations"])
        return result

    def rejected(self, ir, limits=None):
        result = audit_unreachable(ir, limits=limits)
        self.assertFalse(result["ok"], result)
        self.assertTrue(result["violations"])
        self.assertTrue(has_unproved_unreachable(ir, limits=limits))
        return result

    def test_old_blanket_false_positive_and_complete_certificate(self):
        ir = fixture()
        self.assertIn("unreachable", ir)  # Exact old textual predicate rejects.
        result = self.accepted(ir)
        self.assertEqual(result["unreachable_count"], 1)
        condition = result["certificates"][0]["conditions"][0]
        self.assertEqual(condition["possible_values_overapprox"], list(range(4352, 4416)))
        self.assertEqual(condition["case_values"], list(range(4352, 4416)))
        self.assertTrue(condition["every_possible_value_is_an_explicit_case"])
        self.assertTrue(any(node["checks"] for node in result["certificates"][0]["selector_dependencies"]))

    def test_removed_case_and_wider_mask_are_both_rejected(self):
        for ir in (fixture(cases=range(4352, 4415)), fixture(mask=127)):
            self.assertIn("unreachable", ir)
            self.rejected(ir)  # Both old and new reject genuine possible defaults.

    def test_reachable_conditional_incoming_edge_is_rejected(self):
        ir = fixture(header="define void @check(i32 %x, i32 %y, i1 %condition) {",
                     prefix="br i1 %condition, label %dead, label %calculate\ncalculate:\n")
        result = self.rejected(ir)
        self.assertTrue(any("non-default incoming" in row["reason"] for row in result["violations"]))

    def test_switch_case_incoming_edge_is_rejected(self):
        self.rejected(fixture().replace("i32 4352, label %returned", "i32 4352, label %dead"))

    def test_every_unreachable_must_have_its_own_proof(self):
        result = self.rejected(fixture(other="other:\nunreachable"))
        self.assertEqual(result["unreachable_count"], 2)
        self.assertEqual(result["proved_dead_count"], 1)

    def test_other_instruction_in_unreachable_block_is_rejected(self):
        self.rejected(fixture(dead_body="call void @abort()\nunreachable"))

    def test_unreachable_entry_is_rejected(self):
        self.rejected("define void @bad() {\nentry:\nunreachable\n}\n")

    def test_undef_and_poison_are_rejected_even_under_mask(self):
        for token in ("undef", "poison"):
            for mask in (0,63):
                self.rejected(fixture(selector=f"%masked = and i32 {token}, {mask}\n%selector = or i32 %masked, 4352"))

    def test_false_disjoint_precondition_is_rejected(self):
        self.rejected(fixture(base=4384, cases=range(4352, 4416)))
        # Without a poison-producing flag, OR with overlapping known-one bits
        # is valid and its conservative range is still covered.
        self.accepted(fixture(base=4384, cases=range(4352, 4416), flag=""))

    def test_phi_freeze_select_call_and_arithmetic_are_not_opaque_leaves(self):
        expressions = ["phi i32 [ %x, %entry ]", "freeze i32 %x",
                       "select i1 true, i32 %x, i32 %y", "call i32 @unknown()",
                       "add i32 %x, %y", "udiv i32 %x, %y"]
        for expression in expressions:
            with self.subTest(expression=expression):
                self.rejected(fixture(selector=f"%input = {expression}\n%masked = and i32 %input, 63\n%selector = or i32 %masked, 4352"))

    def test_dangerous_flags_are_rejected(self):
        for expression in ("and exact i32 %x, 63", "xor disjoint i32 %x, %y",
                           "trunc nuw i64 %wide to i32", "zext nneg i16 %short to i32"):
            self.rejected(fixture(header="define void @check(i32 %x, i32 %y, i64 %wide, i16 %short) {",
                selector=f"%input = {expression}\n%masked = and i32 %input, 63\n%selector = or i32 %masked, 4352"))

    def test_trunc_zext_and_high_bits_have_no_hidden_assumptions(self):
        ir = fixture(width=64, header="define void @check(i16 %x, i64 %y) {", selector=
            "%small = trunc i64 %y to i8\n%wide = zext i8 %small to i64\n"
            "%masked = and i64 %wide, 63\n%selector = or disjoint i64 %masked, 4352")
        self.accepted(ir)
        self.rejected(ir.replace("zext i8", "zext i16"))

    def test_plain_loads_are_arbitrary_but_pointer_poison_is_rejected(self):
        selector = ("%field = getelementptr i8, ptr %state, i64 24\n"
                    "%registers = load ptr, ptr %field, align 1\n"
                    "%slot = getelementptr i8, ptr %registers, i64 48\n"
                    "%input = load i32, ptr %slot, align 1\n"
                    "%masked = and i32 %input, 63\n%selector = or disjoint i32 %masked, 4352")
        ir = fixture(header="define void @check(ptr %state) {", selector=selector)
        self.accepted(ir)
        for bad in (ir.replace("load ptr, ptr %field", "load ptr, ptr poison"),
                    ir.replace("getelementptr i8", "getelementptr inbounds i8", 1),
                    ir.replace("load i32", "load volatile i32"),
                    ir.replace("%field = getelementptr i8, ptr %state, i64 24", "%field = phi ptr [ %state, %entry ]")):
            self.rejected(bad)

    def test_load_range_and_definedness_metadata_are_not_assumptions(self):
        ir = fixture(header="define void @check(ptr %state) {", selector=
            "%pointer = load ptr, ptr %state, align 1\n"
            "%input = load i32, ptr %pointer, align 1\n"
            "%masked = and i32 %input, 63\n%selector = or disjoint i32 %masked, 4352")
        self.accepted(ir)
        self.accepted(ir.replace("load i32, ptr %pointer, align 1", "load i32, ptr %pointer, align 1, !dbg !0"))
        for attachment in ("range", "noundef", "invariant.load", "tbaa"):
            with self.subTest(integer_load_metadata=attachment):
                self.rejected(ir.replace("load i32, ptr %pointer, align 1", f"load i32, ptr %pointer, align 1, !{attachment} !0"))
        for attachment in ("nonnull", "noundef", "align", "dereferenceable", "dereferenceable_or_null", "range"):
            with self.subTest(pointer_load_metadata=attachment):
                self.rejected(ir.replace("load ptr, ptr %state, align 1", f"load ptr, ptr %state, align 1, !{attachment} !0"))

    def test_poisoning_flag_is_not_erased_by_a_zero_mask(self):
        self.rejected(fixture(selector="%poisoning = or disjoint i32 %x, 1\n"
            "%masked = and i32 %poisoning, 0\n%selector = or i32 %masked, 4352"))

    def test_unsupported_cfg_cannot_hide_an_incoming_edge(self):
        self.rejected(fixture(other="unmodelled:\nindirectbr ptr %unknown, [label %dead]"))

    def test_multiple_switch_defaults_all_need_coverage(self):
        ir = fixture(prefix="br i1 true, label %one, label %two\none:\n")
        second = "%alternative = and i32 %x, 63\n%other = or disjoint i32 %alternative, 4352\n"
        second += "switch i32 %other, label %dead [\n" + "\n".join(f"i32 {v}, label %returned" for v in range(4352,4416)) + "\n]\n"
        ir = ir.replace("\ndead:\n", "\ntwo:\n" + second + "dead:\n")
        result = self.accepted(ir)
        self.assertEqual(result["certificates"][0]["predecessor_count"], 2)
        self.rejected(ir.replace("%alternative = and i32 %x, 63", "%alternative = and i32 %x, 127"))

    def test_small_known_bits_domains_match_independent_exhaustive_values(self):
        for mask in (0,1,3,5,15,63,127,255):
            base = 0 if mask & 128 else 128
            expected = sorted({(value & mask) | base for value in range(256)})
            ir = fixture(width=8, mask=mask, base=base, cases=expected)
            result = self.accepted(ir)
            self.assertEqual(result["certificates"][0]["conditions"][0]["possible_values_overapprox"], expected)

    def test_duplicate_case_and_width_mismatch_are_rejected(self):
        self.rejected(fixture(cases=[*range(4352,4416),4352]))
        self.rejected(fixture().replace("i32 4352, label", "i64 4352, label"))

    def test_cycles_and_missing_ssa_are_rejected(self):
        self.rejected(fixture(selector="%masked = and i32 %selector, 63\n%selector = or i32 %masked, 4352"))
        self.rejected(fixture().replace("xor i32 %x, %y", "xor i32 %absent, %y"))

    def test_limits_fail_closed(self):
        for limits in (Limits(max_work=1), Limits(max_nodes=1), Limits(max_depth=1),
                       Limits(max_values=32), Limits(max_lines=1), Limits(max_ir_bytes=16),
                       Limits(max_edges=1), Limits(max_blocks=1), Limits(max_values=0)):
            with self.subTest(limits=limits): self.rejected(fixture(), limits)
        self.rejected(";" + "x" * 1024 + "\n" + fixture(), Limits(max_ir_bytes=1024))
        # Character length fits but UTF-8 byte length does not. Byte-budget
        # validation uses bounded chunks before line lists/CFG construction.
        text = ";" + "\u4e2d" * 256 + "\n" + fixture(mask=0, cases=[4352])
        byte_limit = (len(text) + len(text.encode("utf-8"))) // 2
        self.assertLess(len(text), byte_limit)
        self.assertGreater(len(text.encode("utf-8")), byte_limit)
        self.accepted(text)
        self.rejected(text, Limits(max_ir_bytes=byte_limit))

    def test_no_unreachable_needs_no_default_certificate(self):
        self.assertTrue(audit_unreachable("define void @ok() {\nentry:\nret void\n}\n")["ok"])
        self.assertFalse(has_unproved_unreachable("; unreachable is only a comment\n"))


if __name__ == "__main__": unittest.main()
