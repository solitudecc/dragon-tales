"""Portable saved-input validation for the separately authorized Loop after replay.

No compile, pass, reference or Guest execution occurs on import. Full original
oracle modules are imported only by the ready replay worker, under explicit
runtime/reference environment pins. No legacy path text is rewritten.
"""
import collections
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import sys

sys.dont_write_bytecode = True
LIMITS = dict(guest_steps=1000000, subprocess_seconds=180,
              start_free=536870912, running_free=67108864)
AFTER_SHA = '714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74'
DRIVER_SHA = 'e0fb71e08b117ddb6c929dc60ff66af938073695ced0fe68b6f50aa9c69c75bb'
BEFORE_SUPPORT_SHA = '8c283ece2b49114d61d96a08b356b37846d6572326c50c4138664d366ee7f7a9'
ROOT_EXIT = (1, 3, 4111, 61440, 1, 0, 0, 0)
VALUES = [0,1,2,7,127,128,32767,32768,2147483647,2147483648,
          4294967294,4294967295,8,15,16,31]

def require(value, reason):
    if not value:
        raise RuntimeError(reason)

def sha(path):
    result=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1048576),b''):
            result.update(block)
    return result.hexdigest()

def pin(path):
    path=Path(path)
    return dict(path=str(path),size=path.stat().st_size,sha256=sha(path))

def pinned(record, pins, symlink=False):
    path=Path(record['path'])
    require(path.is_absolute() and path.is_file() and (symlink or not path.is_symlink()),'unsafe input '+str(path))
    require(sha(path)==record['sha256'],'pin changed '+str(path))
    if 'size' in record:
        require(path.stat().st_size==record['size'],'size changed '+str(path))
    pins[str(path)]=dict(sha256=record['sha256'],resolved=str(path.resolve()))
    return path

def recheck(pins):
    return all(Path(name).is_file() and sha(name)==value['sha256'] and
               str(Path(name).resolve())==value['resolved'] for name,value in pins.items())

def write(path, value):
    Path(path).write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')

