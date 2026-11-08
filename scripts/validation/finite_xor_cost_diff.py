#!/usr/bin/env python3
"""Independent machine XOR references and read-only finite-set checks.

Pure --self-test imports neither Dragon nor Unicorn. --oracle executes two
full-state native-compatible fixture inputs (64 profiles each) plus separate
scalar Cartesian references. --analyze reads those frozen records; it never
runs CPU/LLVM or publishes Graph edges. Native uses unchanged
finite_dispatch_diff.py --fixtures finite-xor-cost.json --trials 64.
"""
from __future__ import annotations
import argparse
import collections
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
DEFAULT = ROOT / 'tests/fixtures/finite-xor-cost.json'
ENTRY, FIRST, LAST, STACK, SENTINEL = 0x1000, 0x1100, 0x113f, 0xc800, 0xf000
MASK64 = (1 << 64) - 1
EDGES = (0, 1, MASK64, 1 << 63, 0x7fffffffffffffff, 0xffffffff, 1 << 31, 0x55555555aaaaaaaa)

def sha(data): return hashlib.sha256(data).hexdigest()
def file_sha(path): return sha(Path(path).read_bytes())
def load(path): return json.loads(Path(path).read_text())
def save(path, value): Path(path).write_text(json.dumps(value, indent=2) + '\n')
def mask(width): return (1 << width) - 1
def omitted_target(published, actual): return actual not in set(published)

def native_fixtures():
    cases = []
    body = bytes.fromhex('89f883e03f89f283e23f31d00500110000ffe0')
    for architecture in ('x86_32', 'x86_64'):
        width = 32 if architecture == 'x86_32' else 64
        left = [((EDGES[t % 8] & ~63) | (t ^ (t >> 1))) & mask(width) for t in range(64)]
        right = [((EDGES[(t + 3) % 8] & ~63) | (t >> 1)) & mask(width) for t in range(64)]
        code = bytearray(b'\xcc' * 0x140)
        code[:len(body)] = body
        code[FIRST - ENTRY:LAST - ENTRY + 1] = b'\xc3' * 64
        names = ('EDI', 'ESI') if width == 32 else ('RDI', 'RSI')
        cases.append(dict(id=architecture + '_finite_xor_cube', architecture=architecture,
            code_hex=bytes(code).hex(), undefined_flags=[], stubs=[], local_functions=[],
            indirect_site=ENTRY + len(body) - 2, expected_targets=list(range(FIRST, LAST + 1)),
            set_gprs={names[0]: left, names[1]: right},
            finite_xor=dict(required_native_trials=64, input_registers=list(names),
                mask=63, target_base=FIRST, source_independence='Different architectural input registers; no same-snapshot equality assumption.',
                final_flags='ADD EAX,0x1100 overwrites all arithmetic XOR flags before observation.')))
    return dict(schema=1, entry=ENTRY, cases=cases,
        scope='Default finite analysis, 64 literal RET targets; native trials must cover all 64, without truncating target coverage.')

def validate(document):
    if document != native_fixtures(): raise ValueError('permanent native fixture bytes, inputs or contract changed')
    for fixture in document['cases']:
        names = fixture['finite_xor']['input_registers']
        observed = [FIRST + ((a & 63) ^ (b & 63)) for a, b in zip(*(fixture['set_gprs'][n] for n in names))]
        if observed != fixture['expected_targets']: raise ValueError('64 real profiles do not cover every target')
    return document

def subvalues(bits):
    values = []
    value = bits
    while True:
        values.append(value)
        if value == 0: break
        value = (value - 1) & bits
    return sorted(values)

