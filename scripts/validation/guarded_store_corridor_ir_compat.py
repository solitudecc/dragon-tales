#!/usr/bin/env python3
"""Exact saved-IR comparison; this is not another native execution."""
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('left',type=Path);p.add_argument('right',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
def files(root):return {str(f.relative_to(root)):f for f in root.rglob('*.ll')}
x=files(a.left);y=files(a.right);assert x and x.keys()==y.keys(),(x.keys()-y.keys(),y.keys()-x.keys())
rows=[]
def same(left,right):
 if left!=right:raise AssertionError('saved IR differs')
for name in sorted(x):
 l=x[name].read_bytes();r=y[name].read_bytes();same(l,r)
 damaged=bytearray(r);damaged[len(damaged)//2]^=1
 try:same(l,damaged)
 except AssertionError:pass
 else:raise AssertionError('IR corruption control escaped')
 rows.append({'path':name,'bytes':len(l),'sha256':hashlib.sha256(l).hexdigest()})
a.output.write_text(json.dumps({'left':str(a.left),'right':str(a.right),'ir_files':len(rows),'fault_controls':len(rows),'native_executions_added':0,'files':rows},indent=2)+'\n')
print(json.dumps({'ir_files':len(rows),'fault_controls':len(rows),'byte_equal':True,'native_executions_added':0}))
