"""Staged full-66112-byte corridor validation; every CLI needs the 512/64/180 guard.

Parse-only calibration is independent of the cloner. Both before optimizations
and complete saved audits must pass before prepare-after can invoke it. Sources,
inputs, expectations and two deliberate-fault IRs stay at frozen c241.
"""
import argparse
import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import time

SPEC_SHA='c2416e443dcf6caf96b7ce23fca5aaeeb21bc1c713a399269e59efbcf6945e13'
RAW_PLAN_SHA='19d5f85e0f4af6df0d3ad93ac28e4e0f98ca55964190827994066faea134615e'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def put(path,value):
    with Path(path).open('x') as stream:
        json.dump(value,stream,indent=2)
        stream.write('\n')


def record(path):
    path=Path(path)
    status=path.lstat()
    if not stat.S_ISREG(status.st_mode) or path.is_symlink():
        raise ValueError(('not a regular source',str(path)))
    digest=sha(path)
    after=path.lstat()
    if any(getattr(status,k)!=getattr(after,k) for k in ('st_dev','st_ino','st_size','st_mtime_ns','st_ctime_ns')):
        raise ValueError(('file changed during hash',str(path)))
    return dict(size=status.st_size,sha256=digest)


def files(root):
    return {str(p.relative_to(root)):record(p) for p in sorted(root.rglob('*')) if p.is_file()}


def verify_files(root,expected,manifest_name):
    actual=files(root)
    if set(actual)!=set(expected)|{manifest_name}:
        raise ValueError(('frozen file set changed',str(root)))
    for name,pin in expected.items():
        if actual[name]!=pin:
            raise ValueError(('frozen file mismatch',str(root/name)))
    return actual


def specification():
    path=Path(__file__).with_name('dispatch_corridor_native_source.py')
    if sha(path)!=SPEC_SHA:
        raise ValueError('c241 independent specification changed')
    sys.dont_write_bytecode=True
    spec=importlib.util.spec_from_file_location('corridor_frozen_spec',path)
    model=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(model)
    return model


def raw_binding(root,model):
    if not root.is_absolute() or sha(root/'PLAN.json')!=RAW_PLAN_SHA:
        raise ValueError('raw plan path/hash mismatch')
    plan=json.loads((root/'PLAN.json').read_text())
    if plan['source_generator_sha256']!=SPEC_SHA:
        raise ValueError('raw plan/specification source mismatch')
    pins={}
    for name,pin in plan['files'].items():
        actual=record(root/name)
        if actual!=pin:
            raise ValueError(('raw LLVM/C/fault source changed',name))
        pins[name]=actual
    pins['PLAN.json']=record(root/'PLAN.json')
    if [hashlib.sha256(model.initial(t)).hexdigest() for t in range(16)]!=plan['input_hashes']:
        raise ValueError('input domain changed')
    if (root/'wrapper.c').read_bytes()!=model.WRAPPER.encode() or (root/'reference.c').read_bytes()!=model.C_SPEC.encode():
        raise ValueError('independent C source changed')
    return plan,dict(root=str(root),files=pins,plan_sha256=RAW_PLAN_SHA,specification_sha256=SPEC_SHA)


class Commands:
    def __init__(self,out):
        self.out,self.rows=out,[]

    def run(self,argv,data=None,env=None,stderr_path=None):
        start=time.monotonic()
        value=subprocess.run([str(x) for x in argv],input=data,capture_output=True,env=env,timeout=180)
        if stderr_path is not None:
            Path(stderr_path).write_bytes(value.stderr)
        self.rows.append(dict(argv=[str(x) for x in argv],exit=value.returncode,seconds=time.monotonic()-start,
                              stdout_size=len(value.stdout),stdout_sha256=hashlib.sha256(value.stdout).hexdigest(),
                              stderr=value.stderr.decode(errors='replace')))
        (self.out/'COMMANDS.json').write_text(json.dumps(self.rows,indent=2)+'\n')
        if value.returncode:
            raise RuntimeError(self.rows[-1])
        return value.stdout


