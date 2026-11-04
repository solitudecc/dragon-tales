"""Obfuscate a complete streaming SHA-256 and audit Dragon's static analysis.

Host C and Unicorn digests must agree with hashlib/NIST vectors. Each ELF's
full LLVM state is then compared with that same ELF's Unicorn execution.
Unresolved exits, wrong entries, missing observed edges and errors cannot pass.
No dynamic edges are ever fed back into the static analysis.
"""
import argparse
import collections
import hashlib
import json
import random
import re
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn import x86_const as U
import tigress_function_diff as image_test

base = image_test.base
INPUT = 0x9000
PROFILES = ('plain', 'flatten-switch', 'arithmetic-flatten')
HOST_DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
unsigned kernel(const unsigned char *, unsigned, unsigned char *, unsigned);
INIT_DECLARATION
static unsigned char input[1000001];
int main(int argc, char **argv) {
  unsigned char digest[32];
  unsigned n=(unsigned)fread(input,1,sizeof(input),stdin);
  if(ferror(stdin) || n==sizeof(input) || argc!=2) return 2;
  INIT_CALL
  unsigned result=kernel(input,n,digest,(unsigned)strtoul(argv[1],0,10));
  if(result!=((unsigned)digest[0]<<24 | (unsigned)digest[1]<<16 |
              (unsigned)digest[2]<<8 | digest[3])) return 3;
  for(unsigned i=0;i<32;++i) printf("%02x",digest[i]);
  puts("");return 0;
}
'''


def vectors():
    # The first two nonempty messages are the NIST NSRL published vectors.
    cases = [('empty', b'', 0), ('abc', b'abc', 1),
             ('nist-56', b'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 7)]
    rng = random.Random(2561901)
    for i, length in enumerate((1, 2, 55, 56, 57, 63, 64, 65, 111, 112, 119, 120, 127, 128, 129, 255, 1024, 4096)):
        cases.append((f'boundary-{length}', rng.randbytes(length), (0, 1, 7, 64)[i % 4]))
    cases += [('zeros-64', bytes(64), 0), ('ones-65', b'\xff' * 65, 1),
              ('all-bytes', bytes(range(256)), 7), ('abc-whole', b'abc', 0)]
    assert hashlib.sha256(cases[1][1]).hexdigest() == 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'
    assert hashlib.sha256(cases[2][1]).hexdigest() == '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1'
    return cases


def initial(adapter, regions, trial, message, chunk):
    fixture = dict(code_hex='', stubs=[], initial_sp=base.STACK + 8,
                   set_gprs={'RDI': INPUT, 'RSI': len(message), 'RDX': base.DATA, 'RCX': chunk})
    regs, memory = adapter.initial(fixture, trial, image_test.ENTRY)
    regs[142] = 0
    for address, data in regions:
        memory[address:address + len(data)] = data
    memory[INPUT:INPUT + len(message)] = message
    return regs, memory


def reference(adapter, regs, memory, budget):
    """x64 full-state oracle with an explicit budget for long SHA computations."""
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    uc.mem_map(0, base.MEM_SIZE)
    uc.mem_write(0, bytes(memory))
    for _, off, width, reg in adapter.gprs:
        uc.reg_write(reg, base.get(regs, off, width))
    uc.reg_write(U.UC_X86_REG_EFLAGS, 2 | sum(regs[o] << b for _, o, b in adapter.flags))
    for _, lo, hi, reg in adapter.vectors:
        uc.reg_write(reg, base.get(regs, lo) | (base.get(regs, hi) << 64))
    for i in range(8):
        uc.reg_write(U.UC_X86_REG_FP0 + i, (base.get(regs, 400 + 10*i), base.get(regs, 408 + 10*i, 2)))
    for off, reg in ((482, U.UC_X86_REG_FPTAG), (484, U.UC_X86_REG_FPCW), (486, U.UC_X86_REG_FPSW)):
        uc.reg_write(reg, base.get(regs, off, 2))
    visits, transfers, last = collections.Counter(), set(), [None, 0]
    def hook(u, pc, size, _):
        if last[0] is not None:
            transfers.add((last[0], pc))
        visits[pc] += 1
        last[:] = [pc, size]
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(image_test.ENTRY, base.SENTINEL, count=budget, timeout=30000000)
    pc = uc.reg_read(U.UC_X86_REG_RIP)
    if pc != base.SENTINEL or last[0] is None:
        raise RuntimeError(f'reference incomplete at {pc:#x}; instructions={sum(visits.values())}, budget={budget}')
    if not base.is_near_return(bytes(uc.mem_read(last[0], last[1]))):
        raise RuntimeError('sentinel reached without a Guest RET')
    out = bytearray(regs)
    for _, off, _, reg in adapter.gprs:
        base.put(out, off, uc.reg_read(reg))
    base.put(out, 128, pc)
    flags = uc.reg_read(U.UC_X86_REG_EFLAGS)
    for _, off, bit in adapter.flags:
        out[off] = (flags >> bit) & 1
    for _, lo, hi, reg in adapter.vectors:
        value = uc.reg_read(reg)
        base.put(out, lo, value & ((1 << 64) - 1));base.put(out, hi, value >> 64)
    for i in range(8):
        lo, hi = uc.reg_read(U.UC_X86_REG_FP0 + i)
        base.put(out, 400 + 10*i, lo);base.put(out, 408 + 10*i, hi, 2)
    sw = uc.reg_read(U.UC_X86_REG_FPSW);out[480] = (sw >> 11) & 7
    for off, reg in ((482, U.UC_X86_REG_FPTAG), (484, U.UC_X86_REG_FPCW), (486, U.UC_X86_REG_FPSW)):
        base.put(out, off, uc.reg_read(reg), 2)
    snapshot = bytes(out), bytes(uc.mem_read(0, base.MEM_SIZE)), collections.Counter(), (1, 3, last[0], pc, 1, 0, 0)
    return snapshot, visits, transfers


def static_audit(data, symbols, strategy, work, visits, transfers):
    image = base.dragon.LoadedImage.load_bytes(data)
    graph = image.disassemble(base.dragon.Configuration(strategy=strategy))
    entries = {pc for pc in graph.function_addresses() if graph.get_function(pc).is_valid}
    instructions = {i.address: i for i in graph.instructions()}
    functions, unlifted, unresolved = [], [], []
    branch_successors = {}
    for address in graph.block_addresses():
        block = graph.get_block(address)
        if block.is_valid:
            branch_successors[block.terminator_address] = set(block.successors())
    names = {pc: name for name, pc in symbols.items()}
    for pc in sorted(entries):
        f = graph.get_function(pc)
        functions.append(dict(name=names.get(pc), address=pc, blocks=f.num_blocks,
                              instructions=f.num_instructions, bytes=f.byte_size,
                              edges=f.edges, complexity=f.cyclomatic_complexity))
        dot = ['digraph cfg {', 'node [shape=box];']
        for address in f.block_addresses():
            block = graph.get_block(address)
            text = '\\l'.join(f'{i.address:x}: {i.disassembly}'.replace('"', '\\"') for i in block.instructions()) + '\\l'
            dot.append(f'n{address:x} [label="{text}"];')
            dot.extend(f'n{address:x} -> n{target:x};' for target in block.successors())
        (work / f'cfg-{names.get(pc, hex(pc))}.dot').write_text('\n'.join(dot + ['}']))
    for address in graph.block_addresses():
        block = graph.get_ignil_block(address)
        if block:
            unlifted.extend(block.unlifted())
    for i in instructions.values():
        if i.is_jump and i.has_indirect_target and not i.branches:
            unresolved.append(dict(address=i.address, instruction=i.disassembly))
    # Observed transitions are a separate check, never analysis hints. This
    # checks only paths actually executed and uses Dragon's transfer classifier.
    observed_jumps = {(source, target) for source, target in transfers
                      if source in instructions and instructions[source].is_jump}
    def missing(successors):
        return [dict(source=s, target=t) for s, t in sorted(observed_jumps) if t not in successors.get(s, set())]
    missing_edges = missing(branch_successors)
    candidates = [(s, t) for s, t in sorted(observed_jumps) if t in branch_successors.get(s, set())]
    checker_control = False
    if candidates:
        source, target = candidates[0]
        damaged = {pc: set(targets) for pc, targets in branch_successors.items()}
        damaged[source].remove(target)
        checker_control = dict(source=source, target=target) in missing(damaged)
    result = dict(functions=functions, blocks=len(graph.block_addresses()), instructions=len(instructions),
                  missing_entries=sorted(set(symbols.values()) - entries), extra_entries=sorted(entries - set(symbols.values())),
                  unlifted=sorted(set(unlifted)), unresolved_jumps=unresolved,
                  observed_instructions=len(visits), observed_instruction_executions=sum(visits.values()),
                  observed_jump_edges=len(observed_jumps), omitted_edge_control_detected=checker_control,
                  observed_missing_instructions=sorted(set(visits) - set(instructions)),
                  observed_missing_jump_edges=missing_edges, elf_sha256=hashlib.sha256(data).hexdigest())
    (work / 'analysis.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--tigress')
    p.add_argument('--profile', action='append', choices=PROFILES)
    p.add_argument('--c-level', action='append', choices=['O0', 'O2'])
    p.add_argument('--strategy', action='append', type=int, choices=[0, 1])
    p.add_argument('--plan', action='append', choices=[x[0] for x in image_test.PLANS])
    p.add_argument('--instruction-budget', type=int, default=10000000)
    p.add_argument('--block-budget', type=int, default=1000000)
    p.add_argument('--control', choices=['c-digest', 'llvm-register', 'llvm-memory'])
    p.add_argument('--expect-control', action='store_true')
    p.add_argument('--quick', action='store_true', help='five boundary vectors; smoke checks only')
    for name, default in [('clang', 'clang-21'), ('ld', 'ld.lld-21'), ('opt', 'opt-21'), ('llc', 'llc-21'), ('cc', 'gcc')]:
        p.add_argument('--' + name, default=default)
    args = p.parse_args()
    profiles, levels, strategies = args.profile or list(PROFILES), args.c_level or ['O0', 'O2'], args.strategy or [0, 1]
    requested_plans = args.plan or ['v2-O0', 'product-O2']
    plans = [x for x in image_test.PLANS if x[0] in requested_plans]
    if not plans or {x[0] for x in plans} != set(requested_plans):
        p.error('every requested execution plan must be recognized')
    if profiles != ['plain'] and not args.tigress:
        p.error('non-plain profiles require --tigress')
    if args.expect_control and not args.control:
        p.error('--expect-control needs a control')
    if min(args.instruction_budget, args.block_budget) < 1:
        p.error('budgets must be positive')
    out = args.out.resolve();out.mkdir(parents=True, exist_ok=True)
    source = base.ROOT / 'tests/fixtures/tigress-sha256.c'
    (out / 'original.c').write_bytes(source.read_bytes())
    version = image_test.run([args.tigress, '--version'], out, 'tigress-version') if args.tigress else 'not used'
    if args.tigress and '2.1' not in version:
        p.error('this campaign is pinned to Tigress 2.1')
    metadata = dict(tigress=version, profiles=profiles, c_levels=levels, strategies=strategies,
                    plans=[x[0] for x in plans], instruction_budget=args.instruction_budget,
                    block_budget=args.block_budget, control=args.control, quick=args.quick,
                    fixture_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                    harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    llvm_lifter_sha256=hashlib.sha256((base.ROOT / 'lib/LLVMLifter.cpp').read_bytes()).hexdigest(),
                    x86_analysis_sha256=hashlib.sha256((base.ROOT / 'lib/disassemblers/x86/x86Analysis.cpp').read_bytes()).hexdigest(),
                    backend_libraries=[dict(path=s, sha256=hashlib.sha256(Path(s).read_bytes()).hexdigest())
                                       for s in sorted({line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
                                                        if line.split()[-1].endswith(('/libdragon.so', '/libdragon_c.so'))})],
                    unicorn_version=image_test.unicorn_version)
    (out / 'metadata.json').write_text(json.dumps(metadata, indent=2))
    # This campaign's driver budget is explicit and independent of the short
    # fixture default. Product IR and static-mode zero-budget checks are intact.
    image_test.compiler.DRIVER = image_test.compiler.DRIVER.replace('INITIAL_BUDGET', str(args.block_budget))
    adapter = image_test.guest.Adapter('x86_64')
    cases = vectors()
    assert hashlib.sha256(b'a'*1000000).hexdigest() == 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0'
    if args.quick:
        cases = [cases[i] for i in (0, 1, 2, 9, 10)]
    host_rows, c_rows, llvm_rows, audits = [], [], [], []
    def save():
        summary = dict(metadata, host_checks=len(host_rows), host_statuses=dict(collections.Counter(r['status'] for r in host_rows)),
                       c_checks=len(c_rows), c_statuses=dict(collections.Counter(r['status'] for r in c_rows)),
                       planned_comparisons=len(profiles)*len(levels)*len(strategies)*len(plans)*len(cases),
                       comparisons=len(llvm_rows), statuses=dict(collections.Counter(r['status'] for r in llvm_rows)),
                       entry_checks=len(audits), entry_failures=sum(bool(a['missing_entries'] or a['extra_entries']) for a in audits),
                       unlifted=sum(len(a['unlifted']) for a in audits), skipped=0, undefined_exclusions=0,
                       register_fields_compared=sum(r.get('fields', 0) for r in llvm_rows),
                       memory_bytes_compared=sum(r.get('memory_bytes', 0) for r in llvm_rows))
        summary['unexecuted_comparisons'] = summary['planned_comparisons'] - sum('vector' in r for r in llvm_rows)
        (out / 'results.json').write_text(json.dumps(dict(summary=summary, host=host_rows, c=c_rows, llvm=llvm_rows, analyses=audits), indent=2))
        return summary
    for profile in profiles:
        work = out / profile;work.mkdir(exist_ok=True)
        transformed = work / 'transformed.c'
        input_c = work / 'input.c'
        text = source.read_text()
        if args.control == 'c-digest':
            text = text.replace('  return ((u32)out[0]', '  out[0]^=1;\n  return ((u32)out[0]')
        input_c.write_text(text)
        if profile == 'plain':
            transformed.write_bytes(input_c.read_bytes())
        else:
            functions = 'sha256_compress,sha256_update,sha256_finish'
            commands = []
            if profile == 'arithmetic-flatten':
                commands += ['--Transform=EncodeArithmetic', '--Functions=' + functions]
            commands += ['--Transform=Flatten', '--Functions=' + functions, '--FlattenDispatch=switch',
                         '--FlattenRandomizeBlocks=true', '--FlattenSplitBasicBlocks=true']
            image_test.run([args.tigress, '--Environment=x86_64:Linux:Gcc:4.6', '--Seed=2561901',
                            *commands, '--out=' + str(transformed), input_c], work, 'tigress')
        # Tigress lowers global initializers into real functions, called by its
        # generated main. A library entry must execute this same initialization;
        # copying the desired K bytes into memory would hide the omission.
        generated_init = bool(re.search(r'void\s+megaInit\s*\(void\)\s*\{', transformed.read_text()))
        if 'K_i$nit' in transformed.read_text() and not generated_init:
            raise RuntimeError('unrecognized Tigress global initialization contract')
        (work / 'initialization.json').write_text(json.dumps(dict(guest_initializer='megaInit' if generated_init else None)))
        host_driver = work / 'host-driver.c'
        host_driver.write_text(HOST_DRIVER.replace('INIT_DECLARATION', 'void megaInit(void);' if generated_init else '')
                               .replace('INIT_CALL', 'megaInit();' if generated_init else ''))
        image_test.WRAPPER = '''.section .text.entry,"ax",@progbits
