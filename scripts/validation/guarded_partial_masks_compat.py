#!/usr/bin/env python3
"""Exact dual-runtime IR and reports comparison, with actual damaged-byte controls."""
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();root=a.root;out=root/'byte-compat';out.mkdir(exist_ok=False)
left=root/'main';right=root/'noz3'
def selected(r):return {p.relative_to(r).as_posix():p for p in r.rglob('*.ll')}
l=selected(left);r=selected(right);assert l.keys()==r.keys() and len(l)==53,(len(l),len(r),l.keys()-r.keys(),r.keys()-l.keys())
rows=[]
for relative,p in sorted(l.items()):
 q=r[relative];x=p.read_bytes();y=q.read_bytes();assert x==y,relative
 broken=bytearray(y);assert broken;broken[len(broken)//2]^=1;assert x!=broken,relative
 rows.append({'relative':relative,'size':len(x),'sha256':hashlib.sha256(x).hexdigest(),'damaged_byte_control_caught':True})
reports=[]
for recipe,mode in [('normal',x) for x in ('plain','history','table_before','table_after','freeze_bool','call_inside')]+[(x,'plain') for x in ('wrong','resumed','bad','budget','zero','old','oldfinite','oldstore')]+[('normal','fatal_'+x) for x in ('undef','poison','shift')]:
 relative=recipe+'/'+mode+'.json';x=json.loads((left/relative).read_text());y=json.loads((right/relative).read_text());assert x==y,relative;reports.append(relative)
report={'ir_files':len(rows),'damaged_byte_controls':len(rows),'equal_reports':reports,'files':rows};(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='files'}))