def tool_binding(args):
    if not all((args.producer,args.producer_sha256,args.runtime,args.runtime_sha256)):
        raise ValueError('producer/runtime exact pins required')
    if not args.producer.is_absolute() or not args.runtime.is_absolute():
        raise ValueError('producer/runtime must have absolute paths')
    if sha(args.producer)!=args.producer_sha256 or sha(args.runtime)!=args.runtime_sha256:
        raise ValueError('producer/runtime pin mismatch')
    return dict(producer=dict(path=str(args.producer),**record(args.producer)),
                runtime=dict(path=str(args.runtime),**record(args.runtime)))


def producer_call(args,commands,mode,raw,case_file,work):
    work.mkdir(parents=True)
    before,after,report=[work/n for n in ('parsed-before.ll','output.ll','REPORT.json')]
    env=dict(os.environ,LD_LIBRARY_PATH=str(args.runtime.parent),LD_PRELOAD='',LD_DEBUG='libs,bindings',
             PYTHONDONTWRITEBYTECODE='1')
    commands.run([args.producer,mode,raw,case_file,before,after,report],env=env,stderr_path=work/'loader.stderr')
    result=json.loads(report.read_text())
    if (result['mode']!=mode or result['raw_input']!=str(raw) or result['case_manifest']!=str(case_file) or
            not result['verified_before'] or not result['verified_after'] or not result['function_argument_identity']):
        raise ValueError(('producer input/identity/verify contract mismatch',str(work)))
    if not result['runtime_maps'] or any(str(args.runtime) not in line for line in result['runtime_maps']):
        raise ValueError(('unexpected runtime mapping',str(work)))
    if mode=='before':
        if result['cloner_calls']!=0 or not result['compiled_before_only'] or before.read_bytes()!=after.read_bytes():
            raise ValueError('parse-only producer invoked a product or changed IR')
    elif result['cloner_calls']!=1 or result['compiled_before_only']:
        raise ValueError('rewrite producer did not make exactly one private API call')
    return result


def prepare_before(args,model,commands):
    if args.raw is None:
        raise ValueError('prepare-before requires raw source root')
    plan,binding=raw_binding(args.raw,model)
    tools=tool_binding(args)
    defined=commands.run(['nm','-C','--defined-only',args.producer]).decode()
    undefined=commands.run(['nm','-D','-C','--undefined-only',args.producer]).decode()
    if 'threadDispatchCorridor' in defined or 'threadDispatchCorridor' in undefined:
        raise ValueError('before-only parser contains/imports cloner implementation')
    (args.out/'parser-symbols.txt').write_text(defined+'\n---UNDEFINED---\n'+undefined)
    for name in ('wrapper.c','reference.c'):
        (args.out/name).write_bytes((args.raw/name).read_bytes())
    (args.out/'cases').mkdir()
    reports=[]
    for case in plan['cases']:
        name=case['name']
        case_file=args.out/'cases'/(name+'.json')
        put(case_file,dict(function=case['function'],candidates=case['candidates']))
        report=producer_call(args,commands,'before',args.raw/(name+'.before.ll'),case_file,args.out/'cases'/name)
        reports.append(dict(name=name,report=report))
    for fault in plan['faults']:
        name=fault['name']
        case_file=args.out/'cases'/(fault['source']+'.json')
        raw=args.raw/(fault['source']+'.controlled-'+name+'.ll')
        report=producer_call(args,commands,'before',raw,case_file,args.out/'faults'/name)
        reports.append(dict(name=name,report=report,deliberate_fault=True))
    if tool_binding(args)!=tools or raw_binding(args.raw,model)[1]!=binding:
        raise ValueError('raw input or parser/runtime changed during prepare-before')
    manifest=dict(schema='dispatch-corridor-before-prepared-v1',raw=binding,tools=tools,spec_sha256=SPEC_SHA,
                  runner_sha256=sha(__file__),reports=reports,files=files(args.out),new_cloner_calls=0,
                  source_fixtures=4,unique_module_inputs=64,unique_initial_arenas=16)
    put(args.out/'BEFORE.json',manifest)
    print(json.dumps(dict(prepared_before=True,cloner_calls=0,before_manifest_sha256=sha(args.out/'BEFORE.json'))))


def load_before(root,model,live=True):
    manifest=json.loads((root/'BEFORE.json').read_text())
    if manifest['spec_sha256']!=SPEC_SHA or manifest['new_cloner_calls']!=0:
        raise ValueError('before preparation source/product scope mismatch')
    verify_files(root,manifest['files'],'BEFORE.json')
    if (root/'wrapper.c').read_bytes()!=model.WRAPPER.encode() or (root/'reference.c').read_bytes()!=model.C_SPEC.encode():
        raise ValueError('prepared independent C changed')
    if live and raw_binding(Path(manifest['raw']['root']),model)[1]!=manifest['raw']:
        raise ValueError('raw binding changed after before preparation')
    return manifest