def specialty_cases():
    shapes = [
        ('independent_cube', 63, 0x100, 15, 0x200, 32, 0, 1),
        ('fixed_high', 7, 0x80000000, 7, 0x40000000, 32, 0, 1),
        ('sparse_bits', 0x55, 0x80008000, 0x15, 0x40002000, 32, 0, 1),
        ('affine_shift', 15, 0x80000001, 0x3c, 0x40000002, 32, 2, 1),
        ('truncate_byte', 0x700, 0x55, 0x700, 0xaa, 8, 0, 1),
        ('truncate_word', 0x70000, 0x55aa, 0x70000, 0xaa55, 16, 0, 1),
        ('non_cube_holes', 7, 0, 0x15, 0x80, 32, 0, 3),
        ('result_over_capacity', 15, 0, 0xf00, 0, 32, 0, 1),
    ]
    result = []
    for arch in ('x86_32', 'x86_64'):
        for name, lm, lf, rm, rf, width, shift, multiply in shapes:
            code = bytearray(b'\x89\xf8\x25' + struct.pack('<I', lm))
            if shift: code += b'\xc1\xe0' + bytes([shift])
            if multiply != 1: code += b'\x6b\xc0' + bytes([multiply])
            code += b'\x0d' + struct.pack('<I', lf) + b'\x89\xf2\x81\xe2' + struct.pack('<I', rm)
            code += b'\x81\xca' + struct.pack('<I', rf)
            xor_pc = ENTRY + len(code)
            code += b'\x31\xd0' + (b'\x0f\xb6\xc0' if width == 8 else b'\x0f\xb7\xc0' if width == 16 else b'')
            result.append(dict(id=arch + '_' + name, architecture=arch, source_width=32,
                width=width, left_mask=lm, left_fixed=lf, right_mask=rm, right_fixed=rf,
                left_shift=shift, left_multiply=multiply, code_hex=(code + b'\xc3').hex(),
                xor_pc=xor_pc, ret_pc=ENTRY + len(code)))
    lm, lf, rm, rf = 0x8000000000000015, 0x4000000000000080, 0x8000000000000005, 0x2000000000000040
    code = bytearray(b'\x48\x89\xf8\x49\xb8' + struct.pack('<Q', lm) + b'\x4c\x21\xc0\x49\xb8' + struct.pack('<Q', lf) + b'\x4c\x09\xc0')
    code += b'\x48\x89\xf2\x49\xb9' + struct.pack('<Q', rm) + b'\x4c\x21\xca\x49\xb9' + struct.pack('<Q', rf) + b'\x4c\x09\xca'
    xor_pc = ENTRY + len(code); code += b'\x48\x31\xd0'
    result.append(dict(id='x86_64_high_variable', architecture='x86_64', source_width=64,
        width=64, left_mask=lm, left_fixed=lf, right_mask=rm, right_fixed=rf,
        left_shift=0, left_multiply=1, code_hex=(code + b'\xc3').hex(), xor_pc=xor_pc, ret_pc=ENTRY + len(code)))
    return result

def produced(f, value, left):
    if left:
        return ((((value & f['left_mask']) << f['left_shift']) * f['left_multiply']) | f['left_fixed']) & mask(f['source_width'])
    return ((value & f['right_mask']) | f['right_fixed']) & mask(f['source_width'])

def exact_values(f):
    return sorted({(produced(f, a, True) ^ produced(f, b, False)) & mask(f['width'])
                   for a in subvalues(f['left_mask']) for b in subvalues(f['right_mask'])})

def compare_set(value, expected, complete):
    if not complete: raise ValueError('tiny finite analysis did not finish')
    if len(expected) > 64:
        if value['kind'] != 2 or value['values']: raise ValueError('over-capacity set must be Unknown')
    elif value['kind'] != 1 or value['values'] != expected:
        raise ValueError('finite result differs from exhaustive independent CPU domain')

