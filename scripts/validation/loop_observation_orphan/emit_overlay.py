"""Emit exact good/all-node-fault overlays from the frozen cache-test overlay.

This newly packaged generator reproduces the source-only transforms used by
the completed orphan experiment. It does not run a compiler or target.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

def sha(raw):return hashlib.sha256(raw).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--good-overlay',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    raw=args.good_overlay.read_bytes();text=raw.decode()
    assert sha(raw)=='7ad0f4675eb9b678337b49de37785459eb81117cbdaedaff2523f19adfc86af3'
    assert sha(args.probe.read_bytes())=='78c96a1f89593363b0cbd54ba173a4ba38d45feee38435fd8506205127b86f4c'
    original='      if (!work.spend() || !covered) return {};'
    fault='      if (!work.spend()) return {}; // TEST FAULT: ignore unanchored collected node'
    assert text.count(original)==1
    mutant=text.replace(original,fault,1).encode()
    assert sha(mutant)=='13832f8ec1e10d8f9cb89763e0129a09a6058ff3d1294892b16c3d4c9ba016ec'
    args.out.mkdir(parents=True,exist_ok=False)
    (args.out/'overlay').mkdir();(args.out/'source-v1').mkdir()
    (args.out/'overlay/LLVMImageTableContext.overlay.cpp').write_bytes(raw)
    (args.out/'overlay/LLVMImageTableContext.no-all-node-coverage.cpp').write_bytes(mutant)
    shutil.copy2(args.probe,args.out/'source-v1/orphan_coverage_probe.cpp')
    manifest=dict(schema='repo-replay-orphan-source-emission-v1',
      good_overlay_sha256=sha(raw),mutant_sha256=sha(mutant),mutation_sites=1,
      source_sha256=sha(args.probe.read_bytes()),compiled=False,target_calls=0,
      original_D_source_sha256='2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8')
    (args.out/'EMIT.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    print(json.dumps(manifest,indent=2))
if __name__=='__main__':main()