def calibrations(args,model):
    if args.before is None or not args.calibrations or not args.calibration_audits:
        raise ValueError('two complete before calibrations and saved audits required')
    before=load_before(args.before,model)
    seen=set();pins=[]
    for folder,audit in zip(args.calibrations,args.calibration_audits):
        result=json.loads((folder/'RESULTS.json').read_text())
        check=json.loads((audit/'REPORT.json').read_text())
        if (not result['passed'] or result['phase']!='before' or result['spec_sha256']!=SPEC_SHA or
                result['runner_sha256']!=sha(__file__) or result['before_manifest_sha256']!=sha(args.before/'BEFORE.json') or
                result['frames']!=128 or result['matches']!=128 or result['controls']!=1664 or result['specified_faults']!=0 or
                result['unique_module_inputs']!=64 or result['unique_initial_arenas']!=16 or
                not check['passed'] or not check['eof_exact'] or check['phase']!='before' or
                check['result_sha256']!=sha(folder/'RESULTS.json') or check['frames_sha256']!=sha(folder/'frames.xz') or
                check['runner_sha256']!=sha(__file__) or check['spec_sha256']!=SPEC_SHA):
            raise ValueError(('failed/mismatched before calibration/audit',str(folder)))
        if result['frame_stream_sha256']!=sha(folder/'frames.xz') or check['host_opt']!=result['host_opt']:
            raise ValueError('calibration stream or optimization binding differs')
        seen.add(result['host_opt'])
        pins.append(dict(result=dict(path=str(folder/'RESULTS.json'),**record(folder/'RESULTS.json')),
                         frames=dict(path=str(folder/'frames.xz'),**record(folder/'frames.xz')),
                         audit=dict(path=str(audit/'REPORT.json'),**record(audit/'REPORT.json'))))
    if seen!={'O0','O2'}:
        raise ValueError('missing distinct before optimization level')
    return before,pins


def prepare_after(args,model,commands):
    before,calibration_pins=calibrations(args,model)  # Must precede every cloner call.
    tools=tool_binding(args)
    defined=commands.run(['nm','-C','--defined-only',args.producer]).decode()
    api='dragon::llvmlifter::detail::threadDispatchCorridor('
    lines=[line for line in defined.splitlines() if api in line]
    if len(lines)!=1 or ' T ' not in lines[0]:
        raise ValueError('prototype private API must be a defined T symbol in rewrite executable')
    (args.out/'private-api-definition.txt').write_text(lines[0]+'\n')
    dynamic=commands.run(['readelf','-d',args.producer]).decode()
    if 'Shared library: [libdragon.so]' not in dynamic or 'libLLVM' in dynamic:
        raise ValueError('prototype must use the pinned base libdragon for LLVM, without shared libLLVM')
    (args.out/'prototype-dynamic.txt').write_text(dynamic)
    raw_root=Path(before['raw']['root'])
    plan,_=raw_binding(raw_root,model)
    reports=[]
    for case in plan['cases']:
        name=case['name']
        report=producer_call(args,commands,'rewrite',raw_root/(name+'.before.ll'),
                             args.before/'cases'/(name+'.json'),args.out/'cases'/name)
        old=args.before/'cases'/name/'parsed-before.ll'
        parsed=args.out/'cases'/name/'parsed-before.ll'
        output=args.out/'cases'/name/'output.ll'
        if old.read_bytes()!=parsed.read_bytes():
            raise ValueError(('actual parser-before bytes changed, including ModuleID',name))
        stats=report['stats']
        if (report['original_candidates']!=len(case['candidates']) or
                stats['candidates_visited']!=len(case['candidates']) or
                stats['complete']!=(case['expected_rejected']==0) or
                stats['rewritten_edges']!=case['expected_rewrites'] or stats['rejected']!=case['expected_rejected'] or
                stats['changed']!=(case['expected_rewrites']!=0) or stats['budget_exhausted']):
            raise ValueError(('private cloner admission/precision gate',name,stats))
        if not stats['changed'] and parsed.read_bytes()!=output.read_bytes():
            raise ValueError(('refusal changed complete actual parsed IR',name))
        reports.append(dict(name=name,report=report,original_parsed_sha256=sha(old),parsed_sha256=sha(parsed),output_sha256=sha(output)))
    if tool_binding(args)!=tools or calibrations(args,model)[1]!=calibration_pins:
        raise ValueError('tools/calibrations changed during rewrite')
    manifest=dict(schema='dispatch-corridor-after-prepared-v1',before_root=str(args.before),
                  before_manifest_sha256=sha(args.before/'BEFORE.json'),spec_sha256=SPEC_SHA,
                  runner_sha256=sha(__file__),tools=tools,calibration_pins=calibration_pins,
                  reports=reports,files=files(args.out),new_cloner_calls=4,
                  prototype_private_API_T_in_executable=True,
                  scope='Private cloner object in executable; base library supplies LLVM symbols; not production integration')
    put(args.out/'AFTER.json',manifest)
    print(json.dumps(dict(prepared_after=True,cloner_calls=4,after_manifest_sha256=sha(args.out/'AFTER.json'))))


