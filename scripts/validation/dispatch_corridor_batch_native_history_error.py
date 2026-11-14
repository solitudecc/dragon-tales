"""Supplementary cloned-corridor error inputs; zero new producer/API calls.

Reuses frozen C/beforeLLVM/mainAfter/NZAfter executables. The only new LLVM
compilation deliberately changes the raw error PHI from history to advanced.
Every CLI must run under the existing CPU0-3/180s/512MiB-64MiB controller.
"""
import argparse
import hashlib
import importlib.util
import json
import lzma
from pathlib import Path
import struct
import sys

SOURCE_SHA = 'ed67354cbf31feaf5e4429d827cd43c11b1d8460f3c8f830cf97fb9147ade3e4'
RUNNER_SHA = 'f282c2acdd3d08045d094742f946f1b344e9a4fedbfe85d49df8d19d884673b5'
PARAMETERS = (('A',0,4),('B',1,(1<<63)+4),('C',0,214))


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(name,path):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name,path)
    value = importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def tools(a):
    source = a.before/'source/scripts/validation'
    if sha(source/'dispatch_corridor_batch_native_source.py') != SOURCE_SHA or sha(source/'dispatch_corridor_batch_native.py') != RUNNER_SHA:
        raise ValueError('frozen model/stream utility differs')
    r = load('frozen_batch_runtime_utilities',source/'dispatch_corridor_batch_native.py')
    return r,load('frozen_batch_model',source/'dispatch_corridor_batch_native_source.py')


def frames(m):
    rows = []
    for name,trial,limit in PARAMETERS:
        initial = m.initial(trial);value = bytearray(initial);value[24:32] = limit.to_bytes(8,'little');value = bytes(value)
        if value[:24] != initial[:24] or value[32:] != initial[32:]: raise ValueError('unexpected input patch')
        expected,coverage = m.specification(value,m.NAMES[0])
        wanted = (72,72,71,70) if name == 'C' else (2,2,1,1)
        if tuple(coverage[k] if k!='states' else len(coverage[k]) for k in ('headers','effects','dispatches','states')) != wanted or not coverage['limit_error']:
            raise ValueError('supplement does not reach the planned cloned error')
        rows.append((name,value,expected,coverage))
    return rows


def frozen_roots(a,r):
    result = {}
    for name,root,manifest in (('before',a.before,'BEFORE-FILESET.json'),('main',a.main,'AFTER-FILESET.json'),('noz3',a.noz3,'AFTER-FILESET.json')):
        data = json.loads((root/manifest).read_text());r.U.verify_files(root,data['files'],manifest)
        result[name] = dict(path=str(root),manifest=r.record(root/manifest))
    return result


def executables(a,r,opt):
    name = 'seventy-state-history'
    values = {'C':a.before/('before-'+opt)/'reference.native',
              'beforeLLVM':a.before/('before-'+opt)/name/'target.native',
              'mainAfterLLVM':a.main/('after-'+opt)/name/'target.native',
              'NZAfterLLVM':a.noz3/('after-'+opt)/name/'target.native'}
    return values,{name:dict(path=str(path),**r.record(path)) for name,path in values.items()}


def source_cases(a,r,opt,pins):
    case='seventy-state-history';driver=a.before/'raw/wrapper.c'
    result={}
    for engine,root,phase in (('C',a.before,'before'),('beforeLLVM',a.before,'before'),('mainAfterLLVM',a.main,'after'),('NZAfterLLVM',a.noz3,'after')):
        source=a.before/'raw/reference.c' if engine=='C' else root/(phase+'-prepared')/case/('parsed-before.ll' if phase=='before' else 'output.ll')
        summary=root/('BEFORE-SUMMARY.json' if phase=='before' else 'AFTER-SUMMARY.json')
        previous=root/(phase+'-'+opt)/'RESULTS.json';old=json.loads(previous.read_text())
        old_engine='C' if engine=='C' else 'LLVM'
        if old['host_opt']!=opt or sum(v['name']==case and v['engine']==old_engine for v in old['rows'])!=8:
            raise ValueError('frozen executable source case/engine relation differs')
        result[engine]=dict(sourceCase=case,oracle_case=0,engine=engine,executable=pins[engine],
                            compiled_source=dict(path=str(source),**r.record(source)),driver=dict(path=str(driver),**r.record(driver)),
                            original_summary=dict(path=str(summary),**r.record(summary)),original_native_result=dict(path=str(previous),**r.record(previous)))
    return result


def write_frame(stream,row,source,expected,actual):
    encoded = json.dumps(row,sort_keys=True).encode();stream.write(struct.pack('<I',len(encoded))+encoded+source+expected+actual)


