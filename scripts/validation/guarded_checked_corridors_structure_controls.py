#!/usr/bin/env python3
"""Fault the strict saved-IR gates; never execute damaged IR natively."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from guarded_checked_corridors_prepare import blocks, validate_ir
from guarded_checked_corridors_oracle import MODES


def main(root, label):
    output = root/(label+'-structure-controls-v2')
    output.mkdir(exist_ok=False)
    records = []
    for mode in MODES:
        if mode == 'call_inside':
            continue
        path = root/label/'normal'/(mode+'.ll')
        original = Path(str(path)+'.parsed-before.ll').read_text()
        text = path.read_text()
        contexts = 2 if mode == 'duplicate' else 1
        backedges = contexts*2 if mode == 'duplicate' else contexts
        proof = validate_ir(original, text, (43,44), contexts, backedges)
        product = proof['actual_fast_products'][0]
        table = blocks(text)
        fast, fallback = product['block'], product['fallback']
        head = next(target for _,target in product['fast_cases'] if target.startswith('head.transition'))
        phi = re.search(r'^\s*%history\.transition[-a-zA-Z$._0-9]* = phi i64 (.*)$', table[head], re.M)
        assert phi
        pairs = re.findall(r'\[ ([^,]+), %([-a-zA-Z$._0-9]+) \]', phi[1])
        fresh = next(v for v,pred in pairs if pred == fast)
        initial, initial_pred = next((v,pred) for v,pred in pairs if pred != fast)
        def replace_body(name, body):
            assert text.count(table[name]) == 1
            return text.replace(table[name], body, 1)
        bad = {
            'fallback-reread': replace_body(fallback, table[fallback].replace(product['selector'], '%loaded', 1)),
            'full-case-omitted': replace_body(fallback, re.sub(r'^\s*i64 43, label %head\n', '', table[fallback], count=1, flags=re.M)),
            'initial-root-zero': replace_body(head, table[head].replace('[ '+initial+', %'+initial_pred+' ]','[ 0, %'+initial_pred+' ]',1)),
            'fresh-root-stale': replace_body(head, table[head].replace('[ '+fresh+', %'+fast+' ]','[ %seed_state, %'+fast+' ]')),
            'fast-root-original': replace_body(fast, table[fast].replace('label %'+head, 'label %head', 1)),
            'prefix-CALL-duplicated': text.replace('  call void @prefix_unknown(ptr %regs, ptr %guest, ptr %config)\n','  call void @prefix_unknown(ptr %regs, ptr %guest, ptr %config)\n'*2,1),
            'prefix-CALL-omitted': text.replace('  call void @prefix_unknown(ptr %regs, ptr %guest, ptr %config)\n','',1),
            'initial-root-poison': replace_body(head, table[head].replace('[ '+initial+', %'+initial_pred+' ]','[ poison, %'+initial_pred+' ]',1)),
        }
        if mode == 'duplicate':
            bad['duplicate-PHI-slot-omitted'] = replace_body(head, table[head].replace(', [ '+fresh+', %'+fast+' ]', '', 1))
        for name, damaged in bad.items():
            assert damaged != text, (mode,name,'fault did not change bytes')
            source = output/(mode+'-'+name+'.ll')
            source.write_text(damaged)
            try:
                validate_ir(original, damaged, (43,44), contexts, backedges)
            except (AssertionError, KeyError, ValueError) as error:
                reason = str(error) or type(error).__name__
            else:
                raise AssertionError(('strict IR gate missed fault',mode,name))
            record = dict(mode=mode, fault=name, detected=True, reason=reason,
                          sha256=hashlib.sha256(source.read_bytes()).hexdigest(), native_executions=0)
            if name in ('initial-root-zero','fresh-root-stale','prefix-CALL-duplicated','prefix-CALL-omitted','initial-root-poison'):
                command = ['opt-21','-passes=verify','-disable-output',str(source)]
                result = subprocess.run(command,capture_output=True,timeout=180)
                (output/(mode+'-'+name+'.verify.stdout')).write_bytes(result.stdout)
                (output/(mode+'-'+name+'.verify.stderr')).write_bytes(result.stderr)
                assert result.returncode == 0, (mode,name,result.stderr.decode())
                record.update(llvm_verified=True,verify_command=command)
            records.append(record)
    assert len(records) == 57
    (output/'RESULT.json').write_text(json.dumps(dict(structural_faults_detected=len(records),
        llvm_valid_faults=sum(bool(r.get('llvm_verified')) for r in records),
        native_executions=0,rows=records),indent=2)+'\n')
    print(json.dumps(dict(structural_faults_detected=len(records),
                         llvm_valid_faults=sum(bool(r.get('llvm_verified')) for r in records),native_executions=0)))


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('root',type=Path)
    parser.add_argument('label')
    args=parser.parse_args()
    main(args.root,args.label)
