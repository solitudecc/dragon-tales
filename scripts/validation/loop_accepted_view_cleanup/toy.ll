; ModuleID = 'mechanical-toy-only'
target datalayout = "e-p:64:64"
@global = global i64 7, align 8
define i64 @ordinary(i64 %x) {
entry:
  %keep = add i64 %x, 0
  ret i64 %keep
}
define i64 @view(ptr %p, i64 %x) {
entry:
  %old.lookup = load i64, ptr %p, align 1
  store i64 %x, ptr @global, align 8
  br i1 true, label %good, label %bad
good:
  %simplify = add i64 %x, 0
  ret i64 %simplify
bad:
  ret i64 %old.lookup
}
!toy.stable = !{!0}
!0 = !{!"original"}
