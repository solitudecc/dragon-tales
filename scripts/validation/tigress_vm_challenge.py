"""Challenge: a nontrivial C kernel under Tigress Virtualize (and friends),
analysed by Dragon and executed through its LLVM lifts.

The kernel mixes a dense switch, a function-pointer table, recursion, 64-bit
multiply/divide, an S-box lookup, nested loops and calls to Guest memcpy/
memset. Profiles range from unprotected to two nested VMs and a
"kitchen sink" (opaque predicates + arithmetic encoding + VM + flattening).

Checks, each against an independent reference:
  C       every image's observable result (EAX and out[0..7]) vs a separate
          Python specification - proves Tigress output is correct;
  CFG     Dragon's function entries vs ELF STT_FUNC symbols; every indirect
          jump/call Unicorn executes vs Dragon's resolved target set: a
          resolved site that misses an executed target is WRONG, an
          unresolved one is only counted; code Unicorn executes that Dragon
          never decoded is counted;
  LLVM    six plans (V2/static/product x O0/O2): complete root returns are
          compared on all modelled registers and 64 KiB memory with the
          image's own Unicorn run; explicit exits (unresolved dispatch,
          budget) are `incomplete`, never passes;
  size    instructions of kernel in the image and in product-O2 LLVM IR.

Exit status 0 only if every C check matches, no CFG result is wrong and
every LLVM comparison is a complete match (so a harder profile that is
merely incomplete makes it 1: the run is a measurement).
"""
import argparse
import collections
import os
import hashlib
import io
import json
import random
import re
import struct
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
import tigress_function_diff as tf
from generated_function_diff import PLANS
import block_deobf_study as block_deobf

base, guest = tf.base, tf.guest
# `junk` predicates and Flatten `flag` conditionals emit x86 inline assembly;
# Tigress test-compiles its output with the host compiler, so on an aarch64
# host it must use the x86-64 cross compiler.
os.environ.setdefault('CILLY_NATIVE_CC', 'x86_64-linux-gnu-gcc')
M = 0xffffffff
ENTRY = tf.ENTRY
MAX_GUEST_INSNS = 20_000_000
V2_BUDGET = 100_000_000

# Virtualized kernels outgrow the shared 64 KiB layout. Keep its DATA/STACK/
# SENTINEL and the 0x1000 entry wrapper (compile_image relies on both), place
# everything else from 0x10000 and give both oracles 1 MiB of Guest memory.
MEM_SIZE = 0x100000
IMAGE_BASE = 0x10000
base.MEM_SIZE = MEM_SIZE
assert tf.compiler.DRIVER.count('memory[65536]') == 1
tf.compiler.DRIVER = tf.compiler.DRIVER.replace('memory[65536]', f'memory[{MEM_SIZE}]')
LINKER = f'''ENTRY(oracle_entry)
SECTIONS {{
 . = 0x1000;
 .text.entry : {{ *(.text.entry) }}
 . = {IMAGE_BASE:#x};
 .text : {{ *(.text .text.*) }}
 . = ALIGN(0x1000);
 .rodata : {{ *(.rodata .rodata.*) }}
 .data : {{ *(.data .data.*) }}
 .bss : {{ *(.bss .bss.*) *(COMMON) }}
 /DISCARD/ : {{ *(.eh_frame*) *(.comment) *(.note*) }}
}}
'''
# Tigress moves constant initializers (SBOX) into megaInit, which its
# generated main() calls. Execute that real Guest initializer first, with the
# kernel arguments preserved; the plain image links an empty weak one.
WRAPPER = '''.section .text.entry,"ax",@progbits
.globl oracle_entry
.type oracle_entry,@function
oracle_entry:
 pushq %rdi
 pushq %rsi
 subq $8, %rsp
 call megaInit
 addq $8, %rsp
 popq %rsi
 popq %rdi
 subq $8, %rsp
 call kernel
 addq $8, %rsp
 cmpl %edi, %eax
 ret
.size oracle_entry,.-oracle_entry
.section .note.GNU-stack,"",@progbits
'''
# Opaque `list` structures allocate nodes: a real Guest bump allocator (its
# heap is image .bss), executed and lifted like all other code.
SUPPORT = tf.SUPPORT + '''__attribute__((weak)) void megaInit(void) {}
static unsigned char heap[1 << 16] __attribute__((aligned(16)));
static unsigned long heap_top;
void *malloc(unsigned long n) {
  n = (n + 15ul) & ~15ul;
  if (n > sizeof heap - heap_top) return 0;
  void *p = heap + heap_top;
  heap_top += n;
  return p;
}
void free(void *p) { (void)p; }
'''


