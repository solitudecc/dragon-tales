"""Freeze the finished source-inclusion-only budget validation, without archives."""
from pathlib import Path
import hashlib
import json
import os
import stat
import sys

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1048576),b''):h.update(block)
    return h.hexdigest()

def identity(path):
    s=path.stat()
    return dict(device=s.st_dev,inode=s.st_ino,nlink=s.st_nlink,size=s.st_size,
                mtime_ns=s.st_mtime_ns,ctime_ns=s.st_ctime_ns,mode=stat.S_IMODE(s.st_mode))

def main():
    root=Path(sys.argv[1]).resolve()
    assert not (root/'FILESET.json').exists() and not (root/'CLOSURE.json').exists()
    audit=json.loads((root/'SAVED-AUDIT.json').read_text());assert audit['correct']
    ready=json.loads((root/'READY.json').read_text())
    summary=dict(schema='loop-range-budget-source-inclusion-summary-v1',correct=True,
      source_inclusion_actual_LoopRanges=True,installed_Image_execution=False,
      guest_native=0,C=0,Unicorn=0,new_installed_API_execution=0,
      source_variants=ready['sources'],runtime_helpers_sha256=ready['runtime_sha256'],
      raw_results={mode:dict(assertions=data['assertions'],failed_assertions=data['failed_assertions'],
                            failures=data['failures'],compile=data['stages']['compile'],
                            actual_unit=data['stages']['probe']) for mode,data in audit['results'].items()},
      actual_units=3,zero_unit_preparation_failures=1,
      common_nine_raw_IR_fixture_sets_byte_equal=True,
      complete_new_assertions=36,
      new_known_result=dict(range=[40,80],nodes_per_outer_get=121,actual_Work_per_get=199,
                            nodes_per_outer_getForWrite=122),
      new_opaque_result=dict(range=[0,140],nodes_per_outer_get=67,nodes_per_outer_getForWrite=68),
      preserved_controls=['Work198 refusal','paid literal depth33 refusal',
         'distinct equal-valued H_C SSA keys refusal','nonknown repeated SSA refusal',
         'nested STORE-query existing scalar debt not reset',
         'distinct Opaque roots pay full individual footprints despite graph overlap',
         'actual complete IR unchanged and verify valid','fixed helper actual maps/LD init'],
      scope_limitations=['H_C maps are conditional visitor test inputs, not newly proved memory facts',
         'no D parent-child transaction or Strong/Anchor restoration revalidation in this unit',
         'older2a3f cache test counts are not validation of these three newer bodies',
         'old positive downstream gates after a failed result were not executed and are not counted passed',
         'no claim on later430 or any later consume-plan/effect-inventory source',
         'no full real-sample closure or guest semantic comparison claimed'],
      preflight_failure='opaque-probe directory remains empty: attempted before original compile completed; executable-absent rejection,0unit/0API,not product failure or skip',
      ready_sha256=sha(root/'READY.json'),saved_audit_sha256=sha(root/'SAVED-AUDIT.json'),
      frozen=True,no_more_writes=True)
    (root/'SUMMARY.json').write_text(json.dumps(summary,indent=2,sort_keys=True)+'\n')
    paths=sorted(root.rglob('*'))
    assert all(not p.is_symlink() for p in paths)
    directories=[str(p.relative_to(root)) for p in paths if p.is_dir()]
    files=[p for p in paths if p.is_file()]
    before={str(p.relative_to(root)):identity(p) for p in files}
    assert all(v['nlink']==1 for v in before.values())
    payload={rel:dict(size=before[rel]['size'],sha256=sha(root/rel)) for rel in before}
    assert before=={rel:identity(root/rel) for rel in before}
    assert files==sorted(p for p in root.rglob('*') if p.is_file())
    manifest=dict(schema='exact-regular-payload-set-v1',payload_count=len(payload),
                  logical_bytes=sum(x['size'] for x in payload.values()),payload=payload,
                  directories=directories,source_identity_before_equals_after=True,
                  source_identities=before,all_regular=True,no_symlinks=True,no_hardlinks=True)
    (root/'FILESET.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    closure=dict(schema='frozen-validation-closure-v1',frozen=True,no_more_writes=True,
                 payload_count=len(payload),logical_bytes=manifest['logical_bytes'],
                 manifest_sha256=sha(root/'FILESET.json'),summary_sha256=sha(root/'SUMMARY.json'),
                 saved_audit_sha256=sha(root/'SAVED-AUDIT.json'),
                 full_files_count_including_controls=len(payload)+2,
                 control_files=['FILESET.json','CLOSURE.json'],empty_failed_preflight_directory_retained=True)
    (root/'CLOSURE.json').write_text(json.dumps(closure,indent=2,sort_keys=True)+'\n')
    print(json.dumps(dict(root=str(root),summary_sha256=closure['summary_sha256'],
       manifest_sha256=closure['manifest_sha256'],closure_sha256=sha(root/'CLOSURE.json'),
       payload_count=len(payload),logical_bytes=manifest['logical_bytes']),indent=2))

if __name__=='__main__':main()
