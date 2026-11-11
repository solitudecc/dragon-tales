#!/usr/bin/env python3
"""Positive and deliberately false context-measurement controls, no native import."""
import ast
from pathlib import Path
import re

source=Path(__file__).with_name('state_context_real.py')
tree=ast.parse(source.read_text());names={'ENTRY','GUARD','CONSTANT_INDEX'}
nodes=[node for node in tree.body if isinstance(node,ast.FunctionDef) and node.name not in ('main','digest','save')
       or isinstance(node,ast.Assign) and all(isinstance(t,ast.Name) and t.id in names for t in node.targets)]
namespace={'re':re};exec(compile(ast.Module(body=nodes,type_ignores=[]),str(source),'exec'),namespace)
measure=namespace['context_shape']
old='''define void @dragon_static_entry_x86_64_10080(ptr %memory, i64 %x) {
entry:
  %data = load ptr, ptr %memory, align 1
  br label %known
known:
  br label %bb_65712
unknown:
  br label %bb_65712
bb_65712:
  %RCX.1 = phi i64 [ 50, %known ], [ %x, %unknown ]
  %unrelated = phi i64 [ 50, %known ], [ 1, %unknown ]
  %cmp = icmp ugt i64 %RCX.1, 50
  br i1 %cmp, label %bb_65712, label %bridge
bridge:
  %size = load i64, ptr %memory, align 1
  br label %bounds
bounds:
  %condition = icmp ugt i64 %size, 8
  br i1 %condition, label %table, label %failure
table:
  %addr = getelementptr i8, ptr %data, i64 %x
  %target = load i64, ptr %addr, align 1
  switch i64 %target, label %default [
    i64 65712, label %bb_65712
    i64 65718, label %bounds
    i64 70000, label %payload
  ]
failure:
  ret void
payload:
  ret void
default:
  ret void
}'''
copied='''copy.b:
  br label %copy.u
copy.u:
  %copy.size = load i64, ptr %memory, align 1
  br label %copy.j
copy.j:
  %copy.cond = icmp ugt i64 %copy.size, 8
  br i1 %copy.cond, label %copy.s, label %failure
copy.s:
  %copy.addr = getelementptr i8, ptr %data, i64 70096
  %copy.target = load i64, ptr %copy.addr, align 1
  switch i64 %copy.target, label %default [
    i64 65712, label %bb_65712
    i64 65718, label %bounds
    i64 70000, label %payload
  ]
'''
new=old.replace('known:\n  br label %bb_65712','known:\n  br label %copy.b',1).replace('failure:\n',copied+'failure:\n',1)
assert measure(old,new,old,new)['valid']
controls=[]
controls.append((old.replace('%RCX.1 = phi i64 [ 50,','%RCX.1 = phi i64 [ 49,'),new,old,new))
controls.append((old,new.replace('br i1 %copy.cond, label %copy.s, label %failure','br label %copy.s'),old,new))
controls.append((old,new.replace('i64 65712, label %bb_65712\n    i64 65718, label %bounds\n    i64 70000, label %payload\n  ]\nfailure:',
    'i64 65712, label %copy.b\n    i64 65718, label %bounds\n    i64 70000, label %payload\n  ]\nfailure:'),old,new))
controls.append((old,new,old,new.replace('i64 70096','i64 70095')))
controls.append((old,new.replace('switch i64 %copy.target, label %default','switch i64 %copy.target, label %payload'),old,new))
controls.append((old,new,old,new.replace('ptr %data, i64 70096','ptr %memory, i64 70096')))
controls.append((old,new.replace('  %target = load i64, ptr %addr, align 1\n',''),old,new))
controls.append((old,new.replace('unknown:\n  br label %bb_65712','unknown:\n  br label %copy.b'),old,new))
controls.append((old,new.replace('%copy.cond = icmp ugt i64 %copy.size, 8','%copy.cond = icmp ugt i64 10, 8'),old,new))
controls.append((old,new.replace('switch i64 %copy.target','switch i64 %copy.size'),old,new.replace('switch i64 %copy.target','switch i64 %copy.size')))
for number,args in enumerate(controls):
    assert not measure(*args)['valid'],number
print('state context measurement: 1 positive and 10 specified false-gain controls pass')
