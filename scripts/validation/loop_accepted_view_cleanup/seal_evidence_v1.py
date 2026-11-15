from pathlib import Path
import json,hashlib,stat,shutil
BASE=Path('/home/ubuntu/vmc-runs');R=BASE/'loop-accepted-view-cleanup-input-evidence-20261115-v1'
roots=[BASE/'loop-accepted-view-cleanup-inputs-20261115-v1']+[BASE/f'loop-accepted-view-cleanup-preparation-20261115-v{i}' for i in [1,2,3]]
def h(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def pin(p):
 s=p.lstat();assert stat.S_ISREG(s.st_mode) and s.st_nlink==1,str(p)
 return {'size':s.st_size,'sha256':h(p),'dev':s.st_dev,'ino':s.st_ino,'nlink':s.st_nlink,'mode':s.st_mode,'mtime_ns':s.st_mtime_ns,'ctime_ns':s.st_ctime_ns}
def put(n,v):
 p=R/n;p.parent.mkdir(parents=True,exist_ok=True)
 with p.open('x') as f:json.dump(v,f,indent=2,sort_keys=True);f.write('\n')
assert not (R/'FILESET.json').exists();assert {p.name for p in R.iterdir()}=={'replay-source'}
selections={};sets={}
for root in roots:
 assert not any(p.is_symlink() for p in root.rglob('*'))
 names={str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()};dirs=sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_dir());sets[str(root)]={'files':sorted(names),'directories':dirs}
 for n in sorted(names):selections[f'evidence/{root.name}/{n}']={'source':root/n,'stamp':pin(root/n)}
for n,record in selections.items():
 d=R/n;d.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(record['source'],d);assert pin(d)['sha256']==record['stamp']['sha256']
assert all(pin(v['source'])==v['stamp'] for v in selections.values())
for root in roots:assert sets[str(root)]['files']==sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_file())
for n,root in enumerate(roots[1:],1):
 q=json.loads((root/'RESULT.json').read_text());assert q['compile']['exit']==0
 if n<3:assert not q['passed'] and q['inventory']['exit']==1 and not (root/'LOOKUPS.json').exists()
 else:assert q['passed'] and q['inventory']['exit']==0
