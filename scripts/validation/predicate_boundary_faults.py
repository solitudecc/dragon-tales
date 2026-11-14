"""Two bounded semantic faults for the frozen predicate-boundary arena oracle.

emit only writes small mutated IR/metadata. run verifies IR and executes exactly
two faults x two host optimization levels x32 original inputs. audit only reads
saved frames. None of these modes invokes a Dragon pass or changes the frozen
producer, normal oracle, baseline, specification, inputs or comparison fields.
"""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import traceback

FRAME=66112
CASE='32-left-positive'
FAULTS=('swapped-select','extra-observer')
LEVELS=('O0','O2')
AUDIT_SHA='10f4cfeba6a253d96f44f055dda3e527e288160b9cdd301f5602d321ad96f2be'
LIMITS=dict(start_free=512*1024**2,running_free=64*1024**2,subprocess_seconds=180)


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def write(path,value):
    path.write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')


def pinned(record):
    path=Path(record['path']);assert path.is_absolute() and path.is_file() and not path.is_symlink()
    assert sha(path)==record['sha256'],str(path)
    return path


def pin(path):
    path=path.resolve();return dict(path=str(path),sha256=sha(path))


def space(path,minimum):
    path=path.resolve()
    while not path.exists():path=path.parent
    free=min(shutil.disk_usage(path).free,shutil.disk_usage('/dev/shm').free)
    assert free>=minimum,(free,minimum)
    return free


def mutations(before):
    needle='select i1 %predicate, i32 7, i32 98'
    assert before.count(needle)==1 and before.count('select i1 %predicate,')==1
    swapped=before.replace(needle,'select i1 %predicate, i32 98, i32 7',1)
    assert swapped.replace('select i1 %predicate, i32 98, i32 7',needle,1)==before
    frame=re.findall(r'^define void @probe\(ptr (%[^ )]+)\) \{$',before,re.M)
    assert len(frame)==1
    calls=list(re.finditer(r'^  call void @observer\(ptr '+re.escape(frame[0])+r', i64 [^)]+\)$',before,re.M))
    assert len(calls)==2 and calls[0].start()<before.index('\nhistory:')
    extra='  call void @observer(ptr '+frame[0]+', i64 1)\n'
    assert extra not in before
    inserted=before[:calls[0].start()]+extra+before[calls[0].start():]
    assert inserted.replace(extra,'',1)==before
    assert len(re.findall(r'^  call void @observer\(',inserted,re.M))==3
    return dict(zip(FAULTS,(swapped,inserted)))


