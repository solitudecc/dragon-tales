import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import sys, re; import dragon
from collections import Counter
path = sys.argv[1]
img = dragon.LoadedImage.load_file(path); g = img.disassemble(dragon.Configuration(strategy=0))
ia = {i.address: i for i in g.instructions()}
c = Counter(); total = 0
for b in g.block_addresses():
    ib = g.get_ignil_block(b)
    if not ib: continue
    for a in ib.unlifted():
        i = ia.get(a)
        if not i: continue
        ops = i.operands if hasattr(i, 'operands') else ''
        form = re.sub(r'\b(r|e)?[a-d]x\b|\b[re]?(si|di|bp|sp)\b|\br\d+[dwb]?\b|\b[a-d][lh]\b|\b[sd]il\b|\b[xw]\d+\b|\b[wx]zr\b|\bsp\b|\bxmm\d+\b|\bst\(\d\)|\b[dsqhb]\d+\b|\bv\d+\.\w+', 'R', ops)
        form = re.sub(r'(byte|word|dword|qword|xmmword|tbyte) ptr \[[^\]]*\]', r'\1[M]', form)
        form = re.sub(r'\[[^\]]*\]!?', '[M]', form)
        form = re.sub(r'#?-?0x[0-9a-f]+|#-?\d+|\b\d+\b', 'I', form)
        c[f'{i.mnemonic} {form}'] += 1; total += 1
print(path, 'unlifted', total)
for k, v in c.most_common(int(sys.argv[2]) if len(sys.argv) > 2 else 30): print(f'  {v:5d}  {k}')
