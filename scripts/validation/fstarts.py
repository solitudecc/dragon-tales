import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import sys, bisect; import dragon
from collections import Counter
path = sys.argv[1]
img = dragon.LoadedImage.load_file(path)
syms = sorted((s.address, s.size) for s in img.symbols() if s.kind == dragon.SymbolKind.FUNCTION and s.address)
A = [s[0] for s in syms]; S = set(A)
for strat in (0, 1):
    g = img.disassemble(dragon.Configuration(strategy=strat))
    c = Counter()
    for a in g.function_addresses():
        if a in S: c['symbol'] += 1; continue
        i = bisect.bisect_right(A, a) - 1
        inside = i >= 0 and a < syms[i][0] + syms[i][1]
        ins = g.get_instruction(a)
        c['inside a symbol body' if inside else ('padding (nop)' if ins and ins.mnemonic in ('nop', 'int3') else 'other')] += 1
    print(path, 'linear' if strat == 0 else 'recursive', dict(c))
