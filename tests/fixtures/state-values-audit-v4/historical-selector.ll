define i64 @oracle(ptr %table) {
entry:
  br label %header
header:
  %history = phi i64 [ 43, %entry ], [ %fresh, %fast ], [ %selector, %fallback ]
  br label %fast
fast:
  %fresh = load i64, ptr %table, align 1
  switch i64 %fresh, label %fallback [ i64 43, label %done
                                      i64 44, label %header ]
fallback:
  %selector = phi i64 [ %history, %fast ]
  switch i64 %selector, label %done [ i64 43, label %done
                                       i64 44, label %header ]
done:
  %answer = phi i64 [ %fresh, %fast ], [ %selector, %fallback ], [ %selector, %fallback ]
  ret i64 %answer
}
