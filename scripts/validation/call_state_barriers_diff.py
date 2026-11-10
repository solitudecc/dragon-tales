"""Same-function CFF and opaque calls against an independent native oracle.

Export fixtures using DRAGON_CALL_STATE_FIXTURE_DIR while running dragon_tests
'[call-state-barriers]'. The opaque writer has no body during optimization; its
test-only body is compiled and linked afterwards. Every normal before/after IR
is checked at O0/O2 on the return, all 512 register bytes, all 64 KiB of Guest
memory, and the call count. No PC/RIP selector or cross-call summary is used.

This tests LLVM CFF/memory/alias semantics; it is not a machine-lifter or Unicorn
campaign. --control false-readonly exercises an intentionally false effect
promise to ensure stale pre-call state cannot pass unnoticed.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import platform
import subprocess

INPUTS = (0, 1, 2, 3, 7, 8, 15, 16, 0x7fffffff, 0x80000000,
          0xfffffffe, 0xffffffff)
CASES = ('guest-after', 'stack-after', 'guest-before', 'stack-before',
         'register-before', 'host-local', 'host-escaped')
DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char *data; uint64_t size; } Memory;
extern uint32_t machine(unsigned char *, Memory *, uint32_t);
static unsigned char registers[512], memory[65536];
static uint32_t calls;
void opaque_writer(unsigned char *regs, Memory *mem, void *exposed, uint32_t x) {
  ++calls;
  regs[48]^=(unsigned char)x;
  uint32_t value=x^0xa5a5a5a5u;
  memcpy(mem->data+0x208,&value,4);
  if(exposed) {
    uint32_t state=(x&2u)?999u:((x&1u)?30u:20u);
    memcpy(exposed,&state,4);
  }
}
int main(int argc,char **argv) {
  if(argc!=2)return 90;
  uint32_t x=(uint32_t)strtoull(argv[1],0,10);
  for(unsigned i=0;i<sizeof registers;++i)registers[i]=(i*13u+x)&255u;
  for(unsigned i=0;i<sizeof memory;++i)memory[i]=(i*37u+x)&255u;
  Memory mem={memory,sizeof memory};
  uint32_t result=machine(registers,&mem,x);
  if(fwrite(&result,1,4,stdout)!=4 || fwrite(&calls,1,4,stdout)!=4 ||
     fwrite(registers,1,sizeof registers,stdout)!=sizeof registers ||
     fwrite(memory,1,sizeof memory,stdout)!=sizeof memory)return 91;
  return 0;
}
'''


def expected(case, x):
    registers = bytearray((i * 13 + x) & 255 for i in range(512))
    memory = bytearray((i * 37 + x) & 255 for i in range(65536))
    registers[48] ^= x & 255
    memory[0x208:0x20c] = (x ^ 0xa5a5a5a5).to_bytes(4, 'little')
    state = 999 if x & 2 else 30 if x & 1 else 20
    if case in ('guest-after', 'stack-after', 'host-local'):
        state = 20
    if state == 20:
        memory[0x200:0x204] = (x ^ 0x13579bdf).to_bytes(4, 'little')
        state = 30
    result = ((x + 7) & 0xffffffff) if state == 30 else x ^ 0xbad
    if case.startswith('guest-'):
        memory[0x100:0x104] = state.to_bytes(4, 'little')
    elif case.startswith('stack-'):
        offset = 0x4000 + (x & 7) * 4
        memory[offset:offset + 4] = state.to_bytes(4, 'little')
    elif case == 'register-before':
        registers[16:20] = state.to_bytes(4, 'little')
    return result.to_bytes(4, 'little') + (1).to_bytes(4, 'little') + registers + memory


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--control', choices=('false-readonly',))
    parser.add_argument('--expect-control', action='store_true')
    parser.add_argument('--opt', default='opt-21')
    parser.add_argument('--llc', default='llc-21')
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    if bool(args.control) != args.expect_control:
        parser.error('control and expect-control must be used together')
    triple = {'aarch64': 'aarch64-linux-gnu',
              'x86_64': 'x86_64-linux-gnu'}.get(platform.machine())
    if not triple:
        raise RuntimeError('Linux aarch64/x86_64 required')
    args.out.mkdir(parents=True, exist_ok=True)
    if list(args.out.iterdir()):
        raise RuntimeError('fresh output directory required')
    rows, modules = [], []
    for case in ('guest-before',) if args.control else CASES:
        for variant in ('false-readonly',) if args.control else ('before', 'after'):
            source = args.fixtures / f'{case}-{variant}.ll'
            text = source.read_text()
            if ('define void @opaque_writer(' in text or
                    'declare void @opaque_writer(' not in text):
                raise RuntimeError('opaque body was available during optimization')
            for level in ('O0', 'O2'):
                work = args.out / case / variant / level
                work.mkdir(parents=True)
                subprocess.run([args.opt, '-passes=verify', '-disable-output', str(source)],
                               check=True, capture_output=True)
                selected = source
                if level == 'O2':
                    selected = work / 'optimized.ll'
                    subprocess.run([args.opt, '-S', '-passes=default<O2>', str(source),
                                    '-o', str(selected)], check=True, capture_output=True)
                subprocess.run([args.opt, '-passes=verify', '-disable-output', str(selected)],
                               check=True, capture_output=True)
                driver, obj, native = work / 'driver.c', work / 'module.o', work / 'native'
                driver.write_text(DRIVER)
                subprocess.run([args.llc, '-O0', '-filetype=obj', '-mtriple=' + triple,
                                str(selected), '-o', str(obj)], check=True, capture_output=True)
                subprocess.run([args.cc, '-O0', str(driver), str(obj), '-o', str(native)],
                               check=True, capture_output=True)
                modules.append(dict(case=case, variant=variant, level=level,
                                    source_sha256=digest(source), selected_sha256=digest(selected)))
                for x in INPUTS:
                    actual = subprocess.run([str(native), str(x)],
                                            check=True, capture_output=True).stdout
                    want = expected(case, x)
                    if len(actual) != len(want):
                        raise RuntimeError('truncated full-state snapshot')
                    differences = []
                    for name, begin, end in (('result', 0, 4), ('calls', 4, 8),
                                             ('registers', 8, 520),
                                             ('memory', 520, len(want))):
                        if actual[begin:end] != want[begin:end]:
                            differences.append(name)
                    stem = work / str(x)
                    stem.with_suffix('.native.bin').write_bytes(actual)
                    stem.with_suffix('.expected.bin').write_bytes(want)
                    rows.append(dict(case=case, variant=variant, level=level, x=x,
                                     status='different' if differences else 'match',
                                     differences=differences))
    statuses = dict(collections.Counter(row['status'] for row in rows))
    if args.control:
        correct = (len(rows) == 24 and statuses.get('different', 0) > 0 and
                   all(row['status'] == ('match' if not (row['x'] & 3) else 'different')
                       for row in rows))
    else:
        correct = len(rows) == len(CASES) * 2 * 2 * len(INPUTS) and statuses == {'match': len(rows)}
    summary = dict(ok=correct, comparisons=len(rows), modules=len(modules), statuses=statuses,
                   register_memory_bytes_compared_per_execution=66048,
                   return_bytes_compared_per_execution=4,
                   call_count_bytes_compared_per_execution=4, control=args.control,
                   exclusions=0, skips=0, normalizations=0,
                   script_sha256=digest(Path(__file__)),
                   scope='same-function LLVM CFF plus linked opaque writer; not machine-lifter equivalence')
    (args.out / 'results.json').write_text(json.dumps(
        dict(summary=summary, modules=modules, results=rows), indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if correct else 1


if __name__ == '__main__':
    raise SystemExit(main())