.globl oracle_entry
.type oracle_entry,@function
oracle_entry:
 subq $40, %rsp
 movq %rdi, 0(%rsp)
 movq %rsi, 8(%rsp)
 movq %rdx, 16(%rsp)
 movq %rcx, 24(%rsp)
 INIT_CALL
 movq 0(%rsp), %rdi
 movq 8(%rsp), %rsi
 movq 16(%rsp), %rdx
 movq 24(%rsp), %rcx
 call kernel
 addq $40, %rsp
 cmpl %edi, %eax
 ret
.size oracle_entry,.-oracle_entry
.section .note.GNU-stack,"",@progbits
'''.replace('INIT_CALL', 'call megaInit' if generated_init else '')
        for level in levels:
            image_work = work / level;image_work.mkdir(exist_ok=True)
            # Tigress 2.1 may synthesize a dummy main in library-only input.
            # Rename that unused host entry at compilation, leaving all SHA
            # functions untouched. Guest --gc-sections drops the unused main.
            image_test.run([args.cc, '-' + level, '-Dmain=tigress_generated_main', '-c', transformed,
                            '-o', image_work / 'host-kernel.o'], out, profile + '-' + level + '-host-object')
            image_test.run([args.cc, '-' + level, image_work / 'host-kernel.o', host_driver,
                            '-o', image_work / 'host'], out, profile + '-' + level + '-host')
            host_cases = cases + ([] if args.quick else [('nist-million-a', b'a'*1000000, 64)])
            for name, message, chunk in host_cases:
                r = subprocess.run([str(image_work / 'host'), str(chunk)], input=message, capture_output=True, timeout=20)
                expected = hashlib.sha256(message).hexdigest()
                host_rows.append(dict(profile=profile, level=level, vector=name, chunk=chunk, length=len(message),
                                      status='match' if r.returncode == 0 and r.stdout.decode().strip() == expected else 'different',
                                      got=r.stdout.decode(errors='replace').strip(), expected=expected, returncode=r.returncode))
            data, regions, symbols = image_test.build_image(transformed, image_work, level, args)
            expected_trials, visits, transfers = [], collections.Counter(), set()
            for trial, (name, message, chunk) in enumerate(cases):
                regs, memory = initial(adapter, regions, trial, message, chunk)
                row = dict(profile=profile, level=level, vector=name, length=len(message), chunk=chunk)
                try:
                    expected, seen, edges = reference(adapter, regs, memory, args.instruction_budget)
                    visits.update(seen);transfers.update(edges)
                    digest = expected[1][base.DATA:base.DATA+32]
                    wanted = hashlib.sha256(message).digest()
                    right = digest == wanted and base.get(expected[0], 0, 4) == int.from_bytes(wanted[:4], 'big')
                    row.update(status='match' if right else 'different', digest=digest.hex(), expected=wanted.hex(), instructions=sum(seen.values()))
                    expected_trials.append((trial, name, regs, memory, expected))
                except Exception as error:
                    row.update(status='reference_error', detail=str(error))
                c_rows.append(row)
            (image_work / 'observed-transitions.json').write_text(json.dumps(sorted(transfers)))
            for strategy in strategies:
                analysis_work = image_work / ('linear' if strategy == 0 else 'recursive');analysis_work.mkdir(exist_ok=True)
                audit = static_audit(data, symbols, strategy, analysis_work, visits, transfers)
                audits.append(dict(profile=profile, level=level, strategy=strategy, **audit))
                for plan, static, product, llvm_level in plans:
                    args.strategy = strategy
                    args.control = metadata['control'] if metadata['control'] in ('llvm-register', 'llvm-memory') else None
                    native_work = analysis_work / plan;native_work.mkdir(exist_ok=True)
                    try:
                        exe, detail = image_test.compile_image(data, symbols, native_work, static, product, llvm_level, args)
                    except Exception as error:
                        llvm_rows.append(dict(profile=profile, level=level, strategy=strategy, plan=plan, status='infrastructure_error', detail=str(error)))
                        save();continue
                    for trial, name, regs, memory, expected in expected_trials:
                        actual, error = base.native(exe, regs, memory, native_work, trial, extra_bytes=64)
                        row = dict(profile=profile, level=level, strategy=strategy, plan=plan, vector=name)
                        if error:
                            row.update(status=error['kind'], detail=error)
                        else:
                            record = struct.unpack('<8Q', actual[3])
                            root = record[0] == 1 and record[1] == 3 and record[3] == base.SENTINEL and record[4] == 1
                            if root:
                                differences, _ = adapter.compare(dict(undefined_flags=[]), expected, actual, False)
                                if detail['unexpected_unreachable']:
                                    differences.append(dict(field='ir', detail='unexpected unreachable'))
                                row.update(status='different' if differences else 'match', differences=differences,
                                           fields=len(adapter.fields), memory_bytes=base.MEM_SIZE)
                            else:
                                row.update(status='incomplete' if record[0] in (2,3,4,5,6) else 'different')
                            row.update(exit=record[:7], root_return=root)
                        llvm_rows.append(row)
                    print(profile, level, strategy, plan, dict(collections.Counter(r['status'] for r in llvm_rows if r['profile']==profile and r['level']==level and r['strategy']==strategy and r['plan']==plan)), flush=True)
                    save()
            args.control = metadata['control']
    summary = save();print(json.dumps(summary, indent=2))
    c_status = 'different' if args.expect_control and args.control == 'c-digest' else 'match'
    llvm_status = 'different' if args.expect_control and args.control in ('llvm-register', 'llvm-memory') else 'match'
    good = all(r['status']==c_status for r in host_rows) and all(r['status']==c_status for r in c_rows)
    good &= all(r['status']==llvm_status and r.get('root_return') for r in llvm_rows)
    good &= not summary['entry_failures'] and not summary['unexecuted_comparisons']
    good &= all(not a['observed_missing_instructions'] and not a['observed_missing_jump_edges'] and not a['unlifted'] for a in audits)
    good &= all(a['omitted_edge_control_detected'] for a in audits)
    return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
