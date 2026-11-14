"""Portable full-arena batch validation; run each CLI under the 512/64/180 guard.

Before C/LLVM calibration and saved audits precede all installed Batch API calls.
The old corridor utilities supply file/command/XZ plumbing, not expectations.
"""
import argparse
import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

SOURCE_SHA = 'ed67354cbf31feaf5e4429d827cd43c11b1d8460f3c8f830cf97fb9147ade3e4'


def load_file(name, path):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


HERE = Path(__file__).resolve().parent
U = load_file('batch_arena_utilities', HERE/'dispatch_corridor_native.py')
sha, record, put, files = U.sha, U.record, U.put, U.files


class Commands:
    """Retain complete stdout/stderr even for a failed command or partial timeout."""
    def __init__(self, out): self.out, self.rows = out, []

    def run(self, argv, data=None, env=None, stderr_path=None):
        index = len(self.rows); start = time.monotonic(); timed_out = False
        try:
            value = subprocess.run([str(x) for x in argv],input=data,capture_output=True,env=env,timeout=180)
            output,error,code = value.stdout,value.stderr,value.returncode
        except subprocess.TimeoutExpired as exc:
            output,error,code = exc.output or b'',exc.stderr or b'',None; timed_out = True
        stdout_file = self.out/('command-'+str(index)+'.stdout.bin')
        stderr_file = self.out/('command-'+str(index)+'.stderr')
        stdout_file.write_bytes(output);stderr_file.write_bytes(error)
        if stderr_path is not None: Path(stderr_path).write_bytes(error)
        self.rows.append(dict(argv=[str(x) for x in argv],exit=code,seconds=time.monotonic()-start,
                              timed_out=timed_out,stdout=dict(path=str(stdout_file),**record(stdout_file)),
                              stderr=dict(path=str(stderr_file),**record(stderr_file))))
        (self.out/'COMMANDS.json').write_text(json.dumps(self.rows,indent=2)+'\n')
        if timed_out or code: raise RuntimeError(self.rows[-1])
        return output


def model():
    path = HERE/'dispatch_corridor_batch_native_source.py'
    if sha(path) != SOURCE_SHA: raise ValueError('independent specification source changed')
    value = load_file('batch_independent_model', path)
    if len(set(value.CONTROLS)) != len(value.CONTROLS): raise ValueError('duplicate comparator controls')
    return value


def own_binding():
    return {str(path):record(path) for path in
            (Path(__file__).resolve(), HERE/'dispatch_corridor_native.py', HERE/'dispatch_corridor_batch_native_source.py')}


def raw_binding(root, m):
    if not root or not root.is_absolute(): raise ValueError('explicit absolute raw source root required')
    plan = json.loads((root/'PLAN.json').read_text())
    if plan['source_sha256'] != SOURCE_SHA or plan['names'] != list(m.NAMES) or plan['trials'] != 8:
        raise ValueError('raw specification/domain binding differs')
    U.verify_files(root, plan['files'], 'PLAN.json')
    if (root/'reference.c').read_text() != m.C_SPEC or (root/'wrapper.c').read_text() != m.WRAPPER:
        raise ValueError('independent C/driver source differs')
    generated = m.sources()
    for name,(ir,candidates) in generated.items():
        if ((root/(name+'.ll')).read_text() != ir or
            json.loads((root/(name+'.candidates.json')).read_text()) != dict(function='view',candidates=candidates)):
            raise ValueError('raw LLVM/candidate source differs')
    for t in range(8):
        if (root/('input-'+str(t)+'.bin')).read_bytes() != m.initial(t): raise ValueError('input bytes differ')
    return dict(root=str(root),plan=record(root/'PLAN.json'),files=plan['files'])


def producer_binding(a):
    for name in ('producer','runtime','producer_build','sdk_manifest'):
        path = getattr(a,name)
        if not path or not path.is_absolute(): raise ValueError('absolute '+name+' required')
    if sha(a.producer) != a.producer_sha256 or sha(a.runtime) != a.runtime_sha256:
        raise ValueError('explicit producer/runtime SHA differs')
    manifest = json.loads(a.sdk_manifest.read_text())
    if set(manifest) != {'runtime','solver'} or manifest['runtime'] != dict(path=str(a.runtime),sha256=a.runtime_sha256):
        raise ValueError('SDK manifest runtime path/hash differs from requested runtime')
    solver = manifest['solver']
    if solver is not None:
        if (not isinstance(solver,dict) or set(solver) != {'path','sha256'} or
            not Path(solver['path']).is_absolute() or sha(solver['path']) != solver['sha256']):
            raise ValueError('SDK manifest solver path/hash invalid')
        solver = dict(path=solver['path'],**record(solver['path']))
    result = {name:dict(path=str(getattr(a,name)),**record(getattr(a,name)))
              for name in ('producer','runtime','producer_build','sdk_manifest')}
    result['solver'] = solver
    return result


