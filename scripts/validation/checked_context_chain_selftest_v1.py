#!/usr/bin/env python3
"""Adversarial tests of the sequential certificate; never runs a producer.

Control raw files are private copies. Existing LLVM inventory/reference tools
only parse/verify them. Original frames and every earlier failure are retained.
"""
import argparse,copy,importlib.util,json,re,subprocess,os,sys
from pathlib import Path
import checked_context_chain_audit_v1 as chain

def pin(prefix):
 ll=Path(str(prefix)+'.ll');j=Path(str(prefix)+'.json')
 return dict(ll=str(ll),json=str(j),ll_sha256=chain.checked.sha(ll),json_sha256=chain.checked.sha(j))

def change_instruction(text,function,old,new):
 start=list(re.finditer(r'^define .*@'+re.escape(function)+r'\(',text,re.M));assert len(start)==1
 first=start[0].start();last=text.index('\n}',first)+2;body=text[first:last]
 assert body.count(old)==1, 'control must change exactly one printed instruction'
 return text[:first]+body.replace(old,new)+text[last:]

def phi_text(phi,incoming):
 ty=chain.checked.phi_type(phi)
 return '  '+phi['operand']+' = phi '+ty+' '+', '.join('[ '+a['value']+', %'+a['block']+' ]' for a in incoming)

def inventory(plan,ll,out,commands):
 t=plan['tools'];fn=json.loads(Path(plan['frames']['bit_after']['json']).read_text())['function']
 cmd=[t['inventory']['path'],str(ll),fn,str(out)];p=subprocess.run(cmd,capture_output=True,text=True,env=dict(os.environ,LD_LIBRARY_PATH=t['runtime']));commands.append(dict(command=cmd,exit=p.returncode,stdout=p.stdout,stderr=p.stderr,scope='pure LLVM parse/verify/import only; no pass/native'))
 assert p.returncode==0, p.stderr

def alpha_controls(plan,root):
 f=plan['frames'];before=chain.raw_text(f['v_before']['ll']);after=chain.raw_text(f['v_after']['ll']);fn=json.loads(Path(f['v_after']['json']).read_text())['function'];positive=chain.alpha.prove(before,after,fn);assert not positive['old_outside_byte_identity'];rows=[]
 start=list(re.finditer(r'^define .*@'+re.escape(fn)+r'\(',after,re.M));assert len(start)==1
 first=start[0].start();last=after.index('\n}',first)+2
 sites=[m for m in re.finditer(r'^  call void [^\n]+!dragon\.guest\.call\.target !5(?=\n|$)',after,re.M) if not first<=m.start()<last];assert sites
 site=sites[0];changed_reference=after[:site.start()]+site[0].replace('!dragon.guest.call.target !5','!dragon.guest.call.target !6')+after[site.end():]
 mutations={
  'changed-call-target-payload':after.replace('!5 = !{i64 67280}','!5 = !{i64 67281}'),
  'outside-call-wrong-existing-payload':changed_reference,
  'changed-quoted-literal':after.replace('source_filename = "','source_filename = "changed-',1),
  'dropped-node':after.replace('!5 = !{i64 67280}\n',''),
 }
 for name,bad in mutations.items():
  assert bad!=after
  try:chain.alpha.prove(before,bad,fn)
  except AssertionError as e:rows.append(dict(control=name,detected=True,reason=str(e),scope='direct metadata certificate; no raw LLVM claim'))
  else:raise AssertionError('undetected alpha control '+name)
 # Number-looking tokens in a quote/comment are literal bytes, never graph IDs.
 a='define void @selected() {\n  ret void\n}\ndefine void @ordinary() {\n  ret void, !tag !0 ; comment !777\n}\n!anchor = !{!0, !1}\n!0 = !{!"literal !999 ; \\22quoted\\22", !1}\n!1 = distinct !{i64 7}\n'
 b=a.replace('!tag !0','!tag !4').replace('!anchor = !{!0, !1}','!anchor = !{!4, !5}').replace('!0 = !{!"literal !999 ; \\22quoted\\22", !1}','!4 = !{!"literal !999 ; \\22quoted\\22", !5}').replace('!1 = distinct','!5 = distinct')
 chain.alpha.prove(a,b,'selected')
 try:chain.alpha.prove(a,b.replace('literal !999','literal !998'),'selected')
 except AssertionError as e:rows.append(dict(control='quoted-number-not-remapped',detected=True,reason=str(e),scope='direct metadata certificate'))
 else:raise AssertionError('quoted literal remapped as numeric MD')
 return dict(real_single_line_alpha_positive=positive,quote_comment_literal_positive=True,controls=rows)

