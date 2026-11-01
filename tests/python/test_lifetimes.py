"""Lifetime tests for the Python API's borrowed views.

Instruction, BasicBlock, Function and a Graph's IGNILBlock all point into
memory the Graph owns. They used to hold only the raw pointer, so once the
Graph was collected - ``dis.analyze(code).instructions()`` drops it before the
list is iterated - every access read freed memory (valgrind: invalid reads in
all of these cases; SIGSEGV under MALLOC_PERTURB_).

These checks are deterministic: they assert the Graph is kept alive by its
views, and that using a view after the Graph was closed explicitly raises
ReferenceError. Run directly (no pytest needed); exits non-zero on failure.
Needs DRAGON_C_LIB, or a build tree dragon.py can find on its own.
"""

import gc
import os
import sys
import weakref

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
import dragon  # noqa: E402

# 0x1000 mov eax, 5 ; 0x1005 call 0x100a ; 0x100a ret
CODE = bytes.fromhex('b805000000' 'e800000000' 'c3')
EXPECTED = ['mov eax, 5', 'call 0', 'ret']

failures = []


def check(name, fn):
    try:
        fn()
    except Exception as e:  # noqa: BLE001 - report every failure, keep going
        failures.append(name)
        print(f'FAIL {name}: {type(e).__name__}: {e}')
    else:
        print(f'ok   {name}')


def analyze():
    dis = dragon.Disassembler(dragon.Architecture.X86_64, dragon.Configuration())
    return dis.analyze(CODE, 0x1000)


def collected(make_view):
    """Build a view from a fresh Graph, drop every other reference to the
    Graph, collect, and return (view, weakref-to-graph)."""
    g = analyze()
    ref = weakref.ref(g)
    view = make_view(g)
    del g
    gc.collect()
    return view, ref


def raises_reference_error(fn):
    try:
        fn()
    except ReferenceError:
        return
    raise AssertionError('expected ReferenceError')


# -- views keep the Graph alive ----------------------------------------------

def t_chained_instructions():
    got = [i.disassembly for i in analyze().instructions() if not gc.collect()]
    assert got == EXPECTED, got


def t_instruction():
    insn, ref = collected(lambda g: g.get_instruction(0x1005))
    assert ref() is not None, 'Graph collected while an Instruction is alive'
    assert insn.disassembly == 'call 0' and insn.callees == [0x100a]


def t_ignil_block():
    blk, ref = collected(lambda g: g.get_ignil_block(0x100a))
    assert ref() is not None, 'Graph collected while an IGNILBlock is alive'
    assert 'RET' in blk.to_string()


def t_basic_block():
    bb, ref = collected(lambda g: g.get_block(0x1000))
    assert ref() is not None, 'Graph collected while a BasicBlock is alive'
    assert [i.disassembly for i in bb.instructions()] == EXPECTED


def t_function():
    fn, ref = collected(lambda g: g.get_function(0x1000))
    assert ref() is not None, 'Graph collected while a Function is alive'
    assert fn.num_instructions == 3 and fn.block_addresses() == [0x1000]


def t_block_instructions_outlive_block():
    insns, ref = collected(lambda g: g.get_block(0x1000).instructions())
    assert ref() is not None, 'Graph collected while its Instructions are alive'
    assert [i.disassembly for i in insns] == EXPECTED


def t_lift_trace_after_graph_dropped():
    dis = dragon.Disassembler(dragon.Architecture.X86_64, dragon.Configuration())
    insns = dis.analyze(CODE, 0x1000).instructions()
    gc.collect()
    with dis.lift_trace(insns) as blk:
        assert 'EAX' in blk.to_string()


# -- explicit close raises instead of reading freed memory -------------------

def t_closed_graph():
    g = analyze()
    insn = g.get_instruction(0x1000)
    blk = g.get_ignil_block(0x1000)
    bb = g.get_block(0x1000)
    fn = g.get_function(0x1000)
    bb_insns = bb.instructions()
    with g:
        pass
    raises_reference_error(lambda: insn.disassembly)
    raises_reference_error(lambda: blk.to_string())
    raises_reference_error(lambda: bb.start_address)
    raises_reference_error(lambda: fn.num_blocks)
    raises_reference_error(lambda: bb_insns[0].address)


def t_closed_owned_block():
    dis = dragon.Disassembler(dragon.Architecture.X86_64, dragon.Configuration())
    g = dis.analyze(CODE, 0x1000)
    blk = dis.lift_trace(g.instructions())
    with blk:
        pass
    raises_reference_error(lambda: blk.size)


for name, fn in list(globals().items()):
    if name.startswith('t_') and callable(fn):
        check(name[2:], fn)

if failures:
    print(f'{len(failures)} failed: {", ".join(failures)}')
    sys.exit(1)
print('all passed')
