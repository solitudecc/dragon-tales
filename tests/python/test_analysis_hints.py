"""C-shim/Python analysis hints, callback lifetime and convention propagation."""
import gc
import os
import struct
import sys
import weakref

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
import dragon


def raises(kind, action):
    try:
        action()
    except kind:
        return
    raise AssertionError(f'expected {kind.__name__}')


def test_entries():
    with dragon.Configuration(strategy=dragon.Strategy.RECURSIVE_TRAVERSAL) as cfg:
        with dragon.Disassembler(config=cfg) as dis:
            code = b'\xc3\xcc\xb8\x2a\0\0\0\xc3'
            with dis.analyze(code, 0x1000) as baseline:
                assert baseline.get_instruction(0x1002) is None
            source = [0x1000, 0x1002, 0xffffffffffffffff]
            with dragon.AnalysisHints(source) as hints:
                source.clear()
                assert hints.entry_points == [0x1000, 0x1002, 0xffffffffffffffff]
                graph = dis.analyze(code, 0x1000, hints=hints)
                assert graph.get_instruction(0x1002).is_function_start
                hints.entry_points = []
                assert hints.entry_points == []
                raises(OverflowError, lambda: setattr(hints, 'entry_points', [-1]))
            assert graph.get_instruction(0x1002).is_function_start
            graph.__exit__()
            raises(ReferenceError, lambda: dis.analyze(code, hints=hints))
            raises(ReferenceError, lambda: hints.entry_points)


def dispatch():
    # cmp edi, 2; ja default; mov edi, edi; jmp qword ptr [rdi*8+0x2000]
    code = bytearray(b'\x83\xff\x02\x0f\x87\x57\0\0\0\x89\xff\xff\x24\xfd\0\x20\0\0')
    code.extend(b'\xcc' * (0x70 - len(code)))
    for offset in (0x30, 0x40, 0x50, 0x60):
        code[offset] = 0xc3
    return code


def test_reader():
    table = struct.pack('<QQQ', 0x1030, 0x1040, 0x1050)

    class Reader:
        calls = 0

        def __call__(self, address, size):
            self.calls += 1
            if 0x2000 <= address and address + size <= 0x2000 + len(table):
                return table[address - 0x2000:address - 0x2000 + size]
            return None

    reader = Reader()
    ref = weakref.ref(reader)
    hints = dragon.AnalysisHints([0x1000], reader)
    del reader
    gc.collect()
    assert ref() is not None
    with dragon.Configuration(strategy=dragon.Strategy.RECURSIVE_TRAVERSAL) as cfg:
        with dragon.Disassembler(config=cfg) as dis:
            with dis.analyze(dispatch(), 0x1000, hints) as graph:
                assert set(graph.get_instruction(0x100b).branches) == {0x1030, 0x1040, 0x1050}
            assert ref().calls > 0
            hints.read_memory = None
            gc.collect()
            assert ref() is None
            with dis.analyze(dispatch(), 0x1000, hints) as graph:
                assert graph.get_instruction(0x100b).branches == []
            hints.read_memory = lambda address, size: b''
            raises(ValueError, lambda: dis.analyze(dispatch(), 0x1000, hints))

            def failing(address, size):
                raise RuntimeError('reader failed')

            hints.read_memory = failing
            raises(RuntimeError, lambda: dis.analyze(dispatch(), 0x1000, hints))
            hints.read_memory = None
            with dis.analyze(dispatch(), 0x1000, hints):
                pass
    hints.__exit__()
    raises(ReferenceError, lambda: setattr(hints, 'read_memory', None))


def test_convention():
    for cc in (dragon.CallingConvention.DEFAULT, dragon.CallingConvention.SYSV,
               dragon.CallingConvention.WIN64):
        with dragon.Configuration(calling_convention=cc) as cfg:
            assert cfg.calling_convention == cc
            cfg.calling_convention = dragon.CallingConvention.WIN64
            assert cfg.calling_convention == dragon.CallingConvention.WIN64
            raises(ValueError, lambda: setattr(cfg, 'calling_convention', 999))
            with dragon.Disassembler(config=cfg) as dis:
                # Prologue; mov eax, ecx; add eax, edx; pop rbp; ret.
                with dis.analyze(bytes.fromhex('554889e589c801d05dc3'), 0x1000) as graph:
                    with dragon.LLVMLifter() as lifter:
                        assert lifter.lift(graph.get_function(0x1000), graph)
                        assert 'i32 sub_1000(i32 RCX, i32 RDX)' in lifter.ir()
        raises(ReferenceError, lambda: cfg.calling_convention)


test_entries()
test_reader()
test_convention()
print('ok: entries, memory reader, lifetime, exceptions, calling convention')
