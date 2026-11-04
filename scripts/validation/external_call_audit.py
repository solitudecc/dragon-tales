#!/usr/bin/env python3
"""Audit how Dragon analyses calls to external (imported) functions in a real
ELF/PE image.

Ground truth is independent of Dragon:
  * import slots come from pyelftools (JUMP_SLOT/GLOB_DAT relocations, also
    against defined symbols, which the dynamic loader may interpose) or
    pefile (the IAT);
  * call/jump sites and PLT/thunk stubs come from objdump's own listing
    (`<name@plt>` headers, `jmp *slot` thunks and chains of `jmp thunk`,
    `call/jmp *slot` memory forms with rex.W/bnd/notrack prefixes, and a
    register loaded from a slot by straight-line code before `call *reg` /
    A64 `adrp; ldr; blr`);
  * function ranges come from objdump's symbol headers.

For every site the script records what Dragon produced:
  decoded     the instruction exists with the same size;
  edge        a direct call/jump has the stub as callee/branch;
  cont        a call to a returning API: the next instruction (same block) or
              the fall-through successor block belongs to the call's function;
  noreturn    a call to a known noreturn API (exit, abort, __stack_chk_fail,
              ...): whether Dragon keeps a fall-through edge, where it leads,
              and whether only through it another symbol's entry joins the
              caller's function (`merged`);
  llvm        (--llvm N) `!dragon.imports` of every lifted caller function:
              each recorded entry is correct, wrong (other name) or spurious
              (no import transfer there); a truth site without one is missing.

Usage: external_call_audit.py BIN OBJDUMP [--strategy 0|1] [--json OUT]
       [--llvm N]   lift at most N caller functions (0 = skip)
       [--control names|fallthrough]   negative controls
Exit status: 0 when no wrong/spurious provenance, 1 otherwise. Missing
names, missing continuations and noreturn fall-through edges are measured,
not asserted (Dragon deliberately assumes an unknown callee returns).
"""
import argparse, bisect, collections, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'python'))
import dragon  # noqa: E402

# APIs whose documented contract is not to return to the caller.
NORETURN = {
    'exit', '_exit', '_Exit', 'quick_exit', 'abort', 'longjmp', '_longjmp',
    'siglongjmp', '__longjmp_chk', '__stack_chk_fail', '__assert_fail',
    '__assert_perror_fail', '__fortify_fail', '__chk_fail', 'err', 'errx',
    'verr', 'verrx', 'pthread_exit', '__cxa_throw', '__cxa_rethrow',
    '__cxa_bad_cast', '__cxa_bad_typeid', '__cxa_throw_bad_array_new_length',
    '__cxa_pure_virtual', '_Unwind_Resume', '__libc_start_main',
    '_ZSt9terminatev', '__std_terminate', 'ExitProcess', 'ExitThread',
    'FatalExit', 'FatalAppExitA', 'FatalAppExitW', '_assert', '_wassert',
    '_amsg_exit', '__report_gsfailure', '_invalid_parameter_noinfo_noreturn',
    '__fastfail',
}


def is_noreturn(name):
    return name in NORETURN or name.startswith('_ZSt9__throw_') or \
        re.match(r'_ZSt\d+__throw_', name) is not None


def strip_version(name):
    return name.split('@', 1)[0]


# -- ground truth --------------------------------------------------------------

def elf_slots(path):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
    slots = {}
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        machine = elf['e_machine']
        wanted = {'EM_X86_64': {6, 7}, 'EM_386': {6, 7},
                  'EM_AARCH64': {1025, 1026}}.get(machine, set())
        for sec in elf.iter_sections():
            if not isinstance(sec, RelocationSection) or sec['sh_link'] == 0:
                continue
            symtab = elf.get_section(sec['sh_link'])
            for rel in sec.iter_relocations():
                if rel['r_info_type'] not in wanted or rel['r_info_sym'] == 0:
                    continue
                sym = symtab.get_symbol(rel['r_info_sym'])
                if not sym.name:
                    continue
                # A defined symbol bound through JUMP_SLOT/GLOB_DAT is still
                # an import slot: the dynamic loader may interpose it.
                if sym['st_info']['type'] not in ('STT_FUNC', 'STT_NOTYPE',
                                                  'STT_GNU_IFUNC'):
                    continue
                slots[rel['r_offset']] = strip_version(sym.name)
    return slots


