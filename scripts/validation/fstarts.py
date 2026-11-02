import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import sys, bisect, time; import dragon
from collections import Counter
path = sys.argv[1]
truth_path = sys.argv[2] if len(sys.argv) > 2 else path
img = dragon.LoadedImage.load_file(path)
truth = dragon.LoadedImage.load_file(truth_path)
assert img is not None and truth is not None, 'image or truth failed to load'
sizes = {}
for symbol in truth.symbols():
    if symbol.kind == dragon.SymbolKind.FUNCTION and symbol.address:
        sizes[symbol.address] = max(sizes.get(symbol.address, 0), symbol.size)
# COFF symbols have no sizes. PE exception records supply actual function
# extents, so an interior hit can be classified without guessing next-symbol
# boundaries. Leaf functions missing from .pdata keep their unknown size.
if truth.format == dragon.Format.PE and truth.is_64bit:
    import pefile
    pe = pefile.PE(truth_path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    for entry in getattr(pe, 'DIRECTORY_ENTRY_EXCEPTION', []):
        address = pe.OPTIONAL_HEADER.ImageBase + entry.struct.BeginAddress
        if address in sizes and entry.struct.EndAddress > entry.struct.BeginAddress:
            sizes[address] = entry.struct.EndAddress - entry.struct.BeginAddress
syms = sorted(sizes.items())
A = [s[0] for s in syms]; S = set(A)
for strat in (0, 1):
    started = time.monotonic()
    g = img.disassemble(dragon.Configuration(strategy=strat))
    elapsed = time.monotonic() - started
    c = Counter()
    for a in g.function_addresses():
        if a in S: c['symbol'] += 1; continue
        i = bisect.bisect_right(A, a) - 1
        inside = i >= 0 and a < syms[i][0] + syms[i][1]
        ins = g.get_instruction(a)
        c['inside a symbol body' if inside else ('padding (nop)' if ins and ins.mnemonic in ('nop', 'int3') else 'other')] += 1
    recovered = set(g.function_addresses())
    print(path, 'linear' if strat == 0 else 'recursive', dict(c),
          f'recall={len(recovered & S)}/{len(S)}', f'instructions={len(g)}', f'seconds={elapsed:.3f}')
