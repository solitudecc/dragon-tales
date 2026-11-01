"""Assembler round trip on real instructions.

Every instruction of a binary (branches and PC-relative forms excluded: the
assembler has no address) is printed as text, assembled, and the bytes are
disassembled again. The text must come back the same; a different text is a
printer/parser disagreement, an assembly error is a text the assembler does
not accept.

Usage: asm_roundtrip.py BINARY [MAX_DISTINCT]
"""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
import sys
from collections import Counter
import dragon

path = sys.argv[1]
LIMIT = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
img = dragon.LoadedImage.load_file(path)
arch = img.architecture
g = img.disassemble(dragon.Configuration(strategy=0))
asm = dragon.Assembler(arch, dragon.Configuration())
dis = dragon.Disassembler(arch, dragon.Configuration())
stats = Counter()
shown = Counter()
seen = set()
for b in sorted(g.block_addresses()):
    bb = g.get_block(b)
    if not bb or not bb.is_valid:
        continue
    for i in bb.instructions():
        if len(seen) >= LIMIT:
            break
        text = f'{i.mnemonic} {i.operands}'.strip()
        if i.is_jump or i.is_call or 'rip' in i.operands or 'pc' == i.operands[-2:] or \
                i.mnemonic.lower() in ('adr', 'adrp', 'ldr') and '#' not in i.operands and '[' not in i.operands:
            continue
        if text in seen:
            continue
        seen.add(text)
        try:
            out = bytes(asm.assemble(text))
        except RuntimeError:
            stats['assembler rejects'] += 1
            if shown['rej'] < 15:
                shown['rej'] += 1
                print(f'REJECT {i.address:#x}: {text}  ({bytes(i.bytes).hex()})')
            continue
        back = list(dis.disassemble(out, i.address).instructions())
        again = ' ; '.join(f'{x.mnemonic} {x.operands}'.strip() for x in back)
        if again == text:
            stats['same'] += 1
        elif out == bytes(i.bytes):
            stats['same bytes, different text?'] += 1
        else:
            stats['DIFFERENT'] += 1
            if shown['diff'] < 25:
                shown['diff'] += 1
                print(f'DIFF {i.address:#x}: {text}  ({bytes(i.bytes).hex()}) -> {again}  ({out.hex()})')
print(_os.path.basename(path), dict(stats))
