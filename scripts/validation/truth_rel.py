"""Check relative jump tables resolved by dragon against the image.
x86-64:  lea rD,[rip+..] # T ; movsxd rax,[rD+rI*4] ; add rax,rD ; jmp rax
aarch64: adrp/add xT ; ldr{b,h} wE,[xT,wI,uxtw..] ; adr xL, L ; add xR,xL,wE,sxt{b,h} #2 ; br xR
N comes from the closest preceding `cmp <idx>, K` (ja/b.hi) as K+1."""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import sys, re, subprocess, bisect
import dragon
path, objdump, strat = sys.argv[1], sys.argv[2], int(sys.argv[3])
img = dragon.LoadedImage.load_file(path)
g = img.disassemble(dragon.Configuration(strategy=strat))
blocks = sorted(g.block_addresses())
args = [objdump, "-d", "--no-show-raw-insn", path] + (["-M", "intel"] if "mingw" in objdump else [])
lines = subprocess.run(args, capture_output=True, text=True).stdout.splitlines()
def rd(a, n, signed):
    v = int.from_bytes(bytes(img.read_virtual(a, n)), "little")
    return v - (1 << (8 * n)) if signed and v >> (8 * n - 1) else v
stat = dict(total=0, exact=0, wrong=0, unresolved=0, nobound=0)
for k, l in enumerate(lines):
    win = lines[max(0, k - 12):k]
    a = None
    if re.search(r"\tjmp\s+rax$", l) and any("movsxd" in x for x in win[-3:]):
        # A loop may hoist the actual table base far above the dispatch.
        # Match the base register used by the load, not the nearest unrelated
        # LEA (str_unpack also loads a string address into R8 just beforehand).
        load = next((re.search(r"\tmovsxd\s+rax,DWORD PTR \[(\w+)\+\w+\*4(?:\+(0x[0-9a-f]+))?\]", x)
                     for x in reversed(win[-3:]) if "movsxd" in x), None)
        if not load: continue
        lea = None
        for x in reversed(lines[:k]):
            if re.search(r"^[0-9a-f]+ <[^>]+>:$", x): break
            if re.search(r"\tlea\s+" + re.escape(load.group(1)) + r",\[rip\+", x):
                lea = x; break
        if not lea: continue
        reg = int(re.search(r"#\s*([0-9a-f]+)", lea).group(1), 16)
        T = reg + int(load.group(2) or "0", 0)   # the table: [reg + idx*4 + disp]
        size, shift, signed, base = 4, 0, True, reg  # targets: reg + entry
        a = int(l.split(":")[0], 16)
    m = re.search(r"\tbr\s+(x\d+)$", l)
    if m and any(re.search(r"\tadd\s+x\d+, x\d+, w\d+, sxt[bh] #2", x) for x in win[-2:]):
        ldr = next((x for x in reversed(win) if re.search(r"\tldr[bh]\s+w\d+, \[x\d+, w\d+, uxtw", x)), None)
        adr = next((x for x in reversed(win) if re.search(r"\tadr\s+x\d+, ", x)), None)
        addl = next((x for x in reversed(win) if re.search(r"\tadd\s+x\d+, x\d+, #0x[0-9a-f]+$", x)), None)
        adrp = next((x for x in reversed(win) if re.search(r"\tadrp\s+x\d+, ", x)), None)
        if not (ldr and adr and addl and adrp): continue
        T = int(re.search(r"adrp\s+x\d+, ([0-9a-f]+)", adrp).group(1), 16) + int(re.search(r"#(0x[0-9a-f]+)$", addl).group(1), 16)
        base = int(re.search(r"adr\s+x\d+, ([0-9a-f]+)", adr).group(1), 16)
        size = 1 if "ldrb" in ldr else 2
        shift, signed = 2, True
        a = int(l.split(":")[0], 16)
    if a is None: continue
    stat["total"] += 1
    N = None
    for x in reversed(lines[max(0, k - 25):k]):
        # register or memory operand: `cmp eax,0x6`, `cmp w0, #6`,
        # `cmp BYTE PTR [rbx+0x65],0x8`
        c = re.search(r"\tcmp\s+(?:\w+|(?:BYTE|WORD|DWORD|QWORD) PTR \[[^\]]+\]), ?#?(0x[0-9a-f]+|\d+)$", x)
        if c: N = int(c.group(1), 0) + 1; break
    blk = blocks[bisect.bisect_right(blocks, a) - 1]
    succ = set(g.get_block(blk).successors())
    if len(succ) <= 1:
        stat["unresolved"] += 1
        if stat["unresolved"] <= 4: print("UNRESOLVED", hex(a), "|", " ; ".join(x.split("\t")[-1] for x in win[-6:]))
        continue
    if N is None: stat["nobound"] += 1; continue
    truth = {(base + (rd(T + i * size, size, signed) << shift)) & (2**64 - 1) for i in range(N)}
    if succ == truth: stat["exact"] += 1
    else:
        stat["wrong"] += 1
        print("MISMATCH", hex(a), "N", N, "missing", [hex(x) for x in truth - succ][:3], "extra", [hex(x) for x in succ - truth][:3])
print(path, "linear" if strat == 0 else "recursive", stat)