def pe_slots(path):
    import pefile
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    slots = {}
    for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        for imp in entry.imports:
            name = imp.name.decode('utf-8', 'replace') if imp.name else f'#{imp.ordinal}'
            slots[imp.address] = name
    return slots


HEADER = re.compile(r'^([0-9a-f]+) <(.+)>:$')
INSN = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
DIRECT = re.compile(r'^(?:0x)?([0-9a-f]+)(?:\s+<([^>]+)>)?\s*$')
COMMENT = re.compile(r'#\s*(?:0x)?([0-9a-f]+)')
ABSMEM = re.compile(r'^\*(?:%ds:)?0x([0-9a-f]+)\s*$')   # i386 call *0x40a1b4

CALLS = {'call', 'calll', 'callq', 'bl'}
JUMPS = {'jmp', 'jmpl', 'jmpq', 'b'}


def parse_objdump(text):
    """Instructions as (addr, mnemonic, operands) in order + symbol headers."""
    insns, headers = [], []
    for line in text.splitlines():
        m = HEADER.match(line)
        if m:
            headers.append((int(m.group(1), 16), m.group(2)))
            continue
        m = INSN.match(line)
        if m and not m.group(2).startswith('(bad)'):
            insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return insns, headers


def target_of(mnem, ops):
    """('direct', addr) / ('mem', slot) / None for a call/jmp operand string."""
    ops = ops.split('//')[0].strip() if mnem in ('bl', 'b') else ops.strip()
    if ops.startswith('*'):
        m = COMMENT.search(ops)
        if m and '(%rip)' in ops:
            return 'mem', int(m.group(1), 16)
        m = ABSMEM.match(ops.split('#')[0].strip())
        if m:
            return 'mem', int(m.group(1), 16)
        return None
    m = DIRECT.match(ops.split('#')[0].strip())
    if m:
        return 'direct', int(m.group(1), 16)
    return None


X86_BRANCH = re.compile(r'^(j|call|ret|loop|bnd|notrack|int|hlt|ud2|syscall)')
A64_BRANCH = re.compile(r'^(b|bl|br|blr|ret|cb|tb|svc|brk)(\.|$)')


def _x86_reg(op):
    op = op.strip()
    m = re.fullmatch(r'%([re]?)([a-z0-9]+?)(d)?', op)
    if not m:
        return None
    # %edi / %rdi, %r8d / %r8 are one register for this purpose.
    return m.group(2)


def _att_dest(ops):
    depth, last = 0, 0
    body = ops.split('#')[0]
    for i, ch in enumerate(body):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch == ',' and depth == 0:
            last = i + 1
    return body[last:].strip(), body[:max(last - 1, 0)].strip() if last else ''


def reg_indirect_slot(insns, i, lo, slots, a64):
    """Slot an import-loaded register call/jump at insns[i] reads, by a
    backward scan of at most 10 straight-line instructions within its symbol;
    None when any intervening instruction might change the register."""
    mnem, ops = insns[i][1], insns[i][2]
    if a64:
        reg = ops.strip().split()[0] if ops.strip() else ''
        if not re.fullmatch(r'x\d+', reg):
            return None
    else:
        if not ops.strip().startswith('*%'):
            return None
        reg = _x86_reg(ops.strip()[1:].split()[0])
    base = imm = None
    for j in range(i - 1, max(lo, i - 11) - 1, -1):
        _, m2, o2 = insns[j]
        if a64:
            if A64_BRANCH.match(m2):
                return None
            parts = [x.strip() for x in o2.split('//')[0].split(',')]
            dest = parts[0] if parts else ''
            if base is None:
                if dest != reg:
                    continue
                mm = re.fullmatch(r'ldr\s*', m2) and re.match(
                    r'\[(x\d+)(?:,\s*#(\d+))?\]$', ','.join(parts[1:]).strip())
                if not mm:
                    return None
                base, imm = mm.group(1), int(mm.group(2) or 0)
                continue
            if dest == base:
                mm = re.match(r'adrp\s*$', m2) and re.match(r'([0-9a-f]+)', parts[1] if len(parts) > 1 else '')
                if not mm:
                    return None
                slot = int(mm.group(1), 16) + imm
                return slot if slot in slots else None
            if dest == reg:
                return None
        else:
            if X86_BRANCH.match(m2):
                return None
            dest, src = _att_dest(o2)
            if _x86_reg(dest) != reg:
                if m2.startswith(('xchg', 'cmpxchg', 'xadd', 'cpuid', 'rdtsc',
                                  'mul', 'div', 'imul', 'idiv', 'cwd', 'cdq',
                                  'cqo', 'pop', 'lods', 'rep', 'leave')):
                    return None  # implicit or two-register writers
                continue
            if not re.fullmatch(r'mov[lq]?', m2):
                return None
            if '(%rip)' in src:
                c = COMMENT.search(o2)
                slot = int(c.group(1), 16) if c else None
            else:
                c = re.fullmatch(r'(?:%ds:)?0x([0-9a-f]+)', src)
                slot = int(c.group(1), 16) if c else None
            return slot if slot in slots else None
    return None