last=roots[-1];inventory=json.loads((last/'LOOKUPS.json').read_text());assert inventory['simple_i64_LOAD_count']==419 and inventory['all_i64_LOAD_count']==419 and inventory['all_integer_LOAD_count']==459 and inventory['non_simple_i64_LOAD_count']==0
assert 'INVENTORY COUNTS unused=69 guard=80 total=149 constant_work=702' in (roots[2]/'inventory.stderr').read_text()
assert h(last/'ACTUAL-B-PLAN.json')=='c0fff647988c3613d5a918a5f497128c5cf8439e1ba9f83012785129193080c1'
(R/'control').mkdir();shutil.copyfile(Path(__file__),R/'control/seal_evidence.py')
put('SOURCE-BINDINGS.json',{'schema':'cleanup-input-inventory-source-snapshot-v1','original_roots':sets,'files':[{'relative_path':n,'source_path':str(v['source']),'before':v['stamp'],'after':v['stamp']} for n,v in sorted(selections.items())],'all_source_stat_ctime_SHA_before_after_equal':True,'original_frozen_inputs_or_outputs_not_modified':True,'new_target_calls':0,'new_Guest_calls':0})
refs={'schema':'exact-existing-SDK-archive-references-v1','ROCK_archive':'/home/ubuntu/vmc-runs/loop-image-final-branch-sdk-archive-20261115-v1/loop-image-final-six-products-and-25-sources.tar.gz','Mac_archive':'/Users/c/Desktop/deob/validation-artifacts/loop-image-final-branch-sdk-archive-20261115-v1/loop-image-final-six-products-and-25-sources.tar.gz','archive_size':335829226,'archive_sha256':'5fc54ef2f322b4ff71075cbd46da4a1d369c822c64a1dd5eeb9bf04352f6c529','dual_certificate':'/home/ubuntu/vmc-runs/loop-image-final-branch-sdk-archive-control-20261115-v1/DUAL-HOST-CERTIFICATE.json','dual_certificate_sha256':'69698410b21423439ff2db47454b68ee0f61a6e48d20bfe9713165be4be3d46a','SDK_manifest_sha256':'fdd8ddddbe64684ccaedb5f09eb1179b0183273dfc5b0efd49618c58c76ae932','large_libraries_or_testbins_repacked':False}
assert h(Path(refs['dual_certificate']))==refs['dual_certificate_sha256'];put('EXTERNAL-REFERENCES.json',refs)
summary={'schema':'cleanup-input-and-three-readonly-inventory-versions-summary-v1','frozen':True,'no_more_writes':True,'v1':{'compile_exit':0,'inventory_exit':1,'strict73_gate_retained':True,'actual_count_not_persisted':True},'v2':{'compile_exit':0,'inventory_exit':1,'strict73_gate_retained':True,'constant_address_unused_candidates':69,'entry_literal_equality_guard_reads':80,'classified_reads':149,'constant_work':702,'missing_four_not_guessed':True},'v3':{'compile_exit':0,'inventory_exit':0,'schema':inventory['schema'],'all_integer_LOADs':459,'all_i64_LOADs':419,'all_original_simple_i64_LOADs':419,'non_simple_i64_LOADs':0,'structural_superset_not_exact_target_count':True,'unchanged_emitter_512_cap':True,'source_module_unchanged':True,'new_numerical_facts':False},'source_sha256':'714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74','closed_report_sha256':'aebcd589202cced2d99411fc6524648ca7bceef10d4e9a0bb83ba5e718b0b1c0','emitter_source_sha256':'0ffa1459a03ad2526b02dd1538326a6f7974895dde469d8c47cb9b061bc947b2','actual_B_plan_sha256':h(last/'ACTUAL-B-PLAN.json'),'lookup_inventory_sha256':h(last/'LOOKUPS.json'),'actual_cleanup_output_separately_owned_not_included':True,'new_target_calls_during_sealing':0,'cleanup_pass_runs_by_this_owner':0,'Guest_native_runs':0,'deletions':0}
put('SUMMARY.json',summary)
(R/'README.md').write_text((R/'replay-source/README.md').read_text()+'\n本证据包完整保留原input seal和v1/v2/v3各文件集，包括源、编译和parse日志、loader、失败结果、v3全库存、实际计划及其全部输入pin。original source/output不变；actual cleanup输出由store单独收口，不在此包。封存不新增编译、pass或Guest。\n')
assert not any(p.is_symlink() for p in R.rglob('*'))
files=[{'path':str(p.relative_to(R)),'size':p.stat().st_size,'sha256':h(p)} for p in sorted(R.rglob('*')) if p.is_file()]
put('FILESET.json',{'schema':'exact-fileset-v1','files':files,'payload_count':len(files),'payload_bytes':sum(x['size'] for x in files),'directories':sorted(str(p.relative_to(R)) for p in R.rglob('*') if p.is_dir()),'excluded_payloads':['FILESET.json','SOURCE-CLOSURE.json'],'no_more_writes':True})
put('SOURCE-CLOSURE.json',{'schema':'cleanup-inventory-evidence-closure-v1','frozen':True,'no_more_writes':True,'fileset_sha256':h(R/'FILESET.json'),'summary_sha256':h(R/'SUMMARY.json'),'source_bindings_sha256':h(R/'SOURCE-BINDINGS.json'),'payload_files':len(files),'payload_bytes':sum(x['size'] for x in files),'source_before_after_stat_ctime_SHA_equal':True,'new_target_or_Guest_calls':0})
assert all(pin(v['source'])==v['stamp'] for v in selections.values())
print('root',R);print('payload_files',len(files),'bytes',sum(x['size'] for x in files));print('fileset_sha256',h(R/'FILESET.json'));print('closure_sha256',h(R/'SOURCE-CLOSURE.json'));print('summary_sha256',h(R/'SUMMARY.json'))