def self_test(path):
    document = validate(load(path)); checks = dict(full64profiles=True, source_registers_distinct=True)
    fs = specialty_cases(); checks['unique_specialties'] = len({f['id'] for f in fs}) == 17
    checks['scalar_Cartesian_trials'] = sum(len(subvalues(f['left_mask'])) * len(subvalues(f['right_mask'])) for f in fs) == 3968
    for kind, edit in [('drop_target', lambda d:d['cases'][0]['expected_targets'].pop()),
                       ('hidden_AF_exclusion', lambda d:d['cases'][0]['undefined_flags'].append('AF')),
                       ('change_input', lambda d:d['cases'][0]['set_gprs']['EDI'].__setitem__(0,1)),
                       ('change_code', lambda d:d['cases'][0].__setitem__('code_hex','c3'))]:
        bad = copy.deepcopy(document); edit(bad)
        try: validate(bad)
        except ValueError: checks[kind + '_rejected'] = True
        else: checks[kind + '_rejected'] = False
    for name, value, complete in [('wrong', dict(kind=1,values=[9]), True), ('empty', dict(kind=1,values=[]), True),
                                   ('unknown', dict(kind=2,values=[]), True), ('partial', dict(kind=1,values=[7]), False)]:
        try: compare_set(value,[7],complete)
        except ValueError: checks[name + '_report_rejected'] = True
        else: checks[name + '_report_rejected'] = False
    holes = next(f for f in fs if f['id']=='x86_32_non_cube_holes')
    values = [produced(holes,a,True) for a in subvalues(7)]
    fixed, possible = values[0], 0
    for v in values: fixed &= v; possible |= v
    checks['non_cube_has_actual_holes'] = len(values) != 1 << (possible ^ fixed).bit_count()
    checks['edge_present_not_missing'] = not omitted_target(range(FIRST,LAST+1),FIRST)
    checks['edge_removed_is_missing'] = omitted_target(range(FIRST+1,LAST+1),FIRST)
    checks['empty_edge_not_success'] = omitted_target([],FIRST)
    if not all(checks.values()): raise RuntimeError(repr(checks))
    return dict(ok=True,checks=checks,CPU_Graph_LLVM_executions=0)

def native_expected(adapter, f, regs, memory):
    import function_guest_diff as guest
    out = bytearray(regs); wide = f['architecture']=='x86_64'
    if not wide:
        for unused,offset,width,unused2 in adapter.gprs: guest.base.put(out,offset,guest.base.get(regs,offset,width))
    a, b = guest.base.get(regs,56,4)&63, guest.base.get(regs,48,4)&63
    target = FIRST + (a ^ b)
    guest.base.put(out,0,target);guest.base.put(out,16,b)
    guest.base.put(out,32,(guest.base.get(regs,32,8 if wide else 4)+(8 if wide else 4))&mask(64 if wide else 32))
    guest.base.put(out,128,SENTINEL)
    for offset,value in [(136,0),(137,int((target&255).bit_count()%2==0)),(138,0),(139,0),(140,0),(141,0)]:out[offset]=value
    return bytes(out),bytes(memory),collections.Counter(),(1,3,target,SENTINEL,1,0,0)

