#!/usr/bin/env python3
"""Run external_call_audit.py on every binary of one or more directories,
both strategies, one subprocess each. Writes OUT/<name>.s<strategy>.json and
prints a summary table.

Usage: external_call_corpus.py OUT DIR... [--llvm N] [--timeout SEC]
"""
import argparse, collections, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def objdump_for(path):
    # Same choice as corpus_check.py (which runs at import time).
    with open(path, 'rb') as f:
        head = f.read(0x400)
    if head[:4] == b'\x7fELF':
        machine = int.from_bytes(head[18:20], 'little')
        return 'x86_64-linux-gnu-objdump' if machine in (0x3e, 0x03) else 'objdump'
    pe = int.from_bytes(head[0x3c:0x40], 'little')
    machine = int.from_bytes(head[pe + 4:pe + 6], 'little')
    return 'x86_64-w64-mingw32-objdump' if machine == 0x8664 else 'i686-w64-mingw32-objdump'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--llvm', type=int, default=100000)
    ap.add_argument('--timeout', type=int, default=3600)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    total = collections.Counter()
    failures = []
    for d in a.dirs:
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if not os.path.isfile(path):
                continue
            for strategy in (0, 1):
                js = os.path.join(a.out, f'{name}.s{strategy}.json')
                if not os.path.exists(js):
                    try:
                        p = subprocess.run(
                            [sys.executable, os.path.join(HERE, 'external_call_audit.py'),
                             path, objdump_for(path), '--strategy', str(strategy),
                             '--llvm', str(a.llvm), '--json', js],
                            capture_output=True, text=True, timeout=a.timeout)
                    except subprocess.TimeoutExpired:
                        failures.append((name, strategy, 'timeout'))
                        continue
                    if not os.path.exists(js):
                        failures.append((name, strategy, p.stderr.strip()[-300:]))
                        continue
                r = json.load(open(js))
                total.update(r['counts'])
                c = r['counts']
                print(f"{name:34s} s{strategy} sites={r['sites']:5d} "
                      f"llvm ok/miss/wrong/spur={c.get('llvm.correct',0)}/{c.get('llvm.missing',0)}/"
                      f"{c.get('llvm.wrong',0)+c.get('llvm.stub_wrong',0)}/{c.get('llvm.spurious',0)} "
                      f"merged={c.get('noreturn.merged',0)}", flush=True)
    print(json.dumps(dict(sorted(total.items())), indent=1))
    for f in failures:
        print('FAILED', *f)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
