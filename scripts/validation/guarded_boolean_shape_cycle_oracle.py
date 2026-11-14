#!/usr/bin/env python3
"""Independent historical-Boolean shape-cycle oracle.

--emit and --plan only write/read text. They never compile, run a product pass,
or execute native. Python's unsigned recurrence is the expected result; neither
transformed IR nor the emitted independent C calibration is the specification.
Run native only through the caller's 512 MiB/64 MiB/180 second outer guard.
"""
import argparse
import ctypes
import gzip
import hashlib
import json
import shutil
import struct
import time
import traceback
from pathlib import Path

MASK = (1 << 64) - 1
ARENA = 0x12000
REGS, EXIT, DESC, CONFIG, GUEST = 0x100, 0x400, 0x500, 0x600, 0x1000
FAILURE = 0xBAD0000000000000
SHAPES = ('self', 'mutual')
MODES = ('ordinary', 'table-before', 'table-after', 'wrong-hint')
FAULTS = {
    'old-history': 'ordinary', 'fresh-history': 'ordinary',
    'omit-effect': 'ordinary', 'double-effect': 'ordinary',
    'wrong-iteration': 'ordinary', 'wrong-state': 'ordinary',
    'reload': 'table-after',
}
LIMITS = dict(command_seconds=180, start_free_bytes=512 * 1024**2,
              running_free_bytes=64 * 1024**2, max_loop_iterations=4,
              max_candidates=64, max_rewrites=32, max_blocks=128,
              max_instructions=16384)


def key(w):
    return 0x6A09E667 if w == 32 else 0x6A09E667F3BCC909


def step(w):
    return 0x9E3779B9 if w == 32 else 0x9E3779B97F4A7C15


def enc(w, n):
    return ((n ^ key(w)) + 5) & ((1 << w) - 1)


def inputs(w):
    """Sixteen fixed keys, including both select arms and logical-size errors."""
    high, mask = 1 << (w - 1), (1 << w) - 1
    return ((0, 0, 3, 0, 65536), (1, 0, 3, 1, 65536),
            (high, high, 3, 2, 65536), (high - 1, 1, 2, 3, 65536),
            (mask, mask, 3, 0, 65536), (mask ^ 1, 0, 3, 1, 65536),
            (0x0123456789ABCDEF & mask, 7, 1, 2, 65536),
            (0xA5A5A5A5A5A5A5A4 & mask, 2, 0, 3, 65536),
            (0, 0, 3, 0, 271), (2, 0, 3, 1, 272),
            (2, 0, 3, 2, 519), (2, 0, 3, 3, 520),
            (high, 0, 0, 0, 520), (0, 2, 2, 1, 65536),
            (mask >> 1, 1, 3, 2, 65536), (8, 15, 3, 3, 65536))


def get(a, o, n=8):
    return int.from_bytes(a[o:o + n], 'little')