def load_after(root,before_root,model):
    manifest=json.loads((root/'AFTER.json').read_text())
    if (manifest['spec_sha256']!=SPEC_SHA or manifest['runner_sha256']!=sha(__file__) or
            manifest['before_manifest_sha256']!=sha(before_root/'BEFORE.json') or manifest['new_cloner_calls']!=4):
        raise ValueError('after/before/specification binding mismatch')
    verify_files(root,manifest['files'],'AFTER.json')
    return manifest


def phase_domain(phase,model):
    if phase=='fault':
        return [('historical-loop','wrong-history'),('historical-state','wrong-target')]
    return [(name,'source') for name in model.NAMES]


def required_keys(phase,model):
    engines=('C','LLVM') if phase=='before' else ('LLVM',)
    return {(name,variant,t,e) for name,variant in phase_domain(phase,model) for t in range(16) for e in engines}


def controls(actual,wanted,model):
    count=0
    for offset in model.CONTROLS:
        corrupt=bytearray(actual);corrupt[offset]^=1
        count+=bytes(corrupt)!=wanted
    return count


def single_xz_eof(path):
    decoder=lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):
            if decoder.eof:
                raise ValueError('trailing compressed bytes after one XZ stream')
            decoder.decompress(chunk)
            if decoder.unused_data:
                raise ValueError('concatenated/trailing bytes after XZ EOF')
    if not decoder.eof:
        raise ValueError('truncated physical XZ stream')