def load_module(path, name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def transfer(root, ledger_sha256, pins):
    root=Path(root).resolve()
    ledger=pinned(dict(path=str(root/'TRANSFER-LEDGER.json'),sha256=ledger_sha256),pins)
    data=json.loads(ledger.read_text())
    require(data['schema']=='loop-image-native-public-transfer-v1' and data['ready'] is True and
            data['no_more_writes'] is True,'transfer not frozen/ready')
    payload={}
    for row in data['files']:
        rel=PurePosixPath(row['relative_path'])
        require(not rel.is_absolute() and rel.parts and '..' not in rel.parts and
                str(rel)==row['relative_path'] and str(rel) not in payload,'unsafe/duplicate transfer member')
        path=root.joinpath(*rel.parts)
        require(path.resolve().is_relative_to(root),'transfer path escapes root')
        pinned(dict(path=str(path),size=row['size'],sha256=row['sha256']),pins)
        payload[str(rel)]=path
    require(len(payload)==data['file_count'] and sum(p.stat().st_size for p in payload.values())==data['logical_bytes'],
            'transfer complete set/size differs')
    actual={str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()}
    require(actual==set(payload)|{'TRANSFER-LEDGER.json','SOURCE-CLOSURE.json'},'transfer file set differs')
    require(all(not p.is_symlink() for p in root.rglob('*')),'transfer symlink')
    pinned(pin(root/'SOURCE-CLOSURE.json'),pins)
    roles={}
    for name,value in data['roles'].items():
        if isinstance(value,list):
            require(all(x in payload for x in value),'role missing from payload '+name)
            roles[name]=[payload[x] for x in value]
        else:
            require(value in payload,'role missing from payload '+name)
            roles[name]=payload[value]
    require(len(roles['initial_frames'])==len(roles['expected_frames'])==16,'original16 domain differs')
    require(sha(roles['main_after'])==sha(roles['NoZ3_after'])==AFTER_SHA and
            roles['main_after'].read_bytes()==roles['NoZ3_after'].read_bytes(),'common production IR differs')
    require(sha(roles['driver'])==DRIVER_SHA,'driver differs')
    for prefix in ['main','NoZ3']:
        report=json.loads(roles[prefix+'_report'].read_text())
        require(report['ok'] and report['source_ordinary_exact'] and not report['budget_changed'] and
                report['known_memory_assertions']==0 and report['native_C_UC_runs']==0,'production report provenance')
        stages=[x for x in report['stages'] if x['case']=='B-SP-table-RSI']
        require(len(stages)==1,'B stage missing/ambiguous')
        stage=stages[0]
        require(stage['closed'] and stage['changed'] and stage['verified'] and not stage['budget_exhausted'] and
                stage['guarded_RSI'] and stage['guard_miss_calls_ordinary_root'] and
                stage['ordinary_bodies_and_identity_unchanged'] and stage['after_sha256']==AFTER_SHA and
                stage['work']<=1000000,'B stage did not close under original caps')
        require(report['raw_sha256']==sha(roles['canonical_before']),'wrong original raw/calibration lineage')
        gates=json.loads(roles[prefix+'_post_gates'].read_text())
        require(gates['all_post_gates_passed'] and gates['eligible'] and gates['validation_status']=='complete' and
                gates['old_strict_Function_set_gate_separately_retained'],'production post-gate failed')
        identity=json.loads(roles[prefix+'_identity_proof'].read_text())
        for scope in ['source_identity','view_identity']:
            proof=identity[scope]
            require(all(proof[key] for key in ['all_globals_identity_and_text_preserved',
                'all_named_metadata_identity_graph_preserved','all_nonview_Functions_exact_text',
                'all_original_Function_Argument_objects_preserved','successful_closed_changed_required_for_import']),
                'ordinary/global/argument/metadata identity gate')
            if scope=='source_identity':
                require(proof['imports']==[] and proof['source_scope_forbids_additions'],'source scope additions')
            else:
                require(len(proof['imports'])==1 and proof['imports'][0]['name']=='llvm.fshl.i32' and
                        proof['imports'][0]['canonical_Intrinsic_getAttributes'],'noncanonical imported declaration')
    before=json.loads(roles['before_results'].read_text())
    summary=json.loads(roles['before_summary'].read_text())
    audit=json.loads(roles['before_saved_audit'].read_text())
    require(before['correct'] and before['complete'] and before['pins_unchanged'] and before['failure'] is None and
            before['native_comparisons']==32 and before['comparator_controls']==96 and
            before['skipped']==before['undefined_exclusions']==before['normalizations']==0,'before calibration incomplete')
    require(summary['correct'] and summary['new_normal_full_matches']==32 and summary['new_comparator_controls']==96 and
            summary['register_fields']==76 and summary['complete_frame_bytes']==66136,'before summary differs')
    require(audit['passed'] and audit['saved_inputs_checked']==audit['saved_outputs_checked']==32 and
            audit['exact_field_controls_rechecked']==96 and audit['result_sha256']==sha(roles['before_results']) and
            audit['register_fields']==76 and audit['raw_bytes_per_output']==66136,'before saved audit failed')
    control=json.loads(roles['checker_control_result'].read_text())
    require(control['passed'] and control['control_count']==14 and control['Image_API_calls']==0,'identity checker controls incomplete')
    identity=json.loads(roles['main_NoZ3_IR_identity'].read_text())
    require(identity['exact_full_bytes_equal'] and identity['sha256']==AFTER_SHA,'backend exact identity gate failed')
    return data,roles

def oracle(roles, plan, pins):
    """Import the complete unchanged original comparator; never invoke UC reference."""
    compat=roles['c_header'].parent.parent
    require(roles['execution_header']==compat/'include/dragon/lifters/llvm/Execution.h','compat header layout')
    folder=compat/'scripts/validation'
    support=folder.parent.parent.parent/'tigress_dispatch_native_before.py'
    require(support.is_file() and sha(support)==BEFORE_SUPPORT_SHA,'original portable support source differs')
    pinned(pin(support),pins)
    helper=load_module(support,'frozen_before_support_for_after')
    for name in ['function_guest_diff','function_dispatch_diff','function_llvm_diff']:
        require(name not in sys.modules,'unexpected prior comparator import '+name)
    sys.path.insert(0,str(folder))
    import function_guest_diff as guest
    base=guest.base
    require(Path(guest.__file__).resolve()==(folder/'function_guest_diff.py').resolve() and
            Path(base.dragon.__file__).resolve()==(compat/'python/dragon.py').resolve(),'comparator/wrapper origin differs')
    require(Path(plan['runtime']['shim']['path']).is_absolute() and
            os.environ.get('DRAGON_C_LIB')==plan['runtime']['shim']['path'] and
            os.environ.get('LD_LIBRARY_PATH')==os.pathsep.join(plan['runtime']['library_path']) and
            not os.environ.get('LD_PRELOAD') and not os.environ.get('LIBUNICORN_PATH'),'runtime environment differs')
    adapter=guest.Adapter('x86_64')
    require(len(adapter.fields)==76 and base.REGS_SIZE==512 and base.MEM_SIZE==65536,'whole-state model differs')
    expected,mutate=helper.comparison_helpers(compat,base)
    return helper,adapter,base,expected,mutate,compat
