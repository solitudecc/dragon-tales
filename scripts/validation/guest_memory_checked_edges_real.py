"""Frozen real CFF replay for checked-edge memory proofs.

Reuses the unchanged full-state comparator and its finite four-ELF input set.
--ssa-after-gvn is a separate matched-pipeline experiment; use exactly the same
placement on baseline/final libraries. Unknown table cells remain unknown.
"""
import argparse, hashlib, json, re, sys, time
from pathlib import Path
sys.dont_write_bytecode=True
import dragon as d
import finite_vm_replay as replay
import static_region_replay as region
study=replay.study; tf=study.tf
# Validation-only sites from the frozen four-ELF study. These addresses never
# enter analysis, target selection, or any transformation.
CFF_JUMPS={0x102d8,0x100b6,0x10adc,0x101f6}
# Validation-only instruction identities, never supplied to the optimizer.
CFF_GUARDS={0x102a0:0x102ba,0x10080:0x100b0,0x10770:0x10ab5,0x10090:0x101f0}

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,x):Path(p).write_text(json.dumps(x,indent=2,default=list)+'\n')

def state_guard_edges(before,after,entry):
    """Measure actual incoming-edge copies, separately from RIP routing folds."""
    name=f'dragon_static_entry_x86_64_{entry:x}'
    def blocks(ir):
        match=re.search(r'^define [^\n]*@'+name+r'\([^\n]*\).*?^}',ir,re.M|re.S)
        if not match:raise RuntimeError('missing measured root')
        return {label:body for label,body in re.findall(
            r'^([\w.]+):[^\n]*\n(.*?)(?=^[\w.]+:|^})',match[0],re.M|re.S)}
    old,new=blocks(before),blocks(after)
    pc=CFF_GUARDS.get(entry);guard=f'bb_{pc}' if pc is not None else None
    if guard not in old or guard not in new:
        return dict(guard_pc=pc,guard_block=guard,guard_present=False,redirects=[])
    def successors(body):return re.findall(r'label %([\w.]+)',body)
    old_targets=successors(old[guard]);new_targets=successors(new[guard])
    branch=re.search(r'^\s*br i1 (%[\w.]+),',old[guard],re.M)
    compare=(re.search(r'^\s*'+re.escape(branch[1])+r' = icmp (\w+) i(\d+) (%[\w.]+), (-?\d+)\s*$',
                      old[guard],re.M) if branch else None)
    state_phi=(re.search(r'^\s*'+re.escape(compare[3])+r' = phi i'+compare[2]+r' (.*)$',
                        old[guard],re.M) if compare else None)
    condition=(dict(predicate=compare[1],width=int(compare[2]),state_phi=compare[3],rhs=int(compare[4]))
               if compare and state_phi else None)
    incoming=[label for label,body in old.items() if guard in successors(body)]
    redirects=[]
    for predecessor in incoming:
        if predecessor not in new:continue
        for copy in successors(new[predecessor]):
            if copy in old or copy not in new:continue
            target=re.search(r'^\s*br label %([\w.]+)\s*$',new[copy],re.M)
            if target and target[1] in old_targets:
                constants=re.findall(r'\[\s*(-?\d+),\s*%'+re.escape(predecessor)+r'\s*\]',old[guard])
                state_constant=(re.search(r'\[\s*(-?\d+),\s*%'+re.escape(predecessor)+r'\s*\]',state_phi[1])
                                if state_phi else None)
                redirects.append(dict(predecessor=predecessor,copy=copy,target=target[1],
                    incoming_constants=[int(v) for v in constants],
                    state_incoming_constant=int(state_constant[1]) if state_constant else None,
                    copy_loads=len(re.findall(r'= load ',new[copy])),
                    original_loads=len(re.findall(r'= load ',old[guard]))))
    unknown=[label for label in incoming if label in new and guard in successors(new[label])]
    return dict(guard_pc=pc,guard_block=guard,guard_present=True,
        before_conditional=bool(re.search(r'^\s*br i1 ',old[guard],re.M)),
        after_conditional=bool(re.search(r'^\s*br i1 ',new[guard],re.M)),
        condition=condition,
        original_successors=old_targets,remaining_successors=new_targets,
        original_incoming=incoming,unknown_incoming_retained=unknown,redirects=redirects)

def state_guard_gain_valid(record):
    """Strict identity check for the frozen f-cff O2 initial RCX=50 edge."""
    condition=record.get('condition') or {}
    return bool(record.get('before_conditional') and record.get('after_conditional') and
        record.get('unknown_incoming_retained') and condition.get('predicate')=='ugt' and
        condition.get('width')==64 and condition.get('rhs')==50 and
        condition.get('state_phi','').startswith('%RCX.') and
        len(record.get('original_successors',[]))==2 and any(
            row.get('state_incoming_constant')==50 and row['target']==record['original_successors'][1]
            for row in record.get('redirects',[])))

