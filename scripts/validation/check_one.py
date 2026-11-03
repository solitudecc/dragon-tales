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
llvm_mode=_os.environ.get('LLVM_MODE','legacy')
if llvm_mode not in ('legacy','v2','static'): raise ValueError('LLVM_MODE must be legacy, v2 or static')
optimize=_os.environ.get('LLVM_OPTIMIZE','1' if llvm_mode=='static' else '0')
if optimize not in ('0','1'):raise ValueError('LLVM_OPTIMIZE must be 0 or 1')
out = {'binary': _os.path.basename(path),'llvm_mode':llvm_mode,'llvm_optimize':optimize=='1'}
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
    # Verify every function. V2 uses bounded modules to fit the 3 GB VM;
    # no function is capped/skipped. Native fixtures separately exercise
    # modules containing multiple V2 implementations and runtime dispatch.
    mode={'legacy':dragon.LLVMExecutionMode.LEGACY,'v2':dragon.LLVMExecutionMode.STATEFUL_V2,
          'static':dragon.LLVMExecutionMode.STATIC_ANALYSIS}[llvm_mode]
    # Static modules retain opaque call boundaries and can grow large.
    # Smaller batches limit extra module memory on the 3 GB VM;
    # every function is still checked before and after the same pipeline.
    chunk_size=32 if llvm_mode=='static' else 128 if llvm_mode=='v2' else max(1,len(valid))
    r['llvm_chunk_size']=chunk_size
    failed=verified=modules=import_references=0
    optimized_verified=optimized_modules=0
    verification_errors=[];optimized_errors=[]
    for start in range(0,max(1,len(valid)),chunk_size):
        L=dragon.LLVMLifter(img.architecture,execution_mode=mode)
        accepted=0
        for a in valid[start:start+chunk_size]:
            try:
                if L.lift(g.get_function(a),g):accepted+=1
                else:failed+=1
            except Exception:failed+=1
        with tempfile.TemporaryDirectory(prefix='dragon-check-') as directory:
            path_ir=_os.path.join(directory,'module.ll')
            ir=L.ir()
            metadata=re.search(r'^!dragon\.imports = !\{([^\n]*)\}',ir,re.M)
            if metadata:import_references+=len(re.findall(r'!\d+',metadata[1]))
            with open(path_ir,'w') as f:f.write(ir)
            v=subprocess.run(['opt-21','-passes=verify','-disable-output',path_ir],capture_output=True,text=True)
            if optimize=='1':
                L.optimize()
                with open(path_ir,'w') as f:f.write(L.ir())
                after=subprocess.run(['opt-21','-passes=verify','-disable-output',path_ir],capture_output=True,text=True)
                optimized_modules+=1
                if after.returncode==0:optimized_verified+=accepted
                else:optimized_errors.append((after.stderr.strip().splitlines() or [f'opt exit {after.returncode}'])[0][:200])
        modules+=1
        if v.returncode==0:verified+=accepted
        else:verification_errors.append((v.stderr.strip().splitlines() or [f'opt exit {v.returncode}'])[0][:200])
        L.__del__()
    r['llvm_lift_failed']=failed
    r['llvm_verified_functions']=verified
    r['llvm_verified_modules']=modules
    r['llvm_import_references']=import_references
    r['llvm_verify']='ok' if not verification_errors else verification_errors[0]
    r['llvm_optimized_verified_functions']=optimized_verified
    r['llvm_optimized_verified_modules']=optimized_modules
    r['llvm_optimized_verify']='ok' if not optimized_errors else optimized_errors[0]
    out[key] = r
print(json.dumps(out))
