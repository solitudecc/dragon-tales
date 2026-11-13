#!/usr/bin/env python3
"""Exact outside-function certificate for a uniquely proved numeric MD rename.

Raw IR and old byte gates are never rewritten. LLVM verification is a separate
mandatory prerequisite. Only numeric metadata labels may differ; every other
outside byte and every complete typed metadata payload/reference is preserved.
"""
import re
from collections import defaultdict

def tokens(text):
 result=[];start=0;i=0
 while i<len(text):
  if text[i]=='"':
   i+=1
   while i<len(text) and text[i]!='"':
    if text[i]=='\\':
     assert i+2<len(text) and re.fullmatch(r'[0-9a-fA-F]{2}',text[i+1:i+3]), 'unsupported LLVM string escape'
     i+=3
    else:
     assert text[i] not in '\r\n', 'multiline quoted syntax refused'
     i+=1
   assert i<len(text), 'unterminated quote';i+=1;continue
  if text[i]==';':
   end=text.find('\n',i);i=len(text) if end<0 else end+1;continue
  if text[i]=='!' and i+1<len(text) and text[i+1].isdigit():
   end=i+2
   while end<len(text) and text[end].isdigit():end+=1
   assert end==len(text) or not (text[end].isalnum() or text[end] in '._$'), 'ambiguous numeric metadata token'
   if i>start:result.append(('text',text[start:i]))
   result.append(('md',text[i+1:end]));i=end;start=end;continue
  i+=1
 if start<len(text):result.append(('text',text[start:]))
 return result

def outside(text,function):
 lines=text.splitlines(True);starts=[n for n,l in enumerate(lines) if l.startswith('define ') and re.search(r'@'+re.escape(function)+r'\(',l)]
 assert len(starts)==1, 'ambiguous selected function identity'
 first=starts[0];last=next(n for n in range(first+1,len(lines)) if lines[n].strip()=='}')
 return ''.join(lines[:first+1]+lines[last:])

def complete_single_line_node(payload):
 # LLVM may accept multiline distinct/DI nodes. This certificate deliberately
 # supports only an entire node on one physical line; never detach a header
 # from continuation lines or confuse an inner closing brace for the root.
 leading=len(payload)-len(payload.lstrip(' \t'))
 match=re.match(r'(?:distinct )?!(?:\{|[A-Za-z_][A-Za-z0-9_]*\()',payload[leading:])
 assert match, 'unsupported numeric MD node definition'
 i=leading+match.end()-1;stack=[];pairs={'}':'{',')':'(',']':'[','>':'<'}
 while i<len(payload):
  c=payload[i]
  if c=='"':
   i+=1
   while i<len(payload) and payload[i]!='"':
    if payload[i]=='\\':
     assert i+2<len(payload) and re.fullmatch(r'[0-9a-fA-F]{2}',payload[i+1:i+3]), 'unsupported LLVM string escape'
     i+=3
    else:assert payload[i] not in '\r\n', 'multiline MD string refused';i+=1
   assert i<len(payload), 'unterminated MD string'
  elif c==';':break
  elif c in '{([<':stack.append(c)
  elif c in '})]>':
   assert stack and stack.pop()==pairs[c], 'unbalanced MD delimiters'
   if not stack:
    tail=payload[i+1:].lstrip(' \t')
    assert not tail or tail.startswith(';'), 'unsupported trailing MD syntax'
    return
  i+=1
 raise AssertionError('multiline or incomplete MD definition refused')

def split(text):
 defs={};ordinary=[]
 for line in text.splitlines(True):
  match=re.match(r'^!([0-9]+) = (.*?)(\n)?$',line)
  if match:
   ident=match[1];assert ident not in defs, 'duplicate MD definition'
   complete_single_line_node(match[2])
   defs[ident]=tokens(match[2]+(match[3] or ''))
  else:ordinary.append(line)
 assert len(defs)<=4096, 'metadata proof node bound exceeded'
 main=tokens(''.join(ordinary))
 refs={v for t,v in main if t=='md'}|{v for body in defs.values() for t,v in body if t=='md'}
 assert refs<=set(defs), 'undefined or unsupported MD reference'
 return main,defs