def root_counts(ir,entry):
    name=f'dragon_static_entry_x86_64_{entry:x}'
    m=re.search(r'^define [^\n]*@'+name+r'\([^\n]*\).*?^}',ir,re.M|re.S)
    if not m:return {'missing_root':True}
    body=m[0];rows=[]
    source_pointers=set(re.findall(r'(%[\w.]+) = getelementptr(?: inbounds| nusw| nuw)* i8, ptr %exit, i(?:32|64) 16\b',body))
    for sw in re.finditer(r'^\s*switch ([^\n]+)\[\n(.*?)^\s*\]',body,re.M|re.S):
        default=re.search(r'label %([\w.]+)',sw[1])
        label=default[1] if default else None
        default_body=re.search(r'^'+re.escape(label or 'absent')+r':[^\n]*\n(.*?)(?=^[\w.]+:|^})',body,re.M|re.S)
        constants=[]
        if default_body:
            constants=[int(value) for value,pointer in re.findall(r'store i64 (\d+), ptr (%[\w.]+)',default_body[1]) if pointer in source_pointers]
        source_pc=constants[0] if len(set(constants))==1 else None
        routing='RIP-entry-router' if 'entry.unsupported' in sw[1] else 'runtime-target-coverage'
        rows.append(dict(head=sw[1].strip(),cases=len(re.findall('label ',sw[2])),
                         kind=routing,default=label,source_pc=source_pc,
                         dispatch_kind=('RIP-entry-router' if routing=='RIP-entry-router' else
                             'CFF' if source_pc in CFF_JUMPS else
                             'original-switch' if source_pc is not None else 'unclassified'),
                         line=ir[:m.start()+sw.start()].count('\n')+1))
    return dict(switches=rows,RIP_entry_switches=sum(x['kind']=='RIP-entry-router' for x in rows),
                runtime_target_switches=sum(x['kind']=='runtime-target-coverage' for x in rows),
                CFF_switches=sum(x['dispatch_kind']=='CFF' for x in rows),
                original_switches=sum(x['dispatch_kind']=='original-switch' for x in rows),
                unclassified_target_switches=sum(x['dispatch_kind']=='unclassified' for x in rows),
                jmp_unresolved_labels=len(re.findall(r'^jmp.unresolved',body,re.M)),
                loads=len(re.findall(r'= load ',body)),instructions=sum(l.startswith('  ') and not l.strip().startswith(';') for l in body.splitlines()))

