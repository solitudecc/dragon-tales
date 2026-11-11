"""Opt-in entry-prefix routing and refusal; native semantics have a separate oracle."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'python'))
import dragon as d


def rejected(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError('invalid entry-prefix options accepted')


assert 'entry-prefix' not in d.PassPipeline.DEFAULT_LLVM
with d.PassPipeline() as pipeline:
    assert 'entry-prefix' in {p['name'] for p in pipeline.passes()}
    for options in ({'unknown': 1}, {'max_blocks': True}, {'max_blocks': -1},
                    {'max_instructions': '2048'}):
        rejected(lambda: pipeline.add('entry-prefix', **options))

# Load a Guest cell; test EDI; decrement and loop to the machine entry; RET.
code = bytes.fromhex('488b04250040000085ff7404ffcfebf0c3')
facts = d.FiniteEntryState(0x1000, registers=[(32, 64, 0x800)],
                           memory=[(0x4000, (14).to_bytes(8, 'little'))])
with d.Disassembler(d.Architecture.X86_64).analyze(code, 0x1000) as graph:
    with d.LLVMLifter(d.Architecture.X86_64, execution_mode=2,
                     static_entry_policy=1) as source:
        assert source.lift(graph.get_function(0x1000), graph)
        original = source.ir()
        with source.guarded_entry_view(facts, context_id='prefix',
                                       provenance='Python routing fixture') as view:
            before = view.ir()
            with d.PassPipeline() as limited:
                limited.add('entry-prefix', max_blocks=1, max_instructions=1)
                report = limited.run(view, max_iterations=1, max_pass_runs=1)
            counters = report['runs'][0]['counters']
            assert counters['candidate_edges'] == 1
            assert counters['peeled_edges'] == 0
            assert counters['budget_exhausted'] > 0
            assert not counters['complete']
            assert view.ir() == before
            with d.PassPipeline() as pipeline:
                pipeline.add('entry-prefix', max_blocks=32, max_instructions=2048)
                report = pipeline.run(view, max_iterations=1, max_pass_runs=1)
                counters = report['runs'][0]['counters']
                assert counters['peeled_edges'] == 1
                assert 0 < counters['cloned_blocks'] <= 32
                assert 0 < counters['cloned_instructions'] <= 2048
                changed = view.ir()
                again = pipeline.run(view, max_iterations=1, max_pass_runs=1)
                assert again['runs'][0]['counters']['peeled_edges'] == 0
                assert view.ir() == changed
            assert source.ir() == original

print('Entry prefix: opt-in routing, strict options, refusal and single-use entry passed')
