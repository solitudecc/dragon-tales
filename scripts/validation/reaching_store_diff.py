"""Compare same-function reaching-store CFF fixtures with a native oracle.

Export with DRAGON_REACHING_STORE_FIXTURE_DIR while running dragon_tests
'[reaching-store]'. Compile each before/after IR at O0/O2 and link helper
bodies only afterwards. Compare return, call count, 512 register bytes and
all 65536 Guest bytes with an independent byte-level model. No RIP selector,
callee summary, execution snapshot, exclusions or normalized fields are used.

Atomic/volatile/fence/convergent/asm fixtures check sequential compatibility;
their ordering/concurrency boundary is enforced by C++ structural regressions.
This is LLVM-level coverage, not a machine-lifter/Unicorn proof.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

INPUTS = (0, 1, 2, 3, 7, 8, 15, 16, 0x7fffffff, 0x80000000,
          0xfffffffe, 0xffffffff)
CASES = ('adjacent', 'pure', 'load', 'readonly-call', 'readnone-call',
         'reassign-after-call', 'unknown-call', 'alias-store', 'partial-store',
         'volatile-load', 'volatile-store', 'atomic-load', 'atomic-store',
         'atomic-rmw', 'compare-exchange', 'fence', 'convergent-call',
         'sideeffect-asm')
GROUPS = (('result', 0, 4), ('calls', 4, 8), ('registers', 8, 520),
          ('memory', 520, 66056))
DRIVER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char *data; uint64_t size; } Memory;
extern uint32_t machine(unsigned char *, Memory *, uint32_t, void *);
_Alignas(16) static unsigned char registers[512], memory[65536];
static uint32_t calls;
void opaque_writer(unsigned char *regs, Memory *mem, void *exposed, uint32_t x) {
  ++calls;
  regs[48]^=(unsigned char)x;
  uint32_t value=x^0xa5a5a5a5u;
  memcpy(mem->data+0x208,&value,4);
  uint32_t state=(x&2u)?999u:((x&1u)?30u:20u);
  memcpy(exposed,&state,4);
}
uint32_t readonly_helper(const void *slot, uint32_t x) {
  (void)x;
  uint32_t value;
  memcpy(&value,slot,4);
  return value;
}
uint32_t pure_helper(const void *slot, uint32_t x) {
  (void)slot;
  return x;
}
int main(int argc,char **argv) {
  if(argc!=2)return 90;
  uint32_t x=(uint32_t)strtoull(argv[1],0,10);
  for(unsigned i=0;i<sizeof registers;++i)registers[i]=(i*13u+x)&255u;
  for(unsigned i=0;i<sizeof memory;++i)memory[i]=(i*37u+x)&255u;
  uint32_t reading=x^0x0f0f0f0fu;
  memcpy(memory+0x300,&reading,4);
  uint32_t initial=x^((x&1u)?1u:0u);
  memcpy(memory+0x304,&initial,4);
  Memory mem={memory,sizeof memory};
  void *alias=memory+((x&1u)?0x100:0x500);
  uint32_t result=machine(registers,&mem,x,alias);
  if(fwrite(&result,1,4,stdout)!=4 || fwrite(&calls,1,4,stdout)!=4 ||
     fwrite(registers,1,sizeof registers,stdout)!=sizeof registers ||
     fwrite(memory,1,sizeof memory,stdout)!=sizeof memory)return 91;
  return 0;
}
'''


def expected(case, x):
    registers = bytearray((i * 13 + x) & 255 for i in range(512))
    memory = bytearray((i * 37 + x) & 255 for i in range(65536))

    def write(offset, value):
        memory[offset:offset + 4] = value.to_bytes(4, 'little')

    def read(offset):
        return int.from_bytes(memory[offset:offset + 4], 'little')

    # Break the accidental low-byte equality between x and the default fill:
    # replacing a live ordinary/readonly LOAD by x must change its branch.
    write(0x300, x ^ 0x0f0f0f0f)
    write(0x304, x ^ (1 if x & 1 else 0))
    state, calls, branch_value = 20, 0, x
    if case in ('unknown-call', 'reassign-after-call'):
        calls = 1
        registers[48] ^= x & 255
        write(0x208, x ^ 0xa5a5a5a5)
        state = 999 if x & 2 else 30 if x & 1 else 20
        if case == 'reassign-after-call':
            state = 20
    elif case == 'alias-store':
        value = 999 if x & 2 else 30
        if x & 1:
            state = value
        else:
            write(0x500, value)
    elif case == 'partial-store':
        state = 20 | ((x & 1) << 8)
    elif case in ('volatile-store', 'atomic-store', 'atomic-rmw'):
        write(0x304, x ^ 0x2468ace0)
    elif case == 'compare-exchange' and read(0x304) == x:
        write(0x304, x ^ 0x2468ace0)
    if case in ('load', 'volatile-load', 'readonly-call', 'convergent-call'):
        branch_value = read(0x300)
    elif case == 'atomic-load':
        branch_value = read(0x304)
    if branch_value & 4:
        result = x ^ 0x51de
    elif state == 20:
        write(0x200, x ^ 0x13579bdf)
        state = 30
        result = (x + 7) & 0xffffffff
    else:
        result = ((x + 7) & 0xffffffff) if state == 30 else x ^ 0xbad
    write(0x100, state)
    return (result.to_bytes(4, 'little') + calls.to_bytes(4, 'little') +
            registers + memory)


def differences(actual, want):
    if len(actual) != 66056 or len(want) != 66056:
        raise RuntimeError('truncated or extended full-state snapshot')
    return [name for name, begin, end in GROUPS if actual[begin:end] != want[begin:end]]


def self_test():
    count = 0
    for case in CASES:
        for x in INPUTS:
            want = expected(case, x)
            if differences(want, want):
                raise RuntimeError('identity comparison failed')
            for name, begin, _ in GROUPS:
                changed = bytearray(want)
                changed[begin] ^= 1
                if differences(changed, want) != [name]:
                    raise RuntimeError('single-field fault escaped comparison')
                count += 1
    for snapshot in (b'', bytes(66055), bytes(66057)):
        try:
            differences(snapshot, expected('pure', 0))
        except RuntimeError:
            count += 1
        else:
            raise RuntimeError('malformed snapshot accepted')
    print(json.dumps(dict(ok=True, single_field_and_length_controls=count)))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, work):
    index = len(list(work.glob('command-*.json')))
    stem = work / f'command-{index:03}'
    result = subprocess.run([str(arg) for arg in argv], capture_output=True)
    stem.with_suffix('.stdout').write_bytes(result.stdout)
    stem.with_suffix('.stderr').write_bytes(result.stderr)
    stem.with_suffix('.json').write_text(json.dumps(
        dict(argv=[str(arg) for arg in argv], returncode=result.returncode), indent=2) + '\n')
    result.check_returncode()
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--control', choices=('stale-alias', 'skip-condition'))
    parser.add_argument('--expect-control', action='store_true')
    parser.add_argument('--opt', default='opt-21')
    parser.add_argument('--llc', default='llc-21')
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.fixtures or not args.out:
        parser.error('--fixtures and --out required')
    if bool(args.control) != args.expect_control:
        parser.error('control and expect-control must be used together')
    triple = {'aarch64': 'aarch64-linux-gnu',
              'x86_64': 'x86_64-linux-gnu'}.get(platform.machine())
    if not triple or sys.byteorder != 'little':
        raise RuntimeError('little-endian Linux aarch64/x86_64 required')
    args.out.mkdir(parents=True, exist_ok=True)
    if list(args.out.iterdir()):
        raise RuntimeError('fresh output directory required')
    rows, modules = [], []
    cases = CASES
    if args.control:
        cases = ('alias-store' if args.control == 'stale-alias' else 'pure',)
    for case in cases:
        for variant in ('unsafe',) if args.control else ('before', 'after'):
            source = args.fixtures / f'{case}-{variant}.ll'
            text = source.read_text()
            for helper in ('opaque_writer', 'readonly_helper', 'pure_helper'):
                if f'define void @{helper}(' in text or f'define i32 @{helper}(' in text:
                    raise RuntimeError('helper body was available during optimization')
            for level in ('O0', 'O2'):
                work = args.out / case / variant / level
                work.mkdir(parents=True)
                command([args.opt, '-passes=verify', '-disable-output', source], work)
                selected = source
                if level == 'O2':
                    selected = work / 'optimized.ll'
                    command([args.opt, '-S', '-passes=default<O2>', source,
                             '-o', selected], work)
                command([args.opt, '-passes=verify', '-disable-output', selected], work)
                driver, obj, native = work / 'driver.c', work / 'module.o', work / 'native'
                driver.write_text(DRIVER)
                command([args.llc, '-O0', '-filetype=obj', '-mtriple=' + triple,
                         selected, '-o', obj], work)
                command([args.cc, '-O0', driver, obj, '-o', native], work)
                modules.append(dict(case=case, variant=variant, level=level,
                                    source_sha256=digest(source), selected_sha256=digest(selected)))
                for x in INPUTS:
                    actual = command([native, x], work)
                    want = expected(case, x)
                    fields = differences(actual, want)
                    stem = work / str(x)
                    stem.with_suffix('.native.bin').write_bytes(actual)
                    stem.with_suffix('.expected.bin').write_bytes(want)
                    rows.append(dict(case=case, variant=variant, level=level, x=x,
                                     status='different' if fields else 'match', differences=fields))
    statuses = dict(collections.Counter(row['status'] for row in rows))
    if args.control:
        def should_differ(x):
            return bool(x & 1) and not bool(x & 4) if args.control == 'stale-alias' else bool(x & 4)
        correct = (len(rows) == 24 and statuses.get('different', 0) > 0 and
                   all((row['status'] == 'different') == should_differ(row['x']) for row in rows))
    else:
        correct = len(rows) == len(CASES) * 2 * 2 * len(INPUTS) and statuses == {'match': len(rows)}
    summary = dict(ok=correct, comparisons=len(rows), modules=len(modules), statuses=statuses,
                   register_memory_bytes_compared_per_execution=66048,
                   return_bytes_compared_per_execution=4, call_count_bytes_compared_per_execution=4,
                   control=args.control, exclusions=0, skips=0, normalizations=0,
                   script_sha256=digest(Path(__file__)),
                   scope='same-function LLVM reaching-store proof; sequential native compatibility only')
    (args.out / 'results.json').write_text(json.dumps(
        dict(summary=summary, modules=modules, results=rows), indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if correct else 1


if __name__ == '__main__':
    raise SystemExit(main())
