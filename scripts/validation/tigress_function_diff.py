"""Tigress C -> x64 ELF -> IGNIL/LLVM semantics, with two independent checks.

First compare the original and transformed machine functions' C observables
against a separate unsigned Python specification. Then compare each image's
complete machine state/memory against its own Unicorn execution. The latter
never compares register allocations or stack bytes between different binaries.
Only generated, bounded test functions are executed. No external-call models.
"""
import argparse
import collections
import hashlib
import io
import json
import platform
import random
import re
import struct
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import __version__ as unicorn_version
import function_dispatch_diff as compiler
import function_guest_diff as guest
from generated_function_diff import PLANS

base = guest.base
ENTRY = 0x1000
INPUTS = [0, 1, 2, 7, 0x7f, 0x80, 0x7fff, 0x8000, 0x7fffffff,
          0x80000000, 0xfffffffe, 0xffffffff, 8, 15, 16, 31]
PROFILES = ['plain', 'flatten-switch', 'flatten-goto', 'arithmetic', 'opaque', 'combined']
MASK = 0xffffffff
LINKER = '''ENTRY(oracle_entry)
SECTIONS {
 . = 0x1000;
 .text : { *(.text.entry) *(.text .text.*) }
 . = ALIGN(0x1000);
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.*) }
 .bss : { *(.bss .bss.*) *(COMMON) }
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
'''
# The final CMP defines every compared arithmetic flag, even when the compiler
# leaves a flag undefined inside kernel. The wrapper is identical in all images.
WRAPPER = '''.section .text.entry,"ax",@progbits
.globl oracle_entry
.type oracle_entry,@function
oracle_entry:
 subq $8, %rsp
 call kernel
 addq $8, %rsp
 cmpl %edi, %eax
 ret
.size oracle_entry,.-oracle_entry
.section .note.GNU-stack,"",@progbits
'''
SUPPORT = '''/* Actual freestanding Guest code, never a host-call model. */
void *memcpy(void *dst, const void *src, unsigned long n) {
  unsigned char *d=dst; const unsigned char *s=src;
  for (unsigned long i=0; i<n; ++i) d[i]=s[i];
  return dst;
}
void *memset(void *dst, int value, unsigned long n) {
  unsigned char *d=dst;
  for (unsigned long i=0; i<n; ++i) d[i]=(unsigned char)value;
  return dst;
}
'''


def run(command, work, tag, timeout=90):
    command = [str(x) for x in command]
    with (work / 'commands.jsonl').open('a') as f:
        f.write(json.dumps(command) + '\n')
    result = subprocess.run(command, cwd=work, capture_output=True, timeout=timeout)
    (work / (tag + '.stdout')).write_bytes(result.stdout)
    (work / (tag + '.stderr')).write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{tag}: exit {result.returncode}: ' + result.stderr.decode(errors='replace')[-2000:])
    return result.stdout.decode(errors='replace')


def constants(seed):
    rng = random.Random(seed)
    return [rng.getrandbits(32) | 1 for _ in range(4)]


def source(seed, wrong=False):
    a, b, c, d = constants(seed)
    return f'''typedef unsigned int u32;
__attribute__((noinline)) u32 helper(u32 x, u32 y) {{ return (x+y) ^ (x*9u); }}
__attribute__((noinline)) u32 kernel(u32 x, u32 *out) {{
  u32 y=x^{a}u;
  if ((x&3u)==0u) y+={b}u; else y-={c}u;
  for (u32 i=0u; i<(x&7u); ++i) {{ y=(y<<3u)|(y>>29u); y+=i+17u; }}
  out[0]=y;
  out[1]=helper(y,x);
  out[2]=out[0]^out[1];
  out[3]=x;
  return (out[2]+{d}u){' ^ 1u' if wrong else ''};
}}
'''


def specification(seed, x):
    a, b, c, d = constants(seed)
    y = ((x ^ a) + (b if x & 3 == 0 else -c)) & MASK
    for i in range(x & 7):
        y = (((y << 3) | (y >> 29)) + i + 17) & MASK
    h = ((y + x) & MASK) ^ ((y * 9) & MASK)
    out = [y, h, y ^ h, x]
    return (out[2] + d) & MASK, struct.pack('<4I', *out)