def load_references(baseline,audit_path):
    assert sha(audit_path)==AUDIT_SHA,'use the frozen independent before calibration'
    audit=json.loads(audit_path.read_bytes())
    assert audit['passed'] and audit['before_native_calibration'] and audit['frames']==1152
    pins=audit['input_sha256']
    required=[baseline/'results.json',baseline/'frames.xz',baseline/'wrapper.c',
              baseline/CASE/'before.ll',baseline/CASE/'after.ll']
    for path in required:assert sha(path)==pins[str(path)]
    assert required[-1].read_bytes()==required[-2].read_bytes()
    saved=json.loads(required[0].read_bytes());assert saved['summary']['established'] and saved['summary']['baseline']
    assert saved['summary']['native_matches']==1152 and saved['summary']['control_detections']==14976
    references={}
    # Whole compressed-file SHA binds the prior audited CRC/EOF and all1152
    # frames. Only this fixture's first64 records are needed; no arena copies.
    with lzma.open(baseline/'frames.xz','rb') as stream:
        for index in range(64):
            prefix=stream.read(4);assert len(prefix)==4
            length,=struct.unpack('<I',prefix);assert 0<length<=65536
            header=stream.read(length);assert len(header)==length
            row=json.loads(header);assert row==saved['results'][index]
            assert header==json.dumps(row,sort_keys=True).encode()
            expected_level=LEVELS[index//32];trial=index%32
            assert (row['name'],row['opt'],row['trial'])==(CASE,expected_level,trial)
            source,expected,actual=[stream.read(FRAME) for _ in range(3)]
            assert len(source)==len(expected)==len(actual)==FRAME
            assert expected==actual and row['match'] and row['controls']==13
            assert row['frame_sha256']=={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',expected),('actual',actual)]}
            assert int.from_bytes(expected[64:68],'little')==21
            if trial in references:assert references[trial]==(source,expected)
            else:references[trial]=(source,expected)
    assert set(references)==set(range(32))
    assert len({hashlib.sha256(v[0]).hexdigest() for v in references.values()})==32
    return references,{str(path):sha(path) for path in required}|{str(audit_path):sha(audit_path)}


def fault_model(expected,fault):
    result=bytearray(expected)
    if fault=='swapped-select':
        assert int.from_bytes(expected[64:68],'little')==21
        result[64:68]=(3*98).to_bytes(4,'little')
    else:
        assert fault=='extra-observer'
        counter=(int.from_bytes(expected[256:264],'little')+1)%(2**64)
        result[256:264]=counter.to_bytes(8,'little')
        result[8704]^=1 # arena512 + Guest[8192], low byte of the u64 observer cell
    return bytes(result)


def assess(expected,actual,fault):
    assert len(expected)==len(actual)==FRAME
    wanted=fault_model(expected,fault)
    offsets=[i for i,(a,b) in enumerate(zip(expected,actual)) if a!=b]
    specified=[i for i,(a,b) in enumerate(zip(expected,wanted)) if a!=b]
    detected=actual!=expected and actual==wanted and offsets==specified
    if fault=='swapped-select':
        detected &= offsets==[64,65] and int.from_bytes(actual[64:68],'little')==294
    else:
        detected &= 8704 in offsets and any(256<=i<264 for i in offsets)
        detected &= set(offsets).issubset(set(range(256,264))|{8704})
    return dict(original_expected_match=actual==expected,specified_detected=bool(detected),difference_offsets=offsets,
                fault_model_full_frame_equal=actual==wanted,compared_bytes=FRAME)


def emit(args):
    baseline,audit=args.baseline.resolve(),args.baseline_audit.resolve()
    out=args.out.resolve();assert not out.exists() and not out.is_relative_to(baseline)
    references,pins=load_references(baseline,audit)
    before=(baseline/CASE/'before.ll').read_text();products=mutations(before)
    out.mkdir(parents=True)
    (out/'before.ll').write_bytes((baseline/CASE/'before.ll').read_bytes())
    (out/'wrapper.c').write_bytes((baseline/'wrapper.c').read_bytes())
    for name,text in products.items():(out/(name+'.ll')).write_text(text)
    manifest=dict(schema='predicate-boundary-semantic-faults-emitted-v1',baseline=str(baseline),baseline_audit=pin(audit),
        reference_pins=pins,files={p.name:sha(p) for p in out.iterdir()},limits=LIMITS,
        case=CASE,inputs=32,host_levels=LEVELS,faults=FAULTS,planned_fault_observations=128,
        expected_accumulator=21,swapped_accumulator=294,extra_counter_delta=1,extra_guest_byte_xor=dict(guest_offset=8192,frame_offset=8704,xor=1),
        mutation_checks=dict(unique_select_matches=1,select_arm_swaps=1,original_observer_calls=2,
            extra_observer_insertions=1,extra_observer_mutant_calls=3,all_other_IR_bytes_unchanged=True),
        references={str(i):dict(input_sha256=hashlib.sha256(v[0]).hexdigest(),expected_sha256=hashlib.sha256(v[1]).hexdigest()) for i,v in references.items()},
        llvm_verify_pending=True,target_executions=0,product_pass_runs=0,specification_or_input_changes=0,
        emitter_sha256=sha(__file__))
    assert pins=={p:sha(p) for p in pins}
    write(out/'EMITTED.json',manifest)
    print('EMITTED_SHA '+sha(out/'EMITTED.json'))


def prepared(path):
    path=path.resolve();manifest=json.loads((path/'EMITTED.json').read_bytes())
    assert manifest['schema']=='predicate-boundary-semantic-faults-emitted-v1' and manifest['limits']==LIMITS
    assert manifest['emitter_sha256']==sha(__file__),'source changed after emit: use a new namespace'
    assert manifest['case']==CASE and manifest['inputs']==32 and manifest['planned_fault_observations']==128
    assert set(manifest['files'])=={'before.ll','wrapper.c',*(name+'.ll' for name in FAULTS)}
    assert tuple(manifest['faults'])==FAULTS and tuple(manifest['host_levels'])==LEVELS
    for name,digest in manifest['files'].items():assert sha(path/name)==digest
    references,pins=load_references(Path(manifest['baseline']),pinned(manifest['baseline_audit']))
    assert pins==manifest['reference_pins']
    assert sha(path/'before.ll')==pins[str(Path(manifest['baseline'])/CASE/'before.ll')]
    assert sha(path/'wrapper.c')==pins[str(Path(manifest['baseline'])/'wrapper.c')]
    wanted=mutations((path/'before.ll').read_text())
    for name,text in wanted.items():assert (path/(name+'.ll')).read_text()==text
    for i,(source,expected) in references.items():
        assert manifest['references'][str(i)]==dict(input_sha256=hashlib.sha256(source).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest())
    return manifest,references,pins


def run(args):
    assert sys.platform=='linux','native execution is VM-only'
    src,out=args.prepared.resolve(),args.out.resolve()
    assert not out.exists() and not out.is_relative_to(src) and not src.is_relative_to(out)
    minimum=space(out,LIMITS['start_free']);manifest,references,reference_pins=prepared(src)
    tools={}
    for name in ('opt','llc','cc'):
        path=shutil.which(str(getattr(args,name)));assert path
        tools[name]=Path(path).resolve()
    tool_pins={str(p):sha(p) for p in tools.values()}
    source_pins={str(p):sha(p) for p in [src/'EMITTED.json',src/'before.ll',src/'wrapper.c',*(src/(name+'.ll') for name in FAULTS),Path(__file__)]}
    out.mkdir(parents=True);commands=[];rows=[];failures=[];submitted=0;verified=0;complete_native_frames=0
    invocation=dict(prepared=str(src),prepared_sha256=sha(src/'EMITTED.json'),tools={k:pin(v) for k,v in tools.items()},
                    source_pins=source_pins,reference_pins=reference_pins,limits=LIMITS,initial_free=minimum)
    write(out/'INVOCATION.json',invocation)
    def command(argv,data=None,native_batch=False):
        nonlocal complete_native_frames
        space(out,LIMITS['running_free']);start=time.monotonic()
        proc=subprocess.run([str(v) for v in argv],input=data,capture_output=True,timeout=180)
        commands.append(dict(argv=[str(v) for v in argv],exit=proc.returncode,seconds=time.monotonic()-start,
            stdout_bytes=len(proc.stdout),stdout_sha256=hashlib.sha256(proc.stdout).hexdigest(),stderr=proc.stderr.decode(errors='replace')))
        write(out/'COMMANDS.json',commands)
        if native_batch:complete_native_frames+=len(proc.stdout)//FRAME
        if proc.returncode:
            (out/f'FAILED-stdout-{len(commands)}.bin').write_bytes(proc.stdout)
            raise RuntimeError(commands[-1])
        return proc.stdout
    try:
        command([tools['opt'],'-passes=verify','-disable-output',src/'before.ll'])
        verified+=1
        with lzma.open(out/'frames.xz','xb',preset=6) as stream:
            for fault in FAULTS:
                command([tools['opt'],'-passes=verify','-disable-output',src/(fault+'.ll')])
                verified+=1
                work=out/fault;work.mkdir()
                shutil.copyfile(src/(fault+'.ll'),work/'mutant.ll')
                shutil.copyfile(src/'wrapper.c',work/'wrapper.c')
                for level in LEVELS:
                    obj,exe=work/(level+'.o'),work/(level+'.native')
                    command([tools['llc'],'-filetype=obj','-'+level,work/'mutant.ll','-o',obj])
                    command([tools['cc'],'-'+level,work/'wrapper.c',obj,'-o',exe])
                    submitted+=32
                    native=command([exe],b''.join(references[i][0] for i in range(32)),native_batch=True)
                    if len(native)!=32*FRAME:
                        (work/(level+'-BAD-stdout.bin')).write_bytes(native)
                        raise RuntimeError(('native stream length',fault,level,len(native)))
                    for trial in range(32):
                        source,expected=references[trial];actual=native[trial*FRAME:(trial+1)*FRAME]
                        row=dict(fault=fault,opt=level,trial=trial,**assess(expected,actual,fault),
                            frame_sha256={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',expected),('actual',actual)]})
                        header=json.dumps(row,sort_keys=True).encode();stream.write(struct.pack('<I',len(header))+header+source+expected+actual)
                        rows.append(row)
                    # Every returned frame is retained before rejecting a bad mutant result.
                    if not all(r['specified_detected'] and not r['original_expected_match'] for r in rows):
                        raise RuntimeError('a fault was not the specified full-state mismatch')
        assert len(rows)==submitted==128
        assert source_pins=={p:sha(p) for p in source_pins} and tool_pins=={p:sha(p) for p in tool_pins}
        assert reference_pins=={p:sha(p) for p in reference_pins}
    except BaseException as error:
        failures.append(dict(type=type(error).__name__,detail=str(error),traceback=traceback.format_exc()));raise
    finally:
        write(out/'RESULTS.json',dict(schema='predicate-boundary-semantic-faults-run-v1',established=not failures and len(rows)==128,
            planned_fault_observations=128,submitted_fault_frames=submitted,fault_observations=len(rows),
            complete_native_frames_observed=complete_native_frames,llvm_verified_ir_count=verified,
            specified_detections=sum(r['specified_detected'] for r in rows),original_expected_matches=sum(r['original_expected_match'] for r in rows),
            normal_matches_claimed=0,comparator_only_controls_claimed=0,
            unique_fault_input_domain=len({(r['fault'],r['trial']) for r in rows}),
            unique_initial_inputs=len({r['frame_sha256']['input'] for r in rows}),planned_unique_fault_input_domain=64,
            limits=LIMITS,skips=0,exclusions=0,normalizations=0,product_pass_runs=0,Unicorn_runs=0,
            invocation_sha256=sha(out/'INVOCATION.json'),frames_sha256=sha(out/'frames.xz') if (out/'frames.xz').exists() else None,
            failures=failures,results=rows,scope='Actual deliberately wrong native executions compared with the unchanged full66112 Python expected. Fault-model equality is not a normal match.'))


def audit(args):
    root,out=args.root.resolve(),args.out.resolve();assert not out.exists() and not out.is_relative_to(root)
    saved=json.loads((root/'RESULTS.json').read_bytes());inv=json.loads((root/'INVOCATION.json').read_bytes())
    assert saved['established'] and saved['fault_observations']==saved['specified_detections']==128
    assert saved['original_expected_matches']==saved['normal_matches_claimed']==0 and not saved['failures']
    src=Path(inv['prepared']);assert sha(src/'EMITTED.json')==inv['prepared_sha256']
    _,references,pins=prepared(src);assert pins==inv['reference_pins']
    paths=[root/'RESULTS.json',root/'INVOCATION.json',root/'frames.xz',Path(__file__)]
    before={str(p):sha(p) for p in paths};assert before[str(root/'frames.xz')]==saved['frames_sha256']
    count=0
    with lzma.open(root/'frames.xz','rb') as stream:
        for fault in FAULTS:
            for level in LEVELS:
                for trial in range(32):
                    prefix=stream.read(4);assert len(prefix)==4
                    size,=struct.unpack('<I',prefix);assert 0<size<=65536
                    header=stream.read(size);assert len(header)==size
                    row=json.loads(header);assert row==saved['results'][count]
                    assert header==json.dumps(row,sort_keys=True).encode() and (row['fault'],row['opt'],row['trial'])==(fault,level,trial)
                    source,expected,actual=[stream.read(FRAME) for _ in range(3)]
                    assert (source,expected)==references[trial] and len(actual)==FRAME
                    assert row['frame_sha256']=={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',expected),('actual',actual)]}
                    found=assess(expected,actual,fault)
                    assert all(row[k]==v for k,v in found.items()) and found['specified_detected'] and not found['original_expected_match']
                    count+=1
        assert stream.read(1)==b''
    # Explicit single-stream XZ CRC/EOF check also rejects trailing garbage.
    decoder=lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    with (root/'frames.xz').open('rb') as stream:
        while not decoder.eof:
            raw=stream.read(1<<20) if decoder.needs_input else b''
            assert raw or not decoder.needs_input,'truncated XZ'
            decoder.decompress(raw,max_length=1<<20)
        assert not decoder.unused_data and not stream.read(1)
    assert before=={str(p):sha(p) for p in paths} and pins=={p:sha(p) for p in pins}
    out.mkdir(parents=True)
    write(out/'REPORT.json',dict(passed=True,observations=count,specified_detections=count,original_expected_matches=0,
        normal_matches_claimed=0,new_native_executions=0,unique_fault_input_domain=64,unique_initial_inputs=32,
        XZ_crc_eof=True,input_sha256=before,baseline_reference_sha256=pins,skips=0,exclusions=0,normalizations=0,
        scope='Saved-only audit of deliberately wrong native frames; original Python expected bytes are unchanged.'))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);sub=parser.add_subparsers(dest='mode',required=True)
    p=sub.add_parser('emit');p.add_argument('--baseline',type=Path,required=True);p.add_argument('--baseline-audit',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p=sub.add_parser('run');p.add_argument('--prepared',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    for name,default in [('opt','opt-21'),('llc','llc-21'),('cc','gcc')]:p.add_argument('--'+name,default=default)
    p=sub.add_parser('audit');p.add_argument('--root',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();{'emit':emit,'run':run,'audit':audit}[args.mode](args)