def ground_truth(path, objdump, fmt, a64):
    slots = elf_slots(path) if fmt == dragon.Format.ELF else pe_slots(path)
    text = subprocess.run([objdump, '-d', '-w', '--no-show-raw-insn', path],
                          capture_output=True, text=True, check=True).stdout
    insns, headers = parse_objdump(text)
    for k, (a, m, o) in enumerate(insns):
        if m in ('bnd', 'notrack', 'rex.W', 'rex') and o.split()[:1] and o.split()[0] in JUMPS | CALLS:
            insns[k] = (a, o.split()[0], o.split(None, 1)[1] if len(o.split()) > 1 else '')
    sizes = {}
    for (a, _, _), (b, _, _) in zip(insns, insns[1:]):
        sizes[a] = b - a
    header_addrs = sorted(a for a, _ in headers)
    def region_start(addr):
        k = bisect.bisect_right(header_addrs, addr) - 1
        return header_addrs[k] if k >= 0 else 0
    # Stubs: objdump-named `x@plt`, and any instruction `jmp *slot` (PE thunks,
    # -fno-plt thunks).
    stubs = {}
    for addr, name in headers:
        if name.endswith('@plt'):
            base = strip_version(name[:-4])
            if base:
                stubs[addr] = base
    for addr, mnem, ops in insns:
        if mnem in JUMPS:
            t = target_of(mnem, ops)
            if t and t[0] == 'mem' and t[1] in slots:
                stubs.setdefault(addr, slots[t[1]])
    # Thunk chains: an instruction `jmp X` whose X is a stub is itself a stub
    # (mingw `jmp __imp_thunk`, operator delete[] -> delete -> free).
    for _ in range(8):
        grew = False
        for addr, mnem, ops in insns:
            if mnem in JUMPS and addr not in stubs:
                t = target_of(mnem, ops)
                if t and t[0] == 'direct' and t[1] in stubs:
                    stubs[addr] = stubs[t[1]]
                    grew = True
        if not grew:
            break
    plt_heads = {a for a, n in headers if n.endswith('@plt')}
    addr_index = {a: k for k, (a, _, _) in enumerate(insns)}
    sites = []
    for k, (addr, mnem, ops) in enumerate(insns):
        kind = 'call' if mnem in CALLS | {'blr'} else \
            'jump' if mnem in JUMPS | {'br'} else None
        if not kind:
            continue
        t = target_of(mnem, ops) if mnem not in ('blr', 'br') else None
        slot = None
        if t and t[0] == 'mem' and t[1] in slots:
            slot = t[1]
        elif not t:
            slot = reg_indirect_slot(insns, k, addr_index.get(region_start(addr), 0),
                                     slots, a64)
        in_plt = region_start(addr) in plt_heads
        if t and t[0] == 'direct' and t[1] in stubs:
            sites.append(dict(addr=addr, size=sizes.get(addr), kind=kind, in_plt=in_plt,
                              form='stub', target=t[1], name=stubs[t[1]]))
        elif slot is not None:
            sites.append(dict(addr=addr, size=sizes.get(addr), kind=kind, in_plt=in_plt,
                              form='slot' if t else 'reg', target=slot,
                              name=slots[slot]))
    funcs = sorted({a for a, n in headers if not n.endswith('@plt')})
    return slots, stubs, sites, funcs


# -- Dragon side ---------------------------------------------------------------

def dragon_view(img, strategy):
    cfg = dragon.Configuration(strategy=strategy)
    graph = img.disassemble(cfg)
    blocks = sorted(graph.block_addresses())
    block_info = {}
    for b in blocks:
        bb = graph.get_block(b)
        if not bb.is_valid:
            continue
        block_info[b] = (bb.terminator_address, set(bb.successors()))
    owner = collections.defaultdict(set)   # block -> function starts
    fblocks = {}
    for f in graph.function_addresses():
        fn = graph.get_function(f)
        if not fn.is_valid:
            continue
        bl = set(fn.block_addresses())
        fblocks[f] = bl
        for b in bl:
            owner[b].add(f)
    return graph, blocks, block_info, owner, fblocks


