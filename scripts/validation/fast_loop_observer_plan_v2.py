#!/usr/bin/env python3
"""Plan from independently verified final module and source-PC CFF family."""
import json,hashlib,sys
from pathlib import Path
import guarded_loop_family_audit as family
import guarded_target_sets_real_audit as audit
root=Path(sys.argv[1]);before=json.loads((root/'module.before.json').read_text());after=json.loads((root/'module.after.json').read_text());manifest=json.loads((root/'manifest.json').read_text());result=family.evaluate(before,after,manifest);entry=audit.closure.admission(after,manifest);fast_nodes={n for group in result['closure']['fast_cycle_sccs'] for n in group};sites=[]
def add(block,kind,label,**kw):sites.append(dict(id=len(sites),block=block,kind=kind,label=label,**kw))
add(after['blocks'][0]['name'],'visit','root_entry');add(entry['block'],'branch','entry_admission',slot=0);add(entry['failure'],'visit','entry_fallback')
for g in after['same_value_guards']:
 add(g['block'],'branch','target_success',slot=0,in_fast_scc=g['block'] in fast_nodes)
 add(g['block'],'branch','target_failure',slot=1,in_fast_scc=g['block'] in fast_nodes)
sw={s['block']:s for s in after['switches']};pc=family.pc_map(after)
for g in result['actual_sets']:
 full=sw[g['fallback']];assert full['default'] in pc;label='target' if pc[full['default']]==result['central_source_pc'] else 'business';values=[audit.closure.int_value(c['value']) for c in g['cases']]
 add(g['block'],'membership',label+'_success',values=values,member=True,in_fast_scc=g['block'] in fast_nodes)
 add(g['block'],'membership',label+'_failure',values=values,member=False,in_fast_scc=g['block'] in fast_nodes)
for block in result['central_blocks']:add(block,'visit','central_dispatcher')
family.assert_central_coverage(result['central_blocks'],[s['block'] for s in sites if s['label']=='central_dispatcher'])
plan=dict(schema='final-ir-loop-observation-v1',function=manifest['view_symbol'],root_symbol=manifest['root_symbol'],input_ir_sha256=hashlib.sha256((root/'raw.ll').read_bytes()).hexdigest(),sites=sites,central_blocks=result['central_blocks'],central_source_pc=result['central_source_pc'],source_pc_groups=result['source_pc_groups'],fast_sccs=result['closure']['fast_cycle_sccs'],conditional_closure=result['closure']['guard_success_entry_closure'],native_inputs=[dict(index=i,sha256=hashlib.sha256((root/f'input-{i}.bin').read_bytes()).hexdigest(),expected_output_sha256=hashlib.sha256((root/f'output-{i}.bin').read_bytes()).hexdigest()) for i in range(4)],scope='View-local single invocation only; final instrumented IR gets no optimization; no new Guest LOAD; ordinary recursive callees not claimed dispatcher-free.')
(root/'PLAN.json').write_text(json.dumps(plan,indent=2)+'\n');(root/'FAMILY-REVIEW.json').write_text(json.dumps(result,indent=2)+'\n');print(len(sites),len(result['central_blocks']),plan['conditional_closure'])