def transforms(profile, version):
    flatten = ['--Transform=Flatten', '--Functions=kernel', '--FlattenDispatch=switch',
               '--FlattenRandomizeBlocks=true', '--FlattenSplitBasicBlocks=true']
    arithmetic = ['--Transform=EncodeArithmetic', '--Functions=kernel']
    if not version.startswith('2.'):
        arithmetic.append('--EncodeArithmeticMaxLevel=1')
    if version.startswith(('2.', '3.')):
        opaque_init = ['--Transform=InitEntropy', '--Functions=kernel',
                       '--Transform=InitOpaque', '--Functions=kernel',
                       '--InitOpaqueStructs=array', '--InitOpaqueSize=4']
    else:
        opaque_init = ['--Transform=InitOpaque', '--Functions=kernel',
                       '--InitOpaqueInvariantKinds=identity,modular,mba',
                       '--InitOpaqueInvariantResilience=local']
    opaque = opaque_init + ['--Transform=AddOpaque', '--Functions=kernel',
                            '--AddOpaqueKinds=true', '--AddOpaqueCount=3']
    if profile.startswith('flatten-'):
        return [x.replace('Dispatch=switch', 'Dispatch=' + profile.split('-', 1)[1]) for x in flatten]
    if profile == 'arithmetic':
        return arithmetic
    if profile == 'opaque':
        return opaque
    if profile == 'combined':
        return opaque + arithmetic + flatten
    if profile == 'virtualize':
        return ['--Transform=Virtualize', '--Functions=kernel', '--VirtualizeDispatch=switch']
    if profile == 'plain':
        return []
    raise ValueError(profile)


