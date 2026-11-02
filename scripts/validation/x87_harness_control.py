"""Negative control: corrupt only the tested side after seeding Unicorn.
Usage: x87_harness_control.py diff|llvm BINARY [BLOCKS]
Success means the real-block harness reports DIFFERENT, never merely a crash.
The normal harnesses and reference state are untouched.
"""
import contextlib
import io
import os
import re
import runpy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
import x87_state

mode, binary = sys.argv[1:3]
if mode not in ('diff', 'llvm'):
    raise SystemExit('mode must be diff or llvm')
blocks = sys.argv[3] if len(sys.argv) > 3 else '5'
original_seed = x87_state.seed_uc

def corrupt_tested_state(uc, state):
    original_seed(uc, state)
    # Unicorn retains the original state; executor/native input uses the
    # changed raw mantissa and CW.IC. The latter cannot alter arithmetic.
    for index, (reg, value) in enumerate(state):
        if reg[0] == 400:
            state[index] = (reg, value ^ 1)
        elif reg[0] == 484:
            state[index] = (reg, value ^ 0x1000)

x87_state.seed_uc = corrupt_tested_state
os.environ['REALBLOCK_FILTER'] = 'fld'
harness = Path(__file__).with_name('realblock_diff.py' if mode == 'diff' else 'realblock_llvm.py')
sys.argv = [str(harness), binary, blocks] + (['1'] if mode == 'diff' else [])
output = io.StringIO()
with contextlib.redirect_stdout(output):
    runpy.run_path(str(harness), run_name='__main__')
result = output.getvalue()
print(result, end='')
count = re.search(r"'DIFFERENT':\s*(\d+)", result)
if not count or int(count[1]) < 1 or 'NATIVE CRASH' in result:
    raise SystemExit('negative control did not produce a measured difference')
print('x87 negative control detected')
