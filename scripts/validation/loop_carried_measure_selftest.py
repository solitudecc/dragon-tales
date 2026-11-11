#!/usr/bin/env python3
"""Pure text-measurement controls; these do not execute native guest IR."""
import loop_carried_context_real as measure

PREFIX='''define void @dragon_static_entry_x86_64_10080(ptr %memory, i64 %x, i32 %route) {
entry:
  %base = load ptr, ptr %memory, align 1
  %size.ptr = getelementptr i8, ptr %memory, i64 8
  switch i32 %route, label %unknown [
    i32 0, label %known50
    i32 1, label %known16
  ]
known50:
  br label %bb_65712
known16:
  br label %bb_65712
unknown:
  br label %bb_65712
bb_65712:
  %RCX.1 = phi i64 [ 50, %known50 ], [ 16, %known16 ], [ %x, %unknown ]
  %condition = icmp ugt i64 %RCX.1, 50
  br i1 %condition, label %failure, label %original.bounds
original.bounds:
  %old.size = load i64, ptr %size.ptr, align 1
  %shift = shl i64 %RCX.1, 3
  %index = add i64 %shift, 69696
  %fit = icmp uge i64 %old.size, 8
  br i1 %fit, label %original.table, label %failure
original.table:
  %old.address = getelementptr i8, ptr %base, i64 %index
  %old.target = load i64, ptr %old.address, align 1
  switch i64 %old.target, label %default [
    i64 1, label %hit1
    i64 2, label %hit2
  ]
failure:
  ret void
default:
  ret void
hit1:
  ret void
hit2:
  ret void
'''

def region(state):
    return f'''copy{state}:
  %state.check{state} = icmp ugt i64 {state}, 50
  br label %bridge{state}
bridge{state}:
  %size{state} = load i64, ptr %size.ptr, align 1
  br label %bounds{state}
bounds{state}:
  %bound{state} = icmp uge i64 %size{state}, 8
  br i1 %bound{state}, label %table{state}, label %failure
table{state}:
  %address{state} = getelementptr i8, ptr %base, i64 {69696+state*8}
  %target{state} = load i64, ptr %address{state}, align 1
  switch i64 %target{state}, label %default [
    i64 1, label %hit1
    i64 2, label %hit2
  ]
'''

BEFORE=PREFIX+'}'
def after(states=(50,16)):
    text=PREFIX
    for state in states:
        text=text.replace(f'known{state}:\n  br label %bb_65712',f'known{state}:\n  br label %copy{state}')
        text=text.replace(f'[ {state}, %known{state} ], ','')
    return text+''.join(region(state) for state in states)+'}'

def result(text):return measure.batch_contexts(BEFORE,text,text)
good=after()
assert measure.batching_ok(result(good))
assert measure.baseline_ok(result(after((50,))))
controls=[
    after((50,)),
    good.replace('unknown:\n  br label %bb_65712','unknown:\n  br label %copy16'),
    good.replace('switch i64 %target16, label %default','switch i64 %target16, label %hit1'),
    good.replace('i64 2, label %hit2\n  ]\n}', 'i64 2, label %copy16\n  ]\n}'),
    good.replace('%state.check16 = icmp ugt i64 16, 50','%state.check16 = icmp ugt i64 17, 50'),
    good.replace('getelementptr i8, ptr %base, i64 69824','getelementptr i8, ptr %base, i64 69832'),
    good.replace('br i1 %bound16, label %table16, label %failure','br label %table16'),
    good.replace('%bound16 = icmp uge i64 %size16, 8','%bound16 = icmp uge i64 %x, 8'),
    good.replace('switch i64 %target16,','switch i64 %x,'),
    good.replace('br i1 %condition, label %failure, label %original.bounds','br label %original.bounds'),
]
for number,text in enumerate(controls):
    assert not measure.batching_ok(result(text)),number
print('loop-carried batch measurement: 1 positive, old single-call baseline, and 10 false-gain controls pass')
