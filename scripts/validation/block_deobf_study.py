"""How well does Dragon undo basic-block obfuscation?

MBA: seven 32-bit operations, Tigress EncodeArithmetic applied once and
twice, clang O0/O2. Per function: machine instructions, IGNIL ops before and
after simplify()/dead_store_eliminate(), product-pipeline LLVM O2 instruction
count (and whether it is the single original operation), and whether the
symbolic executor PROVES the result equals the original operation (Z3).

Opaque predicates: hand-written algebraic ones (always true/false over
32-bit modular arithmetic, checked here by exhaustive reasoning in the
comments and by Z3), a non-opaque control that must keep both edges, and
Tigress AddOpaque (global list structures). Per conditional branch: does
symbolic execution of the function's blocks from entry keep only the real
edge?

Usage: block_deobf_study.py --tigress RUN-TIGRESS --out DIR
"""
import argparse
import collections
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / 'python'))
import dragon  # noqa: E402
from dragon import x86  # noqa: E402

OPS = {'add': 'a + b', 'sub': 'a - b', 'xor': 'a ^ b', 'and': 'a & b', 'or': 'a | b',
       'mul': 'a * b', 'mix': '(a ^ b) + 2u * (a & b)'}
MBA_SRC = 'typedef unsigned int u32;\n' + ''.join(
    f'u32 f_{n}(u32 a, u32 b) {{ return {e}; }}\n' for n, e in OPS.items())
# Each predicate is a fact of 32-bit modular arithmetic:
#   a*(a+1) is a product of consecutive integers, hence even;
#   7b^2-1 mod 8 is in {7,6,3} while a^2 mod 8 is in {0,1,4};
#   a|b >= a^b bitwise (unsigned); a^2 mod 4 is 0 or 1, never 2.
OPAQUE_SRC = '''typedef unsigned int u32;
u32 p_even(u32 a, u32 b) { if (((a * (a + 1u)) & 1u) == 0u) return a + b; return a - b; }
u32 p_mod8(u32 a, u32 b) { if (7u * b * b - 1u != a * a) return a ^ b; return 0u; }
u32 p_orxor(u32 a, u32 b) { if ((a | b) >= (a ^ b)) return a & b; return a | b; }
u32 p_sq4(u32 a, u32 b) { if (((a * a) & 3u) == 2u) return 7u; return a + 1u; }
u32 p_real(u32 a, u32 b) { if ((a & 7u) == 3u) return a; return b; }
u32 t_plain(u32 a, u32 b) { u32 r = a; if (a > b) r = a - b; else r = b - a; return r * 3u; }
'''
OPAQUE_KIND = {'p_even': 'opaque-true', 'p_mod8': 'opaque-true', 'p_orxor': 'opaque-true',
               'p_sq4': 'opaque-false', 'p_real': 'real', 't_plain': 'real'}


def run(cmd, cwd):
    p = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True, timeout=900)
    if p.returncode:
        raise RuntimeError(' '.join(map(str, cmd))[-300:] + '\n' + p.stderr[-1500:])
    return p.stdout


def compile_obj(src, work, level, clang):
    run([clang, '--target=x86_64-linux-gnu', '-' + level, '-fno-pic', '-fno-stack-protector',
         '-fno-asynchronous-unwind-tables', '-w', '-c', src, '-o', 'out.o'], work)
    img = dragon.LoadedImage.load_file(str(work / 'out.o'))
    funcs = {s.name: s for s in img.symbols() if s.kind == dragon.SymbolKind.FUNCTION and not s.imported}
    return img, funcs


def blocks_of(graph, fn):
    return sorted(graph.get_function(fn).block_addresses())


def mba_symbolic(graph, entry, op):
    """Execute the function's single path with symbolic a/b; return whether
    EAX is proved equal to the original operation and the size (number of
    SMT-LIB operators) of the executor's expression for EAX."""
    with dragon.SymbolicExecutor() as ex:
        ex.symbolize_register(x86.RDI, 'a')
        ex.symbolize_register(x86.RSI, 'b')
        ex.set_register(x86.RSP, 0x7fff0000)
        ex.set_register(x86.RBP, 0x7fff0100)
        a, b = ex.read_register(x86.EDI), ex.read_register(x86.ESI)
        addr = entry
        for _ in range(64):
            r = ex.run(graph.get_ignil_block(addr))
            if not r.ok:
                raise RuntimeError(f'symbolic error at {addr:#x}: {r.error}')
            if graph.get_instruction(graph.get_block(addr).terminator_address).is_return:
                break
            if len(r.successors) != 1:
                raise RuntimeError('MBA function is not a single path')
            addr = r.successors[0].address
        eax = ex.read_register(x86.EAX)
        want = {'add': a + b, 'sub': a - b, 'xor': a ^ b, 'and': a & b, 'or': a | b,
                'mul': a * b, 'mix': (a ^ b) + ex.const(2, 32) * (a & b),
                # what each opaque function really computes
                'p_even': a + b, 'p_mod8': a ^ b, 'p_orxor': a & b, 'p_sq4': a + ex.const(1, 32)}[op]
        return ex.proves(eax == want), eax.to_smt2().count('(')


def branch_edges(graph, blocks):
    """For every block ending in a conditional jump: feasible successors when
    the block runs alone with every register and memory input unknown."""
    out = {}
    for addr in blocks:
        insn = graph.get_instruction(graph.get_block(addr).terminator_address)
        if not (insn.is_jump and insn.is_conditional):
            continue
        with dragon.SymbolicExecutor() as ex:
            for reg in (x86.RAX, x86.RBX, x86.RCX, x86.RDX, x86.RSI, x86.RDI, x86.R8, x86.R9,
                        x86.R10, x86.R11, x86.R12, x86.R13, x86.R14, x86.R15):
                ex.symbolize_register(reg)
            ex.set_register(x86.RSP, 0x7fff0000)
            ex.set_register(x86.RBP, 0x7fff0100)
            r = ex.run(graph.get_ignil_block(addr))
            out[addr] = len(r.successors) if r.ok else None
    return out


