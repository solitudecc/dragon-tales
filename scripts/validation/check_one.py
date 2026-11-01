"""Robustness/consistency checks of one binary (run in a subprocess by
corpus_check.py so a crash is caught): disassembly vs objdump, empty
functions, blocks without IGNIL, edges to undecoded addresses, LLVM IR
validity (opt -passes=verify) and analysis time, in both strategies."""
import os as _os, sys as _sys
_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path.insert(0, _os.path.join(_HERE, '..', '..', 'python'))  # dragon.py
import json, re, subprocess, sys, tempfile, time
import dragon

path, objdump = sys.argv[1], sys.argv[2]
out = {'binary': _os.path.basename(path)}
img = dragon.LoadedImage.load_file(path)
if img is None:
    print(json.dumps({**out, 'error': 'load failed'})); sys.exit(0)
text = [s for s in img.sections() if s.executable and s.data]
fsyms = {s.address for s in img.symbols()
         if s.kind == dragon.SymbolKind.FUNCTION and not s.imported
         and any(t.address <= s.address < t.address + len(t.data) for t in text)}
od = subprocess.run([objdump, '-d', '--no-show-raw-insn', path], capture_output=True, text=True).stdout
od_insn = {int(m.group(1), 16) for m in re.finditer(r'^\s*([0-9a-f]+):\t\S', od, re.M)}

for strat in (0, 1):
    key = 'linear' if strat == 0 else 'recursive'
    t0 = time.time()
    g = img.disassemble(dragon.Configuration(strategy=strat))
    r = {'time': round(time.time() - t0, 2)}
    insns = {i.address for i in g.instructions()}
    blocks = g.block_addresses()
    if strat == 0:
        r['objdump_missing'] = len(od_insn - insns)
    r['blocks'] = len(blocks)
    r['blocks_without_ignil'] = sum(1 for b in blocks if g.get_ignil_block(b) is None)
    bad_edges = []
    for b in blocks:
        bb = g.get_block(b)
        for s in (bb.successors() if bb else []):
            if s not in insns:
                bad_edges.append(hex(b) + '->' + hex(s))
    r['edges_to_undecoded'] = len(bad_edges)
    r['edges_to_undecoded_eg'] = bad_edges[:3]
    funcs = list(g.function_addresses())
    valid = [a for a in funcs if g.get_function(a) and g.get_function(a).is_valid]
    r['functions'] = len(valid)
    r['empty_symbol_functions'] = len([a for a in funcs if a in fsyms and a not in set(valid)])
    # LLVM: one module with every function, then verify it.
    L = dragon.LLVMLifter(img.architecture)
    failed = 0
    for a in valid:
        try:
            if not L.lift(g.get_function(a), g):
                failed += 1
        except Exception:
            failed += 1
    r['llvm_lift_failed'] = failed
    with tempfile.NamedTemporaryFile('w', suffix='.ll', delete=False) as f:
        f.write(L.ir())
    v = subprocess.run(['opt-21', '-passes=verify', '-disable-output', f.name], capture_output=True, text=True)
    r['llvm_verify'] = 'ok' if v.returncode == 0 else v.stderr.strip().splitlines()[0][:200]
    _os.unlink(f.name)
    out[key] = r
print(json.dumps(out))