def actual_mapping(lines, path):
    status = path.stat()
    if not lines: raise ValueError('required DSO is not mapped')
    for line in lines:
        columns = line.split(maxsplit=5)
        if len(columns) != 6 or columns[5] != str(path): raise ValueError('unexpected actual DSO mapping path')
        major,minor = (int(n,16) for n in columns[3].split(':'))
        if int(columns[4]) != status.st_ino or (major,minor) != (os.major(status.st_dev),os.minor(status.st_dev)):
            raise ValueError('actual DSO mapping device/inode differs from pinned leaf')


def compiler_binding(a):
    paths = dict(cc=a.cc,llc=a.llc,opt=a.opt,python=Path(sys.executable),execution_header=a.include_root/'dragon/lifters/llvm/Execution.h')
    if not all(path.is_absolute() for path in paths.values()): raise ValueError('absolute tool/include paths required')
    return {name:dict(requested=str(path),resolved=str(path.resolve(strict=True)),**record(path.resolve(strict=True)))
            for name,path in paths.items()}


def prepared(root, m, phase):
    data = json.loads((root/'PREPARED.json').read_text())
    if data['phase'] != phase or data['source_sha256'] != SOURCE_SHA or data['runner_bindings'] != own_binding():
        raise ValueError('prepared phase/source/runner changed')
    U.verify_files(root,data['files'],'PREPARED.json')
    raw_binding(Path(data['raw']['root']),m)
    return data


def expected_keys(m, phase):
    if phase == 'fault': return {(name,t,'LLVM') for name in m.FAULTS for t in range(8)}
    engines = ('C','LLVM') if phase == 'before' else ('LLVM',)
    return {(name,t,e) for name in m.NAMES for t in range(8) for e in engines}


def checked_result(root, audit, m, phase, opt):
    result = json.loads((root/'RESULTS.json').read_text()); check = json.loads((audit/'REPORT.json').read_text())
    if (not result['passed'] or result['phase'] != phase or result['host_opt'] != opt or
        result['runner_bindings'] != own_binding() or result['source_sha256'] != SOURCE_SHA or
        not check['passed'] or not check['physical_single_xz_eof_exact'] or
        check['result'] != record(root/'RESULTS.json') or check['frames'] != record(root/'frames.xz')):
        raise ValueError('failed/mismatched calibration or saved audit')
    if {(r['name'],r['trial'],r['engine']) for r in result['rows']} != expected_keys(m,phase):
        raise ValueError('calibration domain incomplete')
    return dict(result=dict(path=str(root/'RESULTS.json'),**record(root/'RESULTS.json')),
                frames=dict(path=str(root/'frames.xz'),**record(root/'frames.xz')),
                audit=dict(path=str(audit/'REPORT.json'),**record(audit/'REPORT.json')))


def prerequisites(a, m, faults=False):
    if not a.before: raise ValueError('explicit before preparation required')
    before = prepared(a.before,m,'before')
    before_pin = dict(path=str(a.before/'PREPARED.json'),**record(a.before/'PREPARED.json'))
    if len(a.calibration or []) != 2 or len(a.calibration_audit or []) != 2:
        raise ValueError('both O0/O2 before results and audits required')
    pins = [checked_result(r,c,m,'before',o) for r,c,o in zip(a.calibration,a.calibration_audit,('O0','O2'))]
    roots = list(a.calibration)
    if faults:
        if len(a.fault_result or []) != 2 or len(a.fault_audit or []) != 2:
            raise ValueError('both O0/O2 deliberate-fault results and audits required before API invocation')
        pins += [checked_result(r,c,m,'fault',o) for r,c,o in zip(a.fault_result,a.fault_audit,('O0','O2'))]
        roots += list(a.fault_result)
    for root in roots:
        result = json.loads((root/'RESULTS.json').read_text())
        if result['before'] != before_pin or result['raw'] != before['raw']:
            raise ValueError('calibration belongs to another raw/before preparation')
    return pins