def oracle(path, output):
    import function_guest_diff as guest
    import unicorn as U
    from unicorn import x86_const as X
    document=validate(load(path));output.mkdir(parents=True,exist_ok=False);native=[];controls=collections.Counter()
    dependencies={str(p):file_sha(p) for p in [Path(__file__),path,*[ROOT/'scripts/validation'/n for n in ['function_guest_diff.py','function_dispatch_diff.py','function_llvm_diff.py']]]}
    for f in document['cases']:
        adapter=guest.Adapter(f['architecture'])
        for trial in range(64):
            regs,mem=adapter.initial(f,trial,ENTRY);actual=adapter.reference(f,regs,mem,ENTRY);wanted=native_expected(adapter,f,regs,mem)
            packed=struct.pack('<8Q',*actual[3],0);diffs,unused=adapter.compare(f,wanted,(*actual[:3],packed),False)
            if diffs:raise RuntimeError(repr(diffs))
            work=output/'native'/f['id']/str(trial);work.mkdir(parents=True)
            for name,data in [('initial-registers.bin',regs),('initial-memory.bin',mem),('final-registers.bin',actual[0]),('final-memory.bin',actual[1])]: (work/name).write_bytes(data)
            for kind in ['result','memory','single-field']:
                r,m=bytearray(actual[0]),bytearray(actual[1]);field='EAX' if adapter.x32 else 'RAX'
                if kind=='result':r[0]^=1
                elif kind=='memory':m[0x8000]^=1;field='memory'
                else:r[142]^=1;field='DF'
                bad,unused=adapter.compare(f,wanted,(bytes(r),bytes(m),actual[2],packed),False)
                if len(bad)!=1 or bad[0]['field']!=field or kind=='memory' and bad[0]['count']!=1:raise RuntimeError('single-field control failed')
                controls[kind]+=1
            target=actual[3][2]
            if target not in f['expected_targets'] or not omitted_target(set(f['expected_targets'])-{target},target):raise RuntimeError('edge omission escaped control')
            controls['edge']+=1
            native.append(dict(case=f['id'],trial=trial,target=target,complete=True,exit=actual[3],fields=len(adapter.fields),memory_bytes=65536))
    scalar=[]
    for f in specialty_cases():
        wide=f['architecture']=='x86_64';uc=U.Uc(U.UC_ARCH_X86,U.UC_MODE_64 if wide else U.UC_MODE_32)
        uc.mem_map(0,65536);code=bytes.fromhex(f['code_hex']);uc.mem_write(ENTRY,code)
        rp=X.UC_X86_REG_RIP if wide else X.UC_X86_REG_EIP;sp=X.UC_X86_REG_RSP if wide else X.UC_X86_REG_ESP
        di=X.UC_X86_REG_RDI if wide else X.UC_X86_REG_EDI;si=X.UC_X86_REG_RSI if wide else X.UC_X86_REG_ESI
        ax=X.UC_X86_REG_RAX if wide else X.UC_X86_REG_EAX
        rows=[];index=0
        for a in subvalues(f['left_mask']):
            for b in subvalues(f['right_mask']):
                av=(a|(EDGES[index%8]&~f['left_mask']))&mask(f['source_width']);bv=(b|(EDGES[(index+3)%8]&~f['right_mask']))&mask(f['source_width']);index+=1
                uc.reg_write(di,av);uc.reg_write(si,bv);uc.reg_write(sp,STACK);uc.mem_write(STACK,SENTINEL.to_bytes(8 if wide else 4,'little'))
                uc.emu_start(ENTRY,SENTINEL,count=100)
                result=uc.reg_read(ax)&mask(f['width']);expected=(produced(f,av,True)^produced(f,bv,False))&mask(f['width'])
                if result!=expected or uc.reg_read(rp)!=SENTINEL or uc.reg_read(sp)!=STACK+(8 if wide else 4) or bytes(uc.mem_read(ENTRY,len(code)))!=code:raise RuntimeError('scalar CPU model/return/code differs')
                rows.append(dict(left=av,right=bv,value=result,PC=uc.reg_read(rp),SP=uc.reg_read(sp)))
        expected=exact_values(f)
        if sorted({r['value'] for r in rows})!=expected:raise RuntimeError('scalar independent domain coverage differs')
        scalar.append(dict(fixture=f,rows=rows,exact_values=expected,code_sha256=sha(code),scope='Query-width scalar XOR result plus exact real RET PC/SP and unchanged code; no full-state claim.'))
    if any(file_sha(p)!=h for p,h in dependencies.items()):raise RuntimeError('reference source changed during execution')
    save(output/'oracle.json',dict(schema=1,dependencies=dependencies,native=native,native_full_roots=128,scalar=scalar,scalar_returns=sum(len(r['rows']) for r in scalar),controls=dict(controls),fixture_sha256=file_sha(path),script_sha256=file_sha(__file__),CPU_executions=128+sum(len(r['rows']) for r in scalar),Graph_LLVM_executions=0))
    return dict(native_full_roots=128,scalar_returns=sum(len(r['rows']) for r in scalar),controls=dict(controls),Graph_LLVM_executions=0)