def build_image(c_source, work, level, args):
    work.mkdir(parents=True, exist_ok=True)
    (work / 'wrapper.S').write_text(WRAPPER)
    (work / 'support.c').write_text(SUPPORT)
    (work / 'layout.ld').write_text(LINKER)
    common = ['--target=x86_64-linux-gnu', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
              '-fno-pic', '-fno-pie', '-fno-asynchronous-unwind-tables', '-mno-red-zone',
              '-ffunction-sections', '-fdata-sections', '-w']
    tf.run([args.clang, *common, '-' + level, '-c', c_source, '-o', 'kernel.o'], work, 'compile-c', timeout=900)
    tf.run([args.clang, *common, '-' + level, '-c', 'support.c', '-o', 'support.o'], work, 'compile-support')
    tf.run([args.clang, '--target=x86_64-linux-gnu', '-c', 'wrapper.S', '-o', 'wrapper.o'], work, 'compile-wrapper')
    tf.run([args.ld, '--gc-sections', '-T', 'layout.ld', 'wrapper.o', 'kernel.o', 'support.o', '-o', 'fixture.elf'],
           work, 'link')
    data = (work / 'fixture.elf').read_bytes()
    elf = ELFFile(io.BytesIO(data))
    regions = []
    for section in elf.iter_sections():
        if section['sh_flags'] & 2 and section['sh_size']:
            start, size = section['sh_addr'], section['sh_size']
            entry_page = section.name == '.text.entry' and ENTRY <= start and start + size < 0x2000
            if not (entry_page or (IMAGE_BASE <= start and start + size <= MEM_SIZE)):
                raise RuntimeError(f'section {section.name} outside the reserved image region')
            regions.append((start, bytes(size) if section['sh_type'] == 'SHT_NOBITS' else section.data()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
               if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF'}
    if symbols.get('oracle_entry') != ENTRY or 'kernel' not in symbols:
        raise RuntimeError('missing independently known ELF entry symbols')
    return data, regions, symbols

PROFILES = ['plain', 'flatten-indirect', 'virt-switch', 'virt-indirect', 'virt-direct', 'virt-call',
            'virt-hard', 'virt-double', 'kitchen-sink']
# Function-level MBA + control-flow + opaque predicates, on the guarded source
# (`% 9` with a reachable default: the plain switch has a provable bound, so
# what stays unresolved is due to the obfuscation).
FUNCTION_PROFILES = ['f-plain', 'f-mba', 'f-opaque', 'f-cff', 'f-combo', 'f-combo-goto', 'f-combo-indirect']


def constants(seed):
    rng = random.Random(seed)
    c = [rng.getrandbits(32) | 1 for _ in range(6)]
    sbox = [rng.getrandbits(8) for _ in range(64)]
    return c, sbox


def source(seed, wrong=False, guarded=False):
    (c0, c1, c2, c3, c4, c5), sbox = constants(seed)
    modulus = 9 if guarded else 7
    table = ', '.join(f'{b}u' for b in sbox)
    return f'''typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned char u8;
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);
static const u8 SBOX[64] = {{{table}}};
struct st {{ u32 acc; u32 cnt; u64 wide; u8 buf[16]; }};
typedef u32 (*op_fn)(u32, u32);
__attribute__((noinline)) u32 mix(u32 a, u32 b) {{ return ((a << 5) | (a >> 27)) ^ (b * 0x9e3779b9u); }}
__attribute__((noinline)) u32 op_add(u32 a, u32 b) {{ return a + b + {c3}u; }}
__attribute__((noinline)) u32 op_xor(u32 a, u32 b) {{ return a ^ (b << 7) ^ {c4}u; }}
__attribute__((noinline)) u32 op_sub(u32 a, u32 b) {{ return a - (b | 1u) * 3u; }}
__attribute__((noinline)) u32 fact_mod(u32 n) {{ return n < 2u ? 1u : (n * fact_mod(n - 1u)) % 65521u; }}
__attribute__((noinline)) u32 kernel(u32 x, u32 *out) {{
  op_fn ops[4] = {{op_add, op_xor, op_sub, mix}};
  struct st s;
  memset(&s, 0, sizeof s);
  for (u32 i = 0u; i < 8u; ++i) out[i] = i * {c5}u;
  u32 state = x ^ {c0}u;
  for (u32 pc = 0u; pc < 12u; ++pc) {{
    int code = (int)((state ^ (state >> 13) ^ pc) % {modulus}u); /* Tigress 2.1 Virtualize rejects unsigned switch */
    switch (code) {{
    case 0: state += SBOX[state & 63u]; break;
    case 1: state = ops[(state >> 3) & 3u](state, pc); break;
    case 2: s.wide = (u64)state * {c1}ull + s.wide; state ^= (u32)(s.wide >> 32); break;
    case 3: if (state & 1u) state = state / (pc | 1u); else state = state % 1000003u; break;
    case 4: s.buf[pc & 15u] = (u8)state; memcpy(&s.acc, s.buf + (pc & 12u), 4); state += s.acc; break;
    case 5: state = mix(state, fact_mod(pc + (state & 7u))); break;
    default: {{ u32 n = state & 15u; while (n--) state = (state << 1) ^ ((state >> 31) ? {c2}u : 0u); }} break;
    }}
    s.cnt += (u32)code & 3u;
    out[pc & 7u] ^= state;
  }}
  return (state ^ s.cnt ^ (u32)s.wide){' ^ 1u' if wrong else ''};
}}
'''


def specification(seed, x, guarded=False):
    (c0, c1, c2, c3, c4, c5), sbox = constants(seed)
    modulus = 9 if guarded else 7
    def mix(a, b):
        return ((((a << 5) | (a >> 27)) & M) ^ ((b * 0x9e3779b9) & M))
    def fact(n):
        return 1 if n < 2 else (n * fact(n - 1)) % 65521
    ops = [lambda a, b: (a + b + c3) & M, lambda a, b: a ^ ((b << 7) & M) ^ c4,
           lambda a, b: (a - ((b | 1) * 3)) & M, mix]
    cnt = wide = 0
    buf = bytearray(16)
    out = [(i * c5) & M for i in range(8)]
    state = x ^ c0
    cases = collections.Counter()
    for pc in range(12):
        code = (state ^ (state >> 13) ^ pc) % modulus
        cases[code] += 1
        if code == 0:
            state = (state + sbox[state & 63]) & M
        elif code == 1:
            state = ops[(state >> 3) & 3](state, pc)
        elif code == 2:
            wide = (state * c1 + wide) & (2**64 - 1)
            state ^= wide >> 32
        elif code == 3:
            state = state // (pc | 1) if state & 1 else state % 1000003
        elif code == 4:
            buf[pc & 15] = state & 255
            acc = int.from_bytes(buf[pc & 12:(pc & 12) + 4], 'little')
            state = (state + acc) & M
        elif code == 5:
            state = mix(state, fact(pc + (state & 7)))
        else:
            for _ in range(state & 15):
                state = ((state << 1) & M) ^ (c2 if state >> 31 else 0)
        cnt = (cnt + (code & 3)) & M
        out[pc & 7] ^= state
    return (state ^ cnt ^ (wide & M)) & M, struct.pack('<8I', *out), cases


def inputs(seed, count):
    rng = random.Random(seed * 7919)
    values = list(tf.INPUTS)
    while len(values) < count:
        values.append(rng.getrandbits(32))
    return values[:count]


def transforms(profile, seed):
    k = ['--Functions=kernel']
    # Tigress 2.1's `array` opaque structure declares 12 elements, initialises
    # 11 and divides by the zero 12th (SIGFPE); use pointer-based lists.
    opaque = ['--Transform=InitEntropy', *k, '--Transform=InitOpaque', *k,
              '--InitOpaqueStructs=list', '--InitOpaqueSize=4']
    virt = lambda dispatch, *extra: ['--Transform=Virtualize', *k, '--VirtualizeDispatch=' + dispatch, *extra]
    if profile in ('plain', 'f-plain'):
        return []
    add_opaque = ['--Transform=AddOpaque', *k, '--AddOpaqueKinds=call,bug,true,junk,question',
                  '--AddOpaqueCount=6', '--AddOpaqueObfuscate=true', '--AddOpaqueStructs=list']
    mba = ['--Transform=EncodeArithmetic', *k]
    def cff(dispatch):
        return ['--Transform=Flatten', *k, '--FlattenDispatch=' + dispatch, '--FlattenRandomizeBlocks=true',
                '--FlattenSplitBasicBlocks=true', '--FlattenObfuscateNext=true',
                '--FlattenConditionalKinds=branch,compute,flag', '--FlattenOpaqueStructs=list']
    if profile == 'f-mba':
        return mba
    if profile == 'f-opaque':
        return opaque + add_opaque
    if profile == 'f-cff':
        return ['--Transform=Flatten', *k, '--FlattenDispatch=switch', '--FlattenRandomizeBlocks=true',
                '--FlattenSplitBasicBlocks=true']
    if profile.startswith('f-combo'):
        dispatch = {'f-combo': 'switch', 'f-combo-goto': 'goto', 'f-combo-indirect': 'indirect'}[profile]
        # Flatten's compute/flag conditionals cannot convert the opaque list
        # pointer comparisons (Tigress 2.1 internal error), so the predicates
        # are inserted into the flattened blocks afterwards.
        return opaque + mba + cff(dispatch) + add_opaque + mba
    if profile == 'flatten-indirect':
        return ['--Transform=Flatten', *k, '--FlattenDispatch=indirect',
                '--FlattenRandomizeBlocks=true', '--FlattenSplitBasicBlocks=true']
    if profile == 'virt-switch':
        return virt('switch')
    if profile == 'virt-indirect':
        return virt('indirect')
    if profile == 'virt-direct':
        return virt('direct')
    if profile == 'virt-call':
        return virt('call')
    if profile == 'virt-hard':
        return opaque + virt('indirect', '--VirtualizeSuperOpsRatio=2.0', '--VirtualizeMaxMergeLength=8',
                             '--VirtualizeMaxDuplicateOps=2', '--VirtualizeRandomOps=true',
                             # Tigress 2.1 crashes on InstructionHandlerSplitCount and on
                             # EncodeByteArray+ObfuscateDecodeByteArray alone, and emits
                             # code faulting on unmapped memory with them combined here.
                             '--VirtualizeAddOpaqueToVPC=true', '--VirtualizeOpaqueStructs=list',
                             '--VirtualizeNumberOfBogusFuns=2', '--VirtualizeBogusFunsGenerateOutput=false')
    if profile == 'virt-double':
        return virt('switch') + virt('indirect')
    if profile == 'kitchen-sink':
        return (opaque + ['--Transform=AddOpaque', *k, '--AddOpaqueKinds=true', '--AddOpaqueCount=4',
                          '--Transform=EncodeArithmetic', *k]
                + virt('indirect', '--VirtualizeSuperOpsRatio=1.5')
                + ['--Transform=Flatten', *k, '--FlattenDispatch=indirect', '--FlattenRandomizeBlocks=true'])
    raise ValueError(profile)


def function_sizes(data):
    elf = ELFFile(io.BytesIO(data))
    return {s.name: (s['st_value'], s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()
            if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF'}


def initial(adapter, regions, x, trial):
    fixture = dict(code_hex='', stubs=[], set_gprs={'RDI': x, 'RSI': base.DATA}, initial_sp=base.STACK + 8)
    regs, memory = adapter.initial(fixture, trial, ENTRY)
    regs[142] = 0  # C ABI: direction flag clear
    for address, blob in regions:
        memory[address:address + len(blob)] = blob
    return regs, memory


def reference(adapter, regs, memory, watch):
    """Unicorn run to the root exit; records (site, target) of every executed
    instruction in `watch` and every executed PC."""
    executed, edges, prev = set(), set(), [None]
    original = guest.Uc
    def observed(*a):
        uc = original(*a)
        def hook(u, pc, size, _):
            executed.add(pc)
            if prev[0] in watch:
                edges.add((prev[0], pc))
            prev[0] = pc
        uc.hook_add(guest.UC_HOOK_CODE, hook)
        return uc
    guest.Uc = observed
    try:
        result = adapter.reference(dict(stubs=[], undefined_flags=[], max_instructions=MAX_GUEST_INSNS),
                                   regs, memory, ENTRY)
    finally:
        guest.Uc = original
    return result, executed, edges


def cfg_view(data, strategy):
    image = base.dragon.LoadedImage.load_bytes(data)
    graph = image.disassemble(base.dragon.Configuration(strategy=strategy))
    sites = {}
    decoded = set()
    for insn in graph.instructions():
        decoded.add(insn.address)
        if insn.has_indirect_target and (insn.is_jump or insn.is_call) and not insn.is_return:
            sites[insn.address] = dict(kind='call' if insn.is_call else 'jump',
                                       targets=set(insn.callees if insn.is_call else insn.branches),
                                       text=insn.disassembly)
    entries = sorted(pc for pc in graph.function_addresses() if graph.get_function(pc).is_valid)
    unlifted = sum(len(graph.get_ignil_block(b).unlifted()) for b in graph.block_addresses()
                   if graph.get_ignil_block(b))
    return dict(sites=sites, decoded=decoded, entries=entries, unlifted=unlifted,
                instructions=len(decoded), graph=graph)


def ir_instructions(path, symbol):
    text = path.read_text()
    m = re.search(r'^define [^\n]*@' + re.escape(symbol) + r'\([^\n]*\{\n(.*?)^\}', text, re.M | re.S)
    if not m:
        return None
    return sum(1 for line in m[1].splitlines() if line.startswith('  ') and not line.strip().startswith(';'))


def ir_cfg(path, symbol):
    """Basic blocks, conditional branches and switches of one IR function."""
    text = path.read_text()
    m = re.search(r'^define [^\n]*@' + re.escape(symbol) + r'\([^\n]*\{\n(.*?)^\}', text, re.M | re.S)
    if not m:
        return None
    body = m[1]
    return dict(blocks=1 + len(re.findall(r'^[\w.]+:', body, re.M)),
                conditional_br=len(re.findall(r'^\s+br i1 ', body, re.M)),
                switches=len(re.findall(r'^\s+switch ', body, re.M)))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--tigress', required=True)
    ap.add_argument('--profile', action='append', choices=PROFILES + FUNCTION_PROFILES)
    ap.add_argument('--function-level', action='store_true', help='run FUNCTION_PROFILES by default')
    ap.add_argument('--seed', type=int, default=4242)
    ap.add_argument('--trials', type=int, default=24)
    ap.add_argument('--strategy', type=int, choices=[0, 1], default=0)
    ap.add_argument('--function-entry', action='store_true', help='use explicit function-start static views')
    ap.add_argument('--c-level', action='append', choices=['O0', 'O2'])
    ap.add_argument('--plans', default=','.join(p[0] for p in PLANS))
    ap.add_argument('--control', choices=['c-result', 'llvm-register', 'llvm-memory'])
    for name, default in [('clang', 'clang-21'), ('ld', 'ld.lld-21'), ('opt', 'opt-21'), ('llc', 'llc-21'), ('cc', 'gcc')]:
        ap.add_argument('--' + name, default=default)
    args = ap.parse_args()
    profiles = args.profile or (FUNCTION_PROFILES if args.function_level else PROFILES)
    levels = args.c_level or ['O0', 'O2']
    plans = [p for p in PLANS if p[0] in args.plans.split(',')]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    adapter = guest.Adapter('x86_64')
    xs = inputs(args.seed, args.trials)
    seen_cases = collections.Counter()
    for x in xs:
        seen_cases.update(specification(args.seed, x, guarded=any(p.startswith('f-') for p in profiles))[2])
    meta = dict(seed=args.seed, inputs=xs, strategy=args.strategy, profiles=profiles, levels=levels,
                static_entry_policy='function-start' if args.function_entry else 'resumable',
                plans=[p[0] for p in plans], spec_switch_case_hits=dict(sorted(seen_cases.items())),
                library_sha256=hashlib.sha256(Path(base.dragon._LIB).read_bytes()).hexdigest(),
                tigress_version=tf.run([args.tigress, '--version'], out, 'tigress-version').strip().splitlines()[-1][:200],
                control=args.control)
    (out / 'source.c').write_text(source(args.seed))
    (out / 'source-guarded.c').write_text(source(args.seed, guarded=True))
    c_rows, rows, analyses = [], [], []
    def save():
        st = lambda rs: dict(collections.Counter(r['status'] for r in rs))
        summary = dict(meta, c_statuses=st(c_rows), llvm_statuses=st(rows),
                       cfg_wrong_sites=sum(a.get('wrong_sites', 0) for a in analyses))
        (out / 'results.json').write_text(json.dumps(dict(summary=summary, analyses=analyses,
                                                          c_results=c_rows, results=rows), indent=1, default=list))
        return summary
    for profile in profiles:
        work = out / profile
        work.mkdir(exist_ok=True)
        input_c, output_c = work / 'input.c', work / 'transformed.c'
        guarded = profile.startswith('f-')
        input_c.write_text(source(args.seed, wrong=args.control == 'c-result', guarded=guarded))
        try:
            if profile in ('plain', 'f-plain'):
                output_c.write_text(input_c.read_text())
            else:
                tf.run([args.tigress, '--Environment=x86_64:Linux:Gcc:4.6', '--Seed=' + str(args.seed),
                        *transforms(profile, args.seed), '--out=' + str(output_c), input_c], work, 'tigress', timeout=900)
        except Exception as error:
            analyses.append(dict(profile=profile, status='tigress_error', detail=str(error)[-1500:]))
            save()
            print(profile, 'tigress_error', str(error)[-300:], flush=True)
            continue
        for level in levels:
            case = f'{profile}-{level}'
            iw = work / level
            try:
                data, regions, symbols = build_image(output_c, iw, level, args)
            except Exception as error:
                analyses.append(dict(case=case, status='build_error', detail=str(error)[-1500:]))
                save()
                print(case, 'build_error', str(error)[-300:], flush=True)
                continue
            sizes = function_sizes(data)
            view = cfg_view(data, args.strategy)
            ka, ks = sizes['kernel']
            analysis = dict(case=case, status='analysed', image_bytes=len(data), kernel_bytes=ks,
                            kernel_instructions=sum(1 for a in view['decoded'] if ka <= a < ka + ks),
                            functions_symbols=len(sizes), dragon_functions=len(view['entries']),
                            missing_symbol_entries=sorted(set(v[0] for v in sizes.values()) - set(view['entries'])),
                            extra_function_entries=sorted(set(view['entries']) - set(v[0] for v in sizes.values())),
                            unlifted=view['unlifted'], indirect_sites=len(view['sites']),
                            resolved_sites=sum(1 for s in view['sites'].values() if s['targets']))
            graph = view['graph']
            kblocks = [bb for bb in graph.block_addresses() if ka <= bb < ka + ks and graph.get_block(bb).is_valid]
            conditional = [bb for bb in kblocks
                           if (lambda i: i.is_jump and i.is_conditional)(graph.get_instruction(graph.get_block(bb).terminator_address))]
            edges = block_deobf.branch_edges(graph, conditional)
            analysis.update(kernel_blocks=len(kblocks), kernel_conditional_branches=len(conditional),
                            branches_decided_single_edge=sum(1 for v in edges.values() if v == 1),
                            branches_both_edges=sum(1 for v in edges.values() if v == 2),
                            branches_symbolic_error=sum(1 for v in edges.values() if v is None))
            # C level + dynamic CFG edges, one Unicorn run per input.
            expected, executed_all, edges_all = [], set(), set()
            for trial, x in enumerate(xs):
                regs, memory = initial(adapter, regions, x, trial)
                try:
                    result, executed, edges = reference(adapter, regs, memory, set(view['sites']))
                except Exception as error:
                    c_rows.append(dict(case=case, trial=trial, x=x, status='reference_error', detail=str(error)))
                    continue
                executed_all |= executed
                edges_all |= edges
                want_r, want_out, _ = specification(args.seed, x, guarded=guarded)
                got = (base.get(result[0], 0, 4), bytes(result[1][base.DATA:base.DATA + 32]))
                c_rows.append(dict(case=case, trial=trial, x=x,
                                   status='match' if got == (want_r, want_out) else 'different',
                                   guest_instructions=len(executed)))
                expected.append((trial, regs, memory, result))
            wrong = collections.defaultdict(set)
            for site, target in edges_all:
                if view['sites'][site]['targets'] and target not in view['sites'][site]['targets']:
                    wrong[site].add(target)
            executed_sites = {s for s, _ in edges_all}
            analysis.update(executed_indirect_sites=len(executed_sites),
                            executed_resolved_sites=sum(1 for s in executed_sites if view['sites'][s]['targets']),
                            executed_unresolved_sites=sorted(hex(s) + ' ' + view['sites'][s]['text']
                                                             for s in executed_sites if not view['sites'][s]['targets'])[:20],
                            wrong_sites=len(wrong),
                            wrong_detail=[dict(site=hex(s), text=view['sites'][s]['text'],
                                               resolved=sorted(hex(t) for t in view['sites'][s]['targets'])[:8],
                                               executed_missing=sorted(hex(t) for t in ts)[:8]) for s, ts in wrong.items()],
                            executed_pcs=len(executed_all),
                            executed_undecoded=len(executed_all - view['decoded']))
            analyses.append(analysis)
            view.pop('graph', None)
            for plan, static, product, opt_level in plans:
                if not expected:
                    break
                pw = iw / f's{args.strategy}-{plan}'
                pw.mkdir(parents=True, exist_ok=True)
                saved = args.control
                args.control = args.control if args.control in ('llvm-register', 'llvm-memory') else None
                try:
                    exe, info = tf.compile_image(data, symbols, pw, static, product, opt_level, args, budget=V2_BUDGET)
                except Exception as error:
                    rows.append(dict(case=case, plan=plan, status='infrastructure_error', detail=str(error)[-1500:]))
                    args.control = saved
                    continue
                args.control = saved
                if plan == 'product-O2':
                    kernel_symbol = f'dragon_static_{"entry_" if args.function_entry else ""}x86_64_{ka:x}'
                    analysis['kernel_ir_cfg_product_O2'] = ir_cfg(pw / 'optimized.ll', kernel_symbol)
                    analysis['kernel_ir_product_O2'] = ir_instructions(pw / 'optimized.ll', kernel_symbol)
                    analysis['kernel_ir_raw'] = ir_instructions(pw / 'unoptimized.ll', kernel_symbol)
                for trial, regs, memory, want in expected:
                    actual, error = base.native(exe, regs, memory, pw, trial, extra_bytes=64)
                    row = dict(case=case, plan=plan, trial=trial)
                    if error:
                        row.update(status=error['kind'])
                    else:
                        record = struct.unpack('<8Q', actual[3])
                        if record[0] == 1 and record[1] == 3 and record[3] == base.SENTINEL and record[4] == 1:
                            diffs, _ = adapter.compare(dict(undefined_flags=[]), want, actual, False)
                            row.update(status='different' if diffs else 'match', differences=diffs[:4])
                        elif record[0] in (2, 3, 4, 5, 6):
                            row.update(status='incomplete', exit=record[:7])
                        else:
                            row.update(status='different', exit=record[:7])
                    rows.append(row)
            summary = save()
            print(case, json.dumps({k: analysis.get(k) for k in ('kernel_instructions', 'indirect_sites', 'resolved_sites',
                                                                 'executed_indirect_sites', 'executed_resolved_sites',
                                                                 'wrong_sites', 'executed_undecoded', 'unlifted', 'kernel_blocks',
                                                                 'kernel_conditional_branches', 'branches_decided_single_edge',
                                                                 'kernel_ir_product_O2', 'kernel_ir_cfg_product_O2')}),
                  dict(collections.Counter(r['status'] for r in c_rows if r['case'] == case)),
                  dict(collections.Counter(r['status'] for r in rows if r['case'] == case)), flush=True)
    summary = save()
    print(json.dumps(summary, indent=1))
    ok = (all(r['status'] == 'match' for r in c_rows) and summary['cfg_wrong_sites'] == 0
          and rows and all(r['status'] == 'match' for r in rows))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
