"""Small staged whole-arena native oracle for seeded Guest-cell joint invariants.
Every target CLI still requires the external 512MiB/64MiB/180s guard. Emit is
stdlib-only. Before C/Python/LLVM O0/O2 plus saved audits precede every rewrite.
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
import time
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value

common = module('seeded_cell_generic_io', HERE/'dispatch_corridor_native.py')
model = module('seeded_cell_independent_model', HERE/'tigress_seeded_cell_native_source.py')
sha, put, record = common.sha, common.put, common.record


class Commands(common.Commands):
    """Keep the existing 180s command contract and retain even failing stdout."""
    def run(self, argv, data=None, env=None, stderr_path=None):
        index=len(self.rows);start=time.monotonic();failure=None
        try:
            result=subprocess.run([str(x) for x in argv],input=data,capture_output=True,env=env,timeout=180)
            code,out,err=result.returncode,result.stdout,result.stderr
        except subprocess.TimeoutExpired as e:
            code,out,err=None,e.stdout or b'',e.stderr or b'';failure='180s subprocess timeout'
        op=self.out/(f'command-{index:03d}.stdout');ep=Path(stderr_path) if stderr_path else self.out/(f'command-{index:03d}.stderr')
        op.write_bytes(out);ep.write_bytes(err)
        row=dict(argv=[str(x) for x in argv],exit=code,seconds=time.monotonic()-start,
                 stdout_path=str(op),stdout_size=len(out),stdout_sha256=hashlib.sha256(out).hexdigest(),
                 stderr_path=str(ep),stderr_size=len(err),stderr_sha256=hashlib.sha256(err).hexdigest(),failure=failure)
        self.rows.append(row);(self.out/'COMMANDS.json').write_text(json.dumps(self.rows,indent=2)+'\n')
        if code!=0:raise RuntimeError(row)
        return out


def source_pins():
    return {str(p): record(p) for p in (Path(__file__).resolve(), HERE/'tigress_seeded_cell_native_source.py',
            HERE/'dispatch_corridor_native.py', HERE/'tigress_seeded_cell_native_probe.cpp')}


def raw_binding(root, digest):
    if not root or not root.is_absolute() or sha(root/'PLAN.json') != digest:
        raise ValueError('raw root/plan SHA differs')
    plan = json.loads((root/'PLAN.json').read_bytes())
    if plan['source_generator_sha256'] != sha(HERE/'tigress_seeded_cell_native_source.py'):
        raise ValueError('independent model changed')
    common.verify_files(root, plan['files'], 'PLAN.json')
    if (root/'wrapper.c').read_bytes()!=model.WRAPPER.encode() or (root/'reference.c').read_bytes()!=model.C_SPEC.encode():
        raise ValueError('independent C/host protocol differs')
    expected_cases=model.cases(); expected_faults=model.faults()
    for case in expected_cases:
        if (root/case['file']).read_bytes()!=model.llvm_source(case['name']).encode():raise ValueError('LLVM source fixture differs')
    for fault in expected_faults:
        if (root/fault['file']).read_bytes()!=model.llvm_source(fault['source'],fault['name']).encode():raise ValueError('LLVM mutant differs')
    if plan['cases']!=expected_cases or plan['faults']!=expected_faults:raise ValueError('source/fault domain differs')
    if plan['source_fixtures'] != 5 or plan['unique_module_inputs'] != 40 or plan['inputs'] != 8:
        raise ValueError('frozen source/input domain differs')
    for t in range(8):
        if (root/f'input-{t}.bin').read_bytes() != model.initial(t): raise ValueError('input differs')
    for name in model.NAMES:
        for t in range(8):
            if (root/f'{name}.expected-{t}.bin').read_bytes() != model.specification(model.initial(t), name)[0]:
                raise ValueError('frozen expectation differs from independent model')
    return plan, dict(root=str(root), plan_sha256=digest, files=common.files(root))


def tool(path):
    if not path or not path.is_absolute(): raise ValueError('explicit absolute tool path required')
    resolved = path.resolve(strict=True)
    return dict(logical=str(path), resolved=str(resolved), **record(resolved))


def producer_tools(args):
    if not args.producer or not args.runtime or not args.producer_sha256 or not args.runtime_sha256:
        raise ValueError('producer/runtime paths and SHA required')
    p, r = tool(args.producer), tool(args.runtime)
    if p['sha256'] != args.producer_sha256 or r['sha256'] != args.runtime_sha256:
        raise ValueError('producer/runtime pin differs')
    solver = tool(args.solver) if args.solver else None
    if solver and solver['sha256'] != args.solver_sha256: raise ValueError('solver SHA differs')
    if not args.library_path or any(not Path(x).is_absolute() for x in args.library_path):
        raise ValueError('explicit absolute LD library directory list required')
    return dict(producer=p, runtime=r, solver=solver, library_path=args.library_path)


def producer(args, commands, raw, case, out, mode, tools):
    out.mkdir(parents=True, exist_ok=False)
    before, after, report = [out/n for n in ('parsed-before.ll','output.ll','REPORT.json')]
    env = dict(os.environ, LD_LIBRARY_PATH=':'.join(args.library_path), LD_PRELOAD='',
               LD_DEBUG='libs,bindings', PYTHONDONTWRITEBYTECODE='1')
    commands.run([args.producer, mode, raw, case, before, after, report], env=env, stderr_path=out/'loader.stderr')
    result = json.loads(report.read_bytes())
    if (result['mode'] != mode or result['raw_input'] != str(raw) or result['case_manifest'] != str(case)
            or not all(result[k] for k in ('verified_before','verified_after','function_argument_identity','ordinary_definitions_byte_equal'))):
        raise ValueError('producer binding/identity/verification differs')
    mapped = {str(Path(s.split()[-1]).resolve()) for s in result['runtime_maps'] if 'libdragon.so' in s}
    solvers = {str(Path(s.split()[-1]).resolve()) for s in result['runtime_maps'] if 'libz3.so' in s}
    if mapped != {tools['runtime']['resolved']} or solvers != ({tools['solver']['resolved']} if tools['solver'] else set()):
        raise ValueError(('wrong/mixed runtime/solver mapping', mapped, solvers))
    if mode == 'before':
        if result['api_calls'] or not result['compiled_before_only'] or before.read_bytes() != after.read_bytes():
            raise ValueError('before parser called a product or changed IR')
    elif result['api_calls'] != 1 or result['compiled_before_only']:
        raise ValueError('rewrite did not invoke exactly one installed API')
    return result


def prepare_before(args, commands):
    plan, raw = raw_binding(args.raw, args.raw_plan_sha256); tools = producer_tools(args); pins = source_pins()
    symbols = commands.run(['nm','-C', '--defined-only', args.producer]).decode()
    symbols += commands.run(['nm','-D','-C','--undefined-only', args.producer]).decode()
    if 'runClosedImageTableContext(' in symbols or 'runImageTableCheckedContext(' in symbols:
        raise ValueError('before-only parser imports/defines product API')
    (args.out/'parser-symbols.txt').write_text(symbols)
    for name in ('wrapper.c','reference.c'): (args.out/name).write_bytes((args.raw/name).read_bytes())
    reports = []
    for case in plan['cases']:
        cp = args.out/(case['name']+'.json'); put(cp, case['request'])
        reports.append(dict(name=case['name'], result=producer(args, commands, args.raw/case['file'], cp,
                            args.out/'cases'/case['name'], 'before', tools)))
    for fault in plan['faults']:
        reports.append(dict(fault=fault['name'], result=producer(args, commands, args.raw/fault['file'],
                            args.out/(fault['source']+'.json'), args.out/'faults'/fault['name'], 'before', tools)))
    if raw_binding(args.raw,args.raw_plan_sha256)[1] != raw or producer_tools(args) != tools or source_pins() != pins:
        raise ValueError('before inputs/source/tool pins changed')
    manifest = dict(schema='seeded-cell-before-prepared-v1', raw=raw, tools=tools, source_pins=pins,
                    reports=reports, files=common.files(args.out), product_api_calls=0, native_executions=0)
    put(args.out/'BEFORE.json', manifest)
    print(json.dumps(dict(prepared_before=True, before_sha256=sha(args.out/'BEFORE.json'), product_api_calls=0)))


def load_before(root):
    if not root: raise ValueError('before root required')
    value = json.loads((root/'BEFORE.json').read_bytes()); common.verify_files(root,value['files'],'BEFORE.json')
    if value['source_pins'] != source_pins() or value['product_api_calls']:
        raise ValueError('before source/producer scope differs')
    raw_binding(Path(value['raw']['root']),value['raw']['plan_sha256'])
    return value


def require_calibrations(args):
    before=load_before(args.before)
    if not args.calibrations or not args.calibration_audits: raise ValueError('two before results and audits required')
    found=set(); pins=[]
    for r,a in zip(args.calibrations,args.calibration_audits):
        result=json.loads((r/'RESULTS.json').read_bytes());audit=json.loads((a/'REPORT.json').read_bytes())
        if (not result['passed'] or result['phase']!='before' or result['before_sha256']!=sha(args.before/'BEFORE.json') or
                result['source_pins']!=source_pins() or result['frames']!=80 or result['matches']!=80 or
                result['controls']!=80*len(model.CONTROLS) or not audit['passed'] or not audit['eof_exact'] or
                audit['result_sha256']!=sha(r/'RESULTS.json') or audit['stream_sha256']!=sha(r/'frames.xz') or
                audit['host_opt']!=result['host_opt']): raise ValueError('failed/incomplete/mismatched before calibration')
        found.add(result['host_opt']);pins.append(dict(result=dict(path=str(r/'RESULTS.json'),**record(r/'RESULTS.json')),
                                                    audit=dict(path=str(a/'REPORT.json'),**record(a/'REPORT.json'))))
    if found!={'O0','O2'}:raise ValueError('distinct O0/O2 calibrations missing')
    return before,pins


def prepare_after(args,commands):
    before,calibrations=require_calibrations(args);tools=producer_tools(args);pins=source_pins()
    defined=commands.run(['nm','-C','--defined-only',args.producer]).decode()
    undefined=commands.run(['nm','-D','-C','--undefined-only',args.producer]).decode()
    if 'runClosedImageTableContext(' in defined or 'runClosedImageTableContext(' not in undefined:
        raise ValueError('installed API must be UND in producer; no interposed core object')
    (args.out/'producer-symbols.txt').write_text(defined+'\nUNDEFINED\n'+undefined)
    raw=Path(before['raw']['root']);plan,_=raw_binding(raw,before['raw']['plan_sha256']);reports=[]
    for case in plan['cases']:
        cp=args.before/(case['name']+'.json')
        report=producer(args,commands,raw/case['file'],cp,args.out/'cases'/case['name'],'rewrite',tools)
        s=report['stats'];expected=case['expect_closed']
        if s['closed']!=expected or s['changed']!=expected or s['budget_exhausted']:
            raise ValueError(('proof gain/refusal gate differs',case['name'],s))
        if not expected and not report['byte_equal']:raise ValueError('refusal changed IR')
        if report['target_loads_before']!=1 or report['target_loads_after']!=1:raise ValueError('actual target LOAD disappeared/duplicated')
        if report['cell_loads_before']!=1 or report['cell_loads_after']!=1:
            raise ValueError('selected actual cell LOAD disappeared/duplicated')
        reports.append(dict(name=case['name'],result=report))
    if producer_tools(args)!=tools or source_pins()!=pins or require_calibrations(args)[1]!=calibrations:
        raise ValueError('source/tools/calibration pins changed')
    put(args.out/'AFTER.json',dict(schema='seeded-cell-after-prepared-v1',before_sha256=sha(args.before/'BEFORE.json'),
          source_pins=pins,tools=tools,calibrations=calibrations,reports=reports,files=common.files(args.out),api_calls=5,
          accepted_sources=2,refused_byte_exact_sources=3,scope='Installed opt-in closed Image joint table/cell consumer; no default pipeline change'))
    print(json.dumps(dict(prepared_after=True,after_sha256=sha(args.out/'AFTER.json'),api_calls=5)))


def load_after(root,before):
    if not root:raise ValueError('after root required')
    x=json.loads((root/'AFTER.json').read_bytes());common.verify_files(root,x['files'],'AFTER.json')
    if x['before_sha256']!=sha(before/'BEFORE.json') or x['source_pins']!=source_pins() or x['api_calls']!=5:
        raise ValueError('after binding differs')
    return x


def domain(phase):
    if phase=='fault': return [(source,name) for name,source in model.FAULTS]
    return [(name,'source') for name in model.NAMES]


def row_keys(phase):
    engines=('C','LLVM') if phase=='before' else ('LLVM',)
    return {(n,v,t,e) for n,v in domain(phase) for t in range(8) for e in engines}


def compare_row(phase,name,variant,trial,engine,opt,source,actual):
    wanted,coverage=model.specification(source,name);match=actual==wanted
    rule=model.FAULT_RULES.get(variant)
    offset=rule['offset'] if rule else None
    required=phase=='fault' and rule is not None and trial in rule['trials']
    detected=actual[offset:offset+8]!=wanted[offset:offset+8] if required else None
    controls=common.controls(actual,wanted,model) if phase!='fault' else 0
    row=dict(name=name,variant=variant,trial=trial,engine=engine,phase=phase,opt=opt,match=match,coverage=coverage,
             controls=controls,specified_required=required,specified_detected=detected,specified_offset=offset,
             sha256={k:hashlib.sha256(v).hexdigest() for k,v in [('input',source),('expected',wanted),('actual',actual)]})
    if (phase!='fault' and (not match or controls!=len(model.CONTROLS))) or (required and (match or not detected)):
        raise ValueError(('full-state mismatch/missed specified fault/comparator',row))
    return row,wanted


def native(args,commands):
    before=load_before(args.before)
    if args.phase!='before':require_calibrations(args)
    if args.phase=='after':load_after(args.after,args.before)
    if not args.host_opt or not args.phase:raise ValueError('phase and O0/O2 required')
    tools={k:tool(getattr(args,k)) for k in ('cc','llc')};pins=source_pins();before_pin=record(args.before/'BEFORE.json');after_pin=record(args.after/'AFTER.json') if args.phase=='after' else None
    rows=[];failure=None
    try:
        if args.phase=='before':commands.run([args.cc,'-'+args.host_opt,args.before/'wrapper.c',args.before/'reference.c','-o',args.out/'reference.native'])
        with lzma.open(args.out/'frames.xz','xb',preset=1) as stream:
            for name,variant in domain(args.phase):
                work=args.out/(name+'-'+variant);work.mkdir()
                ir=(args.before/'faults'/variant/'parsed-before.ll') if args.phase=='fault' else (args.after/'cases'/name/'output.ll') if args.phase=='after' else args.before/'cases'/name/'parsed-before.ll'
                commands.run([args.llc,'-mtriple=aarch64-unknown-linux-gnu','-filetype=obj','-relocation-model=pic','-'+args.host_opt,ir,'-o',work/'target.o'])
                commands.run([args.cc,'-'+args.host_opt,args.before/'wrapper.c',work/'target.o','-o',work/'target.native'])
                engines=[('LLVM',work/'target.native')]
                if args.phase=='before':engines.insert(0,('C',args.out/'reference.native'))
                for engine,exe in engines:
                    data=commands.run([exe,model.NAMES.index(name)],b''.join(model.initial(t) for t in range(8)))
                    (work/(engine+'.stdout.bin')).write_bytes(data)
                    if len(data)!=8*model.FRAME:raise ValueError('native output length/EOF differs')
                    for t in range(8):
                        source=model.initial(t);actual=data[t*model.FRAME:(t+1)*model.FRAME]
                        row,wanted=compare_row(args.phase,name,variant,t,engine,args.host_opt,source,actual)
                        encoded=json.dumps(row,sort_keys=True).encode();stream.write(struct.pack('<I',len(encoded))+encoded+source+wanted+actual);rows.append(row)
        if {(r['name'],r['variant'],r['trial'],r['engine']) for r in rows}!=row_keys(args.phase) or len(rows)!=len(row_keys(args.phase)):
            raise ValueError('duplicate/incomplete native domain')
        if source_pins()!=pins or record(args.before/'BEFORE.json')!=before_pin or {k:tool(getattr(args,k)) for k in tools}!=tools:
            raise ValueError('source/compiler/before pins changed')
        load_before(args.before)
        if args.phase=='after':load_after(args.after,args.before)
        if after_pin and record(args.after/'AFTER.json')!=after_pin:raise ValueError('after pin changed')
    except Exception as e:failure=type(e).__name__+': '+str(e)
    result=dict(schema='seeded-cell-native-result-v1',passed=failure is None,failure=failure,phase=args.phase,host_opt=args.host_opt,
        source_pins=pins,tools=tools,before_sha256=sha(args.before/'BEFORE.json'),after_sha256=sha(args.after/'AFTER.json') if args.phase=='after' else None,
        rows=rows,frames=len(rows),matches=sum(r['match'] for r in rows),controls=sum(r['controls'] for r in rows),
        specified_faults=sum(r['specified_required'] for r in rows),specified_detections=sum(r['specified_detected'] is True for r in rows),
        unique_module_inputs=len({(r['name'],r['trial']) for r in rows}),unique_initial_arenas=len({r['sha256']['input'] for r in rows}),
        frame_stream_sha256=sha(args.out/'frames.xz') if (args.out/'frames.xz').exists() else None,commands=commands.rows,
        exclusions=0,skips=0,normalizations=0,new_API_calls=0,scope='Whole66112B physical arena; Guest/RegBytes alias exact; all7 Exit fields+guest return; descriptors checked unchanged; no target instruction corpus claim')
    put(args.out/'RESULTS.json',result);print(json.dumps({k:result[k] for k in ('passed','failure','phase','frames','matches','controls','specified_faults','specified_detections')}))
    if failure:raise SystemExit(1)


def audit(args):
    load_before(args.before);result=json.loads((args.audit/'RESULTS.json').read_bytes())
    if not result['passed'] or result['source_pins']!=source_pins() or result['before_sha256']!=sha(args.before/'BEFORE.json') or result['frame_stream_sha256']!=sha(args.audit/'frames.xz'):
        raise ValueError('saved result/preparation/source/stream differs')
    if result['phase']=='after':
        load_after(args.after,args.before)
        if result['after_sha256']!=sha(args.after/'AFTER.json'):raise ValueError('saved after binding differs')
    common.single_xz_eof(args.audit/'frames.xz');rows=[];keys=set()
    with lzma.open(args.audit/'frames.xz','rb') as f:
        while True:
            head=f.read(4)
            if not head:break
            if len(head)!=4:raise ValueError('truncated prefix')
            size=struct.unpack('<I',head)[0]
            if not 0<size<8192:raise ValueError('bad row size')
            data=f.read(size)
            if len(data)!=size:raise ValueError('truncated row')
            saved=json.loads(data);source,wanted,actual=(f.read(model.FRAME) for _ in range(3))
            if any(len(x)!=model.FRAME for x in (source,wanted,actual)):raise ValueError('truncated arena')
            key=(saved['name'],saved['variant'],saved['trial'],saved['engine'])
            if key in keys or key not in row_keys(result['phase']) or source!=model.initial(saved['trial']):raise ValueError('duplicate/out-of-domain source')
            keys.add(key);row,expected=compare_row(result['phase'],saved['name'],saved['variant'],saved['trial'],saved['engine'],result['host_opt'],source,actual)
            if saved!=row or wanted!=expected:raise ValueError('saved model/comparison row differs')
            rows.append(row)
    if keys!=row_keys(result['phase']) or rows!=result['rows']:raise ValueError('incomplete stream/results disagree')
    aggregate=dict(frames=len(rows),matches=sum(r['match'] for r in rows),controls=sum(r['controls'] for r in rows),specified_faults=sum(r['specified_required'] for r in rows),specified_detections=sum(r['specified_detected'] is True for r in rows),unique_module_inputs=len({(r['name'],r['trial']) for r in rows}),unique_initial_arenas=len({r['sha256']['input'] for r in rows}))
    if any(result[k]!=v for k,v in aggregate.items()):raise ValueError('saved aggregate differs')
    put(args.out/'REPORT.json',dict(schema='seeded-cell-saved-audit-v1',passed=True,eof_exact=True,single_xz_CRC_EOF_no_trailing=True,
        phase=result['phase'],host_opt=result['host_opt'],result_sha256=sha(args.audit/'RESULTS.json'),stream_sha256=sha(args.audit/'frames.xz'),
        source_pins=source_pins(),**aggregate,new_target_executions=0))
    print(json.dumps(dict(saved_audit_passed=True,frames=len(rows))))


def main():
    ap=argparse.ArgumentParser(description=__doc__);act=ap.add_mutually_exclusive_group(required=True)
    act.add_argument('--prepare-before',action='store_true');act.add_argument('--prepare-after',action='store_true');act.add_argument('--native',action='store_true');act.add_argument('--audit',type=Path)
    ap.add_argument('--out',type=Path,required=True);ap.add_argument('--raw',type=Path);ap.add_argument('--raw-plan-sha256');ap.add_argument('--before',type=Path);ap.add_argument('--after',type=Path)
    ap.add_argument('--producer',type=Path);ap.add_argument('--producer-sha256');ap.add_argument('--runtime',type=Path);ap.add_argument('--runtime-sha256')
    ap.add_argument('--solver',type=Path);ap.add_argument('--solver-sha256');ap.add_argument('--library-path',nargs='+')
    ap.add_argument('--calibrations',nargs=2,type=Path);ap.add_argument('--calibration-audits',nargs=2,type=Path)
    ap.add_argument('--phase',choices=('before','after','fault'));ap.add_argument('--host-opt',choices=('O0','O2'));ap.add_argument('--cc',type=Path);ap.add_argument('--llc',type=Path)
    a=ap.parse_args()
    if not a.out.is_absolute():raise ValueError('new absolute output path required')
    a.out.mkdir(parents=True,exist_ok=False);commands=Commands(a.out)
    if a.prepare_before:prepare_before(a,commands)
    elif a.prepare_after:prepare_after(a,commands)
    elif a.native:native(a,commands)
    else:audit(a)

if __name__=='__main__':main()