def main(plans,root,multiline):
 sys.setrecursionlimit(20000);root.mkdir();inputs=root/'inputs';inputs.mkdir();outputs=root/'outputs';outputs.mkdir();commands=[];controls=[]
 base=json.loads((plans/'f-combo-O2.json').read_text());function=json.loads(Path(base['frames']['bit_after']['json']).read_text())['function']
 def execute(name,plan):
  planfile=inputs/(name+'.plan.json');chain.write(planfile,plan)
  out=outputs/(name+'.json')
  command=[sys.executable,chain.__file__,'--plan',str(planfile),'--out',str(out)]
  p=subprocess.run(command,capture_output=True,text=True,timeout=180)
  commands.append(dict(command=command,exit=p.returncode,stdout=p.stdout,stderr=p.stderr,timeout_seconds=180,scope='read-only complete chain; no pass/native'))
  r=json.loads(out.read_text());assert p.returncode==(0 if r['established'] else 1)
  return out,r
 positive_out,positive=execute('unchanged-positive',base)
 assert positive['established'], positive.get('refusal')
 def test(name,plan,stage):
  out,r=execute(name,plan)
  assert not r['established'], 'undetected '+name
  assert r['refusal']['stage']==stage,(name,r['refusal'])
  controls.append(dict(control=name,detected=True,stage=stage,reason=r['refusal']['reason'],report=str(out)))
 # Missing stage/chronology cannot be inferred from a convenient final graph.
 p=copy.deepcopy(base);p['stage_order']=['V4','Bit60','C59-to-Bit-parse','C59'];test('stage-reordered',p,'plan')
 p=copy.deepcopy(base);del p['frames']['c59_after'];test('C59-stage-omitted',p,'plan')
 # This is LLVM-valid and inventory-identical, but is not the exact parser
 # rendering the producer imported. Its raw comment must not be normalized.
 prefix=inputs/'bad-bridge';Path(str(prefix)+'.ll').write_bytes(Path(base['frames']['bit_before']['ll']).read_bytes()+b'; changed raw bridge !999\n');Path(str(prefix)+'.json').write_bytes(Path(base['frames']['bit_before']['json']).read_bytes());p=copy.deepcopy(base);p['frames']['bit_before']=pin(prefix);test('bad-whole-module-bridge',p,'C59-to-Bit-parse')
 # A plausible old V-stage JSON cannot stand in for the actual imported C59.
 prefix=inputs/'stale-json';Path(str(prefix)+'.ll').write_bytes(Path(base['frames']['bit_before']['ll']).read_bytes());old=Path(base['frames']['v_after']['json']).read_bytes();assert old!=Path(base['frames']['bit_before']['json']).read_bytes();Path(str(prefix)+'.json').write_bytes(old);p=copy.deepcopy(base);p['frames']['bit_before']=pin(prefix);test('old-JSON-on-new-raw',p,'all-pass-frame-raw-bindings')
 before=json.loads(Path(base['frames']['bit_before']['json']).read_text());after=json.loads(Path(base['frames']['bit_after']['json']).read_text());proof=chain.checked.Proof(before,after).run();blocks,items,defs=chain.checked.state.tables(after)
 context=proof['checked_context_proofs'][0];newblocks={b for _,b,_ in context['path']};gep=next(i for b in newblocks for i in items[b] if i['opcode']=='getelementptr' and 'getelementptr i8,' in i['instruction']);bad=change_instruction(chain.raw_text(base['frames']['bit_after']['ll']),function,gep['instruction'],gep['instruction'].replace('getelementptr i8,','getelementptr i16,',1));prefix=inputs/'wrong-GEP-type';Path(str(prefix)+'.ll').write_text(bad);inventory(base,Path(str(prefix)+'.ll'),Path(str(prefix)+'.json'),commands);p=copy.deepcopy(base);p['frames']['bit_after']=pin(prefix);test('wrong-copied-GEP-source-type',p,'Bit60')
 # Remove one full-fallback case and its real PHI edge slots. The private IR
 # remains valid: every case also has its preserved original switch edge.
 fallback=context['fallback'];sw=next(s for s in after['switches'] if s['block']==fallback);term=items[fallback][-1];case=sw['cases'][0];label=term['operands'][3]['operand'];pattern=r'^[ \t]*'+re.escape(case['value']+', label '+label)+r'[ \t]*\n';newterm,count=re.subn(pattern,'',term['instruction'],count=1,flags=re.M);assert count==1;bad=change_instruction(chain.raw_text(base['frames']['bit_after']['ll']),function,term['instruction'],newterm)
 for phi in chain.checked.phis(items[case['target']]):
  arms=copy.deepcopy(phi['incoming']);n=next(n for n,a in enumerate(arms) if a['block']==fallback);arms.pop(n);assert arms
  oldtext=phi_text(phi,phi['incoming']);assert phi['instruction']==oldtext or phi['instruction'].startswith(oldtext+', !');suffix=phi['instruction'][len(oldtext):];bad=change_instruction(bad,function,phi['instruction'],phi_text(phi,arms)+suffix)
 prefix=inputs/'missing-full-fallback-case';Path(str(prefix)+'.ll').write_text(bad);inventory(base,Path(str(prefix)+'.ll'),Path(str(prefix)+'.json'),commands);p=copy.deepcopy(base);p['frames']['bit_after']=pin(prefix);test('deleted-full-fallback-case-and-edge-inputs',p,'Bit60')
 va=json.loads(Path(base['frames']['v_after']['json']).read_text());ca=json.loads(Path(base['frames']['c59_after']['json']).read_text());cp=chain.checked.Proof(va,ca).run();c=next(c for c in cp['checked_context_proofs'] if c['loop_slots']);table=chain.checked.state.tables(ca)[1];terminal=c['path'][-1][1];phi=next(i for i in chain.checked.phis(table[c['clone_root']]) if chain.checked.phi_type(i).startswith('i') and any(a['block']==terminal and a['value'].startswith('%') for a in i['incoming']));arms=copy.deepcopy(phi['incoming'])
 for arm in arms:
  if arm['block']==terminal:arm['value']='0'
 oldtext=phi_text(phi,phi['incoming']);assert phi['instruction']==oldtext or phi['instruction'].startswith(oldtext+', !');suffix=phi['instruction'][len(oldtext):];bad=change_instruction(chain.raw_text(base['frames']['c59_after']['ll']),function,phi['instruction'],phi_text(phi,arms)+suffix);prefix=inputs/'wrong-fresh-backedge-PHI';Path(str(prefix)+'.ll').write_text(bad);inventory(base,Path(str(prefix)+'.ll'),Path(str(prefix)+'.json'),commands);p=copy.deepcopy(base);p['frames']['c59_after']=pin(prefix);test('wrong-fresh-backedge-PHI',p,'C59')
 alph=alpha_controls(json.loads((plans/'f-combo-O0.json').read_text()),root)
 multi=[]
 for name in ('multiline-distinct','multiline-leading-space','multiline-difile','multiline-range'):
  d=multiline/name;a=chain.raw_text(d/'before.ll');b=chain.raw_text(d/'after.ll')
  try:chain.alpha.prove(a,b,'selected')
  except AssertionError as e:multi.append(dict(control=name,detected=True,reason=str(e),before_sha256=chain.checked.sha(d/'before.ll'),after_sha256=chain.checked.sha(d/'after.ll'),independent_old_false_accept_and_LLVM_verify_preserved=True))
  else:raise AssertionError('multiline metadata graph bypass '+name)
 result=dict(schema='checked-sequential-chain-selftest-v1',unchanged_actual_positive_report=str(positive_out),positive_established=True,positive_closure=positive['final_conditional_metric']['checked_context_guard_success_entry_closure'],whole_chain_controls=controls,read_only_commands_and_private_mutant_imports=commands,metadata_certificate_controls=alph,multiline_regressions=multi,old_unsafe_alpha_source_and_accepts_preserved=True,passes_executed=0,native_executions=0,scope='No producer or native program runs. Full-chain controls use fresh raw bindings and independent CLI subprocesses limited to180s each. Direct alpha controls are separately labelled; four multiline bad graphs retain independent LLVM-valid before/after evidence.')
 chain.write(root/'SELFTEST-v1.json',result);print(json.dumps(dict(whole_chain_controls=len(controls),alpha_controls=len(alph['controls'])+len(multi),all_detected=True,passes_executed=0,native_executions=0)))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--plans',required=True,type=Path);p.add_argument('--out-root',required=True,type=Path);p.add_argument('--multiline-evidence',required=True,type=Path);a=p.parse_args();main(a.plans,a.out_root,a.multiline_evidence)
