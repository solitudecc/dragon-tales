#!/usr/bin/env python3
"""Archive or independently verify all native evidence with an exact manifest."""
import argparse,hashlib,json,tarfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('action',choices=('pack','verify'));p.add_argument('root',type=Path);p.add_argument('archive',type=Path);a=p.parse_args()
def digest(data):return hashlib.sha256(data).hexdigest()
manifest=a.root/'MANIFEST.json'
if a.action=='pack':
 assert a.root.is_dir() and not manifest.exists() and not a.archive.exists()
 assert all((a.root/f'{label}-saved-audit.json').exists() for label in ('main','noz3'))
 files={str(f.relative_to(a.root)):{'size':f.stat().st_size,'sha256':digest(f.read_bytes())} for f in sorted(a.root.rglob('*')) if f.is_file()}
 manifest.write_text(json.dumps({'root':a.root.name,'files':files},indent=2)+'\n')
 with tarfile.open(a.archive,'w:gz') as tar:tar.add(a.root,arcname=a.root.name)
with tarfile.open(a.archive,'r:gz') as tar:
 members=[v for v in tar.getmembers() if v.isfile()];assert len({v.name for v in members})==len(members)
 rootname=a.root.name;entry=tar.extractfile(rootname+'/MANIFEST.json');assert entry is not None;stored=entry.read();data=json.loads(stored);assert data['root']==rootname
 expected={rootname+'/'+name:info for name,info in data['files'].items()};assert {v.name for v in members}==set(expected)|{rootname+'/MANIFEST.json'}
 for member in members:
  content=tar.extractfile(member).read()
  if member.name.endswith('/MANIFEST.json'):assert content==stored
  else:assert member.size==expected[member.name]['size'] and digest(content)==expected[member.name]['sha256'],member.name
if a.root.exists():
 actual={str(f.relative_to(a.root)) for f in a.root.rglob('*') if f.is_file()}
 assert actual==set(data['files'])|{'MANIFEST.json'}
 assert manifest.read_bytes()==stored
 for name,info in data['files'].items():
  f=a.root/name;assert f.stat().st_size==info['size'] and digest(f.read_bytes())==info['sha256'],name
print(json.dumps({'archive':str(a.archive),'sha256':digest(a.archive.read_bytes()),'files':len(data['files'])+1,'payloads':len(data['files']),'exact_archive_verified':True,'exact_expansion_verified':a.root.exists()},indent=2))
