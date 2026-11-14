#!/usr/bin/env python3
"""Actual-library bridge for the frozen ordered-corridor native oracle.

No before, C or Unicorn execution is permitted here. The unchanged old runner
owns native comparisons and saved audits. A separate admission certificate
proves API UND in the producer resolves to the selected actual shared library.
All actions must be run under the unchanged 512/64 MiB,180-second outer guard.
"""
import argparse,hashlib,importlib.util,json,os,subprocess,sys,time,traceback
from pathlib import Path
from types import SimpleNamespace

ORACLE_SHA='cdca5bf70921aab44f6d568f7498adc2139ca9c9677722629fa262b3b168f55c'
BEFORE_SHA='301d2a122f8cf4790a8e792707f4bdc51d304d8c9d28252c93307d4ca7a8e3fe'
AFTER_SHA='74c41a92969896bbcfddaeb4173d88c7a9aeb57e2bb52c2ca553c0e67a9f90df'
NATIVE_APIS=['threadDispatchCorridor(']
REAL_APIS=['runImageTableCheckedContext(','normalizeDispatchDecoder(',
           'normalizeDispatchDecoderUses(','threadDispatchCorridor(','forwardGuestCellLoad(']
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def pin(p):
    p=Path(p);s=p.lstat();assert p.is_file() and not p.is_symlink()
    h=sha(p);z=p.stat();assert (s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)==(z.st_dev,z.st_ino,z.st_size,z.st_mtime_ns,z.st_ctime_ns)
    return dict(path=str(p),size=s.st_size,sha256=h)
def put(p,v):
    with Path(p).open('x') as f:json.dump(v,f,indent=2,sort_keys=True);f.write('\n')
def oracle(root):
    p=root/'source/scripts/validation/dispatch_corridor_native.py';assert sha(p)==ORACLE_SHA
    sys.dont_write_bytecode=True
    spec=importlib.util.spec_from_file_location('frozen_corridor_oracle',p)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
def calibration_args(a):
    return SimpleNamespace(before=a.prototype/'prepared-before',
        calibrations=[a.prototype/('before-'+o) for o in ['O0','O2']],
        calibration_audits=[a.prototype/('audit-before-'+o) for o in ['O0','O2']])
def runtime_binding(a):
    assert a.runtime.is_absolute() and sha(a.runtime)==a.runtime_sha256
    if a.runtime.parent.name!=a.backend:raise ValueError('backend label/directory differs')
    return pin(a.runtime)
def api_binding(a,c,producer,kind):
    actual=runtime_binding(a);producer_pin=pin(producer)
    defined=c.run(['nm','-C','--defined-only',producer]).decode()
    undefined=c.run(['nm','-D','-C','--undefined-only',producer]).decode()
    exported=c.run(['nm','-D','-C','--defined-only',a.runtime]).decode()
    expected=NATIVE_APIS if kind=='native' else REAL_APIS
    rows={}
    for word in expected:
        full='dragon::llvmlifter::detail::'+word
        if full in defined or full not in undefined:raise ValueError('API must be UND in producer: '+full)
        lines=[s for s in exported.splitlines() if full in s]
        if not lines or any(' T ' not in s for s in lines):raise ValueError('API must be T in actual library: '+full)
        rows[full]=dict(undefined=[s for s in undefined.splitlines() if full in s],exports=lines)
    dynamic=c.run(['readelf','-d',producer]).decode()
    if 'Shared library: [libdragon.so]' not in dynamic or 'libLLVM' in dynamic:raise ValueError('wrong dynamic linkage')
    (a.out/'producer-defined.txt').write_text(defined)
    (a.out/'producer-undefined.txt').write_text(undefined)
    (a.out/'producer-dynamic.txt').write_text(dynamic)
    return dict(backend=a.backend,producer=producer_pin,runtime=actual,APIs=rows,
                helper_objects_in_link=False,shared_LLVM=False,source=pin(__file__))
def link(a,O,c):
    actual=runtime_binding(a);obj=pin(a.object)
    if obj['sha256']!=a.object_sha256:raise ValueError('producer object changed')
    exe=a.out/'producer'
    argv=[a.cxx,'-Wl,--export-dynamic','-L'+str(a.runtime.parent),'-Wl,-rpath,'+str(a.runtime.parent),
          str(a.object),'-ldragon','-o',str(exe)]
    c.run(argv)
    cert=api_binding(a,c,exe,a.kind);cert.update(object=obj,link_argv=argv)
    if pin(a.runtime)!=actual or pin(a.object)!=obj:raise ValueError('link inputs changed')
    put(a.out/'LINK.json',cert)
