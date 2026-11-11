#!/usr/bin/env python3
"""Exercise real-IR measurement without importing the native Dragon module."""
import ast
from pathlib import Path
import re

path=Path(__file__).with_name('guest_memory_checked_edges_real.py')
tree=ast.parse(path.read_text())
nodes=[node for node in tree.body if isinstance(node,ast.FunctionDef) and
       node.name in ('state_guard_edges','state_guard_gain_valid')]
namespace={'re':re,'CFF_GUARDS':{0x10080:0x100b0}}
exec(compile(ast.Module(body=nodes,type_ignores=[]),str(path),'exec'),namespace)
measure,valid=namespace['state_guard_edges'],namespace['state_guard_gain_valid']

before='''define void @dragon_static_entry_x86_64_10080(i64 %x) {
known:
  br label %bb_65712
unknown:
  br label %bb_65712
bb_65712:
  %RCX.1 = phi i64 [ STATE, %known ], [ %x, %unknown ]
  %unrelated = phi i64 [ OTHER, %known ], [ 3, %unknown ]
  %condition = icmp ugt i64 %RCX.1, 50
  br i1 %condition, label %failure, label %checked
failure:
  ret void
checked:
  ret void
}'''
def pair(state,other,target='checked'):
    old=before.replace('STATE',str(state)).replace('OTHER',str(other))
    new=old.replace('known:\n  br label %bb_65712','known:\n  br label %copy',1)
    new=new.replace('failure:\n','copy:\n  br label %'+target+'\nfailure:\n',1)
    return old,new

old,new=pair(50,17)
positive=measure(old,new,0x10080)
assert valid(positive)
old,new=pair(49,50)
unrelated=measure(old,new,0x10080)
assert 50 in unrelated['redirects'][0]['incoming_constants']
assert unrelated['redirects'][0]['state_incoming_constant']==49
assert not valid(unrelated)
old,new=pair(50,17,'failure')
assert not valid(measure(old,new,0x10080))
old,new=pair(50,17)
new=new.replace('br i1 %condition, label %failure, label %checked','br label %checked')
assert not valid(measure(old,new,0x10080))
old,new=pair(50,17)
new=new.replace('unknown:\n  br label %bb_65712','unknown:\n  br label %copy')
assert not valid(measure(old,new,0x10080))
print('state guard measurement: 1 positive and 4 specified false-gain controls pass')
