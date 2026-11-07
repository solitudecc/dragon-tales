"""Legacy refuses incomplete functions atomically through C and Python APIs."""
from contextlib import ExitStack
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'python'))
import dragon as d


ARCHES = (d.Architecture.X86_32, d.Architecture.X86_64)
STRATEGIES = (d.Strategy.LINEAR_SWEEP, d.Strategy.RECURSIVE_TRAVERSAL)

# Real decoded MOVBE store has no register definition and no IGNIL op. The
# preceding ordinary STORE and subsequent LOAD must not conceal its marker.
# The second MOVBE fixture puts the marker beyond a clean entry block.
BAD_CASES = (
    ('movbe-store', 'c703443322110f38f1038b03c3', 6, False),
    ('fs-load', '648b03c3', 0, False),
    ('nonentry-movbe', 'b807000000eb000f38f103c3', 7, True),
)


def analyze(arch, strategy, code, address):
    return d.Disassembler(arch, d.Configuration(strategy=strategy)).analyze(
        bytes.fromhex(code), address)


def module_bytes(lifter):
    return lifter.ir().encode('utf-8')


class LegacyUnliftedTests(unittest.TestCase):
    def marked_function(self, stack, arch, strategy, case, address=0x1000):
        name, code, offset, nonentry = case
        graph = stack.enter_context(analyze(arch, strategy, code, address))
        function = stack.enter_context(graph.get_function(address))
        self.assertTrue(function.is_valid, name)
        marker = address + offset
        member_blocks = function.block_addresses()
        marked = []
        for block_address in member_blocks:
            with graph.get_ignil_block(block_address) as block:
                if marker in block.unlifted():
                    marked.append(block_address)
                    if 'movbe' in name:
                        self.assertFalse(any(pc == marker for pc, _, _ in block.ops()), name)
        self.assertEqual(len(marked), 1, (name, member_blocks, marked))
        instruction = graph.get_instruction(marker)
        self.assertIsNotNone(instruction)
        self.assertTrue(instruction.mnemonic.lower().startswith('movbe' if 'movbe' in name else 'mov'))
        if nonentry:
            self.assertNotEqual(marked[0], address)
            with graph.get_ignil_block(address) as entry:
                self.assertEqual(entry.unlifted(), [])
        return graph, function

    def assert_atomic_refusal(self, lifter, function, graph):
        before = module_bytes(lifter)
        self.assertIs(lifter.lift(function, graph), False)
        self.assertEqual(module_bytes(lifter), before)
        # Repeat through the C entry point to check the integer contract too.
        self.assertEqual(d.lib.dragon_llvm_lifter_lift(
            lifter._ptr, function._ptr, graph._ptr), 0)
        self.assertEqual(module_bytes(lifter), before)

    def test_markers_reject_without_creating_a_definition(self):
        for arch in ARCHES:
            for strategy in STRATEGIES:
                for case in BAD_CASES:
                    with self.subTest(arch=arch, strategy=strategy, case=case[0]), ExitStack() as stack:
                        graph, function = self.marked_function(stack, arch, strategy, case)
                        lifter = stack.enter_context(d.LLVMLifter(arch))
                        self.assertNotIn(b'define ', module_bytes(lifter))
                        self.assert_atomic_refusal(lifter, function, graph)
                        self.assertNotIn(b'@sub_1000(', module_bytes(lifter))

    def test_existing_module_survives_repeated_failures_and_later_success(self):
        for arch in ARCHES:
            for strategy in STRATEGIES:
                with self.subTest(arch=arch, strategy=strategy), ExitStack() as stack:
                    lifter = stack.enter_context(d.LLVMLifter(arch))
                    existing = stack.enter_context(analyze(arch, strategy, 'b809000000c3', 0x3000))
                    existing_fn = stack.enter_context(existing.get_function(0x3000))
                    self.assertTrue(lifter.lift(existing_fn, existing))
                    before = module_bytes(lifter)
                    for case in BAD_CASES:
                        graph, function = self.marked_function(stack, arch, strategy, case)
                        self.assert_atomic_refusal(lifter, function, graph)
                        self.assertEqual(module_bytes(lifter), before)
                    # Reusing the rejected entry address must not hit a stale
                    # partial symbol or retained unsupported-address state.
                    good = stack.enter_context(analyze(arch, strategy, 'b807000000c3', 0x1000))
                    good_fn = stack.enter_context(good.get_function(0x1000))
                    self.assertTrue(lifter.lift(good_fn, good))
                    self.assertIn(b'@sub_1000(', module_bytes(lifter))
                    self.assertIn(b'@sub_3000(', module_bytes(lifter))
                    # A marked function with the same address as a pre-existing
                    # good definition must leave that definition intact too.
                    graph, function = self.marked_function(stack, arch, strategy, BAD_CASES[0])
                    self.assert_atomic_refusal(lifter, function, graph)
                    later = stack.enter_context(analyze(arch, strategy, 'b80b000000c3', 0x4000))
                    later_fn = stack.enter_context(later.get_function(0x4000))
                    self.assertTrue(lifter.lift(later_fn, later))
                    self.assertIn(b'@sub_4000(', module_bytes(lifter))

    def test_supported_undef_is_not_an_unlifted_marker(self):
        # MUL's architecturally undefined flags are honest UNDEF values,
        # while the instruction itself is supported and has no marker.
        for arch in ARCHES:
            for strategy in STRATEGIES:
                with self.subTest(arch=arch, strategy=strategy), ExitStack() as stack:
                    graph = stack.enter_context(analyze(arch, strategy, 'f7e3c3', 0x2000))
                    function = stack.enter_context(graph.get_function(0x2000))
                    self.assertTrue(function.is_valid)
                    with graph.get_ignil_block(0x2000) as block:
                        self.assertEqual(block.unlifted(), [])
                        self.assertTrue(any(opcode == d.IGNILOpcode.UNDEF for _, opcode, _ in block.ops()))
                    lifter = stack.enter_context(d.LLVMLifter(arch))
                    self.assertIs(lifter.lift(function, graph), True)
                    self.assertIn(b'@sub_2000(', module_bytes(lifter))
                    c_lifter = stack.enter_context(d.LLVMLifter(arch))
                    self.assertEqual(d.lib.dragon_llvm_lifter_lift(
                        c_lifter._ptr, function._ptr, graph._ptr), 1)

    def test_stateful_modes_keep_accepting_marked_functions(self):
        # This checks the Python acceptance contract. Native boundary tests
        # independently check that execution exits Unsupported at the marker.
        for arch in ARCHES:
            for strategy in STRATEGIES:
                for case in BAD_CASES:
                    for mode in (d.LLVMExecutionMode.STATEFUL_V2, d.LLVMExecutionMode.STATIC_ANALYSIS):
                        with self.subTest(arch=arch, strategy=strategy, case=case[0], mode=mode), ExitStack() as stack:
                            graph, function = self.marked_function(stack, arch, strategy, case)
                            lifter = stack.enter_context(d.LLVMLifter(arch, execution_mode=mode))
                            self.assertIs(lifter.lift(function, graph), True)
                            prefix = 'dragon_v2' if mode == d.LLVMExecutionMode.STATEFUL_V2 else 'dragon_static'
                            arch_name = 'x86_64' if arch == d.Architecture.X86_64 else 'x86_32'
                            self.assertIn(f'@{prefix}_{arch_name}_1000('.encode(), module_bytes(lifter))


if __name__ == '__main__':
    unittest.main(verbosity=2)