def return_cone(ir, symbol):
    """Arithmetic instructions in the def-use cone of the value stored to
    RAX (offset 0 of the register file) on the normal-return path."""
    m = re.search(r'^define [^\n]*@' + re.escape(symbol) + r'\([^\n]*\{\n(.*?)^\}', ir, re.M | re.S)
    if not m:
        return None, []
    body = m[1]
    defs = dict(re.findall(r'^\s+(%[\w.]+) = (.*)$', body, re.M))
    base = re.search(r'(%[\w.]+) = getelementptr i8, ptr %state, i64 24\n\s+(%[\w.]+) = load ptr, ptr \1', body)
    if not base:
        return None, []
    regs = base[2]
    # the return path stores Exit.reason 1 (Return) before the snapshot
    ret_block = next((blk for blk in re.split(r'\n(?=[\w.]+:)', body) if 'store i64 1, ptr %exit' in blk), '')
    store = re.search(r'store i64 (%[\w.]+), ptr ' + re.escape(regs) + r', align', ret_block)
    if not store:
        return 0, []
    seen, ops, work = set(), collections.Counter(), [store[1]]
    while work:
        v = work.pop()
        if v in seen or v not in defs:
            continue
        seen.add(v)
        text = defs[v]
        opcode = text.split()[0]
        if opcode in ('load', 'getelementptr', 'phi'):
            continue
        ops[opcode] += 1
        work.extend(re.findall(r'%[\w.]+', text))
    return sum(ops.values()), sorted(ops)


def analyse(img, funcs, names, mba):
    graph = img.disassemble()
    rows = []
    for name in names:
        fn = funcs[name].address
        blocks = blocks_of(graph, fn)
        insns = sum(len(graph.get_block(b).instructions()) for b in blocks)
        raw = simp = dse = 0
        for b in blocks:
            ib = graph.get_ignil_block(b)
            raw += ib.size
            s, _ = ib.simplify()
            simp += s.size
            d, _ = s.dead_store_eliminate(dragon.Architecture.X86_64)
            dse += d.size
        row = dict(function=name, blocks=len(blocks), machine_insns=insns, ignil=raw,
                   ignil_simplified=simp, ignil_after_dse=dse)
        lifter = dragon.LLVMLifter(dragon.Architecture.X86_64, execution_mode=dragon.LLVMExecutionMode.STATIC_ANALYSIS)
        if lifter.lift(graph.get_function(fn), graph):
            lifter.optimize()
            row['llvm_return_cone'], row['llvm_cone_ops'] = return_cone(lifter.ir(), f'dragon_static_x86_64_{fn:x}')
        try:
            if mba:
                row['proved_equal'], row['z3_expr_ops'] = mba_symbolic(graph, fn, name[2:])
            else:
                edges = branch_edges(graph, blocks)
                row['kind'] = OPAQUE_KIND[name]
                if OPAQUE_KIND[name].startswith('opaque'):
                    # also covers predicates the compiler turned into select/cmov
                    row['proved_equal'], row['z3_expr_ops'] = mba_symbolic(graph, fn, name)
                row['branches'] = len(edges)
                row['decided_single_edge'] = sum(1 for v in edges.values() if v == 1)
                row['both_edges'] = sum(1 for v in edges.values() if v == 2)
        except Exception as error:
            row['symbolic_error'] = str(error)[:300]
        rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tigress', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--clang', default='clang-21')
    args = ap.parse_args()
    out = args.out.resolve()
    results = []
    variants = [('mba-0', MBA_SRC, []),
                ('mba-1', MBA_SRC, ['--Transform=EncodeArithmetic', '--Functions=' + ','.join('f_' + n for n in OPS)]),
                ('mba-2', MBA_SRC, ['--Transform=EncodeArithmetic', '--Functions=' + ','.join('f_' + n for n in OPS)] * 2),
                ('opaque-hand', OPAQUE_SRC, []),
                ('opaque-tigress', OPAQUE_SRC, ['--Transform=InitEntropy', '--Functions=t_plain',
                                                '--Transform=InitOpaque', '--Functions=t_plain',
                                                '--InitOpaqueStructs=list', '--InitOpaqueSize=4',
                                                '--Transform=AddOpaque', '--Functions=t_plain',
                                                '--AddOpaqueKinds=true', '--AddOpaqueCount=4'])]
    for name, src, transform in variants:
        work = out / name
        work.mkdir(parents=True, exist_ok=True)
        (work / 'in.c').write_text(src)
        if transform:
            run([args.tigress, '--Environment=x86_64:Linux:Gcc:4.6', '--Seed=7', *transform,
                 '--out=' + str(work / 'out.c'), work / 'in.c'], work)
            source = work / 'out.c'
        else:
            source = work / 'in.c'
        names = ['f_' + n for n in OPS] if name.startswith('mba') else list(OPAQUE_KIND)
        for level in ('O0', 'O2'):
            lw = work / level
            lw.mkdir(exist_ok=True)
            img, funcs = compile_obj(source, lw, level, args.clang)
            for row in analyse(img, funcs, names, name.startswith('mba')):
                row.update(variant=name, level=level)
                results.append(row)
                print(json.dumps(row), flush=True)
    (out / 'results.json').write_text(json.dumps(results, indent=1))


if __name__ == '__main__':
    main()