def run(a,r,m):
    roots = frozen_roots(a,r);inputs = frames(m);commands = r.Commands(a.out)
    exes,pins = executables(a,r,a.host_opt);rows = []
    relations=source_cases(a,r,a.host_opt,pins)
    header = a.include_root/'dragon/lifters/llvm/Execution.h'
    if sha(header) != 'b0674f3684a1393a89c06b00beee6402eaa20e388f418491b7c1713016b1b560': raise ValueError('Execution.h ABI differs')
    compiler = {str(p):r.record(p.resolve(strict=True)) for p in (a.cc,a.llc,a.opt,header,Path(sys.executable))}
    fault_binding = None
    if a.phase == 'fault':
        if len(a.normal_result or [])!=2 or len(a.normal_audit or [])!=2:
            raise ValueError('both normal O0/O2 comparisons and audits must precede fault compilation')
        for folder,audit_root,opt in zip(a.normal_result,a.normal_audit,('O0','O2')):
            previous=json.loads((folder/'RESULTS.json').read_text());check=json.loads((audit_root/'REPORT.json').read_text())
            if (not previous['passed'] or previous['phase']!='normal' or previous['host_opt']!=opt or
                previous['frames']!=12 or previous['matches']!=12 or previous['controls']!=324 or
                previous['runner_sha256']!=sha(__file__) or previous['roots']!=roots or
                not check['passed'] or not check['physical_single_xz_eof_exact'] or
                check['result']!=r.record(folder/'RESULTS.json') or check['frames']!=r.record(folder/'frames.xz')):
                raise ValueError('normal calibration/audit binding differs')
        raw = a.before/'raw/seventy-state-history.ll';text = raw.read_text()
        old = '%past=phi i64 [%history,%effect]';new = '%past=phi i64 [%advanced,%effect]'
        if text.count(old) != 1: raise ValueError('error PHI fault sentinel not unique')
        mutant = a.out/'wrong-error-history.ll';mutant.write_text(text.replace(old,new))
        commands.run([a.opt,'-passes=verify','-disable-output',mutant])
        obj,target = a.out/'fault.o',a.out/'fault.native'
        commands.run([a.llc,'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic','-'+a.host_opt,mutant,'-o',obj])
        commands.run([a.cc,'-'+a.host_opt,'-I'+str(a.include_root),a.before/'raw/wrapper.c',obj,'-o',target])
        fault_binding=dict(raw=dict(path=str(mutant),**r.record(mutant)),original_raw=dict(path=str(raw),**r.record(raw)),
                           object=dict(path=str(obj),**r.record(obj)),executable=dict(path=str(target),**r.record(target)),
                           only_replacement=dict(old=old,new=new))
        exes = {'faultLLVM':target}
        relations={'faultLLVM':dict(sourceCase=m.NAMES[0],oracle_case=0,engine='faultLLVM',executable=fault_binding['executable'],
                                    compiled_source=fault_binding['raw'],original_raw=fault_binding['original_raw'],
                                    driver=dict(path=str(a.before/'raw/wrapper.c'),**r.record(a.before/'raw/wrapper.c')),
                                    original_summary=dict(path=str(a.before/'BEFORE-SUMMARY.json'),**r.record(a.before/'BEFORE-SUMMARY.json')))}
    with lzma.open(a.out/'frames.xz','xb',preset=1) as stream:
        for engine,exe in exes.items():
            output = commands.run([exe,'0'],b''.join(v[1] for v in inputs))
            if len(output) != 3*m.FRAME: raise ValueError('supplement output length differs')
            for index,(name,source,expected,coverage) in enumerate(inputs):
                actual = output[index*m.FRAME:(index+1)*m.FRAME];match = actual==expected
                specified = actual[m.RETURN:m.RETURN+8] != expected[m.RETURN:m.RETURN+8]
                controls = r.U.controls(actual,expected,m) if a.phase=='normal' else 0
                row = dict(name=name,sourceCase=m.NAMES[0],engine=engine,phase=a.phase,opt=a.host_opt,match=match,coverage=coverage,
                           specified_fault=specified if a.phase=='fault' else False,controls=controls,
                           sha256={n:hashlib.sha256(b).hexdigest() for n,b in [('input',source),('expected',expected),('actual',actual)]})
                write_frame(stream,row,source,expected,actual);rows.append(row)
                if (a.phase=='normal' and (not match or controls!=27)) or (a.phase=='fault' and (match or not specified)):
                    raise ValueError(('missed error-history mismatch or full-state/control failure',row))
    if frozen_roots(a,r)!=roots or executables(a,r,a.host_opt)[1]!=pins: raise ValueError('frozen inputs/executables changed')
    if {str(p):r.record(p.resolve(strict=True)) for p in (a.cc,a.llc,a.opt,header,Path(sys.executable))} != compiler:
        raise ValueError('compiler/ABI header changed')
    result = dict(passed=True,phase=a.phase,host_opt=a.host_opt,rows=rows,frames=len(rows),matches=sum(v['match'] for v in rows),
                  controls=sum(v['controls'] for v in rows),specified_faults=sum(v['specified_fault'] for v in rows),
                  source_sha256=SOURCE_SHA,runner_sha256=sha(__file__),roots=roots,executables=pins,tools=compiler,
                  sourceCases=relations,
                  fault_binding=fault_binding,
                  frames_file=r.record(a.out/'frames.xz'),new_api_calls=0,new_unicorn_calls=0,skips=0,exclusions=0,normalizations=0)
    r.put(a.out/'RESULTS.json',result)