def block_of(blocks, block_info, addr):
    i = bisect.bisect_right(blocks, addr) - 1
    if i < 0:
        return None
    b = blocks[i]
    info = block_info.get(b)
    if info and b <= addr <= info[0]:
        return b
    return None


def parse_import_md(ir):
    """(site -> name) from !dragon.imports entries."""
    nodes = {}
    for line in ir.splitlines():
        m = re.match(r'^!(\d+) = !\{!"([^"]*)", i64 (-?\d+), i64 (-?\d+), !"([^"]*)"\}', line)
        if m:
            nodes[m.group(1)] = (int(m.group(3)) & (2**64 - 1), m.group(5))
    out = collections.defaultdict(set)
    for line in ir.splitlines():
        if line.startswith('!dragon.imports = !{'):
            for ref in re.findall(r'!(\d+)', line[len('!dragon.imports'):]):
                if ref in nodes:
                    site, name = nodes[ref]
                    out[site].add(name)
    return out


def audit(path, objdump, strategy, llvm_limit, control=None):
    img = dragon.LoadedImage.load_file(path)
    slots, stubs, sites, funcs = ground_truth(
        path, objdump, img.format, img.architecture == dragon.Architecture.ARM64)
    if control == 'names':
        # Negative control: every expected name is wrong; provenance must
        # report wrong (never correct) for each recorded site.
        for site in sites:
            site['name'] = site['name'] + '.control'
    graph, blocks, block_info, owner, fblocks = dragon_view(img, strategy)
    func_set = set(funcs)
    preds = collections.defaultdict(set)
    for blk, (_, succ) in block_info.items():
        for t in succ:
            preds[t].add(blk)
    counts = collections.Counter()
    details = collections.defaultdict(list)

    def next_func(addr):
        i = bisect.bisect_right(funcs, addr)
        return funcs[i] if i < len(funcs) else None

    def symbol_func(addr):
        i = bisect.bisect_right(funcs, addr) - 1
        return funcs[i] if i >= 0 else None

    for s in sites:
        if s['in_plt']:
            continue  # the PLT stub's own jump: provenance truth only
        a = s['addr']
        nr = is_noreturn(s['name'].removesuffix('.control'))
        tag = f"{s['kind']}-{s['form']}" + ('-noreturn' if nr else '')
        counts[f'sites.{tag}'] += 1
        insn = graph.get_instruction(a)
        if insn is None or (s['size'] and insn.size != s['size']):
            counts[f'undecoded.{tag}'] += 1
            details['undecoded'].append((hex(a), s['name']))
            continue
        if s['form'] == 'stub':
            targets = insn.callees if s['kind'] == 'call' else insn.branches
            if s['target'] not in targets:
                counts[f'no_edge.{tag}'] += 1
                details['no_edge'].append((hex(a), s['name'], [hex(t) for t in targets]))
        b = block_of(blocks, block_info, a)
        if b is None:
            counts[f'no_block.{tag}'] += 1
            continue
        fs = owner.get(b, set())
        if not fs:
            counts[f'no_function.{tag}'] += 1
        if s['kind'] != 'call':
            continue
        ft = a + insn.size + (1 if control == 'fallthrough' else 0)
        fb = ft if ft in block_info else block_of(blocks, block_info, ft)
        mid_block = block_info[b][0] > a
        if mid_block:
            # Call inside a block: the next listed instruction must be ft.
            addrs = [i.address for i in graph.get_block(b).instructions()]
            k = addrs.index(a) if a in addrs else -1
            has_edge = followed = 0 <= k < len(addrs) - 1 and addrs[k + 1] == ft
            fb = b if followed else fb
        else:
            has_edge = ft in block_info[b][1]
            followed = has_edge and ft in block_info
        same_fn = fb is not None and bool(owner.get(fb, set()) & fs)
        if not nr:
            if followed and same_fn:
                counts[f'cont_ok.{tag}'] += 1
            else:
                counts[f'cont_missing.{tag}'] += 1
                details['cont_missing'].append((hex(a), s['name']))
            continue
        # noreturn API
        if not has_edge:
            counts['noreturn.stops'] += 1
            continue
        counts['noreturn.followed'] += 1
        if mid_block:
            counts['noreturn.mid_block'] += 1
            details['noreturn_mid_block'].append((hex(a), s['name']))
            continue
        caller_sym = symbol_func(a)
        if ft not in block_info:
            # Edge to an address that is no block (typically padding).
            counts['noreturn.edge_to_nonblock'] += 1
            continue
        # Real code after the call is reachable some other way only if the
        # fall-through block has another predecessor.
        if ft in func_set or ft in fblocks:
            # Edge into the entry of another (symbol or Dragon) function.
            counts['noreturn.edge_into_function_entry'] += 1
            details['edge_into_function_entry'].append((hex(a), s['name'], hex(ft)))
        elif preds.get(ft, set()) - {b}:
            counts['noreturn.fake_edge_into_live_block'] += 1
            details['fake_edge_into_live_block'].append((hex(a), s['name'], hex(ft)))
        else:
            counts['noreturn.fake_only_path'] += 1
            details['fake_only_path'].append((hex(a), s['name'], hex(ft)))
        # Walk what Dragon added to the caller through this edge and look for
        # another symbol's entry: a wrong function boundary.
        merged = set()
        seen, work = {b}, [ft]
        while work:
            x = work.pop()
            if x in seen or x not in block_info:
                continue
            seen.add(x)
            if not (owner.get(x, set()) & fs):
                continue
            if x in func_set and x != caller_sym and x not in fs:
                merged.add(x)
                continue
            if x != ft and (preds.get(x, set()) - seen):
                continue  # reached independently: not only via the fake edge
            work.extend(block_info[x][1])
        if merged:
            counts['noreturn.merged'] += 1
            details['merged'].append((hex(a), s['name'], hex(ft),
                                      [hex(m) for m in sorted(merged)],
                                      [hex(f) for f in sorted(fs)]))

    if llvm_limit:
        by_fn = collections.defaultdict(list)
        for s in sites:
            b = block_of(blocks, block_info, s['addr'])
            for f in sorted(owner.get(b, ())) if b is not None else ():
                by_fn[f].append(s)
                break
        lifted = 0
        truth = {s['addr']: s['name'] for s in sites}
        for f in sorted(by_fn)[:llvm_limit]:
            lifter = dragon.LLVMLifter(img.architecture, 'audit',
                                       execution_mode=dragon.LLVMExecutionMode.STATIC_ANALYSIS)
            fn = graph.get_function(f)
            if not lifter.lift(fn, graph):
                counts['llvm.lift_failed'] += 1
                continue
            lifted += 1
            md = parse_import_md(lifter.ir())
            for s in by_fn[f]:
                got = md.get(s['addr'], set())
                if not got:
                    counts['llvm.missing'] += 1
                    details['llvm_missing'].append((hex(s['addr']), s['name']))
            # Every recorded entry is judged, whichever function lifted it.
            for site, names in md.items():
                if site not in truth:
                    counts['llvm.spurious'] += 1
                    details['llvm_spurious'].append((hex(site), sorted(names)))
                elif names == {truth[site]}:
                    counts['llvm.correct'] += 1
                else:
                    counts['llvm.wrong'] += 1
                    details['llvm_wrong'].append((hex(site), truth[site], sorted(names)))
        counts['llvm.functions'] = lifted

    return dict(binary=os.path.basename(path), strategy=strategy,
                slots=len(slots), stubs=len(stubs), sites=len(sites),
                counts=dict(sorted(counts.items())),
                details={k: v[:40] for k, v in details.items()},
                detail_totals={k: len(v) for k, v in details.items()})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary')
    ap.add_argument('objdump')
    ap.add_argument('--strategy', type=int, default=0)
    ap.add_argument('--llvm', type=int, default=0)
    ap.add_argument('--json')
    ap.add_argument('--control', choices=['names', 'fallthrough'],
                    help='negative control; must produce wrong/cont_missing')
    a = ap.parse_args()
    r = audit(a.binary, a.objdump, a.strategy, a.llvm, a.control)
    r['control'] = a.control
    text = json.dumps(r, indent=1)
    if a.json:
        with open(a.json, 'w') as f:
            f.write(text)
    print(text)
    c = r['counts']
    bad = c.get('llvm.wrong', 0) + c.get('llvm.spurious', 0)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