def prove(before,after,function):
 left=outside(before,function);right=outside(after,function)
 assert len(left.encode())+len(right.encode())<=64*1024*1024, 'outside lexical byte bound exceeded'
 a,ad=split(left);b,bd=split(right)
 assert len(ad)==len(bd), 'MD node added or removed'
 assert len(a)==len(b), 'outside token count changed'
 mapping={};inverse={};work=0
 def bind(x,y):
  assert x in ad and y in bd
  assert x not in mapping or mapping[x]==y, 'inconsistent MD mapping'
  assert y not in inverse or inverse[y]==x, 'merged MD nodes'
  fresh=x not in mapping;mapping[x]=y;inverse[y]=x;return fresh
 for x,y in zip(a,b):
  assert x[0]==y[0], 'outside token kind changed'
  if x[0]=='text':assert x[1]==y[1], 'outside non-ID bytes changed'
  else:bind(x[1],y[1])
 def shape(body):return tuple((kind,'<MD>' if kind=='md' else value) for kind,value in body)
 ashapes={x:shape(v) for x,v in ad.items()};bshapes={y:shape(v) for y,v in bd.items()}
 assert sum(len(v) for body in list(ad.values())+list(bd.values()) for t,v in body if t=='text')<=16*1024*1024, 'metadata literal byte bound exceeded'
 groups=defaultdict(list)
 for y,key in bshapes.items():groups[key].append(y)
 candidates={}
 for x,key in ashapes.items():
  choices=groups.get(key,[]);work+=len(choices)
  assert work<=1000000, 'metadata candidate work bound exceeded'
  assert choices, 'typed MD payload or string changed'
  candidates[x]=set(choices)
 changed=True
 while changed:
  changed=False
  for x,y in list(mapping.items()):
   assert ashapes[x]==bshapes[y], 'mapped typed MD payload changed'
   for old,new in zip(ad[x],bd[y]):
    work+=1;assert work<=1000000, 'metadata proof work bound exceeded'
    if old[0]=='md':changed=bind(old[1],new[1]) or changed
  for x in ad:
   if x in mapping:continue
   possible=[]
   for y in candidates[x]:
    if y in inverse:continue
    valid=True
    for old,new in zip(ad[x],bd[y]):
     work+=1;assert work<=1000000, 'metadata proof work bound exceeded'
     if old[0]!='md':continue
     u,v=old[1],new[1]
     if (u in mapping and mapping[u]!=v) or (v in inverse and inverse[v]!=u) or ((u==x)!=(v==y)):valid=False;break
    if valid:possible.append(y)
   assert possible, 'no injective typed/reference MD correspondence'
   if len(possible)==1:changed=bind(x,possible[0]) or changed
 assert len(mapping)==len(ad)==len(inverse), 'ambiguous MD graph refused'
 for x,y in mapping.items():
  expected=[(t,mapping[v] if t=='md' else v) for t,v in ad[x]]
  assert expected==bd[y], 'MD reference graph or typed payload changed'
 return dict(schema='outside-metadata-alpha-certificate-v1',complete_numeric_MD_bijection={x:mapping[x] for x in sorted(mapping,key=int)},all_typed_payload_strings_distinct_and_reference_edges_exact=True,all_outside_ordinary_non_definition_bytes_exact=True,all_MD_definition_payload_bytes_exact_under_proved_bijection=True,MD_definition_order_not_used_as_identity=True,all_outside_MD_use_sites_preserve_corresponding_payload=True,selected_function_body_excluded_and_requires_separate_stage_proof=True,original_IR_untouched=True,normalization_performed=False,proof_work=work,old_outside_byte_identity=(left==right))