def native(args,model,commands):
    if args.before is None or args.phase is None or args.host_opt is None:
        raise ValueError('native requires before preparation, phase and host optimization')
    before=load_before(args.before,model)
    if args.phase!='before':
        calibrations(args,model)
    if args.phase=='after':
        if args.after is None:raise ValueError('after-native requires rewrite preparation')
        load_after(args.after,args.before,model)
    source_pins=files(args.before)
    if args.phase=='after':source_pins.update({'AFTER/'+k:v for k,v in files(args.after).items()})
    rows=[];failure=None
    try:
        if args.phase=='before':
            reference=args.out/'C-reference.native'
            commands.run([args.cc,'-'+args.host_opt,args.before/'wrapper.c',args.before/'reference.c','-o',reference])
        with lzma.open(args.out/'frames.xz','xb',preset=1) as stream:
            for name,variant in phase_domain(args.phase,model):
                work=args.out/name;work.mkdir()
                if args.phase=='fault':ir=args.before/'faults'/variant/'parsed-before.ll'
                elif args.phase=='before':ir=args.before/'cases'/name/'parsed-before.ll'
                else:ir=args.after/'cases'/name/'output.ll'
                obj,target=work/'target.o',work/'target.native'
                commands.run([args.llc,'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic',
                              '-'+args.host_opt,ir,'-o',obj])
                commands.run([args.cc,'-'+args.host_opt,args.before/'wrapper.c',obj,'-o',target])
                engines=[('LLVM',target)]
                if args.phase=='before':engines.insert(0,('C',reference))
                sources=[model.initial(t) for t in range(16)]
                for engine,executable in engines:
                    result=commands.run([executable,model.NAMES.index(name)],b''.join(sources))
                    if len(result)!=model.FRAME*16:
                        raise ValueError(('native output length',name,engine,len(result)))
                    for trial,source in enumerate(sources):
                        wanted,coverage=model.specification(source,name)
                        actual=result[trial*model.FRAME:(trial+1)*model.FRAME]
                        match=actual==wanted
                        offset=96 if variant=='wrong-history' else 64 if variant=='wrong-target' else None
                        specified=(actual[offset:offset+4]!=wanted[offset:offset+4]) if offset is not None else None
                        counted=0 if args.phase=='fault' else controls(actual,wanted,model)
                        row=dict(name=name,variant=variant,trial=trial,engine=engine,phase=args.phase,opt=args.host_opt,
                                 match=match,controls=counted,specified_offset=offset,specified_fault=specified,coverage=coverage,
                                 sha256={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',wanted),('actual',actual)]})
                        encoded=json.dumps(row,sort_keys=True).encode()
                        stream.write(struct.pack('<I',len(encoded))+encoded+source+wanted+actual)
                        rows.append(row)
                        if (args.phase=='fault' and (match or not specified)) or (args.phase!='fault' and (not match or counted!=13)):
                            raise ValueError(('native mismatch/missed deliberate fault/comparator',row))
        keys=[(r['name'],r['variant'],r['trial'],r['engine']) for r in rows]
        if len(set(keys))!=len(keys) or set(keys)!=required_keys(args.phase,model):
            raise ValueError('incomplete actual native domain')
        load_before(args.before,model)
        final=files(args.before)
        if args.phase=='after':
            load_after(args.after,args.before,model);final.update({'AFTER/'+k:v for k,v in files(args.after).items()})
        if source_pins!=final:raise ValueError('frozen preparation changed during native')
    except Exception as error:
        failure=type(error).__name__+': '+str(error)
    result=dict(schema='dispatch-corridor-native-result-v1',passed=failure is None,failure=failure,
                phase=args.phase,host_opt=args.host_opt,spec_sha256=SPEC_SHA,runner_sha256=sha(__file__),
                before_manifest_sha256=sha(args.before/'BEFORE.json'),
                after_manifest_sha256=sha(args.after/'AFTER.json') if args.phase=='after' else None,
                rows=rows,frames=len(rows),matches=sum(r['match'] for r in rows),controls=sum(r['controls'] for r in rows),
                specified_faults=sum(r['specified_fault'] is True for r in rows),
                unique_module_inputs=len({(r['name'],r['trial']) for r in rows}),
                unique_initial_arenas=len({r['sha256']['input'] for r in rows}),
                frame_stream_sha256=sha(args.out/'frames.xz') if (args.out/'frames.xz').exists() else None,
                source_pins_before=source_pins,commands=commands.rows,new_cloner_calls=0,
                scope='Full66112-byte comparison; frozen c241 new domain only; deliberate mutants not original73dc; no skips/exclusions/normalization')
    put(args.out/'RESULTS.json',result)
    print(json.dumps({k:result[k] for k in ('passed','failure','phase','frames','matches','controls','specified_faults')}))
    if failure:raise SystemExit(1)


def audit(args,model):
    if args.before is None:raise ValueError('saved audit requires before preparation')
    load_before(args.before,model,live=False)
    result=json.loads((args.audit/'RESULTS.json').read_text())
    if (not result['passed'] or result['spec_sha256']!=SPEC_SHA or result['runner_sha256']!=sha(__file__) or
            result['before_manifest_sha256']!=sha(args.before/'BEFORE.json') or
            sha(args.audit/'frames.xz')!=result['frame_stream_sha256']):
        raise ValueError('failed saved result or source/preparation/stream binding mismatch')
    phase=result['phase']
    if phase not in ('before','after','fault') or result['host_opt'] not in ('O0','O2'):
        raise ValueError('invalid saved phase/optimization')
    if phase=='after':
        if args.after is None or sha(args.after/'AFTER.json')!=result['after_manifest_sha256']:
            raise ValueError('saved after preparation differs')
        load_after(args.after,args.before,model)
    single_xz_eof(args.audit/'frames.xz')
    rows=[];keys=set()
    with lzma.open(args.audit/'frames.xz','rb') as stream:
        while True:
            header=stream.read(4)
            if not header:break
            if len(header)!=4:raise ValueError('truncated frame prefix')
            size=struct.unpack('<I',header)[0]
            if not 0<size<8192:raise ValueError('invalid JSON header length')
            encoded=stream.read(size)
            if len(encoded)!=size:raise ValueError('truncated JSON header')
            row=json.loads(encoded)
            source,wanted,actual=(stream.read(model.FRAME) for _ in range(3))
            if any(len(x)!=model.FRAME for x in (source,wanted,actual)):raise ValueError('truncated full arena')
            key=(row['name'],row['variant'],row['trial'],row['engine'])
            if key in keys or key not in required_keys(phase,model):raise ValueError('duplicate/out-of-domain frame')
            keys.add(key)
            if row['phase']!=phase or row['opt']!=result['host_opt'] or source!=model.initial(row['trial']):
                raise ValueError('row/source identity differs')
            expected,coverage=model.specification(source,row['name'])
            if wanted!=expected or row['coverage']!=coverage:raise ValueError('saved independent expectation differs')
            match=actual==wanted
            offset=96 if row['variant']=='wrong-history' else 64 if row['variant']=='wrong-target' else None
            specified=(actual[offset:offset+4]!=wanted[offset:offset+4]) if offset is not None else None
            counted=0 if phase=='fault' else controls(actual,wanted,model)
            if (row['match']!=match or row['controls']!=counted or row['specified_offset']!=offset or
                    row['specified_fault']!=specified):raise ValueError('saved comparison/control field differs')
            if (phase=='fault' and (match or not specified)) or (phase!='fault' and (not match or counted!=13)):
                raise ValueError('missed specified fault or ordinary full-state mismatch')
            if row['sha256']!={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',wanted),('actual',actual)]}:
                raise ValueError('full arena SHA differs')
            rows.append(row)
    if keys!=required_keys(phase,model) or rows!=result['rows']:raise ValueError('incomplete stream or result row disagreement')
    aggregate=dict(frames=len(rows),matches=sum(r['match'] for r in rows),controls=sum(r['controls'] for r in rows),
                   specified_faults=sum(r['specified_fault'] is True for r in rows),
                   unique_module_inputs=len({(r['name'],r['trial']) for r in rows}),
                   unique_initial_arenas=len({r['sha256']['input'] for r in rows}))
    if any(result[k]!=v for k,v in aggregate.items()):raise ValueError('saved aggregate/domain disagreement')
    report=dict(schema='dispatch-corridor-saved-audit-v1',passed=True,eof_exact=True,physical_single_xz_eof_exact=True,phase=phase,host_opt=result['host_opt'],
                result_sha256=sha(args.audit/'RESULTS.json'),frames_sha256=sha(args.audit/'frames.xz'),
                spec_sha256=SPEC_SHA,runner_sha256=sha(__file__),before_manifest_sha256=sha(args.before/'BEFORE.json'),
                **aggregate,new_product_executions=0,new_native_executions=0)
    put(args.out/'REPORT.json',report)
    print(json.dumps(report))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    action=parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--prepare-before',action='store_true')
    action.add_argument('--prepare-after',action='store_true')
    action.add_argument('--native',action='store_true')
    action.add_argument('--audit',type=Path)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--raw',type=Path)
    parser.add_argument('--before',type=Path)
    parser.add_argument('--after',type=Path)
    parser.add_argument('--producer',type=Path)
    parser.add_argument('--producer-sha256')
    parser.add_argument('--runtime',type=Path)
    parser.add_argument('--runtime-sha256')
    parser.add_argument('--calibrations',nargs=2,type=Path)
    parser.add_argument('--calibration-audits',nargs=2,type=Path)
    parser.add_argument('--phase',choices=('before','after','fault'))
    parser.add_argument('--host-opt',choices=('O0','O2'))
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--llc',default='llc-21')
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    commands=Commands(args.out)
    try:
        model=specification()
        if args.prepare_before:prepare_before(args,model,commands)
        elif args.prepare_after:prepare_after(args,model,commands)
        elif args.native:native(args,model,commands)
        else:audit(args,model)
    except BaseException as error:
        put(args.out/'FAILURE.json',dict(error=repr(error),runner_sha256=sha(__file__),spec_sha256=SPEC_SHA,commands=commands.rows))
        raise


if __name__=='__main__':
    main()