def build_image(c_source, work, level, args):
    work.mkdir(parents=True, exist_ok=True)
    (work / 'wrapper.S').write_text(WRAPPER)
    (work / 'support.c').write_text(SUPPORT)
    (work / 'layout.ld').write_text(LINKER)
    common = ['--target=x86_64-linux-gnu', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
              '-fno-pic', '-fno-pie', '-fno-asynchronous-unwind-tables', '-mno-red-zone',
              '-ffunction-sections', '-fdata-sections']
    run([args.clang, *common, '-' + level, '-c', c_source, '-o', 'kernel.o'], work, 'compile-c')
    run([args.clang, *common, '-' + level, '-c', 'support.c', '-o', 'support.o'], work, 'compile-support')
    run([args.clang, '--target=x86_64-linux-gnu', '-c', 'wrapper.S', '-o', 'wrapper.o'], work, 'compile-wrapper')
    run([args.ld, '--gc-sections', '-T', 'layout.ld', 'wrapper.o', 'kernel.o', 'support.o', '-o', 'fixture.elf'], work, 'link')
    data = (work / 'fixture.elf').read_bytes()
    elf = ELFFile(io.BytesIO(data))
    regions = []
    for section in elf.iter_sections():
        if section['sh_flags'] & 2 and section['sh_size']:
            start, size = section['sh_addr'], section['sh_size']
            if not (ENTRY <= start and start + size < 0x7000):
                raise RuntimeError('fixture exceeds reserved low image region')
            regions.append((start, bytes(size) if section['sh_type'] == 'SHT_NOBITS' else section.data()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
               if s['st_info']['type'] == 'STT_FUNC' and s['st_shndx'] != 'SHN_UNDEF'}
    if symbols.get('oracle_entry') != ENTRY or 'kernel' not in symbols:
        raise RuntimeError('missing independently known ELF entry symbols')
    return data, regions, symbols


def initial(adapter, regions, trial):
    fixture = dict(code_hex='', stubs=[], set_gprs={'RDI': INPUTS, 'RSI': base.DATA}, initial_sp=base.STACK + 8)
    regs, memory = adapter.initial(fixture, trial, ENTRY)
    regs[142] = 0  # C ABI entry precondition: direction flag is clear.
    for address, data in regions:
        memory[address:address + len(data)] = data
    return regs, memory


def unicorn(adapter, regs, memory, diagnostic=None):
    # The diagnostic observer only records PC/bytes, including the faulting
    # instruction. It never modifies execution, inputs or stop conditions.
    original = guest.Uc
    def observed(*args):
        uc = original(*args)
        if diagnostic is not None:
            def hook(u, pc, size, _):
                diagnostic.update(pc=pc, size=size, code=bytes(u.mem_read(pc, min(size, 15))).hex())
            uc.hook_add(guest.UC_HOOK_CODE, hook)
        return uc
    guest.Uc = observed
    try:
        return adapter.reference(dict(stubs=[], undefined_flags=[]), regs, memory, ENTRY)
    finally:
        guest.Uc = original


def observables(snapshot):
    regs, memory = snapshot[:2]
    return base.get(regs, 0, 4), memory[base.DATA:base.DATA + 16]


def compile_image(data, symbols, work, static, product, level, args):
    image = base.dragon.LoadedImage.load_bytes(data)
    graph = image.disassemble(base.dragon.Configuration(strategy=args.strategy))
    lifter = base.dragon.LLVMLifter(2, execution_mode=2 if static else 1)
    entries = sorted(pc for pc in graph.function_addresses() if graph.get_function(pc).is_valid)
    if ENTRY not in entries:
        raise RuntimeError('analysis missed root entry')
    unlifted = []
    for address in graph.block_addresses():
        block = graph.get_ignil_block(address)
        if block:
            unlifted.extend((address, x) for x in block.unlifted())
    for pc in entries:
        if not lifter.lift(graph.get_function(pc), graph):
            raise RuntimeError(f'LLVM lift rejected function {pc:#x}; unlifted={unlifted[:8]}')
    (work / 'unoptimized.ll').write_text(lifter.ir())
    if product:
        lifter.optimize()
    raw = work / 'raw.ll'
    raw.write_text(lifter.ir())
    run([args.opt, '-passes=verify', '-disable-output', raw], work, 'verify-raw')
    selected = raw
    if level == 'O2':
        selected = work / 'optimized.ll'
        run([args.opt, '-S', '-passes=default<O2>', raw, '-o', selected], work, 'optimize')
    run([args.opt, '-passes=verify', '-disable-output', selected], work, 'verify-final')
    ir = raw.read_text()
    prefix = 'dragon_static' if static else 'dragon_v2'
    names = []
    for pc in entries:
        symbol = f'{prefix}_x86_64_{pc:x}'
        header = re.search(r'define void @' + symbol + r'\([^\n]*\) #(\d+)', ir)
        attrs = re.search(r'^attributes #' + header[1] + r' = \{([^\n]*)', ir, re.M) if header else None
        resume = re.search(r'"dragon.entrypoints"="([^"]*)"', attrs[1]) if attrs else None
        names.extend((int(x, 16), symbol) for x in resume[1].split(',')) if resume else names.append((pc, symbol))
    declarations = '\n'.join(f'void {symbol}(S*,M*,C*,E*);' for symbol in sorted({s for _, s in names}))
    driver = compiler.DRIVER.replace('DECLARATIONS', declarations).replace('ENTRIES', ','.join(f'{{{pc}ULL,{symbol}}}' for pc, symbol in names))
    driver = driver.replace('INITIAL_COUNT', str(len(names))).replace('INITIAL_BUDGET', '10000').replace('RESUME_CODE', '')
    if static:
        # Unknown callees remain opaque during all optimization. The test-only
        # dispatcher is linked afterwards and executes only lifted Guest bytes.
        external = 'void dragon_static_external(S*s,M*m,C*c,E*e){uint64_t pc=get64(s->regs_bytes+128);'
        external += ''.join(f'if(pc=={pc}ULL){{{symbol}(s,m,c,e);return;}}' for pc, symbol in names)
        external += 'e->reason=DRAGON_EXIT_NEED_TARGET;}\n'
        driver = driver.replace('int main(', external + 'int main(')
        driver = driver.replace('dragon_llvm_execute_v2(&s,&m,&c,&e,0xf000);', f'{prefix}_x86_64_1000(&s,&m,&c,&e);')
        driver = driver.replace('}; E e={0};', '}; E e={0}; c.budget=0;')
        driver = driver.replace('  f=fopen(argv[2]', '  if(c.budget!=0)return 95;\n  f=fopen(argv[2]')
    if args.control == 'llvm-register':
        driver = driver.replace('  f=fopen(argv[2]', '  regs[0]^=1;\n  f=fopen(argv[2]')
    elif args.control == 'llvm-memory':
        driver = driver.replace('  f=fopen(argv[2]', f'  memory[{base.DATA}]^=1;\n  f=fopen(argv[2]')
    (work / 'driver.c').write_text(driver)
    triple = {'aarch64': 'aarch64-linux-gnu', 'x86_64': 'x86_64-linux-gnu'}.get(platform.machine())
    if not triple:
        raise RuntimeError('native validation requires a Linux aarch64/x64 host')
    run([args.llc, '-O0', '-filetype=obj', '-mtriple=' + triple, selected, '-o', 'module.o'], work, 'llc')
    runtime = Path(base.dragon._LIB).resolve()
    link = [] if static else [str(runtime), '-Wl,-rpath,' + str(runtime.parent)]
    run([args.cc, '-O0', '-I' + str(base.ROOT / 'include'), '-I' + str(base.ROOT / 'c_shim'),
         'driver.c', 'module.o', *link, '-o', 'native'], work, 'native-link')
    return work / 'native', dict(entries=entries, unlifted=unlifted, elf_sha256=hashlib.sha256(data).hexdigest(),
                                 missing_symbol_entries=sorted(set(symbols.values()) - set(entries)),
                                 extra_function_entries=sorted(set(entries) - set(symbols.values())),
                                 unexpected_unreachable='unreachable' in selected.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tigress', help='Tigress executable or environment wrapper; required for non-plain profiles')
    parser.add_argument('--profile', action='append', choices=PROFILES + ['flatten-indirect', 'virtualize'])
    parser.add_argument('--version', help='recorded Tigress version; otherwise queried from the tool')
    parser.add_argument('--seeds', type=int, default=2)
    parser.add_argument('--first-seed', type=int, default=1901)
    parser.add_argument('--trials', type=int, default=16)
    parser.add_argument('--strategy', type=int, choices=[0, 1], default=0)
    parser.add_argument('--control', choices=['c-result', 'llvm-register', 'llvm-memory'])
    parser.add_argument('--expect-control', action='store_true')
    for name, default in [('clang', 'clang-21'), ('ld', 'ld.lld-21'), ('opt', 'opt-21'), ('llc', 'llc-21'), ('cc', 'gcc')]:
        parser.add_argument('--' + name, default=default)
    args = parser.parse_args()
    profiles = args.profile or PROFILES
    if args.seeds < 1 or args.trials < len(INPUTS):
        parser.error('positive seeds and at least 16 boundary inputs are required')
    if args.expect_control and not args.control:
        parser.error('--expect-control needs an explicit control')
    if profiles != ['plain'] and not args.tigress:
        parser.error('non-plain profiles require --tigress; missing tool is not a successful skip')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    version = args.version or 'not-used'
    if args.tigress and not args.version:
        version_output = run([args.tigress, '--version'], out, 'tigress-version')
        match = re.search(r'\b([234]\.\d+(?:\.\d+)?)\b', version_output)
        if not match:
            raise RuntimeError('cannot identify Tigress version; supply --version explicitly')
        version = match[1]
    # The C shim may dynamically link the analysis backend. Hash the actual
    # mapped backend too: hashing only libdragon_c.so would miss its changes.
    mapped_backends = set()
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) == 6 and Path(fields[5]).name == 'libdragon.so':
            mapped_backends.add(Path(fields[5]))
    metadata = dict(tigress=args.tigress, tigress_version=version, profiles=profiles,
                    strategy=args.strategy, unicorn_version=unicorn_version,
                    clang=run([args.clang, '--version'], out, 'compiler-version').splitlines()[0],
                    source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    library_sha256=hashlib.sha256(Path(base.dragon._LIB).read_bytes()).hexdigest(),
                    backend_libraries=[dict(path=str(p), sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(mapped_backends)],
                    x86_analysis_sha256=hashlib.sha256((base.ROOT / 'lib/disassemblers/x86/x86Analysis.cpp').read_bytes()).hexdigest(),
                    x86_info_sha256=hashlib.sha256((base.ROOT / 'lib/disassemblers/x86/x86Info.cpp').read_bytes()).hexdigest(),
                    llvm_lifter_sha256=hashlib.sha256((base.ROOT / 'lib/LLVMLifter.cpp').read_bytes()).hexdigest())
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    adapter = guest.Adapter('x86_64')
    rows, c_rows, analyses = [], [], []
    for seed in range(args.first_seed, args.first_seed + args.seeds):
        seed_dir = out / str(seed)
        seed_dir.mkdir(exist_ok=True)
        original = seed_dir / 'original.c'
        original.write_text(source(seed))
        references = {}
        for c_level in ('O0', 'O2'):
            _, regions, _ = build_image(original, seed_dir / ('original-' + c_level), c_level, args)
            for trial in range(args.trials):
                regs, memory = initial(adapter, regions, trial)
                result = unicorn(adapter, regs, memory)
                observed = observables(result)
                specified = specification(seed, INPUTS[trial % len(INPUTS)])
                if observed != specified:
                    raise RuntimeError(f'original C/specification disagree seed={seed} trial={trial}')
                references[c_level, trial] = observed
        for profile in profiles:
            work = seed_dir / profile
            work.mkdir(exist_ok=True)
            input_c, output_c = work / 'input.c', work / 'transformed.c'
            input_c.write_text(source(seed, wrong=args.control == 'c-result'))
            transform_error = None
            try:
                if profile == 'plain':
                    output_c.write_text(input_c.read_text())
                else:
                    run([args.tigress, '--Environment=x86_64:Linux:Gcc:4.6', '--Seed=' + str(seed),
                         *transforms(profile, version), '--out=' + str(output_c), input_c], work, 'tigress')
            except Exception as error:
                transform_error = str(error)
            for c_level in ('O0', 'O2'):
                case = f'{seed}-{profile}-{c_level}'
                image_work = work / c_level
                try:
                    if transform_error:
                        raise RuntimeError(transform_error)
                    data, regions, symbols = build_image(output_c, image_work, c_level, args)
                    expected_trials = []
                    for trial in range(args.trials):
                        regs, memory = initial(adapter, regions, trial)
                        diagnostic = {}
                        (image_work / f'reference-input-{trial}.bin').write_bytes(regs + memory)
                        try:
                            expected = unicorn(adapter, regs, memory, diagnostic)
                            expected_trials.append((trial, regs, memory, expected))
                            got, want = observables(expected), references[c_level, trial]
                            c_rows.append(dict(case=case, trial=trial, status='match' if got == want else 'different',
                                               got=[got[0], got[1].hex()], expected=[want[0], want[1].hex()]))
                        except Exception as error:
                            c_rows.append(dict(case=case, trial=trial, status='reference_error', detail=str(error), last_instruction=diagnostic))
                    for plan, static, product, level in PLANS:
                        if not expected_trials:
                            continue  # explicitly counted as unexecuted, never successful skips
                        native_work = image_work / plan
                        native_work.mkdir(exist_ok=True)
                        try:
                            exe, analysis = compile_image(data, symbols, native_work, static, product, level, args)
                            analyses.append(dict(case=case, plan=plan, symbols=symbols, **analysis))
                            for trial, regs, memory, expected in expected_trials:
                                actual, error = base.native(exe, regs, memory, native_work, trial, extra_bytes=64)
                                row = dict(case=case, plan=plan, trial=trial)
                                if error:
                                    row.update(status=error['kind'], detail=error)
                                else:
                                    record = struct.unpack('<8Q', actual[3])
                                    root_return = record[0] == 1 and record[1] == 3 and record[3] == base.SENTINEL and record[4] == 1
                                    if root_return:
                                        differences, _ = adapter.compare(dict(undefined_flags=[]), expected, actual, False)
                                        if analysis['unexpected_unreachable']:
                                            differences.append(dict(field='ir', detail='unexpected unreachable'))
                                        row.update(status='different' if differences else 'match', differences=differences,
                                                   register_fields_compared=len(adapter.fields), memory_bytes_compared=base.MEM_SIZE)
                                    elif record[0] in (2, 3, 4, 5, 6):
                                        # A partial snapshot cannot be compared with a completed
                                        # function's snapshot and called a semantic mismatch/pass.
                                        row.update(status='incomplete', detail='explicit exit before root return; no complete-function comparison')
                                    else:
                                        row.update(status='different', differences=[dict(field='exit', got=record[:7], expected=expected[3])])
                                    row.update(exit=record[:7], root_return=root_return)
                                rows.append(row)
                        except Exception as error:
                            rows.append(dict(case=case, plan=plan, status='infrastructure_error', detail=str(error)))
                except Exception as error:
                    c_rows.append(dict(case=case, status='infrastructure_error', detail=str(error)))
                print(case, dict(collections.Counter(r['status'] for r in rows if r['case'] == case)), flush=True)
                summary = dict(metadata, cases=len(profiles) * args.seeds * 2,
                               c_checks=len(c_rows), c_statuses=dict(collections.Counter(r['status'] for r in c_rows)),
                               comparisons=len(rows), statuses=dict(collections.Counter(r['status'] for r in rows)),
                               planned_c_checks=len(profiles) * args.seeds * 2 * args.trials,
                               planned_comparisons=len(profiles) * args.seeds * 2 * len(PLANS) * args.trials,
                               original_specification_checks=args.seeds * 2 * args.trials,
                               root_returns=sum(r.get('root_return', False) for r in rows),
                               boundaries=sum('exit' in r and not r['root_return'] for r in rows),
                               skipped=0, undefined_exclusions=0, control=args.control,
                               register_fields_compared=sum(r.get('register_fields_compared', 0) for r in rows),
                               memory_bytes_compared=sum(r.get('memory_bytes_compared', 0) for r in rows))
                summary['unexecuted_c_checks'] = summary['planned_c_checks'] - sum('trial' in r for r in c_rows)
                summary['unexecuted_comparisons'] = summary['planned_comparisons'] - sum('trial' in r for r in rows)
                image_analyses = [a for a in analyses if a['plan'] == 'v2-O0']
                summary['function_entry_checks'] = len(image_analyses)
                summary['function_entry_failures'] = sum(bool(a['missing_symbol_entries'] or a['extra_function_entries']) for a in image_analyses)
                summary['missing_symbol_entries'] = sum(len(a['missing_symbol_entries']) for a in image_analyses)
                summary['extra_function_entries'] = sum(len(a['extra_function_entries']) for a in image_analyses)
                summary['unlifted_instructions'] = sum(len(a['unlifted']) for a in image_analyses)
                (out / 'results.json').write_text(json.dumps(dict(summary=summary, c_results=c_rows, analyses=analyses, results=rows), indent=2))
    print(json.dumps(summary, indent=2))
    c_status = 'different' if args.expect_control and args.control == 'c-result' else 'match'
    llvm_status = 'different' if args.expect_control and args.control in ('llvm-register', 'llvm-memory') else 'match'
    complete = len(c_rows) == summary['cases'] * args.trials and len(rows) == summary['cases'] * len(PLANS) * args.trials
    correct = complete and not summary['function_entry_failures'] and all(r['status'] == c_status for r in c_rows) and all(r['status'] == llvm_status and r.get('root_return') for r in rows)
    return 0 if correct else 1


if __name__ == '__main__':
    raise SystemExit(main())
