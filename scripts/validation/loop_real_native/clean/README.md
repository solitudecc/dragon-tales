# Binding-only native check of the actual six-pass cleanup

These four sources are the executed thin binding entry, metadata planner, after-only32-run worker and pure saved checker. They add no observer locator or instrumentation. The original parent `loop_real_native_adapter.cpp` (SHA `1b7bfbbe06a045ded628b922a5c7d07f5f859468c507d495d6fa8ce73394d34b`) and parent Python common/saved helpers remain unchanged.

The C++ wrapper includes that parent source through `DRAGON_LOOP_NATIVE_ADAPTER_SOURCE`; when compiling directly from this repository, set that macro to the absolute parent file path (or quoted `../loop_real_native_adapter.cpp`). It renames the old main and calls its exact parse/descriptorABI/bind functions. It never calls sites/observe. Its synthetic-only mode checks nine conditions, including valid duplicate-call/wrong-target refusals; it makes no production-ready/closure assertion for a toy.

Actual frozen sources and compiler argv are in `/home/ubuntu/vmc-runs/loop-clean-native-source-20261115-v1/` and `loop-clean-native-20261115-v1/COMPILE.json`. That captured flat staging directory contains byte-identical copies of the parent adapter/common helper; these are not independent product implementations. Restore the captured staging paths for exact replay. Python scripts use the unchanged parent helper modules; direct source-tree invocation may set PYTHONPATH to the parent directory. The saved checker additionally imports the unchanged parent saved-audit inventory/JSON-storage functions. No runtime or source path strings are rewritten.

| Executed source | SHA256 |
|---|---|
|`loop_clean_native_adapter.cpp`|`b8bcf099dcb31a374bc37e335ee58d5a83afdfc1ac063f9283a6db4dfea62c5c`|
|`loop_clean_native_prepare.py`|`a307b50fd7717fc180fd297e03ab0571920ea0f9a497a156655c5ac7e9f86223`|
|`loop_clean_native_replay.py`|`5810f7bc0ab73b516baa4600a442f7c72758c25cf0448d65bdf9fbb50a8bd91c`|
|`loop_clean_native_saved_audit.py`|`57d5fa4e90be05dc4951bdc812f76fcda7dd9148a7478080276206e8077fce5b`|

Native plan SHA `518e649948a345b84ebbf3672460d7665c724b9ff2ac573f95797ebbc6368c67`, guard SHA `75894c0eaf9bb09c747e3b48f6cdf652f2c2a0d17e0089de9697951fec6940e3`. The worker retains original O0/O2 opt→verify→llc-O0→GCC-O0 compilation and complete66,136-byte comparison,76 fields, root Exit and budget, and96 exact comparator corruptions. All ModuleIDs are the actual parser/tool paths and are never normalized. It runs32 after-only machine executions; it does not repeat the earlier76 executions, C/UC references or raw observer evidence.

See `docs/loop-clean-native-validation-20261115.md` and the separately sealed evidence package for actual source/runtime/input/tool/header pins, commands, raw stdout/stderr and all outputs. The cleanup input is actual `8418fd314005fa8b771473ea56e907a352416b459246e4f0e39d796c55ee1d2e`, derived by the fixed six passes from closed B `714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74`. No new clean observer evidence, arbitrary-input semantic proof or unconditional deobfuscation is claimed.
