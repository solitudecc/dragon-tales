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
    # Initial bytes are readable, but are not a runtime contents assertion.
    table = struct.pack('<QQQ', 0x1030, 0x1040, 0x1050)

    class Reader:
        def __call__(self, address, size):
            if 0x2000 <= address <= 0x2000 + len(table) and size <= 0x2000 + len(table) - address:
                return table[address - 0x2000:address - 0x2000 + size]
            return None

    reader = Reader()
    ref = weakref.ref(reader)
    hints = dragon.AnalysisHints([0x1000], reader)
    del reader
    gc.collect()
    assert ref() is not None
    assert hints.read_known_memory is None
    with dragon.Configuration(strategy=dragon.Strategy.RECURSIVE_TRAVERSAL) as cfg:
        with dragon.Disassembler(config=cfg) as dis:
            with dis.analyze(dispatch(), 0x1000, hints) as graph:
                assert graph.get_instruction(0x100b).branches == []
            hints.read_memory = None
            gc.collect()
            assert ref() is None
            with dis.analyze(dispatch(), 0x1000, hints) as graph:
                assert graph.get_instruction(0x100b).branches == []
    hints.__exit__()
    raises(ReferenceError, lambda: setattr(hints, 'read_memory', None))


def test_known_reader():
    table = struct.pack('<QQQ', 0x1030, 0x1040, 0x1050)

    class Reader:
        def __init__(self):
            self.calls = []

        def __call__(self, block, index, pc, address, size):
            self.calls.append((block, index, pc, address, size))
            if 0x2000 <= address <= 0x2000 + len(table) and size <= 0x2000 + len(table) - address:
                return table[address - 0x2000:address - 0x2000 + size]
            return None

    for strategy in (dragon.Strategy.LINEAR_SWEEP, dragon.Strategy.RECURSIVE_TRAVERSAL):
        reader = Reader()
        ref = weakref.ref(reader)
        hints = dragon.AnalysisHints([0x1000], read_known_memory=reader)
        del reader
        gc.collect()
        assert ref() is not None
        with dragon.Configuration(strategy=strategy) as cfg:
            with dragon.Disassembler(config=cfg) as dis:
                graph = dis.analyze(dispatch(), 0x1000, hints)
                assert set(graph.get_instruction(0x100b).branches) == {0x1030, 0x1040, 0x1050}
                calls = ref().calls
                assert calls
                assert {request[3] for request in calls} == {0x2000, 0x2008, 0x2010}
                for block, index, pc, address, size in calls:
                    assert block == 0x1009 and pc == 0x100b and size == 8
                    assert graph.get_ignil_block(block).ops()[index] == (pc, dragon.IGNILOpcode.LOAD, index)
                    assert 0x2000 <= address and address + size <= 0x2018
                hints.read_known_memory = None
                gc.collect()
                assert ref() is None
                # The graph owns the published result, not the callback.
                assert len(graph.get_instruction(0x100b).branches) == 3
                graph.__exit__()
                with dis.analyze(dispatch(), 0x1000, hints) as unknown:
                    assert unknown.get_instruction(0x100b).branches == []
                for bad in (b'', table, object()):
                    hints.read_known_memory = lambda *args, value=bad: value
                    raises(TypeError if type(bad) is object else ValueError,
                           lambda: dis.analyze(dispatch(), 0x1000, hints))
                # No partial target set when even one required entry lacks a fact.
                hints.read_known_memory = lambda block, index, pc, address, size: (None if address == 0x2010 else table[address-0x2000:address-0x2000+size])
                with dis.analyze(dispatch(), 0x1000, hints) as partial:
                    assert partial.get_instruction(0x100b).branches == []

                def failing(*args):
                    raise RuntimeError('known reader failed')

                hints.read_known_memory = failing
                raises(RuntimeError, lambda: dis.analyze(dispatch(), 0x1000, hints))
                raises(TypeError, lambda: setattr(hints, 'read_known_memory', 42))
                hints.read_known_memory = lambda *args: None
                with dis.analyze(dispatch(), 0x1000, hints) as unknown:
                    assert unknown.get_instruction(0x100b).branches == []
                # C snapshots callbacks; Python must retain the replaced cffi
                # trampoline until the synchronous native analysis returns.
                def replacing(block, index, pc, address, size):
                    hints.read_known_memory = None
                    return table[address-0x2000:address-0x2000+size]

                hints.read_known_memory = replacing
                with dis.analyze(dispatch(), 0x1000, hints) as replaced:
                    assert len(replaced.get_instruction(0x100b).branches) == 3
                assert hints.read_known_memory is None
        reader = Reader()
        ref = weakref.ref(reader)
        hints.read_known_memory = reader
        del reader
        hints.__exit__()
        gc.collect()
        assert ref() is None
        raises(ReferenceError, lambda: hints.read_known_memory)
        raises(ReferenceError, lambda: setattr(hints, 'read_known_memory', None))


def routed_code():
    code = dispatch()
    code.extend(b'\xcc' * (0x90 - len(code)))
    code[0x80] = 0xc3  # Extra caller seed, absent from ELF entry metadata.
    return bytes(code)


