# Exact executed reference sources

These files retain their original basenames and bytes from the final K native
campaign's `source-v3`. `SOURCE-v3.json` binds their SHA-256 values. The failed
relative-path auditor is retained as evidence, not as a recommended runner.

Use the sibling `invariant_self_loop_saved_audit.py` for read-only archived
review. It loads `ARCHIVE_ROOT/source-v3/oracle.py` explicitly under a private
module identity and checks its frozen hash. Do not replace that import with a
bare `import oracle`, or rename the executed model without updating an explicit
source mapping. Original native compilation/execution remains Linux AArch64 and
clang-21 O2-only; the read-only saved-data auditor is portable Python.