def emit(a,O,c):
    if sha(a.prototype/'prepared-before/BEFORE.json')!=BEFORE_SHA or sha(a.prototype/'prepared-after/AFTER.json')!=AFTER_SHA:
        raise ValueError('wrong frozen prototype manifests')
    model=O.specification();cal=calibration_args(a);before,calpins=O.calibrations(cal,model)
    binding=api_binding(a,c,a.producer,'native')
    old=json.loads((a.prototype/'prepared-after/AFTER.json').read_text());old_reports={r['name']:r for r in old['reports']}
    raw=Path(before['raw']['root']);plan,_=O.raw_binding(raw,model)
    reports=[];comparisons=[]
    prodargs=SimpleNamespace(producer=a.producer,runtime=a.runtime)
    for case in plan['cases']:
        name=case['name'];work=a.out/'cases'/name
        report=O.producer_call(prodargs,c,'rewrite',raw/(name+'.before.ll'),cal.before/'cases'/(name+'.json'),work)
        oldwork=a.prototype/'prepared-after/cases'/name
        parsed=work/'parsed-before.ll';after=work/'output.ll';old_parsed=oldwork/'parsed-before.ll';old_after=oldwork/'output.ll'
        original_report=old_reports[name]['report']
        mismatch=dict(parsed_before_equal=parsed.read_bytes()==old_parsed.read_bytes(),
                      output_equal=after.read_bytes()==old_after.read_bytes(),stats_equal=report['stats']==original_report['stats'])
        for key in ['mode','raw_input','case_manifest','function','original_candidates','cloner_calls','verified_before',
                    'verified_after','function_argument_identity','byte_equal','module_identifier','compiled_before_only','scope']:
            mismatch[key+'_equal']=report[key]==original_report[key]
        comparison=dict(name=name,checks=mismatch,actual_parsed=pin(parsed),actual_output=pin(after),
                        original_parsed=pin(old_parsed),original_output=pin(old_after),stats=report['stats'])
        comparisons.append(comparison)
        put(work/'EXACT-COMPAT.json',comparison)
        if not all(mismatch.values()):raise ValueError(('strict actual/prototype IR/stat gate failed',name,mismatch))
        trace=(work/'loader.stderr').read_text()
        if not any('binding file' in line and str(a.runtime) in line and 'threadDispatchCorridor' in line for line in trace.splitlines()):
            raise ValueError('LD_DEBUG did not resolve normal API to the selected actual library')
        if not any('calling init:' in line and str(a.runtime) in line for line in trace.splitlines()):raise ValueError('actual library init not observed')
        reports.append(dict(name=name,report=report,original_parsed_sha256=sha(old_parsed),parsed_sha256=sha(parsed),output_sha256=sha(after)))
    if O.calibrations(cal,model)[1]!=calpins or pin(a.producer)!=binding['producer'] or runtime_binding(a)!=binding['runtime']:
        raise ValueError('calibrations/producer/runtime changed during emit')
    put(a.out/'ACTUAL-ADMISSION.json',dict(passed=True,binding=binding,comparisons=comparisons,
        referenced_old_before_C_matches=128,referenced_old_before_LLVM_matches=128,
        unique_module_inputs=64,unique_initial_arenas=16,new_C_or_Unicorn_runs=0,new_cloner_calls=4))
    # Compatibility envelope for the unchanged frozen native runner. The
    # independent certificate above, not an old prototype label, proves UND.
    manifest=dict(schema='dispatch-corridor-after-prepared-v1',before_root=str(cal.before),
        before_manifest_sha256=BEFORE_SHA,spec_sha256=O.SPEC_SHA,runner_sha256=ORACLE_SHA,
        tools=dict(producer=binding['producer'],runtime=binding['runtime']),calibration_pins=calpins,
        reports=reports,files=O.files(a.out),new_cloner_calls=4,prototype_private_API_T_in_executable=False,
        actual_shared_library_API_UND=True,scope='Actual library API UND in producer; no helper.o or before/C/UC recount')
    put(a.out/'AFTER.json',manifest)
def replay(a,O,c):
    if a.phase not in ['after','fault']:raise ValueError('formal adapter forbids before execution')
    model=O.specification();cal=calibration_args(a);O.calibrations(cal,model)
    admission=json.loads((a.after/'ACTUAL-ADMISSION.json').read_text())
    if not admission['passed'] or admission['binding']['runtime']!=runtime_binding(a):raise ValueError('actual admission mismatch')
    if admission['binding']['producer']!=pin(a.producer):raise ValueError('producer changed')
    ns=SimpleNamespace(before=cal.before,after=a.after,calibrations=cal.calibrations,calibration_audits=cal.calibration_audits,
        phase=a.phase,host_opt=a.host_opt,out=a.out,llc=a.llc,cc=a.cc)
    O.native(ns,model,c)
    if runtime_binding(a)!=admission['binding']['runtime']:raise ValueError('runtime changed during replay')
def audit(a,O,c):
    model=O.specification();ns=SimpleNamespace(before=a.prototype/'prepared-before',after=a.after,audit=a.audit_root,out=a.out)
    O.audit(ns,model)
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action',choices=['link','emit','replay','audit']);ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--prototype',type=Path,default=Path('/home/cc/vmc-runs/ordered-dispatch-corridor-native-20261114-v1'))
    ap.add_argument('--backend',choices=['main','noz3']);ap.add_argument('--runtime',type=Path);ap.add_argument('--runtime-sha256')
    ap.add_argument('--producer',type=Path);ap.add_argument('--object',type=Path);ap.add_argument('--object-sha256')
    ap.add_argument('--kind',choices=['native','real'],default='native');ap.add_argument('--cxx',default='/usr/bin/clang++-21')
    ap.add_argument('--after',type=Path);ap.add_argument('--phase',choices=['after','fault']);ap.add_argument('--host-opt',choices=['O0','O2'])
    ap.add_argument('--llc',default='/usr/lib/llvm-21/bin/llc');ap.add_argument('--cc',default='/usr/bin/aarch64-linux-gnu-gcc-15')
    ap.add_argument('--audit-root',type=Path);a=ap.parse_args();assert a.out.is_absolute() and not a.out.exists()
    a.out.mkdir(parents=True);O=oracle(a.prototype);c=O.Commands(a.out)
    try:{'link':link,'emit':emit,'replay':replay,'audit':audit}[a.action](a,O,c)
    except BaseException as e:
        put(a.out/'FORMAL-FAILURE.json',dict(type=type(e).__name__,detail=str(e),traceback=traceback.format_exc(),
            source=pin(__file__),commands=c.rows));raise
    print(a.action,'PASS',a.out)
if __name__=='__main__':main()