def prepare(a, m, commands, after):
    binding = raw_binding(a.raw,m); tools = producer_binding(a)
    calibration = prerequisites(a,m,True) if after else []
    if after and binding != prepared(a.before,m,'before')['raw']:
        raise ValueError('after producer raw differs from before calibration')
    defined = commands.run(['nm','-C','--defined-only',a.producer]).decode()
    undefined = commands.run(['nm','-D','-C','--undefined-only',a.producer]).decode()
    api = 'dragon::llvmlifter::detail::threadDispatchCorridorBatch('
    if 'threadDispatchCorridor' in defined or ('threadDispatchCorridor' in undefined and not after):
        raise ValueError('producer contains a helper implementation or before-only API import')
    if after:
        exported = commands.run(['nm','-D','-C','--defined-only',a.runtime]).decode()
        if sum(api in line for line in undefined.splitlines()) != 1 or not any(api in line and ' T ' in line for line in exported.splitlines()):
            raise ValueError('Batch API is not producer UND -> installed SDK T')
        (a.out/'sdk-exports.txt').write_text(exported)
    (a.out/'producer-symbols.txt').write_text(defined+'\n---UND---\n'+undefined)
    reports = []
    cases = [(name,a.raw/(name+'.ll'),a.raw/(name+'.candidates.json')) for name in m.NAMES]
    if not after:
        cases += [(name,a.raw/(name+'.fault.ll'),a.raw/(rule['source']+'.candidates.json')) for name,rule in m.FAULTS.items()]
    prefix = [str(a.runtime.parent)]
    if tools['solver'] is not None: prefix.append(str(Path(tools['solver']['path']).parent))
    env = dict(os.environ,LD_LIBRARY_PATH=':'.join(prefix),LD_PRELOAD='',LD_DEBUG='libs,bindings',PYTHONDONTWRITEBYTECODE='1')
    for name,raw,candidates in cases:
        work = a.out/name; work.mkdir(); mode = 'rewrite' if after else 'before'
        paths = [work/'parsed-before.ll',work/'output.ll',work/'REPORT.json']
        commands.run([a.producer,mode,raw,candidates,*paths],env=env,stderr_path=work/'loader.stderr')
        report = json.loads(paths[2].read_text())
        if (report['raw_input'] != str(raw) or report['case_manifest'] != str(candidates) or
            not report['verified_before'] or not report['verified_after'] or not report['function_argument_identity'] or
            report['cloner_calls'] != int(after) or report['compiled_before_only'] == after):
            raise ValueError('producer source/API/actual mapping contract failed')
        actual_mapping(report['runtime_maps'],a.runtime)
        if tools['solver'] is None:
            if report['solver_maps']: raise ValueError('solver mapped in declared No-Z3 SDK')
        else:
            actual_mapping(report['solver_maps'],Path(tools['solver']['path']))
        if not after and paths[0].read_bytes() != paths[1].read_bytes(): raise ValueError('before parser changed IR')
        if after:
            if paths[0].read_bytes() != (a.before/name/'parsed-before.ll').read_bytes():
                raise ValueError('new SDK parsed-before differs byte-for-byte from calibrated before')
            stats = report['stats']; expected = 71 if name == m.NAMES[0] else 1
            if (not stats['changed'] or not stats['complete'] or stats['budget_exhausted'] or
                stats['rewritten_edges'] != expected or stats['rejected'] or report['cyclic_switches']):
                raise ValueError('explicit batch precision gate failed')
        reports.append(report)
    if producer_binding(a) != tools or raw_binding(a.raw,m) != binding: raise ValueError('live producer/raw changed')
    data = dict(schema='batch-native-prepared-v1',phase='after' if after else 'before',source_sha256=SOURCE_SHA,
                raw=binding,producer=tools,runner_bindings=own_binding(),calibrations=calibration,reports=reports,
                new_api_calls=len(m.NAMES) if after else 0,files=files(a.out))
    put(a.out/'PREPARED.json',data)


def row_check(m, phase, name, trial, actual, wanted):
    match = actual == wanted; required = False; specified = False
    if phase == 'fault':
        rule = m.FAULTS[name]; required = trial in rule['trials']; at = rule['offset']
        specified = actual[at:at+8] != wanted[at:at+8]
        if (required and (match or not specified)) or (not required and not match):
            raise ValueError(('missed specified fault or unexpected nontrigger',name,trial))
        return dict(match=match,controls=0,required_fault=required,specified_fault=specified)
    count = U.controls(actual,wanted,m)
    if not match or count != len(m.CONTROLS): raise ValueError(('full-state mismatch/comparator failure',name,trial))
    return dict(match=True,controls=count,required_fault=False,specified_fault=False)


