#!/usr/bin/env python3
"""Compare exact main/no-Z3 output sets and prove the same byte gate rejects corruption."""
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();r=a.root
left={str(f.relative_to(r/'main')):f for f in (r/'main').glob('*/*.ll')};right={str(f.relative_to(r/'noz3')):f for f in (r/'noz3').glob('*/*.ll')}
assert set(left)==set(right) and len(left)==41
rows=[]
def same(x,y):
 if x!=y:raise AssertionError('main/no-Z3 IR byte mismatch')
for name in sorted(left):
 x=left[name].read_bytes();y=right[name].read_bytes();same(x,y)
 damaged=bytearray(y);damaged[len(damaged)//2]^=1
 try:same(x,damaged)
 except AssertionError:pass
 else:raise AssertionError('IR comparator corruption escaped')
 rows.append({'path':name,'bytes':len(x),'sha256':hashlib.sha256(x).hexdigest()})
result={'ir_files':len(rows),'fault_controls':len(rows),'native_executions_added':0,'files':rows}
(r/'main-noz3-ir-compat.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'exact_ir':len(rows),'fault_controls':len(rows)}))
