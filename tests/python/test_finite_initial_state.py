"""Conditional facts are owned and never change the default graph contract."""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
import dragon


def raises(kind, action):
    try:
        action()
    except kind:
        return
    raise AssertionError(f'expected {kind.__name__}')


def value(report, values=None):
    item, = report['values']
    assert item['kind'] == (dragon.ValueSetKind.UNKNOWN if values is None else dragon.ValueSetKind.FINITE), report
    assert item['values'] == ([] if values is None else values), report


def test_owned_queries():
    payload = bytearray.fromhex('44332211')
    state = dragon.FiniteEntryState(0x1000, memory=[(0x4000, payload)])
    payload[:] = b'\xff' * 4
    assert state.memory == ((0x4000, bytes.fromhex('44332211')),)
    for strategy in (dragon.Strategy.LINEAR_SWEEP, dragon.Strategy.RECURSIVE_TRAVERSAL):
        with dragon.Configuration(strategy=strategy) as cfg, dragon.Disassembler(config=cfg) as dis:
            with dis.analyze(bytes.fromhex('8b042500400000c3'), 0x1000) as graph:
                ir = graph.get_ignil_block(0x1000)
                before = ir.to_string()
                baseline = graph.finite_values(query_register=(0x1000, 0, 32))
                value(baseline)
                report = graph.finite_values_with_initial_state([state], query_register=(0x1000, 0, 64))
                assert report['conditional'] and report['assumptions_valid'] and report['complete']
                value(report, [0x11223344])
                assert report['entry_states'] == [{'entry': 0x1000, 'registers': [],
                    'memory': [{'address': 0x4000, 'bytes': '44332211'}]}]
                json.dumps(report)
                load, = [index for _, opcode, index in ir.ops() if opcode == dragon.IGNILOpcode.LOAD]
                cells = graph.finite_memory_values_with_initial_state(
                    [(0x1000, load, 0x4000, 32), (0x1000, len(ir.ops()), 0x4000, 32)], [state])
                first, invalid = cells['values']
                assert first['kind'] == dragon.ValueSetKind.FINITE and first['values'] == [0x11223344]
                assert invalid['kind'] == dragon.ValueSetKind.UNKNOWN and invalid['values'] == []
                empty = graph.finite_values_with_initial_state([], query_register=(0x1000, 0, 32))
                assert empty['conditional'] and empty['assumptions_valid'] and not empty['entry_states']
                for key in baseline:
                    assert empty[key] == baseline[key], (key, empty, baseline)
                assert ir.to_string() == before
                assert graph.finite_values(query_register=(0x1000, 0, 32)) == baseline
            raises(ReferenceError, lambda: graph.finite_values_with_initial_state([state]))


def test_registers_and_rejection():
    with dragon.Disassembler() as dis:
        with dis.analyze(bytes.fromhex('90c3'), 0x1000) as graph:
            low = dragon.FiniteEntryState(0x1000, registers=[(0, 32, 0x89abcdef)])
            value(graph.finite_values_with_initial_state([low], query_register=(0x1000, 0, 64)))
            full = dragon.FiniteEntryState(0x1000, registers=[(0, 32, 0x89abcdef), (4, 32, 0x12345678)])
            value(graph.finite_values_with_initial_state([full], query_register=(0x1000, 0, 64)), [0x1234567889abcdef])
            overlap = dragon.FiniteEntryState(0x1000, registers=[(0, 32, 1), (1, 8, 1)])
            for states in ([overlap], [low, low], [dragon.FiniteEntryState(0x1001)],
                           [dragon.FiniteEntryState(0x1000, memory=[(0x4000, b'ab'), (0x4001, b'b')])]):
                result = graph.finite_values_with_initial_state(states, query_register=(0x1000, 0, 64))
                assert not result['assumptions_valid'] and not result['complete'] and not result['entry_states']
                value(result)
            budget = graph.finite_values_with_initial_state([low], max_facts=0, query_register=(0x1000, 0, 64))
            assert not budget['complete']
            value(budget)
            raises(TypeError, lambda: graph.finite_values_with_initial_state([{}]))
            for kwargs in ({'max_facts': -1}, {'max_operations': True}, {'prove_leaf_calls': 1},
                           {'query_register': (0, 0, 65)}, {'entry_points': [-1]}):
                raises(ValueError, lambda: graph.finite_values_with_initial_state([low], **kwargs))
            raises(ValueError, lambda: graph.finite_memory_values_with_initial_state([(0, -1, 0, 8)], []))
            # A caller changing private data cannot evade revalidation.
            low._registers = ((0, 32, 1 << 32),)
            raises(ValueError, lambda: graph.finite_values_with_initial_state([low]))
        with dis.analyze(bytes.fromhex('b8efcdab89c3'), 0x1000) as graph:
            low = dragon.FiniteEntryState(0x1000, registers=[(0, 32, 0)])
            value(graph.finite_values_with_initial_state([low], query_register=(0x1000, 0, 64)), [0x89abcdef])


def test_constructor_bounds():
    for args in ({'entry': -1}, {'entry': True}, {'entry': 1 << 64},
                 {'entry': 0, 'registers': [(0, 7, 1)]},
                 {'entry': 0, 'registers': [(0, 32, 1 << 32)]},
                 {'entry': 0, 'registers': [(0xffffffff, 8, 1)]},
                 {'entry': 0, 'memory': [(0, b'')]},
                 {'entry': 0, 'memory': [((1 << 64) - 1, b'ab')]},
                 {'entry': 0, 'memory': [(0, b'x' * 262144)]}):
        raises(ValueError, lambda: dragon.FiniteEntryState(**args))
    raises(TypeError, lambda: dragon.FiniteEntryState(0, memory=[(0, 'text')]))


if __name__ == '__main__':
    test_constructor_bounds()
    test_owned_queries()
    test_registers_and_rejection()
    print('conditional initial state Python API checks passed')
