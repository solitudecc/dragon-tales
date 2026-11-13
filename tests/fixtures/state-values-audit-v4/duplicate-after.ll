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
  %eA = icmp eq i64 %a, 2
  %fA = freeze i1 %eA
  br i1 %fA, label %gateA, label %fallbackA
fallbackA:
  br i1 %cA, label %shared, label %rootA
seedB:
  call void @unknown(ptr %regs)
  %cB = icmp ult i64 %b, 10
  %eB = icmp eq i64 %b, 2
  %fB = freeze i1 %eB
  br i1 %fB, label %gateB, label %fallbackB
fallbackB:
  br i1 %cB, label %rootB, label %shared
rootA:
  br i1 %pick, label %dispatchA, label %dispatchA
dispatchA:
  %unused = phi i64 [ 7, %rootA ], [ 7, %rootA ], [ 7, %cloneRootA ]
  %loadA = load i64, ptr %table, align 1
  switch i64 %loadA, label %shared [ i64 43, label %returnA ]
rootB:
  br label %dispatchB
dispatchB:
  %loadB = load i64, ptr %tp, align 1
  switch i64 %loadB, label %shared [ i64 44, label %returnB ]
returnA:
  %liveA = phi i64 [ %loadA, %dispatchA ], [ %cloadA, %cloneDispatchA ], [ %cloadA, %targetFallbackA ]
  ret i64 %liveA
returnB:
  %liveB = phi i64 [ %loadB, %dispatchB ], [ %cloadB, %cloneDispatchB ], [ %cloadB, %targetFallbackB ]
  ret i64 %liveB
shared:
  %answer = phi i64 [ %a, %fallbackA ], [ %b, %fallbackB ], [ %loadA, %dispatchA ], [ %loadB, %dispatchB ], [ %cloadA, %targetFallbackA ], [ %cloadB, %targetFallbackB ]
  ret i64 %answer
gateA:
  br label %cloneRootA
cloneRootA:
  br i1 %pick, label %cloneDispatchA, label %dispatchA
cloneDispatchA:
  %cloadA = load i64, ptr %table, align 1
  %targetEqA = icmp eq i64 %cloadA, 43
  br i1 %targetEqA, label %returnA, label %targetFallbackA
targetFallbackA:
  switch i64 %cloadA, label %shared [ i64 43, label %returnA ]
gateB:
  br label %cloneRootB
cloneRootB:
  br label %cloneDispatchB
cloneDispatchB:
  %cloadB = load i64, ptr %tp, align 1
  %targetEqB = icmp eq i64 %cloadB, 44
  br i1 %targetEqB, label %returnB, label %targetFallbackB
targetFallbackB:
  switch i64 %cloadB, label %shared [ i64 44, label %returnB ]
}