def analyze(path, output, evidence, pin_path):
    import os
    sys.path.insert(0,os.environ.get('DRAGON_TEST_PYTHON',str(ROOT/'python')));import dragon
    import caller_context_diff as context
    validate(load(path));cpu=load(evidence/'oracle.json');pin=load(pin_path);context.actual_maps(dragon,pin)
    if cpu['fixture_sha256']!=file_sha(path) or cpu['script_sha256']!=file_sha(__file__):raise ValueError('CPU evidence source changed')
    output.mkdir(parents=True,exist_ok=False);rows=[]
    document=validate(load(path))
    expected_native_keys={(f['id'],trial) for f in document['cases'] for trial in range(64)}
    if len(cpu['native'])!=128 or {(r['case'],r['trial']) for r in cpu['native']}!=expected_native_keys or not all(r['complete'] for r in cpu['native']):raise ValueError('native CPU key coverage differs')
    specials=specialty_cases()
    if len(cpu['scalar'])!=len(specials) or {r['fixture']['id'] for r in cpu['scalar']}!={f['id'] for f in specials}:raise ValueError('scalar case coverage differs')
    for f in document['cases']:
        for strategy in (0,1):
            with dragon.Disassembler(2 if f['architecture']=='x86_64' else 1,dragon.Configuration(strategy=strategy)) as dis:
                with dis.analyze(bytes.fromhex(f['code_hex']),ENTRY) as graph:
                    sites=[]
                    for at in graph.block_addresses():
                        ir=graph.get_ignil_block(at)
                        if ir and ir.ops() and ir.ops()[-1][:2]==(f['indirect_site'],dragon.IGNILOpcode.JMP):sites.append(at)
                    if len(sites)!=1:raise RuntimeError('not one native indirect query block')
                    before=context.graph_fingerprint(graph)
                    result=graph.finite_values(entry_points=[ENTRY])
                    compare_set(result['jump_targets'][sites[0]],f['expected_targets'],result['complete'])
                    if context.graph_fingerprint(graph)!=before:raise RuntimeError('native read-only query mutated Graph')
                    rows.append(dict(case=f['id'],strategy=strategy,result=result,graph_before=before,graph_after=before,actual_maps=context.actual_maps(dragon,pin),prove_leaf_calls=False,scope='Native fixture default finite target query; not a new CPU execution.'))
    for f in specials:
        ref=next(r for r in cpu['scalar'] if r['fixture']['id']==f['id'])
        keys=[(r['left']&f['left_mask'],r['right']&f['right_mask']) for r in ref['rows']]
        wanted_keys={(a,b) for a in subvalues(f['left_mask']) for b in subvalues(f['right_mask'])}
        if len(keys)!=len(wanted_keys) or set(keys)!=wanted_keys or ref['fixture']!=f or ref['exact_values']!=exact_values(f) or sorted({r['value'] for r in ref['rows']})!=ref['exact_values']:raise ValueError('scalar evidence coverage differs')
        for strategy in (0,1):
            with dragon.Disassembler(2 if f['architecture']=='x86_64' else 1,dragon.Configuration(strategy=strategy)) as dis:
                with dis.analyze(bytes.fromhex(f['code_hex']),ENTRY) as graph:
                    sites=[]
                    for at in graph.block_addresses():
                        ir=graph.get_ignil_block(at)
                        if ir and any(pc==f['ret_pc'] and op==dragon.IGNILOpcode.RET for pc,op,index in ir.ops()):sites.append(at)
                    if len(sites)!=1:raise RuntimeError('not one real RET query block')
                    before=context.graph_fingerprint(graph)
                    result=graph.finite_values(entry_points=[ENTRY],query_register=(sites[0],0,f['width']))
                    compare_set(result['values'][0],ref['exact_values'],result['complete'])
                    if context.graph_fingerprint(graph)!=before:raise RuntimeError('read-only query mutated Graph')
                    rows.append(dict(case=f['id'],strategy=strategy,result=result,graph_before=before,graph_after=before,actual_maps=context.actual_maps(dragon,pin),prove_leaf_calls=False))
    save(output/'analysis.json',dict(schema=1,rows=rows,CPU_LLVM_executions=0,script_sha256=file_sha(__file__),fixture_sha256=file_sha(path),oracle_sha256=file_sha(evidence/'oracle.json'),libraries=pin))
    return dict(read_only_reports=len(rows),CPU_LLVM_executions=0)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--fixtures',type=Path,default=DEFAULT)
    a=p.add_mutually_exclusive_group(required=True);a.add_argument('--self-test',action='store_true');a.add_argument('--oracle',type=Path);a.add_argument('--analyze',type=Path)
    p.add_argument('--oracle-evidence',type=Path);p.add_argument('--library-pin',type=Path);args=p.parse_args()
    if args.self_test:r=self_test(args.fixtures)
    elif args.oracle:r=oracle(args.fixtures,args.oracle)
    else:
        if not args.oracle_evidence or not args.library_pin:p.error('--analyze requires frozen --oracle-evidence and --library-pin')
        r=analyze(args.fixtures,args.analyze,args.oracle_evidence,args.library_pin)
    print(json.dumps(r,indent=2))
if __name__=='__main__':main()
