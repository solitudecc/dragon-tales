"""Independent full-arena native oracle for the private nearest-STORE helper.

Six source fixtures, 16 fixed inputs, two host optimization levels. Emit once;
calibrate C/Python and original LLVM at BOTH host levels before executing any
transformed/mutant native output. All 66112 bytes are compared; no exclusions,
normalization or partial-state equality. Snapshot controls are not semantic
fault observations. This is synthetic helper validation, not a machine lifter
or public builtin, and is independent of the old predicate campaign counts.
"""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import struct
import subprocess
import time

FRAME = 66112
INPUTS = 16
WIDTHS = (32, 128)
MODES = ('closed', 'call-before', 'call-after')
CONTROL_OFFSETS = (0, 32, 64, 80, 96, 112, 200, 256, 264, 2048, 8704, 66048, FRAME - 1)
RUNTIME_SHA = '32c22c410e96aae4a6bbff544af815d9ce4f4d02f4fd4303cb4ca1a6984e396b'
HELPER_SHA = '9fe5d8811f2e5f5d594ab08e80f698a2be3553b4dab903eb3641de93e3af3906'
XOR_WORD = 0xd1b54a32d192ed039e3779b97f4a7c15

WRAPPER = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned __int128 U;
unsigned oracle_width, oracle_mode;
void probe(unsigned char *);
void opaque_cell(unsigned char *p, uint32_t width, uint32_t tag) {
  U value = 0, key = ((U)UINT64_C(0xd1b54a32d192ed03) << 64) |
                    (U)UINT64_C(0x9e3779b97f4a7c15);
  uint64_t count, trace;
  memcpy(&value, p + 2048, width / 8);
  memcpy(&count, p + 264, 8); memcpy(&trace, p + 8704, 8);
  count += 1; trace = ((trace << 7) | (trace >> 57)) ^ (uint64_t)value ^ tag;
  p[9000 + tag] ^= (unsigned char)value ^ (unsigned char)tag;
  value ^= key;
  memcpy(p + 2048, &value, width / 8);
  memcpy(p + 264, &count, 8); memcpy(p + 8704, &trace, 8);
}
int main(int argc, char **argv) {
  unsigned char arena[66112]; size_t size;
  if (argc != 3) return 4;
  oracle_width = (unsigned)strtoul(argv[1], 0, 10);
  oracle_mode = (unsigned)strtoul(argv[2], 0, 10);
  while ((size = fread(arena, 1, sizeof arena, stdin)) != 0) {
    if (size != sizeof arena) return 2;
    probe(arena);
    if (fwrite(arena, 1, sizeof arena, stdout) != sizeof arena) return 3;
  }
  return ferror(stdin) || fflush(stdout) != 0;
}
'''

# Independent C specification: byte-wise packing, separate observer, and a
# structured bounded loop. It never calls the wrapper's opaque_cell function.
C_SPEC = r'''
#include <stdint.h>
typedef unsigned __int128 W;
extern unsigned oracle_width, oracle_mode;
static W take(const unsigned char *p, unsigned n) {
  W value = 0;
  for (unsigned i = n; i != 0; --i) value = (value << 8) | p[i-1];
  return value;
}
static void put(unsigned char *p, W value, unsigned n) {
  for (unsigned i = 0; i != n; ++i) { p[i] = (unsigned char)value; value >>= 8; }
}
static void observe(unsigned char *p, unsigned bytes, unsigned tag) {
  W old = take(p + 2048, bytes);
  uint64_t trace = (uint64_t)take(p + 8704, 8);
  uint64_t count = (uint64_t)take(p + 264, 8);
  put(p + 264, (uint64_t)(count + 1), 8);
  trace = (trace << 7) + (trace >> 57);
  trace ^= (uint64_t)old; trace ^= tag;
  put(p + 8704, trace, 8);
  p[9000 + tag] = p[9000 + tag] ^ (unsigned char)old ^ tag;
  W key = (W)UINT64_C(0x9e3779b97f4a7c15) +
          ((W)UINT64_C(0xd1b54a32d192ed03) << 64);
  put(p + 2048, old ^ key, bytes);
}
void probe(unsigned char *p) {
  unsigned width = oracle_width, bytes = width / 8;
  W mask = width == 128 ? ~(W)0 : (((W)1 << width) - 1);
  W history = take(p, bytes), checksum = 0;
  unsigned route = (unsigned)take(p + 32, 4);
  unsigned limit = 3 + ((unsigned)take(p + 40, 4) % 4);
  put(p + 2048, history, bytes);
  for (unsigned iteration = 0; iteration < limit; ++iteration) {
    W fresh = take(p + 2048, bytes);
    W rotated = ((checksum * 32) | (checksum >> (width - 5))) & mask;
    checksum = ((rotated ^ fresh) + history) & mask;
    put(p + 96, fresh, bytes); put(p + 112, history, bytes);
    put(p + 256, (uint64_t)((uint64_t)take(p + 256, 8) + 1), 8);
    unsigned tag = ((route + iteration) % 2) ? 1 : 2;
    W next = (fresh + 1) & mask;
    if (oracle_mode == 1) observe(p, bytes, tag);
    put(p + 2048, next, bytes);
    if (oracle_mode == 2) observe(p, bytes, tag);
    unsigned offset = tag == 1 ? 272 : 280;
    put(p + offset, (uint64_t)((uint64_t)take(p + offset, 8) + 1), 8);
    history = next;
  }
  put(p + 64, take(p + 2048, bytes), bytes);
  put(p + 80, checksum, bytes); put(p + 200, limit, 4);
  put(p + 66048, UINT64_C(0xdecafbad12345678), 8);
}
'''


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def initial(width, trial):
    mask = (1 << width) - 1
    sign = 1 << (width - 1)
    edges = (0, 1, mask, sign, sign - 1, sign + 1, 2, 3, mask ^ 1,
             mask // 3, mask * 2 // 3, 0xffffffff, 1 << 64,
             (1 << 64) - 1, 1 << 96, (1 << 127) | 0x7fffffff)
    if width == 32:
        edges = edges[:11] + (7, 8, 15, 16, 31)
    arena = bytearray(hashlib.shake_256(
        ('guest-cell-ssa/' + str(width) + '/' + str(trial)).encode()).digest(FRAME))
    arena[:width // 8] = (edges[trial] & mask).to_bytes(width // 8, 'little')
    arena[32:36] = trial.to_bytes(4, 'little')
    arena[40:44] = trial.to_bytes(4, 'little')
    return bytes(arena)


def expected(source, width, mode):
    arena = bytearray(source)
    size, mask, mask64 = width // 8, (1 << width) - 1, (1 << 64) - 1
    read = lambda offset, n=size: int.from_bytes(arena[offset:offset + n], 'little')

    def store(offset, value, n=size):
        arena[offset:offset + n] = (value & ((1 << (8 * n)) - 1)).to_bytes(n, 'little')

    def observe(tag):
        value, trace = read(2048), read(8704, 8)
        store(264, read(264, 8) + 1, 8)
        rotated = ((trace << 7) | (trace >> 57)) & mask64
        store(8704, rotated ^ (value & mask64) ^ tag, 8)
        arena[9000 + tag] ^= (value & 255) ^ tag
        store(2048, value ^ XOR_WORD)

    history, route = read(0), read(32, 4)
    limit = 3 + (read(40, 4) & 3)
    store(2048, history)
    checksum, visits, distinct = 0, [0, 0], 0
    for iteration in range(limit):
        fresh = read(2048)
        distinct += fresh != history
        rotated = ((checksum << 5) | (checksum >> (width - 5))) & mask
        checksum = ((rotated ^ fresh) + history) & mask
        store(96, fresh)
        store(112, history)
        store(256, read(256, 8) + 1, 8)
        tag = 1 if (route + iteration) & 1 else 2
        visits[tag - 1] += 1
        next_value = (fresh + 1) & mask
        if mode == 'call-before':
            observe(tag)
        store(2048, next_value)
        if mode == 'call-after':
            observe(tag)
        offset = 272 if tag == 1 else 280
        store(offset, read(offset, 8) + 1, 8)
        history = next_value
    store(64, read(2048))
    store(80, checksum)
    store(200, limit, 4)
    store(66048, 0xdecafbad12345678, 8)
    return bytes(arena), dict(iterations=limit, branch_visits=visits,
                             fresh_history_differ=distinct,
                             opaque_calls=0 if mode == 'closed' else limit)


def file_pins(root):
    manifest = json.loads((root / 'EMITTED.json').read_text())
    result = {}
    for row in manifest['files']:
        path = root / row['path']
        stat = path.stat()
        if stat.st_size != row['bytes'] or sha(path) != row['sha256']:
            raise ValueError(('frozen file mismatch', str(path)))
        result[str(path)] = [stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns, row['sha256']]
    return manifest, result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--emit', action='store_true')
    parser.add_argument('--producer', type=Path)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--helper-object', type=Path)
    parser.add_argument('--frozen', type=Path)
    parser.add_argument('--phase', choices=('before', 'after', 'fault'))
    parser.add_argument('--host-opt', choices=('O0', 'O2'))
    parser.add_argument('--calibrations', nargs=2, type=Path)
    parser.add_argument('--audit', type=Path)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--llc', default='llc-21')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    commands, rows, failure = [], [], None

    def run(argv, data=None):
        start = time.monotonic()
        value = subprocess.run([str(x) for x in argv], input=data,
                               capture_output=True, timeout=180)
        commands.append(dict(argv=[str(x) for x in argv], exit=value.returncode,
                             seconds=time.monotonic() - start,
                             stderr=value.stderr.decode(errors='replace')))
        (args.out / 'COMMANDS.json').write_text(json.dumps(commands, indent=2) + '\n')
        if value.returncode:
            raise RuntimeError(commands[-1])
        return value.stdout

    if args.audit:
        audit_saved(args.audit, args.out)
        return
    if args.emit:
        if not all((args.producer, args.runtime, args.helper_object)):
            raise ValueError('emit requires producer/runtime/helper-object')
        if sha(args.runtime) != RUNTIME_SHA or sha(args.helper_object) != HELPER_SHA:
            raise ValueError('actual runtime/helper object pin mismatch')
        (args.out / 'wrapper.c').write_text(WRAPPER)
        (args.out / 'reference.c').write_text(C_SPEC)
        modules = []
        for width in WIDTHS:
            for mode in MODES:
                work = args.out / f'{width}-{mode}'
                work.mkdir()
                paths = [work / x for x in ('before.ll', 'after.ll', 'bad-phi.ll', 'STATS.json')]
                run([args.producer, width, mode, *paths])
                stats = json.loads(paths[-1].read_text())
                if stats['changed'] != (mode != 'call-after') or stats['budget_exhausted']:
                    raise ValueError(('unexpected helper admission', width, mode, stats))
                if mode == 'call-after' and paths[0].read_bytes() != paths[1].read_bytes():
                    raise ValueError('refusal changed complete IR bytes')
                if not stats['runtime_maps'] or any(str(args.runtime) not in line
                                                   for line in stats['runtime_maps']):
                    raise ValueError(('unexpected mapped backend', stats['runtime_maps']))
                modules.append(dict(name=work.name, **stats))
        files = [dict(path=str(p.relative_to(args.out)), bytes=p.stat().st_size, sha256=sha(p))
                 for p in sorted(args.out.rglob('*')) if p.is_file()]
        (args.out / 'EMITTED.json').write_text(json.dumps(dict(
            schema='guest-cell-ssa-emitted-v1', files=files, modules=modules,
            tool_pins=[dict(path=str(p), sha256=sha(p))
                       for p in (Path(__file__), args.producer, args.runtime, args.helper_object)],
            unique_source_fixtures=6, unique_module_inputs=96, unique_initial_arenas=32,
            inputs=16, host_opts=['O0', 'O2'],
            scope='Synthetic private helper, six new sources; no old campaign recounted'), indent=2) + '\n')
        print(json.dumps(dict(emitted=True, manifest_sha256=sha(args.out / 'EMITTED.json'))))
        return
    if not all((args.frozen, args.phase, args.host_opt)):
        raise ValueError('native requires frozen/phase/host-opt')
    manifest, initial_pins = file_pins(args.frozen)
    frozen_sha = sha(args.frozen / 'EMITTED.json')
    calibration_pins = []
    if args.phase != 'before':
        if not args.calibrations:
            raise ValueError('both before calibration results required')
        observed = set()
        for path in args.calibrations:
            calibration = json.loads((path / 'RESULTS.json').read_text())
            if (not calibration['passed'] or calibration['phase'] != 'before' or
                    calibration['frozen_sha256'] != frozen_sha or
                    calibration['oracle_sha256'] != sha(__file__) or
                    calibration['matches'] != 192 or calibration['controls'] != 2496):
                raise ValueError(('failed/mismatched before calibration', str(path)))
            observed.add(calibration['host_opt'])
            calibration_pins.append(dict(path=str(path / 'RESULTS.json'),
                                         sha256=sha(path / 'RESULTS.json'),
                                         frames_sha256=calibration['frame_stream_sha256']))
        if observed != {'O0', 'O2'}:
            raise ValueError('missing host optimization calibration')
    try:
        if args.phase == 'before':
            reference = args.out / 'C-reference.native'
            run([args.cc, '-' + args.host_opt, args.frozen / 'wrapper.c',
                 args.frozen / 'reference.c', '-o', reference])
        with lzma.open(args.out / 'frames.xz', 'xb', preset=1) as stream:
            for width in WIDTHS:
                for mode in MODES:
                    if args.phase == 'fault' and mode != 'closed':
                        continue
                    name = f'{width}-{mode}'
                    kind = 'refusal' if mode == 'call-after' else 'gain'
                    work = args.out / name
                    work.mkdir()
                    sources = [initial(width, trial) for trial in range(INPUTS)]
                    filename = {'before': 'before.ll', 'after': 'after.ll', 'fault': 'bad-phi.ll'}[args.phase]
                    obj, executable = work / 'target.o', work / 'target.native'
                    run([args.llc, '-mtriple=aarch64-unknown-linux-gnu', '-filetype=obj',
                         '-relocation-model=pic', '-' + args.host_opt,
                         args.frozen / name / filename, '-o', obj])
                    run([args.cc, '-' + args.host_opt, args.frozen / 'wrapper.c', obj, '-o', executable])
                    engines = [('LLVM', executable)]
                    if args.phase == 'before':
                        engines.insert(0, ('C', reference))
                    for engine, target in engines:
                        native = run([target, width, MODES.index(mode)], b''.join(sources))
                        if len(native) != FRAME * INPUTS:
                            raise ValueError(('native output length', name, engine, len(native)))
                        for trial, source in enumerate(sources):
                            wanted, coverage = expected(source, width, mode)
                            actual = native[trial * FRAME:(trial + 1) * FRAME]
                            match = wanted == actual
                            specified = (actual[2048:2048 + width // 8] != wanted[2048:2048 + width // 8] or
                                         actual[96:128] != wanted[96:128])
                            controls = 0
                            if args.phase != 'fault':
                                for offset in CONTROL_OFFSETS:
                                    corrupt = bytearray(actual)
                                    corrupt[offset] ^= 1
                                    controls += bytes(corrupt) != wanted
                            row = dict(name=name, width=width, mode=mode, kind=kind,
                                       phase=args.phase, opt=args.host_opt, engine=engine,
                                       trial=trial, match=match, controls=controls,
                                       specified_fault=specified if args.phase == 'fault' else None,
                                       coverage=coverage, sha256={k:hashlib.sha256(v).hexdigest()
                                         for k,v in [('input',source),('expected',wanted),('actual',actual)]})
                            encoded = json.dumps(row, sort_keys=True).encode()
                            stream.write(struct.pack('<I', len(encoded)) + encoded + source + wanted + actual)
                            rows.append(row)
                            if (not all(coverage['branch_visits']) or
                                    (args.phase == 'fault' and (match or not specified)) or
                                    (args.phase != 'fault' and (not match or controls != len(CONTROL_OFFSETS)))):
                                raise ValueError(('native mismatch/missed fault/control', row))
        _, final_pins = file_pins(args.frozen)
        if initial_pins != final_pins or sha(args.frozen / 'EMITTED.json') != frozen_sha:
            raise ValueError('frozen inputs changed during native run')
    except Exception as error:
        failure = type(error).__name__ + ': ' + str(error)
    result = dict(schema='guest-cell-ssa-native-v1', passed=failure is None, failure=failure,
                  phase=args.phase, host_opt=args.host_opt, frozen_root=str(args.frozen),
                  frozen_sha256=frozen_sha, oracle_sha256=sha(__file__), rows=rows,
                  frames=len(rows), matches=sum(x['match'] for x in rows),
                  controls=sum(x['controls'] for x in rows),
                  specified_faults=sum(x['specified_fault'] is True for x in rows),
                  unique_module_inputs=len({(x['name'],x['trial']) for x in rows}),
                  unique_initial_arenas=len({x['sha256']['input'] for x in rows}),
                  frame_stream_sha256=sha(args.out / 'frames.xz') if (args.out / 'frames.xz').exists() else None,
                  commands=commands, declared_pins_before=initial_pins,
                  calibration_pins=calibration_pins,
                  scope='Whole 66112-byte arena; native matches/rejections/faults and snapshot controls separate; no exclusions/skips/normalization')
    (args.out / 'RESULTS.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k:result[k] for k in ['passed','failure','phase','frames','matches','controls','specified_faults']}))
    if failure:
        raise SystemExit(1)


def audit_saved(saved, out):
    result = json.loads((saved / 'RESULTS.json').read_text())
    if not result['passed'] or result['oracle_sha256'] != sha(__file__):
        raise ValueError('failed result or oracle source mismatch')
    if sha(saved / 'frames.xz') != result['frame_stream_sha256']:
        raise ValueError('stream hash mismatch')
    rows, keys = [], set()
    with lzma.open(saved / 'frames.xz', 'rb') as stream:
        while True:
            header = stream.read(4)
            if not header:
                break
            if len(header) != 4:
                raise ValueError('truncated row prefix')
            length = struct.unpack('<I', header)[0]
            if not 0 < length < 8192:
                raise ValueError('invalid row length')
            encoded = stream.read(length)
            if len(encoded) != length:
                raise ValueError('truncated row JSON')
            row = json.loads(encoded)
            if (row['name'] != f"{row['width']}-{row['mode']}" or
                    row['width'] not in WIDTHS or row['mode'] not in MODES or
                    row['phase'] != result['phase'] or row['opt'] != result['host_opt']):
                raise ValueError('row identity does not match declared fixture/run')
            source, wanted, actual = (stream.read(FRAME) for _ in range(3))
            if any(len(x) != FRAME for x in (source,wanted,actual)):
                raise ValueError('truncated arena')
            key = (row['name'],row['trial'],row['engine'])
            if key in keys:
                raise ValueError('duplicate frame domain')
            keys.add(key)
            if source != initial(row['width'],row['trial']):
                raise ValueError('input outside frozen domain')
            expected_frame, coverage = expected(source,row['width'],row['mode'])
            if wanted != expected_frame or row['coverage'] != coverage:
                raise ValueError('saved expected differs from independent model')
            match = wanted == actual
            specified = (actual[2048:2048+row['width']//8] != wanted[2048:2048+row['width']//8] or
                         actual[96:128] != wanted[96:128])
            controls = 0
            if result['phase'] != 'fault':
                for offset in CONTROL_OFFSETS:
                    corrupt=bytearray(actual);corrupt[offset]^=1
                    controls += bytes(corrupt) != wanted
            if row['match'] != match or row['controls'] != controls:
                raise ValueError('saved comparison/control mismatch')
            if result['phase'] == 'fault':
                if match or not specified or row['specified_fault'] is not True:
                    raise ValueError('missed specified semantic fault')
            elif not match or controls != len(CONTROL_OFFSETS):
                raise ValueError('saved native mismatch')
            if row['sha256'] != {k:hashlib.sha256(v).hexdigest() for k,v in
                                  [('input',source),('expected',wanted),('actual',actual)]}:
                raise ValueError('arena hash mismatch')
            rows.append(row)
    engines = ('C','LLVM') if result['phase']=='before' else ('LLVM',)
    modes = ('closed',) if result['phase']=='fault' else MODES
    required={(f'{w}-{m}',t,e) for w in WIDTHS for m in modes for t in range(INPUTS) for e in engines}
    if keys != required or rows != result['rows']:
        raise ValueError('incomplete/out-of-domain stream or result mismatch')
    if (result['frames'] != len(rows) or result['matches'] != sum(x['match'] for x in rows) or
            result['controls'] != sum(x['controls'] for x in rows) or
            result['specified_faults'] != sum(x['specified_fault'] is True for x in rows)):
        raise ValueError('saved aggregate counts disagree with complete stream')
    report=dict(schema='guest-cell-ssa-saved-audit-v1',passed=True,eof_exact=True,
                result_path=str(saved/'RESULTS.json'),result_sha256=sha(saved/'RESULTS.json'),
                oracle_sha256=sha(__file__),phase=result['phase'],host_opt=result['host_opt'],
                frames=len(rows),matches=sum(x['match'] for x in rows),
                controls=sum(x['controls'] for x in rows),
                specified_faults=sum(x['specified_fault'] is True for x in rows),
                unique_module_inputs=len({(x['name'],x['trial']) for x in rows}),
                unique_initial_arenas=len({x['sha256']['input'] for x in rows}),
                scope='Saved bytes only: no compiler/product/native rerun. Final-state assertions do not claim an intermediate trace.')
    (out/'REPORT.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
