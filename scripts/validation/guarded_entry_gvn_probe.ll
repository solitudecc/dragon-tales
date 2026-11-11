; Independent small capability probe: checks imply values only on success.
; Run opt-21 -S -passes=gvn,instcombine,verify; no Dragon code is required.
target datalayout = "e-p:64:64"
declare void @unknown(ptr)
define i64 @packed(ptr %base) {
entry:
  %p = getelementptr i8, ptr %base, i64 24
  %v = load i64, ptr %p, align 1
  %eq = icmp eq i64 %v, 123
  br i1 %eq, label %yes, label %no
yes:
  %again = load i64, ptr %p, align 1
  ret i64 %again
no:
  ret i64 0
}
define i64 @call_barrier(ptr %base) {
entry:
  %p = getelementptr i8, ptr %base, i64 24
  %v = load i64, ptr %p, align 1
  %eq = icmp eq i64 %v, 123
  br i1 %eq, label %yes, label %no
yes:
  call void @unknown(ptr %base)
  %again = load i64, ptr %p, align 1
  ret i64 %again
no:
  ret i64 0
}
define i64 @alias_barrier(ptr %base, ptr %other) {
entry:
  %p = getelementptr i8, ptr %base, i64 24
  %v = load i64, ptr %p, align 1
  %eq = icmp eq i64 %v, 123
  br i1 %eq, label %yes, label %no
yes:
  store i64 999, ptr %other, align 1
  %again = load i64, ptr %p, align 1
  ret i64 %again
no:
  ret i64 0
}
