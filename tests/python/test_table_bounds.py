"""The real-binary table oracle must not borrow unrelated comparisons."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'scripts', 'validation'))
from table_bounds import unsigned_index_bound


def bound(text, load=0x100c, index='w0'):
    return unsigned_index_bound(text.strip().splitlines(), load, index)


assert bound('''
1000: cmp w0, #0x5
1004: b.hi 1020 <default>
1008: nop
100c: ldrh w1, [x2, w0, uxtw #1]
1010: br x1
1020: ret
''') == 6
assert bound('''
1000: cmp w26, #0x3
1004: b.hi 1020 <default>
1008: nop
100c: ldrh w1, [x2, w0, uxtw #1]
1010: br x1
1020: ret
''') is None
assert bound('''
1000: cmp w0, #0x5
1004: b.hi 1020 <default>
1008: mov w0, #0x99
100c: ldrh w1, [x2, w0, uxtw #1]
1010: br x1
1020: ret
''') is None
assert bound('''
1000: cmp w0, #0x5
1004: b.lo 100c <table>
1008: ret
100c: ldrh w1, [x2, w0, uxtw #1]
1010: br x1
''') == 5
assert bound('''
0ff0: cbz x8, 100c <table>
1000: cmp w0, #0x5
1004: b.hi 1020 <default>
1008: nop
100c: ldrh w1, [x2, w0, uxtw #1]
1010: br x1
1020: ret
''') is None
assert bound('''
1000: cmp ecx,0x5
1003: ja 1020 <default>
1005: mov edi,ecx
1007: jmp 100c <table>
100c: jmp DWORD PTR [edi*4+0x3000]
1020: ret
''', index='edi') == 6
print('ok: table bounds follow the index and all incoming paths')