def dispatch_elf():
    text = routed_code()
    table = struct.pack('<QQQ', 0x1030, 0x1040, 0x1050)
    names = b'\0.text\0.rodata\0.shstrtab\0'
    data_offset = 64 + len(text)
    names_offset = data_offset + len(table)
    section_offset = (names_offset + len(names) + 7) & ~7

    def section(name, kind, flags, address, offset, size):
        return struct.pack('<IIQQQQIIQQ', name, kind, flags, address, offset, size, 0, 0, 1, 0)

    header = struct.pack('<4sBBBBB7xHHIQQQIHHHHHH', b'\x7fELF', 2, 1, 1, 0, 0,
                         2, 0x3e, 1, 0x1000, 0, section_offset, 0, 64, 0, 0, 64, 4, 3)
    body = header + text + table + names
    return (body + bytes(section_offset-len(body)) + section(0, 0, 0, 0, 0, 0) +
            section(1, 1, 6, 0x1000, 64, len(text)) +
            section(7, 1, 2, 0x2000, data_offset, len(table)) +
            section(15, 3, 0, 0, names_offset, len(names)))


class RoutedReader:
    def __init__(self):
        self.calls = []
        self.failure = None

    def __call__(self, block, index, pc, address, size):
        self.calls.append((block, index, pc, address, size))
        if self.failure is not None:
            raise self.failure
        table = struct.pack('<QQQ', 0x1030, 0x1040, 0x1050)
        if 0x2000 <= address <= 0x2018 and size <= 0x2018-address:
            return table[address-0x2000:address-0x2000+size]
        return None


def test_loaded_image_hints():
    for strategy in (dragon.Strategy.LINEAR_SWEEP, dragon.Strategy.RECURSIVE_TRAVERSAL):
        with dragon.LoadedImage.load_bytes(dispatch_elf()) as image, dragon.Configuration(strategy=strategy) as cfg:
            with image.disassemble(cfg) as unknown:
                assert unknown.get_instruction(0x100b).branches == []
            reader = RoutedReader()
            ref = weakref.ref(reader)
            hints = dragon.AnalysisHints([0x1080], read_known_memory=reader)
            del reader
            graph = image.disassemble(cfg, hints=hints)
            assert set(graph.get_instruction(0x100b).branches) == {0x1030, 0x1040, 0x1050}
            # Image metadata root and extra root must both survive the union.
            assert {0x1000, 0x1080} <= set(graph.function_addresses())
            assert ref().calls
            hints.__exit__()
            gc.collect()
            assert ref() is None
            assert len(graph.get_instruction(0x100b).branches) == 3
            graph.__exit__()
            raises(ReferenceError, lambda: image.disassemble(cfg, hints=hints))
            raises(TypeError, lambda: image.disassemble(cfg, hints=42))
            failure = ValueError('loaded-image known reader failed')
            reader = RoutedReader()
            reader.failure = failure
            with dragon.AnalysisHints(read_known_memory=reader) as bad:
                try:
                    image.disassemble(cfg, hints=bad)
                except ValueError as error:
                    assert error is failure
                else:
                    raise AssertionError('LoadedImage swallowed callback exception')
                reader.failure = None
                with image.disassemble(cfg, hints=bad) as recovered:
                    assert len(recovered.get_instruction(0x100b).branches) == 3


