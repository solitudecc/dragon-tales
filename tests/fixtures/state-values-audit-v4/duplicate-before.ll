declare void @unknown(ptr)
define i64 @oracle(ptr %regs, ptr %table, i1 %pick) {
entry:
  %a = load i64, ptr %regs, align 1
  %bp = getelementptr i8, ptr %regs, i64 8
  %b = load i64, ptr %bp, align 1
  %tp = getelementptr i8, ptr %table, i64 8
  br i1 %pick, label %seedA, label %seedB
seedA:
  call void @unknown(ptr %regs)
  %cA = icmp ugt i64 %a, 50
  br i1 %cA, label %shared, label %rootA
seedB:
  call void @unknown(ptr %regs)
  %cB = icmp ult i64 %b, 10
  br i1 %cB, label %rootB, label %shared
rootA:
  br i1 %pick, label %dispatchA, label %dispatchA
dispatchA:
  %unused = phi i64 [ 7, %rootA ], [ 7, %rootA ]
  %loadA = load i64, ptr %table, align 1
  switch i64 %loadA, label %shared [ i64 43, label %returnA ]
rootB:
  br label %dispatchB
dispatchB:
  %loadB = load i64, ptr %tp, align 1
  switch i64 %loadB, label %shared [ i64 44, label %returnB ]
returnA:
  ret i64 %loadA
returnB:
  ret i64 %loadB
shared:
  %answer = phi i64 [ %a, %seedA ], [ %b, %seedB ], [ %loadA, %dispatchA ], [ %loadB, %dispatchB ]
  ret i64 %answer
}