def put(a, o, v, n=8):
    a[o:o + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')


def digest(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def name(w, shape, mode):
    return f'i{w}-{shape}-{mode}'


def finish(label, kind, answer, target, state, tag):
    s = label + ':\n'
    for off, value in ((0, answer), (24, '%a64'), (32, '%usedold'),
                       (40, target), (48, '%iteration'), (56, state),
                       (64, '%b64'), (72, '%counter_next')):
        s += f'  %{label}.r{off}=getelementptr i8,ptr %regs,i64 {off}\n'
        s += f'  store i64 {value},ptr %{label}.r{off},align 1\n'
    for i, value in enumerate((str(kind), '0', '36864', target,
                               '1' if kind == 1 else '0', '%x', tag)):
        s += f'  %{label}.e{i}=getelementptr i8,ptr %exit,i64 {8*i}\n'
        s += f'  store i64 {value},ptr %{label}.e{i},align 1\n'
    return s + f'  ret i64 {answer}\n'


def ir(w, shape, mode, fault=None):
    W = f'i{w}'
    s = '''source_filename = "boolean-shape-cycle-independent"
target datalayout = "e-p:64:64"
define i64 @oracle(ptr %regs,ptr %memory,ptr %exit,ptr %config) {
entry:
  %guest=load ptr,ptr %memory,align 1
  %sizep=getelementptr i8,ptr %memory,i64 8
  %size=load i64,ptr %sizep,align 1
  %xp=getelementptr i8,ptr %regs,i64 8
  %x=load i64,ptr %xp,align 1
  %yp=getelementptr i8,ptr %regs,i64 16
  %y=load i64,ptr %yp,align 1
  %np=getelementptr i8,ptr %regs,i64 80
  %n=load i64,ptr %np,align 1
  %limit=and i64 %n,3
  %override=load i64,ptr %config,align 1
  %stateptr=getelementptr i8,ptr %guest,i64 264
  %counterptr=getelementptr i8,ptr %guest,i64 208
  %traceptr=getelementptr i8,ptr %guest,i64 216
  %tracebptr=getelementptr i8,ptr %guest,i64 224
  %oldptr=getelementptr i8,ptr %guest,i64 232
  %accptr=getelementptr i8,ptr %guest,i64 128
'''
    s += ('  %xword=trunc i64 %x to i32\n  %yword=trunc i64 %y to i32\n'
          if w == 32 else
          '  %xword=add i64 %x,0\n  %yword=add i64 %y,0\n')
    s += f'''  br label %head
head:
  %iteration=phi i64 [0,%entry],[%next_iteration,%loop]
  %histA=phi {W} [%xword,%entry],[%nextA,%loop]
'''
    s += (f'  %histB=phi {W} [%yword,%entry],[%nextB,%loop]\n'
          if shape == 'mutual' else f'  %histB=add {W} %yword,0\n')
    s += '''  %seedroom=icmp uge i64 %size,272
  br i1 %seedroom,label %seed,label %early
seed:
'''
    s += ('  %iword=trunc i64 %iteration to i32\n' if w == 32 else
          '  %iword=add i64 %iteration,0\n')
    s += f'  %step=add {W} %iword,{step(w)}\n'
    s += f'  %nextA=add {W} '+('%histB' if shape == 'mutual' else '%histA')+',%step\n'
    if shape == 'mutual':
        s += f'  %nextB=xor {W} %histA,{key(w)}\n'
    source = '%xword' if fault == 'old-history' else '%histA'
    s += (f'  %observed=xor {W} {source},%histB\n' if shape == 'mutual'
          else f'  %observed=add {W} {source},0\n')
    s += f'  %low=and {W} %observed,1\n  %choose6=icmp ne {W} %low,0\n'
    s += f'  %proposal=select i1 %choose6,{W} {enc(w,6)},{W} {enc(w,0)}\n'
    if fault == 'wrong-state':
        s += f'  store {W} {enc(w,6)},ptr %stateptr,align 1\n'
    else:
        s += f'  store {W} %proposal,ptr %stateptr,align 1\n'
    if w == 32:
        s += '  %a64=zext i32 %histA to i64\n  %b64=zext i32 %histB to i64\n  %future64=zext i32 %nextA to i64\n'
    else:
        s += '  %a64=add i64 %histA,0\n  %b64=add i64 %histB,0\n  %future64=add i64 %nextA,0\n'
    s += '  %usedold=add i64 '+('%future64' if fault == 'fresh-history' else '%a64')+',0\n'
    s += '  %counter=load i64,ptr %counterptr,align 1\n'
    s += f'  %counter_next=add i64 %counter,{0 if fault == "omit-effect" else 2 if fault == "double-effect" else 1}\n'
    if fault != 'omit-effect':
        s += '  store i64 %counter_next,ptr %counterptr,align 1\n'
    s += '''  store i64 %a64,ptr %traceptr,align 1
  store i64 %b64,ptr %tracebptr,align 1
  store i64 %usedold,ptr %oldptr,align 1
  br label %state_load
state_load:
'''
    s += f'''  %state=load {W},ptr %stateptr,align 1
  %unbias=sub {W} %state,5
  %decoded=xor {W} %unbias,{key(w)}
'''
    s += ('  %index64=zext i32 %decoded to i64\n  %state64=zext i32 %state to i64\n'
          if w == 32 else '  %index64=add i64 %decoded,0\n  %state64=add i64 %state,0\n')
    s += '''  %state_valid=icmp ult i64 %index64,4
  br i1 %state_valid,label %bounds,label %state_error
bounds:
  %scaled=shl i64 %index64,3
  %index=add i64 %scaled,512
  %ptr=getelementptr i8,ptr %guest,i64 %index
  %end=add i64 %index,8
  %room=icmp ule i64 %end,%size
  br i1 %room,label %before_load,label %table_error
before_load:
'''
    if mode == 'table-before':
        s += '  store i64 %override,ptr %ptr,align 1\n'
    s += '  br label %dispatch\ndispatch:\n  %target=load i64,ptr %ptr,align 1\n'
    if mode == 'table-after':
        s += '  store i64 99,ptr %ptr,align 1\n'
    if fault == 'reload':
        s += '  %reloaded=load i64,ptr %ptr,align 1\n'
    s += '  switch i64 '+('%reloaded' if fault == 'reload' else '%target')+''',label %miss [
    i64 43,label %a
    i64 44,label %b
    i64 99,label %c
  ]
a:
  br label %business
b:
  br label %business
c:
  br label %business
miss:
  br label %business
business:
  %tag=phi i64 [1,%a],[2,%b],[3,%c],[4,%miss]
  %acc=load i64,ptr %accptr,align 1
  %a1=add i64 %acc,%target
  %a2=add i64 %a1,%usedold
  %a3=add i64 %a2,%b64
  %a4=add i64 %a3,%iteration
  %accnext=add i64 %a4,%tag
  store i64 %accnext,ptr %accptr,align 1
  %answer=xor i64 %accnext,%a64
  %again=icmp ult i64 %iteration,%limit
  br i1 %again,label %loop,label %done
loop:
'''
    s += f'  %next_iteration=add i64 %iteration,{2 if fault == "wrong-iteration" else 1}\n  br label %head\n'
    s += finish('done', 1, '%answer', '%target', '%state64', '%tag')
    s += f'''early:
  store i64 {FAILURE ^ 208},ptr %regs,align 1
'''
    for i, value in enumerate(('5', '0', '36864', '208', '0', '%x', '11')):
        s += f'  %early.e{i}=getelementptr i8,ptr %exit,i64 {8*i}\n  store i64 {value},ptr %early.e{i},align 1\n'
    s += f'  ret i64 {FAILURE ^ 208}\n'
    s += f'state_error:\n  %badstate=xor i64 264,{FAILURE}\n  br label %state_done\n'
    s += finish('state_done', 5, '%badstate', '264', '%state64', '12')
    s += f'table_error:\n  %badtable=xor i64 %index,{FAILURE}\n  br label %table_done\n'
    return s + finish('table_done', 5, '%badtable', '%index', '%state64', '13') + '}\n'


def initial(w, k, guest):
    x, y, n, table, size = k
    a = bytearray((i * 37 + 11) & 255 for i in range(ARENA))
    for off, value in ((8, x), (16, y), (80, n)):
        put(a, REGS + off, value)
    put(a, DESC, guest)
    put(a, DESC + 8, size)
    put(a, CONFIG, (43, 44, 99, 777)[(table + 1) % 4])
    put(a, GUEST + 128, 0)
    put(a, GUEST + 208, 0)
    put(a, GUEST + 264, enc(w, 6), w // 8)
    put(a, GUEST + 512, (43, 44, 99, 777)[table])
    return a


def model(w, shape, mode, k, guest, fault=None):
    """Byte-array machine state plus an independently evaluated u32/u64 recurrence."""
    x, y, n, table, size = k
    arena = initial(w, k, guest)
    modulus = 1 << w
    current_a, current_b, iteration = x % modulus, y % modulus, 0

    def result(kind, answer, target, state, tag, early=False):
        if early:
            put(arena, REGS, answer)
        else:
            for off, value in ((0, answer), (24, current_a), (32, old_used),
                               (40, target), (48, iteration), (56, state),
                               (64, current_b), (72, counter_next)):
                put(arena, REGS + off, value)
        for i, value in enumerate((kind, 0, 36864, target,
                                   int(kind == 1), x, tag)):
            put(arena, EXIT + 8 * i, value)
        return struct.pack('<Q', answer & MASK) + arena

    for _ in range(4):
        if size < 272:
            return result(5, FAILURE ^ 208, 208, 0, 11, True)
        increment = (step(w) + iteration) % modulus
        next_a = ((current_b if shape == 'mutual' else current_a) + increment) % modulus
        next_b = (current_a ^ key(w)) if shape == 'mutual' else current_b
        observed = (x % modulus) if fault == 'old-history' else current_a
        if shape == 'mutual':
            observed ^= current_b
        decoded = 6 if observed % 2 else 0
        if fault == 'wrong-state':
            decoded = 6
        # Arithmetic decode/encoding is separate from the LLVM expression DAG.
        state = enc(w, decoded)
        put(arena, GUEST + 264, state, w // 8)
        old_used = next_a if fault == 'fresh-history' else current_a
        counter_next = (get(arena, GUEST + 208) +
                        (0 if fault == 'omit-effect' else 2 if fault == 'double-effect' else 1)) & MASK
        if fault != 'omit-effect':
            put(arena, GUEST + 208, counter_next)
        for off, value in ((216, current_a), (224, current_b), (232, old_used)):
            put(arena, GUEST + off, value)
        if decoded >= 4:
            return result(5, FAILURE ^ 264, 264, state, 12)
        index = 512 + 8 * decoded
        if index + 8 > size:
            return result(5, FAILURE ^ index, index, state, 13)
        if mode == 'table-before':
            put(arena, GUEST + index, get(arena, CONFIG))
        target = get(arena, GUEST + index)
        if mode == 'table-after':
            put(arena, GUEST + index, 99)
        selected = get(arena, GUEST + index) if fault == 'reload' else target
        tag = {43: 1, 44: 2, 99: 3}.get(selected, 4)
        acc = (get(arena, GUEST + 128) + target + old_used +
               current_b + iteration + tag) & MASK
        put(arena, GUEST + 128, acc)
        if iteration >= (n & 3):
            return result(1, acc ^ current_a, target, state, tag)
        current_a, current_b = next_a, next_b
        iteration += 2 if fault == 'wrong-iteration' else 1
    raise AssertionError('bounded model exceeded four actual iterations')


def reference_c(w, shape, mode):
    """Independent unsigned C calibration; never used to derive Python expected."""
    return f'''#include <stdint.h>
typedef uint{w}_t word;
static uint64_t rd(const unsigned char *p,unsigned n) {{
  uint64_t v=0; for(unsigned j=0;j<n;++j) v|=(uint64_t)p[j]<<(8*j); return v;
}}
static void wr(unsigned char *p,uint64_t v,unsigned n) {{
  for(unsigned j=0;j<n;++j) p[j]=(unsigned char)(v>>(8*j));
}}
uint64_t oracle(unsigned char *r,unsigned char *m,unsigned char *e,unsigned char *cfg) {{
  unsigned char *g=(unsigned char *)(uintptr_t)rd(m,8);
  uint64_t size=rd(m+8,8),x=rd(r+8,8),y=rd(r+16,8),limit=rd(r+80,8)&3;
  word a=(word)x,b=(word)y; uint64_t i=0;
  for(unsigned turns=0;turns<4;++turns) {{
    if(size<272) {{
      uint64_t ret=UINT64_C({FAILURE ^ 208}); wr(r,ret,8);
      uint64_t z[7]={{5,0,36864,208,0,x,11}};
      for(unsigned j=0;j<7;++j) wr(e+8*j,z[j],8); return ret;
    }}
    word delta=(word)(i+UINT64_C({step(w)}));
    word nexta=(word)({"b" if shape == "mutual" else "a"}+delta);
    word nextb={"(word)(a ^ UINT64_C("+str(key(w))+"))" if shape == "mutual" else "b"};
    word observed={"(word)(a^b)" if shape == "mutual" else "a"};
    uint64_t decoded=(observed&1)?6:0;
    word state=(word)(((word)decoded^(word)UINT64_C({key(w)}))+5);
    wr(g+264,(uint64_t)state,{w//8});
    uint64_t counter=rd(g+208,8)+1; wr(g+208,counter,8);
    wr(g+216,(uint64_t)a,8); wr(g+224,(uint64_t)b,8); wr(g+232,(uint64_t)a,8);
    uint64_t target=264,kind=5,tag=12,ret=UINT64_C({FAILURE})^264;
    if(decoded<4) {{
      uint64_t index=512+decoded*8; target=index; tag=13; ret=UINT64_C({FAILURE})^index;
      if(index+8<=size) {{
        {"wr(g+index,rd(cfg,8),8);" if mode == "table-before" else ""}
        target=rd(g+index,8);
        {"wr(g+index,99,8);" if mode == "table-after" else ""}
        tag=target==43?1:target==44?2:target==99?3:4;
        uint64_t acc=rd(g+128,8)+target+(uint64_t)a+(uint64_t)b+i+tag;
        wr(g+128,acc,8); ret=acc^(uint64_t)a; kind=1;
        if(i<limit) {{ a=nexta;b=nextb;++i;continue; }}
      }}
    }}
    uint64_t ro[8]={{ret,(uint64_t)a,(uint64_t)a,target,i,(uint64_t)state,(uint64_t)b,counter}};
    unsigned offsets[8]={{0,24,32,40,48,56,64,72}};
    for(unsigned j=0;j<8;++j) wr(r+offsets[j],ro[j],8);
    uint64_t ex[7]={{kind,0,36864,target,kind==1,x,tag}};
    for(unsigned j=0;j<7;++j) wr(e+8*j,ex[j],8); return ret;
  }}
  __builtin_trap();
}}
'''


def plan():
    return dict(schema='guarded-boolean-shape-cycle-plan-v1', limits=LIMITS,
                fixtures=16, unique_source_functions=12,
                fixture_input_tuples=256, unique_source_input_tuples=192,
                input_keys_per_width=16, widths=[32, 64], shapes=SHAPES,
                modes=MODES, wrong_hint_is_duplicate_source_domain=True,
                native_matches_per_optimization_pair=512,
                fault_observations_per_pair=448,
                comparator_controls_per_pair=4608,
                correctness='Python model versus complete return+ARENA, no normalization or exclusions',
                sequence=['emit only', 'LLVM parse/verify all before and fault inputs; never native structural-only poison/undef',
                          'before-only C/Python/LLVM O0 and O2 calibration',
                          'old/new public pass prepare, explicit formal LD pins',
                          'sequential main/no-Z3 O0/O2 native and saved audit under unchanged outer guard'],
                precision='independent synthetic conditional fast edge; not real125 acceptance or full deflattening',
                product_or_native_executions=0)


def emit(out):
    assert not out.exists()
    out.mkdir(parents=True)
    files, cases, structural = [], [], []

    def save(n, text):
        (out / n).write_text(text)
        files.append(n)

    for w in (32, 64):
        save(f'INPUTS-i{w}.json', json.dumps(inputs(w), indent=2) + '\n')
        for shape in SHAPES:
            for mode in MODES:
                stem = name(w, shape, mode)
                save(stem + '.before.ll', ir(w, shape, mode))
                save(stem + '.reference.c', reference_c(w, shape, mode))
                hint = 44 if mode == 'wrong-hint' else 43
                save(stem + '.manifest.json', json.dumps(dict(view_symbol='oracle', memory=[
                    dict(offset='0x200', bytes=hint.to_bytes(8, 'little').hex())]), indent=2) + '\n')
                cases.append(dict(name=stem, width=w, shape=shape, mode=mode,
                                  inputs=f'INPUTS-i{w}.json', predicted_index=0,
                                  original_bound_error_index=6,
                                  semantic_duplicate_of=name(w, shape, 'ordinary') if mode == 'wrong-hint' else None))
            for fault, mode in FAULTS.items():
                save(name(w, shape, mode) + '.fault-' + fault + '.ll', ir(w, shape, mode, fault))
            for bad in ('poison', 'undef'):
                fn = name(w, shape, bad) + '.structural-only.ll'
                original = ir(w, shape, 'ordinary')
                assert original.count('[%nextA,%loop]') == 1
                save(fn, original.replace('[%nextA,%loop]', f'[{bad},%loop]', 1))
                structural.append(dict(file=fn, native=False, reason='explicit undef/poison historical PHI arm; structural refusal only'))
    manifest = dict(schema='guarded-boolean-shape-cycle-emitted-v1', source_sha256=digest(__file__),
                    plan=plan(), cases=cases, faults=FAULTS, structural_only=structural,
                    hint_scope='Only target hints; state6 has no hint and must retain original bounds/error. wrong-hint shares ordinary IR/C/model exactly.',
                    original_integer_get='Unknown cyclic integers stay unknown; no loop constant/value fact is exported.',
                    files={n: dict(bytes=(out / n).stat().st_size, sha256=digest(out / n)) for n in files})
    (out / 'EMITTED.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    print(json.dumps(dict(emit=str(out), files=len(files), bytes=sum((out / n).stat().st_size for n in files),
                         manifest_sha256=digest(out / 'EMITTED.json'), producer_runs=0, native_runs=0), indent=2))


def load(path):
    lib = ctypes.CDLL(str(path.resolve()))
    fn = lib.oracle
    fn.argtypes = [ctypes.c_void_p] * 4
    fn.restype = ctypes.c_uint64
    return lib, fn


def execute(fn, buffer, initial_bytes):
    address = ctypes.addressof(buffer)
    ctypes.memmove(address, bytes(initial_bytes), ARENA)
    value = fn(address + REGS, address + DESC, address + EXIT, address + CONFIG)
    return struct.pack('<Q', value) + bytes(buffer)


def run(a):
    assert not a.out.exists()
    a.out.mkdir(parents=True)
    started, active = time.monotonic(), dict(phase='startup')
    report = dict(schema='guarded-boolean-shape-cycle-native-v1', passed=False,
                  completed=False, limits=LIMITS, source_sha256=digest(__file__),
                  calibration_only=a.calibrate, plan=plan(), matches=0,
                  comparator_controls=0, faults={}, failures=[], binary_pins={})

    def guard(start=False):
        paths = {'output': a.out}
        if Path('/dev/shm').exists():
            paths['tmpfs'] = Path('/dev/shm')
        free = {k: shutil.disk_usage(v).free for k, v in paths.items()}
        if start:
            report['initial_free_bytes'] = free
        prior = report.setdefault('minimum_free_bytes', dict(free))
        for k, v in free.items():
            prior[k] = min(prior[k], v)
        assert min(free.values()) >= LIMITS['start_free_bytes' if start else 'running_free_bytes']
        assert time.monotonic() - started < LIMITS['command_seconds']

    def library(p):
        h, path = digest(p), str(p.resolve())
        old = report['binary_pins'].setdefault(path, h)
        assert old == h
        return load(p)

    buffer = (ctypes.c_ubyte * ARENA)()
    guest = ctypes.addressof(buffer) + GUEST
    try:
        guard(True)
        with gzip.open(a.out / 'full-frames.bin.gz', 'wb') as normal, gzip.open(a.out / 'fault-frames.bin.gz', 'wb') as faults:
            for w in (32, 64):
                for si, shape in enumerate(SHAPES):
                    for mi, mode in enumerate(MODES):
                        stem = name(w, shape, mode)
                        active = dict(phase='load', fixture=stem)
                        bl, bf = library(a.before_dir / (stem + '.so'))
                        second_dir = a.reference_dir if a.calibrate else a.after_dir
                        al, af = library(second_dir / (stem + '.so'))
                        for i, k in enumerate(inputs(w)):
                            active = dict(phase='calibration' if a.calibrate else 'normal', fixture=stem, input_number=i, input=list(k))
                            guard()
                            init, expected = initial(w, k, guest), model(w, shape, mode, k, guest)
                            before, second = execute(bf, buffer, init), execute(af, buffer, init)
                            normal.write(struct.pack('<4I', w, si, mi, i))
                            for frame in (init, expected, before, second):
                                normal.write(frame)
                            active.update(expected_sha256=hashlib.sha256(expected).hexdigest(), before_sha256=hashlib.sha256(before).hexdigest(), second_sha256=hashlib.sha256(second).hexdigest())
                            assert before == expected and second == expected
                            report['matches'] += 2
                            offsets = [0, 8 + REGS, 8 + REGS + 24, 8 + REGS + 32]
                            offsets += [8 + GUEST + n for n in (128, 208, 216, 224, 232, 264, 512)]
                            offsets += [8 + EXIT + n * 8 for n in range(7)]
                            for offset in offsets:
                                damaged = bytearray(expected)
                                damaged[offset] ^= 1
                                assert damaged != expected
                                report['comparator_controls'] += 1
                    if a.fault_dir and not a.calibrate:
                        for fi, (fault, mode) in enumerate(FAULTS.items()):
                            stem = name(w, shape, mode)
                            lib, fn = library(a.fault_dir / (stem + '.fault-' + fault + '.so'))
                            counts = dict(observations=0, specified=0, detected=0)
                            report['faults'][name(w, shape, fault)] = counts
                            for i, k in enumerate(inputs(w)):
                                active = dict(phase='fault', fixture=stem, fault=fault, input_number=i, input=list(k))
                                guard()
                                init, expected = initial(w, k, guest), model(w, shape, mode, k, guest)
                                faulty = model(w, shape, mode, k, guest, fault)
                                actual = execute(fn, buffer, init)
                                faults.write(struct.pack('<4I', w, si, fi, i))
                                for frame in (init, expected, faulty, actual):
                                    faults.write(frame)
                                assert actual == faulty
                                counts['observations'] += 1
                                if faulty != expected:
                                    counts['specified'] += 1
                                    assert actual != expected
                                    counts['detected'] += 1
                            assert counts['specified'] > 0
            assert report['matches'] == 512
            assert report['comparator_controls'] == 4608
            assert digest(__file__) == report['source_sha256']
            for path, h in report['binary_pins'].items():
                assert digest(Path(path)) == h
            report['passed'] = report['completed'] = True
    except BaseException as e:
        report['failures'].append(dict(case=active, type=type(e).__name__, detail=str(e), traceback=traceback.format_exc()))
        raise
    finally:
        report['seconds'], report['last_case'] = time.monotonic() - started, active
        report['streams'] = {n: dict(bytes=(a.out / n).stat().st_size, sha256=digest(a.out / n))
                             for n in ('full-frames.bin.gz', 'fault-frames.bin.gz') if (a.out / n).exists()}
        (a.out / 'RESULTS.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')


def audit(path):
    """Recompute every saved whole-state frame without loading any native code."""
    result = json.loads((path / 'RESULTS.json').read_bytes())
    assert result['completed'] and result['passed']
    assert result['source_sha256'] == digest(__file__)
    assert result['limits'] == LIMITS and result['plan'] == json.loads(json.dumps(plan()))
    assert result['comparator_controls'] == 4608
    normal_count = fault_count = detections = 0
    normal_keys, fault_keys = set(), set()
    fault_counts = {}
    for stream, fault_stream in (('full-frames.bin.gz', False), ('fault-frames.bin.gz', True)):
        assert digest(path / stream) == result['streams'][stream]['sha256']
        with gzip.open(path / stream, 'rb') as f:
            while header := f.read(16):
                assert len(header) == 16
                w, si, variant, i = struct.unpack('<4I', header)
                assert w in (32, 64) and si < 2 and i < 16
                shape, k = SHAPES[si], inputs(w)[i]
                init = f.read(ARENA)
                expected, one, two = (f.read(ARENA + 8) for _ in range(3))
                assert len(init) == ARENA and all(len(x) == ARENA + 8 for x in (expected, one, two))
                guest = get(init, DESC)
                assert init == initial(w, k, guest)
                if fault_stream:
                    assert variant < len(FAULTS)
                    assert (w, si, variant, i) not in fault_keys
                    fault_keys.add((w, si, variant, i))
                    fault, mode = tuple(FAULTS.items())[variant]
                    assert expected == model(w, shape, mode, k, guest)
                    assert one == model(w, shape, mode, k, guest, fault) == two
                    counts = fault_counts.setdefault(name(w, shape, fault),
                                                      dict(observations=0, specified=0, detected=0))
                    counts['observations'] += 1
                    counts['specified'] += one != expected
                    counts['detected'] += two != expected
                    fault_count += 1
                    detections += one != expected
                else:
                    assert variant < len(MODES)
                    assert (w, si, variant, i) not in normal_keys
                    normal_keys.add((w, si, variant, i))
                    assert expected == model(w, shape, MODES[variant], k, guest) == one == two
                    normal_count += 1
    assert normal_count == 256 and result['matches'] == 2 * normal_count
    assert normal_keys == {(w, si, mi, i) for w in (32, 64) for si in range(2)
                           for mi in range(4) for i in range(16)}
    expected_fault_keys = set() if result['calibration_only'] else {
        (w, si, fi, i) for w in (32, 64) for si in range(2)
        for fi in range(len(FAULTS)) for i in range(16)}
    assert fault_keys == expected_fault_keys
    assert fault_counts == result['faults']
    assert fault_count == sum(x['observations'] for x in result['faults'].values())
    assert detections == sum(x['detected'] for x in result['faults'].values())
    print(json.dumps(dict(saved_audit=True, normal_frames=normal_count, fault_frames=fault_count,
                          specified_fault_detections=detections, native_executions=0), indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    q = p.add_mutually_exclusive_group(required=True)
    q.add_argument('--emit', type=Path)
    q.add_argument('--plan', action='store_true')
    q.add_argument('--run', action='store_true')
    q.add_argument('--audit', type=Path)
    p.add_argument('--out', type=Path)
    p.add_argument('--before-dir', type=Path)
    p.add_argument('--after-dir', type=Path)
    p.add_argument('--reference-dir', type=Path)
    p.add_argument('--fault-dir', type=Path)
    p.add_argument('--calibrate', action='store_true')
    a = p.parse_args()
    if a.emit:
        return emit(a.emit)
    if a.plan:
        return print(json.dumps(plan(), indent=2))
    if a.audit:
        return audit(a.audit)
    assert a.out and a.before_dir
    assert (a.calibrate and a.reference_dir and not a.after_dir and not a.fault_dir) or (not a.calibrate and a.after_dir and a.fault_dir and not a.reference_dir)
    return run(a)


if __name__ == '__main__':
    main()
