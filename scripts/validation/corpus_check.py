"""Run check_one.py over every binary in a directory, one subprocess each
(crash/timeout isolated), and print anomalies. Usage: corpus_check.py DIR"""
import glob, json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
def objdump_for(path):
    with open(path, 'rb') as f:
        head = f.read(0x400)
    if head[:4] == b'\x7fELF':
        return 'objdump'
    pe = int.from_bytes(head[0x3c:0x40], 'little')
    machine = int.from_bytes(head[pe + 4:pe + 6], 'little')
    return 'x86_64-w64-mingw32-objdump' if machine == 0x8664 else 'i686-w64-mingw32-objdump'
timeout = int(os.environ.get('TIMEOUT', 900))
for path in sorted(glob.glob(os.path.join(sys.argv[1], '*'))):
    try:
        p = subprocess.run([sys.executable, os.path.join(HERE, 'check_one.py'), path, objdump_for(path)],
                           capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        print(f'{os.path.basename(path):34s} TIMEOUT'); continue
    if p.returncode != 0:
        print(f'{os.path.basename(path):34s} CRASH rc={p.returncode} {p.stderr.strip().splitlines()[-1:] }'); continue
    r = json.loads(p.stdout.strip().splitlines()[-1])
    if 'error' in r:
        print(f'{r["binary"]:34s} {r["error"]}'); continue
    issues = []
    for k in ('linear', 'recursive'):
        x = r[k]
        for f in ('blocks_without_ignil', 'edges_to_undecoded', 'empty_symbol_functions', 'llvm_lift_failed'):
            if x.get(f):
                issues.append(f'{k}.{f}={x[f]}' + (f' {x["edges_to_undecoded_eg"]}' if f == 'edges_to_undecoded' else ''))
        if x['llvm_verify'] != 'ok':
            issues.append(f'{k}.llvm_verify: {x["llvm_verify"]}')
    if r['linear'].get('objdump_missing'):
        issues.append(f'objdump_missing={r["linear"]["objdump_missing"]}')
    t = f'{r["linear"]["time"]:6.1f}s/{r["recursive"]["time"]:6.1f}s'
    print(f'{r["binary"]:34s} {t} fn={r["linear"]["functions"]:5d} ' + ('OK' if not issues else ' | '.join(issues)))
