#!/usr/bin/env python3
"""Saved-only audit of one B ELF original/new LLVM replay; executes no target."""
from pathlib import Path
import argparse,ast,collections,hashlib,json,random,struct,traceback
REGS,MEM,DATA=512,65536,32768
FIELDS=[(n,8*i,8) for i,n in enumerate(['RAX','RCX','RDX','RBX','RSP','RBP','RSI','RDI']+[f'R{i}' for i in range(8,16)])]
FIELDS +=[(n,136+i,1) for i,n in enumerate(['CF','PF','AF','ZF','SF','OF','DF'])]
FIELDS +=[(f'XMM{i}',143+8*i,8) for i in range(16)]+[(f'XMM{i}H',271+8*i,8) for i in range(16)]
FIELDS +=[(f'FPU{i}L',400+10*i,8) for i in range(8)]+[(f'FPU{i}H',408+10*i,2) for i in range(8)]
FIELDS +=[('FPTOP',480,1),('FPTAG',482,2),('FPCW',484,2),('FPSW',486,2),('PC',128,8)]
assert len(FIELDS)==76
def sha(b):return hashlib.sha256(b).hexdigest()
def parsed(b):
    assert len(b)==66136 and b[:8]==b'DTFLOW1\0'
    return b[24:536],b[536:66072],struct.unpack_from('<QQ',b,8),list(struct.unpack('<8Q',b[-64:]))
