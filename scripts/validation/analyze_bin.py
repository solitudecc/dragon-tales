"""Basic end-to-end analysis of a real binary with dragon, checked against
objdump and the symbol table."""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
_sys.path.insert(0, _HERE)
import os, re, subprocess, sys, time
from collections import Counter

import dragon

path, objdump = sys.argv[1], sys.argv[2]
strategy = int(os.environ.get('STRATEGY', 1))
lift_llvm = os.environ.get('LLVM', '1') == '1'

t0 = time.time()
img = dragon.LoadedImage.load_file(path)
t_load = time.time() - t0
arch = img.architecture
text = [s for s in img.sections() if s.executable and s.data]
print(f'== {os.path.basename(path)}  arch={arch} base={img.image_base:#x} entry={img.entry:#x}')
print(f'   exec sections: ' + ', '.join(f'{s.name}@{s.address:#x}+{len(s.data):#x}' for s in text))

# ground truth: objdump instructions + function symbols
od = subprocess.run([objdump, '-d', '--no-show-raw-insn', path], capture_output=True, text=True).stdout
od_insn = {}
for m in re.finditer(r'^\s*([0-9a-f]+):\t(\S+)', od, re.M):
    od_insn[int(m.group(1), 16)] = m.group(2)
in_text = lambda a: any(s.address <= a < s.address + len(s.data) for s in text)
fsyms = {s.address: s.name for s in img.symbols()
         if s.kind == dragon.SymbolKind.FUNCTION and not s.imported and in_text(s.address)} \
    if hasattr(dragon, 'SymbolKind') else {}

cfg = dragon.Configuration(strategy=strategy)
t0 = time.time()
g = img.disassemble(cfg)
t_dis = time.time() - t0
insns = g.instructions()
ia = {i.address: i for i in insns}
blocks = g.block_addresses()
funcs = [a for a in g.function_addresses() if g.get_function(a) and g.get_function(a).is_valid]
print(f'   load {t_load:.2f}s  disassemble+lift {t_dis:.1f}s ({"recursive" if strategy else "linear"})')
print(f'   instructions {len(ia)}  blocks {len(blocks)}  functions {len(funcs)} valid / {len(g.function_addresses())} starts')

# 1. disassembly agreement with objdump
common = set(ia) & set(od_insn)
print(f'   vs objdump: {len(od_insn)} insns; dragon covers {len(common)} ({100*len(common)/max(1,len(od_insn)):.1f}%), '
      f'{len(set(ia) - set(od_insn))} dragon-only addresses')

# 2. function starts vs symbols
if fsyms:
    starts = set(g.function_addresses())
    hit = starts & set(fsyms)
    print(f'   function symbols {len(fsyms)}: found {len(hit)} (recall {100*len(hit)/len(fsyms):.1f}%), '
          f'{len(starts - set(fsyms))} starts with no symbol')
    invalid = [a for a in g.function_addresses() if not (g.get_function(a) and g.get_function(a).is_valid)]
    inv_sym = [a for a in invalid if a in fsyms]
    print(f'   invalid function views: {len(invalid)} ({len(inv_sym)} of them symbols, e.g. ' + ', '.join(fsyms[a] for a in inv_sym[:6]) + ')')
    nb = sum(1 for a in inv_sym if a not in set(blocks))
    print(f'     of those symbol starts, not a block start: {nb}')

# 3. IGNIL coverage
unl = Counter(); nolift = 0; nins = 0
ind_total = ind_resolved = 0; ind_examples = []; nsucc = Counter(); bad_targets = 0
for b in blocks:
    ib = g.get_ignil_block(b)
    bb = g.get_block(b)
    nins += bb.size if bb else 0
    if ib is None:
        nolift += 1; continue
    for a in ib.unlifted():
        i = ia.get(a)
        unl[i.mnemonic if i else '?'] += 1
    if ib.size:
        last = ib.op_to_string(ib.size - 1)
        if last.startswith('JMP(') and not re.match(r'JMP\(0x', last):
            ind_total += 1
            succ = bb.successors() if bb else []
            if succ:
                ind_resolved += 1
                nsucc[len(succ)] += 1
                bad_targets += sum(1 for t in succ if t not in od_insn)
            elif len(ind_examples) < 4:
                ins = bb.instructions()
                ind_examples.append(f'{b:#x}: {ins[-1].mnemonic if ins else "?"}')
total_unl = sum(unl.values())
print(f'   IGNIL: {len(blocks)-nolift}/{len(blocks)} blocks lifted; unlifted insns {total_unl}/{nins} ({100*total_unl/max(1,nins):.2f}%)')
print('     top unlifted: ' + ', '.join(f'{m}×{c}' for m, c in unl.most_common(12)))
print(f'   indirect jumps: {ind_total}, with resolved successors {ind_resolved}; targets not on an objdump insn: {bad_targets}')
print('     successors per resolved jump: ' + ', '.join(f'{k}:{v}' for k, v in sorted(nsucc.items())))
od_tables = len(re.findall(r'\tjmp\s+\*0x[0-9a-f]+\(,%[a-z0-9]+,[48]\)', od))
od_ind = len(re.findall(r'\tjmp\s+\*', od)) + len(re.findall(r'\tbr\s', od))
print(f'     objdump: {od_ind} indirect jmp/br, {od_tables} of form jmp *table(,idx,scale)')
if ind_examples: print('     unresolved e.g. ' + '; '.join(ind_examples))

# 4. LLVM
if lift_llvm:
    ok = fail = 0; t0 = time.time(); sigs = []
    for a in funcs:
        L = dragon.LLVMLifter(arch)
        try:
            if L.lift(g.get_function(a), g):
                ok += 1
                if len(sigs) < 3 and a in fsyms:
                    m = re.search(r'"dragon.signature"="([^"]+)"', L.ir())
                    if m: sigs.append(f'{fsyms[a]}: {m.group(1)}')
            else: fail += 1
        except Exception as e:
            fail += 1
    print(f'   LLVM: lifted {ok}/{ok+fail} functions in {time.time()-t0:.1f}s')
    for s in sigs: print('     ' + s)
