"""Prove corpus reporting rejects incomplete and deliberately broken records."""
import copy,json,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts/validation'))
from static_corpus_summary import summarize

with tempfile.TemporaryDirectory() as folder:
    root=Path(folder);current=root/'current';baseline=root/'baseline'
    current.mkdir();baseline.mkdir()
    strategy=dict(time=1,functions=2,blocks=3,blocks_without_ignil=0,
        edges_to_undecoded=0,edges_to_undecoded_eg=[],empty_symbol_functions=0,
        llvm_lift_failed=0,llvm_verified_functions=2,llvm_verified_modules=1,
        llvm_import_references=4,llvm_verify='ok',llvm_optimized_verified_functions=2,
        llvm_optimized_verified_modules=1,llvm_optimized_verify='ok')
    good=dict(llvm_mode='static',llvm_optimize=True,linear=copy.deepcopy(strategy),recursive=copy.deepcopy(strategy))
    (baseline/'sample.json').write_text(json.dumps(good))
    def rejected(record):
        (current/'sample.json').write_text(json.dumps(record))
        return bool(summarize([current],[baseline])['errors'])
    assert not rejected(good)
    mutations=[('llvm_verified_functions',1),('llvm_optimized_verified_functions',1),
               ('llvm_lift_failed',1),('llvm_verify','bad IR'),
               ('llvm_optimized_verify','bad optimized IR'),('blocks',4),
               ('llvm_verified_modules',0),('llvm_optimized_verified_modules',0)]
    for key,value in mutations:
        broken=copy.deepcopy(good);broken['linear'][key]=value;assert rejected(broken),key
    broken=copy.deepcopy(good);broken['llvm_optimize']=False;assert rejected(broken)
    (current/'sample.json').unlink();assert summarize([current],[baseline])['errors']
print('Static corpus summary: 1 positive and 10 negative controls passed')