def audit(a,r,m):
    result = json.loads((a.audit/'RESULTS.json').read_text())
    if not result['passed'] or result['source_sha256']!=SOURCE_SHA or result['runner_sha256']!=sha(__file__): raise ValueError('saved source/runner differs')
    if result['roots']!=frozen_roots(a,r) or result['frames_file']!=r.record(a.audit/'frames.xz'): raise ValueError('saved root/frame binding differs')
    r.U.single_xz_eof(a.audit/'frames.xz');inputs = {name:(source,expected,coverage) for name,source,expected,coverage in frames(m)}
    rows = []
    with lzma.open(a.audit/'frames.xz','rb') as stream:
        while True:
            prefix = stream.read(4)
            if not prefix: break
            if len(prefix)!=4: raise ValueError('truncated saved prefix')
            size = struct.unpack('<I',prefix)[0]
            if not 0<size<16384: raise ValueError('invalid saved row size')
            encoded = stream.read(size)
            if len(encoded)!=size: raise ValueError('truncated saved row')
            row = json.loads(encoded);source,expected,actual = (stream.read(m.FRAME) for _ in range(3))
            if any(len(b)!=m.FRAME for b in (source,expected,actual)): raise ValueError('truncated arena')
            if row['phase']!=result['phase'] or row['opt']!=result['host_opt'] or row['name'] not in inputs or row['sourceCase']!=m.NAMES[0]:
                raise ValueError('invalid saved frame identity')
            if (source,expected,row['coverage'])!=inputs[row['name']]: raise ValueError('supplement independent expectation differs')
            match = actual==expected;specified = actual[m.RETURN:m.RETURN+8]!=expected[m.RETURN:m.RETURN+8]
            controls = r.U.controls(actual,expected,m) if result['phase']=='normal' else 0
            if row['match']!=match or row['controls']!=controls or row['specified_fault']!=(specified if result['phase']=='fault' else False): raise ValueError('saved result differs')
            if (result['phase']=='normal' and (not match or controls!=27)) or (result['phase']=='fault' and (match or not specified)):
                raise ValueError('saved mismatch/missed fault')
            if row['sha256']!={n:hashlib.sha256(b).hexdigest() for n,b in [('input',source),('expected',expected),('actual',actual)]}:
                raise ValueError('saved arena SHA differs')
            rows.append(row)
    engines = ('C','beforeLLVM','mainAfterLLVM','NZAfterLLVM') if result['phase']=='normal' else ('faultLLVM',)
    keys = [(v['name'],v['engine']) for v in rows]
    if len(keys)!=len(set(keys)) or set(keys)!={(n,e) for n in inputs for e in engines} or rows!=result['rows']:
        raise ValueError('saved domain incomplete or duplicated')
    aggregate = dict(frames=len(rows),matches=sum(v['match'] for v in rows),controls=sum(v['controls'] for v in rows),specified_faults=sum(v['specified_fault'] for v in rows))
    if any(result[k]!=v for k,v in aggregate.items()): raise ValueError('saved aggregates differ')
    r.put(a.out/'REPORT.json',dict(passed=True,physical_single_xz_eof_exact=True,result=r.record(a.audit/'RESULTS.json'),
                                 frames=r.record(a.audit/'frames.xz'),counts=aggregate,new_api_calls=0,new_native_calls=0))


def main():
    p=argparse.ArgumentParser();p.add_argument('action',choices=['run','audit']);p.add_argument('--before',required=True,type=Path)
    p.add_argument('--main',required=True,type=Path);p.add_argument('--noz3',required=True,type=Path);p.add_argument('--out',required=True,type=Path)
    p.add_argument('--phase',choices=['normal','fault']);p.add_argument('--host-opt',choices=['O0','O2']);p.add_argument('--audit',type=Path)
    p.add_argument('--cc',type=Path);p.add_argument('--llc',type=Path);p.add_argument('--opt',type=Path);p.add_argument('--include-root',type=Path)
    p.add_argument('--normal-result',type=Path,nargs=2);p.add_argument('--normal-audit',type=Path,nargs=2)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False);r,m=tools(a)
    try:
        if a.action=='run':run(a,r,m)
        else:audit(a,r,m)
    except Exception as error:
        r.put(a.out/'FAILURE.json',dict(error=type(error).__name__+': '+str(error),new_api_calls=0));raise


if __name__=='__main__':main()
