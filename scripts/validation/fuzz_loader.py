"""Robustness fuzzing of the loader and the analyses on damaged inputs.

Takes seed binaries, damages them (random byte flips, header field
overwrites, truncation, random bytes) and runs load + disassemble (both
strategies) + LLVM lift of a few functions in a child process. Reports
crashes (signals), hangs (timeouts) and Python exceptions other than a clean
"load failed". Every failing input is saved for reproduction.

Usage: fuzz_loader.py OUTDIR ITERATIONS SEED_BINARY...
"""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CHILD = r'''
import os, sys
sys.path.insert(0, os.path.join(%r, '..', '..', 'python'))
import dragon
img = dragon.LoadedImage.load_bytes(open(sys.argv[1], 'rb').read())
if img is None:
    print('LOADFAIL'); sys.exit(0)
for s in img.sections():
    s.name; s.data
img.symbols(); img.entry
for strat in (0, 1):
    g = img.disassemble(dragon.Configuration(strategy=strat))
    n = 0
    for a in list(g.function_addresses())[:40]:
        f = g.get_function(a)
        if f and f.is_valid:
            L = dragon.LLVMLifter(img.architecture)
            L.lift(f, g); L.ir(); n += 1
print('OK')
''' % HERE


def mutate(data, rng):
    d = bytearray(data)
    kind = rng.randrange(6)
    if kind == 0:                      # scattered byte flips
        for _ in range(rng.randrange(1, 64)):
            d[rng.randrange(len(d))] = rng.getrandbits(8)
    elif kind == 1:                    # damage the headers (first 4 KiB)
        for _ in range(rng.randrange(1, 16)):
            i = rng.randrange(min(len(d), 4096))
            d[i] = rng.choice([0, 0xff, 0x7f, 0x80, rng.getrandbits(8)])
    elif kind == 2:                    # truncate
        d = d[:rng.randrange(64, len(d))]
    elif kind == 3:                    # overwrite a 32-bit field with an extreme value
        for _ in range(rng.randrange(1, 6)):
            i = rng.randrange(0, min(len(d) - 4, 4096))
            d[i:i + 4] = rng.choice([b'\xff\xff\xff\xff', b'\x00\x00\x00\x80', b'\xff\xff\xff\x7f',
                                     b'\x00\x00\x00\x00', os.urandom(4)])
    elif kind == 4:                    # random block in the middle (code/data)
        i = rng.randrange(len(d))
        n = rng.randrange(1, 4096)
        d[i:i + n] = os.urandom(min(n, len(d) - i))
    else:                              # keep only the header and garble the rest
        d = d[:4096] + os.urandom(rng.randrange(0, 65536))
    return bytes(d)


outdir, iterations = sys.argv[1], int(sys.argv[2])
seeds = [open(p, 'rb').read() for p in sys.argv[3:]]
os.makedirs(outdir, exist_ok=True)
rng = random.Random(7)
stats = {}
for it in range(iterations):
    data = mutate(rng.choice(seeds), rng)
    path = os.path.join(outdir, 'cur.bin')
    open(path, 'wb').write(data)
    try:
        p = subprocess.run([sys.executable, '-c', CHILD, path], capture_output=True, text=True, timeout=60)
        if p.returncode < 0:
            kind = f'signal {-p.returncode}'
        elif p.returncode != 0:
            last = (p.stderr.strip().splitlines() or ['?'])[-1]
            kind = 'exception: ' + last[:120]
        else:
            kind = p.stdout.strip().splitlines()[-1] if p.stdout.strip() else 'no output'
    except subprocess.TimeoutExpired:
        kind = 'TIMEOUT'
    stats[kind] = stats.get(kind, 0) + 1
    if kind not in ('OK', 'LOADFAIL'):
        keep = os.path.join(outdir, f'fail_{it:05d}.bin')
        os.rename(path, keep)
        if stats[kind] <= 3:
            print(f'{kind}: {keep}', flush=True)
print(stats)