def run(a, m, commands):
    before = prepared(a.before,m,'before'); source_pins = files(a.before)
    if raw_binding(a.raw,m) != before['raw']: raise ValueError('native raw differs from prepared raw')
    calibration = prerequisites(a,m,a.phase=='after') if a.phase != 'before' else []
    if a.phase == 'after':
        after = prepared(a.after,m,'after')
        if after['raw'] != before['raw'] or after['calibrations'] != calibration:
            raise ValueError('after/before calibration binding differs')
        source_pins.update({'AFTER/'+k:v for k,v in files(a.after).items()})
    tool_pins = compiler_binding(a); rows = []
    if a.phase == 'before':
        reference = a.out/'reference.native'
        commands.run([a.cc,'-'+a.host_opt,'-I'+str(a.include_root),a.raw/'wrapper.c',a.raw/'reference.c','-o',reference])
    names = list(m.FAULTS) if a.phase == 'fault' else list(m.NAMES)
    with lzma.open(a.out/'frames.xz','xb',preset=1) as stream:
        for name in names:
            work = a.out/name; work.mkdir()
            ir = (a.after if a.phase == 'after' else a.before)/name/('output.ll' if a.phase == 'after' else 'parsed-before.ll')
            commands.run([a.opt,'-passes=verify','-disable-output',ir])
            obj,target = work/'target.o',work/'target.native'
            commands.run([a.llc,'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic','-'+a.host_opt,ir,'-o',obj])
            commands.run([a.cc,'-'+a.host_opt,'-I'+str(a.include_root),a.raw/'wrapper.c',obj,'-o',target])
            engines = [('C',reference),('LLVM',target)] if a.phase == 'before' else [('LLVM',target)]
            inputs = [m.initial(t) for t in range(8)]
            for engine,exe in engines:
                case = m.FAULTS[name]['source'] if a.phase == 'fault' else name
                output = commands.run([exe,m.NAMES.index(case)],b''.join(inputs))
                if len(output) != m.FRAME*8: raise ValueError('native output length differs')
                for trial,source in enumerate(inputs):
                    wanted,coverage = m.specification(source,case); actual = output[trial*m.FRAME:(trial+1)*m.FRAME]
                    values = row_check(m,a.phase,name,trial,actual,wanted)
                    row = dict(name=name,trial=trial,engine=engine,phase=a.phase,opt=a.host_opt,coverage=coverage,**values,
                               sha256={n:hashlib.sha256(b).hexdigest() for n,b in [('input',source),('expected',wanted),('actual',actual)]})
                    encoded = json.dumps(row,sort_keys=True).encode();stream.write(struct.pack('<I',len(encoded))+encoded+source+wanted+actual);rows.append(row)
    keys = [(r['name'],r['trial'],r['engine']) for r in rows]
    if len(keys) != len(set(keys)) or set(keys) != expected_keys(m,a.phase): raise ValueError('native domain incomplete')
    final = files(a.before)
    if a.phase == 'after': final.update({'AFTER/'+k:v for k,v in files(a.after).items()})
    if source_pins != final or compiler_binding(a) != tool_pins: raise ValueError('source/compiler changed during native')
    result = dict(schema='batch-native-results-v1',passed=True,phase=a.phase,host_opt=a.host_opt,source_sha256=SOURCE_SHA,
                  runner_bindings=own_binding(),tools=tool_pins,source_pins=source_pins,raw=raw_binding(a.raw,m),
                  before=dict(path=str(a.before/'PREPARED.json'),**record(a.before/'PREPARED.json')),
                  after=dict(path=str(a.after/'PREPARED.json'),**record(a.after/'PREPARED.json')) if a.phase=='after' else None,
                  calibration=calibration,rows=rows,frames=len(rows),matches=sum(r['match'] for r in rows),
                  controls=sum(r['controls'] for r in rows),required_faults=sum(r['required_fault'] for r in rows),
                  specified_faults=sum(r['specified_fault'] for r in rows),new_api_calls=0,
                  frames_file=record(a.out/'frames.xz'),skips=0,exclusions=0,normalizations=0)
    put(a.out/'RESULTS.json',result)


