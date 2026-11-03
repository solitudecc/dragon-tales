"""Fail-closed summary of static corpus verification and structural baselines."""
import argparse,json
from pathlib import Path

STRUCTURE=('objdump_missing','blocks','blocks_without_ignil','edges_to_undecoded',
           'edges_to_undecoded_eg','functions','empty_symbol_functions')

def summarize(current,baseline):
    summary=dict(samples=0,strategies=0,functions=0,raw_modules=0,optimized_modules=0,
                 import_references=0,unchanged_strategies=0,errors=[])
    for current_dir,baseline_dir in zip(current,baseline):
        found={p.name:p for p in current_dir.glob('*.json')}
        reference={p.name:p for p in baseline_dir.glob('*.json')}
        if not reference:summary['errors'].append(f'empty baseline {baseline_dir}')
        if set(found)!=set(reference):summary['errors'].append(dict(corpus=str(current_dir),missing=sorted(set(reference)-set(found)),extra=sorted(set(found)-set(reference))))
        for name in sorted(set(found)&set(reference)):
            now=json.loads(found[name].read_text());before=json.loads(reference[name].read_text())
            summary['samples']+=1
            if now.get('llvm_mode')!='static' or now.get('llvm_optimize') is not True:
                summary['errors'].append(f'{name}: not an optimized static run')
            for strategy in ('linear','recursive'):
                n,b=now[strategy],before[strategy];summary['strategies']+=1
                changed=[key for key in STRUCTURE if n.get(key)!=b.get(key)]
                if changed:summary['errors'].append(dict(binary=name,strategy=strategy,changed=changed))
                else:summary['unchanged_strategies']+=1
                count=n['functions'];summary['functions']+=count
                summary['raw_modules']+=n['llvm_verified_modules']
                summary['optimized_modules']+=n['llvm_optimized_verified_modules']
                summary['import_references']+=n['llvm_import_references']
                if n['llvm_lift_failed'] or n['llvm_verify']!='ok' or n['llvm_optimized_verify']!='ok' or n['llvm_verified_functions']!=count or n['llvm_optimized_verified_functions']!=count:
                    summary['errors'].append(dict(binary=name,strategy=strategy,detail='lift/verify failure or incomplete function coverage'))
                if n['llvm_verified_modules']<1 or n['llvm_optimized_verified_modules']!=n['llvm_verified_modules']:
                    summary['errors'].append(dict(binary=name,strategy=strategy,detail='incomplete module coverage'))
    return summary

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--current',type=Path,action='append',required=True)
    p.add_argument('--baseline',type=Path,action='append',required=True);p.add_argument('--out',type=Path,required=True);args=p.parse_args()
    if len(args.current)!=len(args.baseline):p.error('pair each current corpus with its baseline')
    summary=summarize(args.current,args.baseline);args.out.write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
    return 1 if summary['errors'] or not summary['samples'] else 0

if __name__=='__main__':raise SystemExit(main())