def main():
    p=argparse.ArgumentParser();p.add_argument('--frozen',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--tigress',required=True)
    p.add_argument('--ssa-after-gvn',action='store_true',help='also run Guest SSA after GVN; match baseline/final placement')
    group=p.add_mutually_exclusive_group()
    group.add_argument('--baseline-state-guards',action='store_true',help='measure guards and require zero edge copies')
    group.add_argument('--require-state-guard-gain',action='store_true',help='require an actual f-cff O2 known-edge copy')
    a=p.parse_args();a.out=a.out.resolve();a.frozen=a.frozen.resolve()
    a.out.mkdir(parents=True,exist_ok=False)
    files=[a.frozen/profile/level/'fixture.elf' for profile in ('f-cff','f-combo') for level in ('O0','O2')]
    files += [a.frozen/profile/'transformed.c' for profile in ('f-cff','f-combo')]
    frozen={str(x):sha(x) for x in files};libs=region.mapped_libraries(d)
    save(a.out/'preflight.json',dict(inputs=frozen,libraries=libs,python=str(Path(d.__file__)),harness_sha256=sha(__file__),
          inputs_uint32=[0,1,0x80000000,0xffffffff],memory_bytes=study.MEM_SIZE,read_known_memory=False,
          Graph_finite_dispatch=False,static_entry_policy='function-start',real_Guest_megaInit=True,
          ssa_after_gvn=a.ssa_after_gvn,
          measure_state_guards=a.baseline_state_guards or a.require_state_guard_gain,
          normalization=[],exclusions=[],skips=[]))
    active=None;modules=[];controls=[]
    compile_original=tf.compile_image;lift_original=d.LLVMLifter.lift;opt_original=d.LLVMLifter.optimize
    inputs_original=study.inputs;compare_original=study.guest.Adapter.compare
    def inputs(seed,count):
        if count!=4:raise RuntimeError('this bounded validation declares exactly four inputs')
        return [0,1,0x80000000,0xffffffff]
    def lift(lifter,fn,graph):
        nonlocal active
        if active is None:raise RuntimeError('lift outside module')
        if active['graph'] is None:
            active['graph']=graph;active['snapshot']=region.graph_snapshot(graph)
            active['before_sha']=region.canonical_sha(active['snapshot'])
        if graph is not active['graph']:raise RuntimeError('Graph changed')
        extents=active['symbols'].get(fn.start_address,[]);ranges={(s['start'],s['end']) for s in extents}
        row=dict(entry=fn.start_address,symbols=extents)
        if len(ranges)==1:
            bounds=next(iter(ranges));leaders=region.select_region(bounds,active['snapshot'])['selected_starts']
            extra=sorted(set(leaders)-set(fn.block_addresses()))
            ok=lifter.lift_static_region(fn,graph,extra,rebuild_decoded_blocks=True)
            row.update(method='lift-static-region',bounds=bounds,selected_starts=leaders,additional=extra,accepted=bool(ok))
        else:
            ok=lift_original(lifter,fn,graph);row.update(method='ordinary',accepted=bool(ok))
        active['lifts'].append(row);return ok
    def optimize(lifter,*args,**kwargs):
        if active is None:raise RuntimeError('optimize outside module')
        before=lifter.ir();(active['work']/'before-pipeline.ll').write_text(before)
        def ssa(ctx,opts):
            stats=ctx.target.run_guest_memory_ssa(run_mem2reg=False)
            return d.PassResult(changed=stats.changed,
                status=d.PassStatus.SUCCESS if stats.complete else d.PassStatus.INCOMPLETE,
                counters={k:int(v) for k,v in vars(stats).items() if k!='available'})
        steps=['guest-memory-ssa','mem2reg','instcombine','gvn','mba','predicates','dispatch-specialize','dispatch-deflatten','control-flow','dce']
        if a.ssa_after_gvn:steps.insert(4,'guest-memory-ssa-post-gvn')
        measure=a.baseline_state_guards or a.require_state_guard_gain
        measured={}
        def capture(stage):
            def record(ctx,opts):
                measured[stage]=ctx.target.ir()
                (active['work']/(stage+'-dispatch.ll')).write_text(measured[stage])
                return d.PassResult()
            return record
        if measure:
            index=steps.index('dispatch-specialize')
            steps[index:index]=['measure-before-dispatch']
            steps.insert(index+2,'measure-after-dispatch')
        with d.PassPipeline() as pp:
            pp.register('guest-memory-ssa',ssa)
            if a.ssa_after_gvn:pp.register('guest-memory-ssa-post-gvn',ssa)
            if measure:
                pp.register('measure-before-dispatch',capture('before'),read_only=True)
                pp.register('measure-after-dispatch',capture('after'),read_only=True)
            for name in steps:pp.add(name)
            report=pp.run(lifter,max_iterations=1,max_pass_runs=32)
        after=lifter.ir();(active['work']/'after-pipeline.ll').write_text(after)
        active['pipeline']=report
        if measure:
            if set(measured)!={'before','after'}:raise RuntimeError('missing state-guard snapshots')
            active['state_guard_edges']=state_guard_edges(measured['before'],measured['after'],active['kernel'])
            save(active['work']/'state-guard-edges.json',active['state_guard_edges'])
        active['root_before']=root_counts(before,active['kernel']);active['root_after']=root_counts(after,active['kernel'])
        save(active['work']/'pipeline-report.json',dict(report=report,root_before=active['root_before'],root_after=active['root_after']))
        if not report['ok']:raise RuntimeError('pipeline failed')
        return report
    def compile_image(data,symbols,work,static,product,level,options,budget=10000):
        nonlocal active
        if active is not None or not static or not product or not options.function_entry:raise RuntimeError('unexpected plan')
        active=dict(work=Path(work),kernel=symbols['kernel'],symbols=region.function_symbols(data,study.ELFFile),
                    graph=None,snapshot=None,lifts=[],elf_sha256=hashlib.sha256(data).hexdigest())
        current=active
        try:return compile_original(data,symbols,work,static,product,level,options,budget=budget)
        finally:
            if current['graph'] is not None:
                after=region.canonical_sha(region.graph_snapshot(current['graph']))
                current['Graph_unchanged']=after==current['before_sha']
                if not current['Graph_unchanged']:raise RuntimeError('Graph mutated')
            record={k:v for k,v in current.items() if k not in ('graph','snapshot','symbols')};record['work']=str(record['work'])
            modules.append(record);save(a.out/'modules.json',modules);active=None
    def compare(adapter,f,want,actual,corrupt):
        differences,record=compare_original(adapter,f,want,actual,corrupt)
        if not differences and not corrupt:
            regs=bytearray(actual[0]);regs[0]^=1
            rd,_=compare_original(adapter,f,want,(bytes(regs),actual[1],actual[2],actual[3]),False)
            mem=bytearray(actual[1]);mem[study.base.DATA]^=1
            md,_=compare_original(adapter,f,want,(actual[0],bytes(mem),actual[2],actual[3]),False)
            row=dict(register_detected=any(x['field']=='RAX' for x in rd),memory_detected=any(x['field']=='memory' for x in md),
                     fields=len(adapter.fields),memory_bytes=len(actual[1]))
            row['exit_fields_detected']=[]
            for index in range(7):
                extra=bytearray(actual[3]);extra[index*8]^=1
                ed,_=compare_original(adapter,f,want,(actual[0],actual[1],actual[2],bytes(extra)),False)
                row['exit_fields_detected'].append(any(x['field']=='exit' for x in ed))
            controls.append(row)
            if not row['register_detected'] or not row['memory_detected'] or not all(row['exit_fields_detected']):
                raise RuntimeError('comparator control failed')
        return differences,record
    previous=sys.argv
    tf.compile_image=compile_image;d.LLVMLifter.lift=lift;d.LLVMLifter.optimize=optimize
    study.inputs=inputs;study.guest.Adapter.compare=compare
    status=None
    try:
        sys.argv=[str(Path(replay.__file__)),'--baseline','--frozen',str(a.frozen),'--out',str(a.out/'oracle'),
                  '--tigress',a.tigress,'--function-level','--function-entry','--profile','f-cff','--profile','f-combo',
                  '--c-level','O0','--c-level','O2','--strategy','0','--plans','product-O0','--trials','4']
        save(a.out/'invocation.json',dict(argv=sys.argv,cwd=str(Path.cwd()),started=time.time()))
        status=replay.main()
    finally:
        sys.argv=previous;tf.compile_image=compile_original;d.LLVMLifter.lift=lift_original;d.LLVMLifter.optimize=opt_original
        study.inputs=inputs_original;study.guest.Adapter.compare=compare_original
        stable=frozen=={str(x):sha(x) for x in files} and libs==region.mapped_libraries(d)
        save(a.out/'postflight.json',dict(oracle_returncode=status,runtime_inputs_unchanged=stable,controls=controls))
        if not stable:raise RuntimeError('runtime/input changed')
    raw=json.loads((a.out/'oracle/results.json').read_text())
    from collections import Counter
    summary=dict(returncode=status,C_statuses=dict(Counter(x['status'] for x in raw['c_results'])),
                 LLVM_statuses=dict(Counter(x['status'] for x in raw['results'])),modules=len(modules),
                 Graph_unchanged=all(m.get('Graph_unchanged') for m in modules),controls=controls,root_counts=[
                 dict(work=m['work'],before=m.get('root_before'),after=m.get('root_after')) for m in modules])
    summary['ssa_after_gvn']=a.ssa_after_gvn
    if a.baseline_state_guards or a.require_state_guard_gain:
        summary['state_guard_edges']=[dict(work=m['work'],**m['state_guard_edges']) for m in modules]
        if a.baseline_state_guards:
            summary['state_guard_requirement_ok']=not any(m['state_guard_edges']['redirects'] for m in modules)
        else:
            focus=[m for m in modules if m['kernel']==0x10080]
            summary['state_guard_requirement_ok']=(len(focus)==1 and
                state_guard_gain_valid(focus[0]['state_guard_edges']) and
                focus[0]['root_after']['CFF_switches']==1 and focus[0]['root_after']['original_switches']==1)
    summary['GuestSSA_incomplete_modules']=[str(m['work']) for m in modules if any(
        r['name'].startswith('guest-memory-ssa') and not r['counters'].get('complete')
        for r in m.get('pipeline',{}).get('runs',[]))]
    summary['dispatch_specialize_runs']=[dict(work=m['work'],kernel=m['kernel'],
        status=r['status'],complete=r['status']==d.PassStatus.SUCCESS,
        budget_exhausted=r['counters'].get('budget_exhausted',0),counters=r['counters'])
        for m in modules for r in m.get('pipeline',{}).get('runs',[]) if r['name']=='dispatch-specialize']
    summary['dispatch_specialize_incomplete_modules']=[r['work'] for r in summary['dispatch_specialize_runs']
        if r['status']==d.PassStatus.INCOMPLETE or r['budget_exhausted']]
    summary['exact_coverage']=(len(modules)==4 and summary['C_statuses']=={'match':16} and
        summary['LLVM_statuses']=={'match':16} and len(controls)==16 and
        all(len(x['exit_fields_detected'])==7 and all(x['exit_fields_detected']) for x in controls))
    summary['ok']=status==0 and summary['exact_coverage'] and summary['Graph_unchanged']
    summary['ok']=summary['ok'] and summary.get('state_guard_requirement_ok',True)
    save(a.out/'summary.json',summary);print(json.dumps(summary,indent=2));return 0 if summary['ok'] else 1
if __name__=='__main__':raise SystemExit(main())