def audit(a, m):
    result = json.loads((a.audit/'RESULTS.json').read_text())
    if not result['passed'] or result['source_sha256'] != SOURCE_SHA or result['runner_bindings'] != own_binding():
        raise ValueError('saved result/source bindings differ')
    if record(a.audit/'frames.xz') != result['frames_file']: raise ValueError('saved stream SHA differs')
    U.single_xz_eof(a.audit/'frames.xz'); phase = result['phase']; rows = []
    with lzma.open(a.audit/'frames.xz','rb') as stream:
        while True:
            prefix = stream.read(4)
            if not prefix: break
            if len(prefix) != 4: raise ValueError('truncated frame prefix')
            size = struct.unpack('<I',prefix)[0]
            if not 0 < size < 16384: raise ValueError('invalid row header size')
            encoded = stream.read(size)
            if len(encoded) != size: raise ValueError('truncated row header')
            row = json.loads(encoded); source,wanted,actual = (stream.read(m.FRAME) for _ in range(3))
            if any(len(b) != m.FRAME for b in (source,wanted,actual)): raise ValueError('truncated arena')
            if (row['phase'] != phase or row['opt'] != result['host_opt'] or source != m.initial(row['trial'])):
                raise ValueError('saved phase/input differs')
            case = m.FAULTS[row['name']]['source'] if phase == 'fault' else row['name']
            expected,coverage = m.specification(source,case)
            if wanted != expected or row['coverage'] != coverage: raise ValueError('saved independent expectation differs')
            if any(row[k] != v for k,v in row_check(m,phase,row['name'],row['trial'],actual,wanted).items()): raise ValueError('saved comparison differs')
            if row['sha256'] != {n:hashlib.sha256(b).hexdigest() for n,b in [('input',source),('expected',wanted),('actual',actual)]}:
                raise ValueError('saved arena hashes differ')
            rows.append(row)
    keys = [(r['name'],r['trial'],r['engine']) for r in rows]
    if len(set(keys)) != len(keys) or set(keys) != expected_keys(m,phase) or rows != result['rows']:
        raise ValueError('saved domain/row ordering differs')
    for key in ('frames','matches','controls','required_faults','specified_faults'):
        value = len(rows) if key == 'frames' else sum(r[{'matches':'match','required_faults':'required_fault','specified_faults':'specified_fault'}.get(key,key)] for r in rows)
        if result[key] != value: raise ValueError('saved aggregate differs')
    put(a.out/'REPORT.json',dict(passed=True,phase=phase,host_opt=result['host_opt'],physical_single_xz_eof_exact=True,
                               result=record(a.audit/'RESULTS.json'),frames=record(a.audit/'frames.xz'),new_target_executions=0))


def main():
    p = argparse.ArgumentParser(); p.add_argument('action',choices=['prepare-before','prepare-after','run','audit'])
    p.add_argument('--out',required=True,type=Path); p.add_argument('--raw',type=Path); p.add_argument('--before',type=Path); p.add_argument('--after',type=Path)
    p.add_argument('--producer',type=Path); p.add_argument('--producer-sha256'); p.add_argument('--runtime',type=Path); p.add_argument('--runtime-sha256')
    p.add_argument('--producer-build',type=Path); p.add_argument('--sdk-manifest',type=Path)
    p.add_argument('--phase',choices=['before','after','fault']); p.add_argument('--host-opt',choices=['O0','O2'])
    p.add_argument('--cc',type=Path); p.add_argument('--llc',type=Path); p.add_argument('--opt',type=Path); p.add_argument('--include-root',type=Path)
    for name in ('calibration','calibration-audit','fault-result','fault-audit'): p.add_argument('--'+name,type=Path,nargs=2)
    p.add_argument('--audit',type=Path); a = p.parse_args(); a.out.mkdir(parents=True,exist_ok=False)
    m = model(); commands = Commands(a.out)
    try:
        if a.action.startswith('prepare'): prepare(a,m,commands,a.action=='prepare-after')
        elif a.action == 'run':
            if not a.raw or not a.before or not a.phase or not a.host_opt: raise ValueError('explicit raw/before/phase/optimization required')
            raw_binding(a.raw,m); run(a,m,commands)
        else: audit(a,m)
    except Exception as error:
        put(a.out/'FAILURE.json',dict(error=type(error).__name__+': '+str(error),commands=commands.rows,runner_bindings=own_binding()))
        raise


if __name__ == '__main__': main()
