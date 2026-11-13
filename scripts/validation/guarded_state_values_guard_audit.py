#!/usr/bin/env python3
"""Strict fixture guard shape: freeze only the fresh EQ, keeping old SSA/comparison."""
import re

def block(text,name):
 m=re.search(r'^'+re.escape(name)+r':[^\n]*\n(.*?)(?=^\S|\Z)',text,re.M|re.S);assert m,name;return m.group(1)

def definition(text,name):
 rows=re.findall(r'^  '+re.escape(name)+r' = [^\n]*$',text,re.M);assert len(rows)==1,(name,rows);return rows[0]

def audit(before,after,value):
 seed=block(after,'seed')
 # The condition is not a state reload, a frozen original comparison, or a
 # freeze used to invent state bits/ranges. Original state definitions stay.
 for name in ('%statep','%raw','%state','%cmp'):assert definition(before,name)==definition(after,name),(name,'changed original definition')
 eq=re.search(r'^  (%[-\w.]+) = icmp eq i64 %state, '+str(value)+r'$',seed,re.M);assert eq,'not a fresh EQ of original actual state'
 frozen=re.search(r'^  (%[-\w.]+) = freeze i1 '+re.escape(eq.group(1))+r'$',seed,re.M);assert frozen,'new EQ branch condition is not frozen'
 branch=re.search(r'^  br i1 '+re.escape(frozen.group(1))+r', label %([-\w.]+), label %([-\w.]+)(?:, [^\n]*)?$',seed,re.M);assert branch,'new branch does not consume that single frozen EQ'
 assert len(re.findall(r' = freeze i1 ',seed))==1,'unexpected extra guard freezes'
 fallback=block(after,branch.group(2));rows=[x.strip() for x in fallback.splitlines() if x.strip()];assert len(rows)==1,'fallback repeats original effects'
 # The existing core marks generated block terminators; that annotation is not
 # a replacement for the unchanged original condition and successor slots.
 fallback_branch=re.sub(r', !dragon\.guarded-transitions\.generated ![0-9]+$','',rows[0]);old_branch=block(before,'seed').strip().splitlines()[-1].strip();assert fallback_branch==old_branch,'original fallback condition/direction changed'
 return branch.groups()
