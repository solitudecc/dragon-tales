"""Conservative objdump-only bounds for the register used by a table load.

Walk predecessor instructions, rather than borrowing a nearby comparison
from unrelated code. Every incoming path must establish the same unsigned
prefix bound before redefining the index. Unsupported forms stay unknown.
"""
import re


def register_family(name):
    name = name.strip().lower()
    if re.fullmatch(r'[wx]\d+', name):
        return 'x' + name[1:]
    aliases = {'eax': 'rax', 'ax': 'rax', 'al': 'rax', 'ah': 'rax',
               'ebx': 'rbx', 'bx': 'rbx', 'bl': 'rbx', 'bh': 'rbx',
               'ecx': 'rcx', 'cx': 'rcx', 'cl': 'rcx', 'ch': 'rcx',
               'edx': 'rdx', 'dx': 'rdx', 'dl': 'rdx', 'dh': 'rdx',
               'edi': 'rdi', 'di': 'rdi', 'dil': 'rdi',
               'esi': 'rsi', 'si': 'rsi', 'sil': 'rsi'}
    return aliases.get(name, re.sub(r'^(r\d+)[dwb]$', r'\1', name))


def index_bound_reader(lines):
    instructions = {}
    for line in lines:
        match = re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*$', line)
        if match:
            instructions[int(match[1], 16)] = (match[2], match[3].split('//')[0].strip())
    addresses = sorted(instructions)
    previous = {b: a for a, b in zip(addresses, addresses[1:]) if b-a <= 16}
    following = {a: b for b, a in previous.items()}
    predecessors = {address: [] for address in addresses}
    for address, (mnemonic, operands) in instructions.items():
        jump = (mnemonic.startswith('j') or mnemonic in ('b','cbz','cbnz','tbz','tbnz','loop','loope','loopne')
                or mnemonic.startswith('b.'))
        conditional = jump and mnemonic not in ('jmp', 'b')
        target_text = operands.split(',')[-1].strip()
        target = re.match(r'^(?:0x)?([0-9a-f]+)(?:\s*<|$)', target_text) if jump else None
        if target and int(target[1], 16) in predecessors:
            predecessors[int(target[1], 16)].append(address)
        if (not jump or conditional) and mnemonic not in ('ret', 'retq', 'br', 'ud2', 'int3'):
            if address in following:
                predecessors[following[address]].append(address)
    # These branch outcomes establish index < count. No signed comparison
    # proves the missing lower bound for an unsigned table index.
    conditions = {'ja': (False, 1), 'jnbe': (False, 1), 'b.hi': (False, 1),
                  'jae': (False, 0), 'jnb': (False, 0), 'b.hs': (False, 0), 'b.cs': (False, 0),
                  'jbe': (True, 1), 'jna': (True, 1), 'b.ls': (True, 1),
                  'jb': (True, 0), 'jnae': (True, 0), 'b.lo': (True, 0), 'b.cc': (True, 0)}
    active = set()
    budget = 2048

    def walk(address, successor, register):
        nonlocal budget
        budget -= 1
        key = address, register
        if budget < 0 or len(active) >= 256 or key in active:
            return None
        active.add(key)
        try:
            mnemonic, operands = instructions[address]
            if mnemonic in conditions and address in previous:
                comparison, args = instructions[previous[address]]
                match = re.fullmatch(r'(\w+)\s*,\s*#?(0x[0-9a-f]+|\d+)', args)
                branch_target = re.match(r'^(?:0x)?([0-9a-f]+)(?:\s*<|$)', operands)
                taken, increment = conditions[mnemonic]
                if (comparison == 'cmp' and match and branch_target and
                        register_family(match[1]) == register and
                        (successor == int(branch_target[1],16)) == taken):
                    count = int(match[2], 0) + increment
                    return count if 0 < count <= 65536 else None
            if mnemonic in ('call', 'callq', 'bl', 'blr'):
                return None
            args = operands.split(',')
            if mnemonic in ('xchg','xadd') and len(args) > 1 and register_family(args[1]) == register:
                return None
            if mnemonic.startswith('ldp') and len(args) > 1 and register_family(args[1]) == register:
                return None
            if mnemonic in ('mul','imul','div','idiv','cdq','cqo') and register in ('rax','rdx'):
                return None
            if mnemonic in ('cpuid','syscall','sysenter','rep','repne','repe'):
                return None
            reads_only = (mnemonic.startswith(('j', 'b.','st')) or
                          mnemonic in ('b', 'br', 'cmp', 'test', 'tst', 'nop', 'push'))
            if not reads_only and args and register_family(args[0]) == register:
                if mnemonic == 'mov' and len(args) == 2 and re.fullmatch(r'\s*[a-z][a-z0-9]*\s*', args[1]):
                    register = register_family(args[1])
                else:
                    return None
            incoming = predecessors.get(address, [])
            if not incoming:
                return None
            bounds = [walk(parent, address, register) for parent in incoming]
            return bounds[0] if bounds[0] is not None and all(b == bounds[0] for b in bounds) else None
        finally:
            active.remove(key)

    def bound(load_address, index_register):
        nonlocal budget
        active.clear()
        budget = 2048
        incoming = predecessors.get(load_address, [])
        if not incoming:
            return None
        register = register_family(index_register)
        bounds = [walk(parent, load_address, register) for parent in incoming]
        return bounds[0] if bounds[0] is not None and all(b == bounds[0] for b in bounds) else None
    return bound


def unsigned_index_bound(lines, load_address, index_register):
    return index_bound_reader(lines)(load_address, index_register)
