import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import sys, re, subprocess, struct
import dragon
from table_bounds import index_bound_reader
path, objdump = sys.argv[1], sys.argv[2]
img = dragon.LoadedImage.load_file(path); g = img.disassemble(dragon.Configuration(strategy=int(sys.argv[3]) if len(sys.argv) > 3 else 0))
lines = subprocess.run([objdump, "-d", "-M", "intel", "--no-show-raw-insn", path], capture_output=True, text=True).stdout.splitlines()
index_bound = index_bound_reader(lines)
blocks = sorted(g.block_addresses())
import bisect
ok = bad = unres = unknown = unreadable = 0
for k, l in enumerate(lines):
    m = re.match(r"\s*([0-9a-f]+):\s+jmp\s+DWORD PTR \[(\w+)\*4\+0x([0-9a-f]+)\]", l)
    if not m: continue
    a, T = int(m.group(1), 16), int(m.group(3), 16)
    N = index_bound(a, m.group(2))
    blk = blocks[bisect.bisect_right(blocks, a) - 1]
    succ = set(g.get_block(blk).successors())
    if len(succ) <= 1:
        unres += 1; print("UNRESOLVED", hex(a), "| " + " ; ".join(x.split("\t")[-1].strip() for x in lines[max(0,k-5):k+1])); continue
    if N is None: unknown += 1; continue
    values = [img.read_virtual(T + 4*i, 4) for i in range(N)]
    if any(value is None or len(value) != 4 for value in values):
        unreadable += 1
        print('NO ORACLE', hex(a), 'table', hex(T), 'count', N, '(unmapped range)')
        continue
    truth = {struct.unpack("<I", bytes(value))[0] for value in values}
    if succ == truth: ok += 1
    else:
        bad += 1; print("MISMATCH", hex(a), "N", N, "missing", [hex(x) for x in truth - succ][:4], "extra", [hex(x) for x in succ - truth][:4])
print(f"tables: exact {ok}, wrong {bad}, unresolved {unres}, no visible bound {unknown}, unreadable {unreadable}")
