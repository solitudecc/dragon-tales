"""Independently verify both finished exact file sets; write only an external receipt."""
from pathlib import Path
import hashlib
import json
import shlex
import subprocess

PROGRAM=r'''
from pathlib import Path
import hashlib,json,stat,sys
root=Path(sys.argv[1]).resolve()
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1048576),b''):h.update(b)
 return h.hexdigest()
def sig(p):
 s=p.stat();return (s.st_dev,s.st_ino,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
manifest=json.loads((root/'FILESET.json').read_text())
closure=json.loads((root/'CLOSURE.json').read_text())
assert closure['frozen'] and closure['no_more_writes']
assert sha(root/'FILESET.json')==closure['manifest_sha256']
assert sha(root/'SUMMARY.json')==closure['summary_sha256']
assert sha(root/'SAVED-AUDIT.json')==closure['saved_audit_sha256']
paths=list(root.rglob('*'));assert all(not p.is_symlink() for p in paths)
files={str(p.relative_to(root)):p for p in paths if p.is_file()}
assert set(files)==set(manifest['payload'])|{'FILESET.json','CLOSURE.json'}
assert sorted(str(p.relative_to(root)) for p in paths if p.is_dir())==manifest['directories']
before={name:sig(p) for name,p in files.items()}
assert all(x[2]==1 for x in before.values())
got={name:{'size':p.stat().st_size,'sha256':sha(p)} for name,p in files.items()}
assert {name:got[name] for name in manifest['payload']}==manifest['payload']
assert before=={name:sig(p) for name,p in files.items()}
assert sum(x['size'] for x in manifest['payload'].values())==manifest['logical_bytes']
print(json.dumps({'root':str(root),'payload_count':len(manifest['payload']),
 'logical_bytes':manifest['logical_bytes'],'files':got,'file_count':len(files),
 'all_regular_no_links':True,'identity_before_after_equal':True,
 'empty_preflight_namespace_preserved':(root/'stages/opaque-probe').is_dir() and not list((root/'stages/opaque-probe').iterdir())},sort_keys=True))
'''

def main():
    local=Path('/Users/c/Desktop/deob/validation-artifacts/loop-known-leaf-accounting-private-20261115-v1')
    remote='/home/ubuntu/vmc-runs/loop-known-leaf-accounting-private-20261115-v1'
    mac=json.loads(subprocess.check_output(['/usr/bin/python3','-c',PROGRAM,str(local)],text=True))
    argv=['ssh','-n','-o','ControlPath=/tmp/dragon-rock5b-20261114.sock','ubuntu@192.168.10.241',
          'PYTHONDONTWRITEBYTECODE=1 python3 -c '+shlex.quote(PROGRAM)+' '+shlex.quote(remote)]
    rock=json.loads(subprocess.check_output(argv,text=True))
    assert mac['files']==rock['files']
    result=dict(schema='dual-host-explicit-exact-set-verification-v1',correct=True,
       Mac=mac,ROCK=rock,all_file_bytes_equal=True,guest_native=0,
       checker_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    target=local.parent/(local.name+'-DUAL-VERIFIED.json')
    assert not target.exists()
    target.write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    print(json.dumps(dict(path=str(target),sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
       payload_count=mac['payload_count'],logical_bytes=mac['logical_bytes'],correct=True),indent=2))

if __name__=='__main__':main()