def test_finite_dispatch_hints():
    class ImageReader:
        def __call__(self, address, size):
            return None

    code = routed_code()
    for strategy in (dragon.Strategy.LINEAR_SWEEP, dragon.Strategy.RECURSIVE_TRAVERSAL):
        with dragon.Configuration(strategy=strategy) as cfg, dragon.Disassembler(config=cfg) as dis:
            reader = RoutedReader()
            ref = weakref.ref(reader)
            image_reader = ImageReader()
            image_ref = weakref.ref(image_reader)
            hints = dragon.AnalysisHints([0x1080], image_reader, reader)
            with dragon.PassPipeline(builtins=False) as pipeline:
                pipeline.register_finite_dispatch(code, 0x1000, entry_points=[0x1000], hints=hints).add('finite-dispatch')
                # Retain registration-time bridges, not a mutable source hints.
                hints.entry_points = []
                hints.read_known_memory = lambda *args: None
                hints.read_memory = None
                hints.__exit__()
                del hints, reader, image_reader
                gc.collect()
                assert ref() is not None
                assert image_ref() is not None
                for repeat in range(2):
                    if repeat:
                        pipeline.clear().add('finite-dispatch')  # Registry still owns callback.
                    with dis.analyze(code, 0x1000) as graph:
                        assert graph.get_instruction(0x100b).branches == []
                        report = pipeline.run(graph)
                        assert report['ok'] and report['changed']
                        assert set(graph.get_instruction(0x100b).branches) == {0x1030, 0x1040, 0x1050}
                        assert {0x1000, 0x1080} <= set(graph.function_addresses())
                assert ref().calls
                pipeline._install('finite-dispatch', dragon.PassStage.GRAPH,
                                  lambda ctx, options: None, override=True)
                gc.collect()
                assert ref() is None  # Successful override releases snapshot.
                assert image_ref() is None

            reader = RoutedReader()
            ref = weakref.ref(reader)
            failure = ValueError('registered known reader failed')
            reader.failure = failure
            hints = dragon.AnalysisHints([0x1080], read_known_memory=reader)
            pipeline = dragon.PassPipeline(builtins=False)
            pipeline.register_finite_dispatch(code, 0x1000, entry_points=[0x1000], hints=hints).add('finite-dispatch')
            ticks = []
            pipeline.register('after', lambda ctx, options: ticks.append(1), stage=dragon.PassStage.GRAPH).add('after')
            hints.__exit__()
            del hints, reader
            with dis.analyze(code, 0x1000) as failed_target:
                try:
                    pipeline.run(failed_target)
                except dragon.PipelineError as error:
                    assert error.__cause__ is failure
                    assert not error.report['ok'] and len(error.report['runs']) == 1
                    assert error.report['runs'][0]['status'] == dragon.PassStatus.ERROR
                else:
                    raise AssertionError('FiniteDispatch swallowed callback exception')
                assert ticks == []  # Native error must stop the next pass.
            ref().failure = None
            with dis.analyze(code, 0x1000) as recovered:
                assert pipeline.run(recovered)['ok']
                assert len(recovered.get_instruction(0x100b).branches) == 3
                assert ticks == [1]
            del failure  # A caller-held exception traceback can itself retain a reader.
            pipeline.close()
            gc.collect()
            assert ref() is None  # Close releases registration-time snapshot.


def check_active_owner(owner, analyze):
    expected = f'cannot close a {type(owner).__name__} during analysis'
    for method_name in ('__del__', '__exit__'):
        close = getattr(owner, method_name)
        reader = RoutedReader()
        closed_hints = []
        hints = dragon.AnalysisHints()

        def guarded(*args):
            try:
                close()
            except RuntimeError as error:
                assert str(error) == expected
            else:
                raise AssertionError('active analysis owner was closed')
            # Hints are separately snapshotted and remain legal to close.
            if not closed_hints:
                hints.__exit__()
                closed_hints.append(True)
            return reader(*args)

        hints.read_known_memory = guarded
        with analyze(hints) as graph:
            assert len(graph.get_instruction(0x100b).branches) == 3
        assert closed_hints and reader.calls

        def unhandled(*args):
            close()
            return reader(*args)

        with dragon.AnalysisHints(read_known_memory=unhandled) as failing:
            try:
                analyze(failing)
            except RuntimeError as error:
                assert str(error) == expected
            else:
                raise AssertionError('active close error was swallowed')
        # Both a successful callback and a callback error must release the
        # active counter in finally; the owner is still live and reusable.
        with dragon.AnalysisHints(read_known_memory=reader) as fresh:
            with analyze(fresh) as recovered:
                assert len(recovered.get_instruction(0x100b).branches) == 3


def test_active_disassembler():
    with dragon.Disassembler() as dis:
        check_active_owner(dis, lambda hints: dis.analyze(dispatch(), 0x1000, hints))
        assert dis.is_valid


def test_active_loaded_image():
    with dragon.LoadedImage.load_bytes(dispatch_elf()) as image:
        check_active_owner(image, lambda hints: image.disassemble(hints=hints))
        assert image.architecture == dragon.Architecture.X86_64


def test_failed_snapshot_is_released_on_override():
    class EphemeralReader:
        def __call__(self, *args):
            raise ValueError('fresh callback failure')

    code = routed_code()
    reader = EphemeralReader()
    ref = weakref.ref(reader)
    hints = dragon.AnalysisHints(read_known_memory=reader)
    with dragon.Disassembler() as dis, dragon.PassPipeline(builtins=False) as pipeline:
        pipeline.register_finite_dispatch(code, 0x1000, entry_points=[0x1000], hints=hints).add('finite-dispatch')
        hints.__exit__()
        del hints, reader

        def fail_without_retaining_exception():
            with dis.analyze(code, 0x1000) as target:
                try:
                    pipeline.run(target)
                except dragon.PipelineError as error:
                    assert isinstance(error.__cause__, ValueError)
                    assert str(error.__cause__) == 'fresh callback failure'
                else:
                    raise AssertionError('callback exception was swallowed')

        fail_without_retaining_exception()
        pipeline._install('finite-dispatch', dragon.PassStage.GRAPH,
                          lambda ctx, options: None, override=True)
        gc.collect()
        assert ref() is None
        with dis.analyze(code, 0x1000) as target:
            assert pipeline.run(target)['ok']


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
test_known_reader()
test_loaded_image_hints()
test_finite_dispatch_hints()
test_active_disassembler()
test_active_loaded_image()
test_failed_snapshot_is_released_on_override()
test_convention()
print('ok: entries, image and trusted readers, exact LOAD sites, lifetime, exceptions, calling convention')
