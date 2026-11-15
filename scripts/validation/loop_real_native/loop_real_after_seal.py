"""Exact multi-root regular-file evidence pack; no targets or cleanup."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import stat
import tarfile

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for b in iter(lambda:stream.read(1048576),b''):h.update(b)
    return h.hexdigest()

def collect(roots):
    result={}
    for value in roots:
        root=Path(value);assert root.is_dir() and not root.is_symlink()
        for path in sorted(root.rglob('*')):
            assert not path.is_symlink()
            if not path.is_file():assert path.is_dir();continue
            s=path.stat();assert stat.S_ISREG(s.st_mode) and s.st_nlink==1
            key=root.name+'/'+str(path.relative_to(root));assert key not in result
            result[key]=dict(path=str(path),size=s.st_size,sha256=sha(path),
                identity=[s.st_dev,s.st_ino,s.st_mode,s.st_nlink,s.st_mtime_ns,s.st_ctime_ns])
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--config',type=Path,required=True)
    a=p.parse_args();config=json.loads(a.config.read_text());archive=Path(config['archive']);control=Path(config['control'])
    assert not archive.exists() and not control.exists();control.mkdir()
    before=collect(config['roots']);assert before
    files={k:dict(size=v['size'],sha256=v['sha256']) for k,v in before.items()}
    manifest=dict(schema='loop-real-native-complete-evidence-v1',files=files,payload_count=len(files),
        logical_bytes=sum(v['size'] for v in files.values()),roots=config['roots'],all_regular=True,no_links=True,
        external_dependencies=config['external_dependencies'],no_more_writes=True)
    encoded=(json.dumps(manifest,sort_keys=True,indent=2)+'\n').encode()
    (control/'MANIFEST.json').write_bytes(encoded)
    (control/'SOURCE-IDENTITY.json').write_text(json.dumps(before,sort_keys=True,indent=2)+'\n')
    with tarfile.open(archive,'w:gz',compresslevel=3,dereference=True,format=tarfile.PAX_FORMAT) as tar:
        info=tarfile.TarInfo('MANIFEST.json');info.size=len(encoded);tar.addfile(info,io.BytesIO(encoded))
        for key,row in before.items():tar.add(row['path'],arcname=key,recursive=False)
    after=collect(config['roots']);assert before==after,'source set/stat/SHA changed during pack'
    result=dict(archive=str(archive),archive_sha256=sha(archive),archive_bytes=archive.stat().st_size,
        payload_count=len(files),logical_bytes=manifest['logical_bytes'],manifest_sha256=sha(control/'MANIFEST.json'),
        source_identity_file_sha256=sha(control/'SOURCE-IDENTITY.json'),source_set_stat_sha_unchanged=True,
        no_links=True,no_more_writes=True,sealer_sha256=sha(Path(__file__)),config_sha256=sha(a.config))
    (control/'PACK-RESULT.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
