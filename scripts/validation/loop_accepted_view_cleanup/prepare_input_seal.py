from pathlib import Path
import hashlib,json,stat,shutil
BASE=Path('/home/ubuntu/vmc-runs');PUB=BASE/'loop-image-native-public-inputs-20261115-v1';PREP=BASE/'loop-accepted-view-cleanup-preparation-20261115-v1';OUT=BASE/'loop-accepted-view-cleanup-inputs-20261115-v1'
def h(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def pin(p):
 s=p.lstat();assert stat.S_ISREG(s.st_mode) and s.st_nlink==1
 return {'path':str(p),'size':s.st_size,'sha256':h(p),'dev':s.st_dev,'ino':s.st_ino,'nlink':s.st_nlink,'mtime_ns':s.st_mtime_ns,'ctime_ns':s.st_ctime_ns}
def put(n,v):
 with (OUT/n).open('x') as f:json.dump(v,f,indent=2,sort_keys=True);f.write('\n')
ledger=json.loads((PUB/'TRANSFER-LEDGER.json').read_text());assert h(PUB/'TRANSFER-LEDGER.json')=='8674465843ef192335054e7220914e980874f1d4fa659baaecc60631fd032480';assert ledger['ready']
idx={v['relative_path']:v for v in ledger['files']};roles=ledger['roles']
select={'source/production-B.ll':PUB/roles['main_after'],'producer/REPORT.json':PUB/roles['main_report'],'producer/POST-GATES.json':PUB/roles['main_post_gates'],'producer/IDENTITY.json':PUB/roles['main_identity_proof'],'producer/manifest.json':PUB/'production/main/B-SP-table-RSI.manifest.json','provenance/public-transfer-ledger.json':PUB/'TRANSFER-LEDGER.json','provenance/public-transfer-status.json':PUB/'SOURCE-CLOSURE.json','emitter/loop_accepted_view_cleanup.cpp':PREP/'source/loop_accepted_view_cleanup.cpp'}
for p in select.values():
 if p.is_relative_to(PUB) and str(p.relative_to(PUB)) in idx:
  exp=idx[str(p.relative_to(PUB))];assert h(p)==exp['sha256'] and p.stat().st_size==exp['size']
assert h(select['source/production-B.ll'])=='714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74';assert h(select['producer/REPORT.json'])=='aebcd589202cced2d99411fc6524648ca7bceef10d4e9a0bb83ba5e718b0b1c0';assert h(select['emitter/loop_accepted_view_cleanup.cpp'])=='0ffa1459a03ad2526b02dd1538326a6f7974895dde469d8c47cb9b061bc947b2'
before={n:pin(p) for n,p in select.items()};OUT.mkdir()
for n,p in select.items():
 q=OUT/n;q.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,q);assert h(q)==before[n]['sha256']
assert {n:pin(p) for n,p in select.items()}==before
put('SOURCE-BINDINGS.json',{'schema':'cleanup-exact-source-selection-bindings-v1','source_before':before,'source_after':before,'public_ledger_sha256':h(PUB/'TRANSFER-LEDGER.json'),'FileSet_provenance_is_not_proof':True,'native':0,'pass_runs':0})
put('STATUS.json',{'schema':'accepted-view-cleanup-source-status-v1','no_more_writes':True,'ready':True,'selected_case':'B-SP-table-RSI','source_verified_closed':True,'source_budget_exhausted':False,'source_sha256':before['source/production-B.ll']['sha256'],'closed_report_sha256':before['producer/REPORT.json']['sha256'],'only_file_preparation':True,'new_cleanup_passes':0,'new_Guest_runs':0})
files=[{'path':str(p.relative_to(OUT)),'size':p.stat().st_size,'sha256':h(p)} for p in sorted(OUT.rglob('*')) if p.is_file()]
put('FILESET.json',{'schema':'exact-fileset-v1','files':files,'payload_count':len(files),'payload_bytes':sum(v['size'] for v in files),'excluded_payloads':['FILESET.json'],'no_more_writes':True})
print('seal',OUT);print('fileset_sha256',h(OUT/'FILESET.json'));print('status_sha256',h(OUT/'STATUS.json'));print('payload',len(files),sum(v['size'] for v in files))