def differences(w,g):
    wr,wm,wc,we=parsed(w);gr,gm,gc,ge=parsed(g);out=[]
    for n,o,s in FIELDS:
        a=int.from_bytes(wr[o:o+s],'little');b=int.from_bytes(gr[o:o+s],'little')
        if a!=b:out.append(dict(field=n,got=hex(b),expected=hex(a)))
    changed=[i for i,(a,b) in enumerate(zip(wm,gm)) if a!=b]
    if changed:out.append(dict(field='memory',count=len(changed),examples=[dict(address=hex(i),got=gm[i],expected=wm[i]) for i in changed[:8]]))
    assert wc==gc
    if we[:7]!=ge[:7]:out.append(dict(field='exit',got=ge[:7],expected=we[:7]))
    assert we[7]==ge[7]==0
    return out
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--plan',type=Path,required=True);ap.add_argument('--root',type=Path,required=True)
    ap.add_argument('--phase',choices=['before','after'],required=True);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();assert not a.out.exists();a.out.mkdir(parents=True)
    pins={};report=dict(passed=False,phase=a.phase,new_native_product_Unicorn_runs=0,failures=[])
    def read(p):
        p=Path(p);assert p.is_file() and not p.is_symlink();b=p.read_bytes();h=sha(b)
        assert str(p) not in pins or pins[str(p)]==h;pins[str(p)]=h;return b
    try:
        pb=read(a.plan);plan=json.loads(pb);result_path=a.root/'RESULTS.json';result=json.loads(read(result_path))
        inv=json.loads(read(a.root/'INVOCATION.json'))
        assert result['correct'] and result['complete'] and result['phase']==a.phase
        assert result['plan_sha256']==inv['plan_sha256']==sha(pb)
        assert result['native_comparisons']==result['root_returns']==32 and result['statuses']=={'match':32}
        assert result['comparator_controls']==96 and result['failure'] is None
        for k in ['new_native_C_comparisons','new_Unicorn_comparisons','producer_runs','product_pass_runs','lift_runs','skipped','undefined_exclusions','normalizations']:assert result[k]==0
        for pin in plan['bindings'].values():assert sha(read(pin['path']))==pin['sha256']
        for pin in inv['source_pins'].items():assert sha(read(pin[0]))==pin[1]
        for pin in inv['calibration_pins'].items():assert sha(read(pin[0]))==pin[1]
        rows={(r['host_llvm_level'],r['trial']):r for r in result['results']}
        controls={(r['host_llvm_level'],r['trial'],r['field']):r for r in result['controls']}
        assert len(rows)==len(result['results'])==32 and set(rows)=={(o,i) for o in ['O0','O2'] for i in range(16)}
        assert len(controls)==len(result['controls'])==96 and set(controls)=={(o,i,f) for o in ['O0','O2'] for i in range(16) for f in ['RAX','memory','exit']}
        assert {p.name for p in a.root.glob('expected-*.bin')}=={f'expected-{i}.bin' for i in range(16)}
        payload=json.loads(read(plan['bindings']['B_payload']['path']))
        old=json.loads(read(plan['bindings']['old_replay_results']['path']))
        refs={r['trial']:r for r in old['references'] if r['case']=='manual-O2'}
        spec_pins=[p for p in plan['source_compatibility']['unchanged'] if p['path'].endswith('/practical_holdout_source.py')]
        assert len(spec_pins)==1
        sp=spec_pins[0];sb=read(sp['path']);assert sha(sb)=='6ce5d1417bd1ada1c12e4b4a4adf32e35233f60445868c9ffdbde004245bd5b5'
        tree=ast.parse(sb);names={'SEED','INPUTS','MASK'};funcs={'_constants','_rotate_word','_octet_replacement','specification'}
        nodes=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in funcs or isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id in names for target in n.targets for t in ast.walk(target))]
        ns={'random':random,'struct':struct};exec(compile(ast.Module(body=nodes,type_ignores=[]),sp['path'],'exec'),ns)
        assert ns['SEED']==2026111307 and ns['INPUTS']==[r['value'] for r in plan['inputs']]
        unique=set();field_counts=collections.Counter()
        for item in plan['inputs']:
            i=item['trial'];ib=read(item['input']['path']);eb=read(item['expected']['path'])
            assert len(ib)==66048 and sha(ib)==item['input']['sha256']==refs[i]['input_sha256']
            q=payload['files'][item['original_B_input_member']];assert len(ib)==q['size'] and sha(ib)==q['sha256']
            unique.add(sha(ib));assert sha(eb)==item['expected']['sha256']==refs[i]['expected_snapshot_sha256']
            assert read(a.root/f'expected-{i}.bin')==eb
            er,em,ec,ee=parsed(eb);assert ee==[1,3,4111,61440,1,0,0,0]
            assert (int.from_bytes(er[:4],'little'),em[DATA:DATA+16])==ns['specification'](ns['SEED'],item['value'])
            for level in ['O0','O2']:
                assert read(a.root/level/f'input-{i}.bin')==ib
                actual=read(a.root/level/f'output-{i}.bin');assert actual==eb and differences(eb,actual)==[]
                row=rows[(level,i)]
                assert row['status']=='match' and row['root_return'] and row['raw_snapshot_equal'] and row['differences']==[]
                assert row['register_fields_compared']==76 and row['memory_bytes_compared']==65536
                assert row['exit']==ee[:7] and row['budget_left']==0
                for field,off in [('RAX',24),('memory',24+REGS+DATA),('exit',len(actual)-64)]:
                    bad=bytearray(actual);bad[off]^=1;diff=differences(eb,bytes(bad))
                    assert len(diff)==1 and diff[0]['field']==field
                    ctrl=controls[(level,i,field)];assert ctrl['differences']==diff
                    assert ctrl['scope']=='comparator-only snapshot corruption; no extra native run'
                    field_counts[field]+=1
        assert len(unique)==16
        for level in ['O0','O2']:
            w=a.root/level
            for prefix in ['input','output']:assert {p.name for p in w.glob(prefix+'-*.bin')}=={f'{prefix}-{i}.bin' for i in range(16)}
            assert sha(read(w/'driver.c'))==plan['bindings']['old_driver']['sha256']
        for p,h in list(pins.items()):assert sha(Path(p).read_bytes())==h,'changed during saved audit '+p
        report.update(passed=True,result_sha256=sha(read(result_path)),raw_snapshot_bytes_each=66136,
            raw_complete_snapshot_matches=32,register_fields_per_row=76,Guest_bytes_per_row=65536,Exit_words_per_row=8,
            unique_original_B_inputs=16,comparator_only_controls=96,control_fields=dict(field_counts),
            old_C_Unicorn_counts_recounted=0,ModuleID_normalizations=0,input_sha256=pins)
    except BaseException as e:
        report['failures'].append(dict(type=type(e).__name__,detail=str(e),traceback=traceback.format_exc()));raise
    finally:
        p=a.out/'REPORT.json';p.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(a.phase,'saved32 complete snapshots +96 controls PASS',sha((a.out/'REPORT.json').read_bytes()))

if __name__=='__main__':main()
