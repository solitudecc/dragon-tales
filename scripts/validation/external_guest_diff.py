"""Import-slot API calls executed through their actual Guest code (i386
stdcall callee pop, non-ABI callee clobbers, an API returning a function
pointer, Win64 home space, AArch64 PLT/IP0 use) vs Unicorn, V2 O0/O2.

Uses function_guest_diff.py's oracle unchanged: full register state, all
64 KiB of memory, exit records, no normalizations. Each case's image must
name its import slot, and the root's !dragon.imports provenance is checked.

--static runs StaticAnalysis through the product optimizer instead. A static
function propagates the import thunk's transfer, so every case is compared
at the declared boundary on entry to the API code (0x2000), exactly as
function_static_guest_diff.py does for its image cases: this checks the
complete state handed to the API, not the API's effects.
Other options as function_guest_diff.py (--trials, --case, --corrupt-output).
"""
import json
import sys
import tempfile
from pathlib import Path

import function_guest_diff as guest

FIXTURES = guest.base.ROOT / 'tests/fixtures/llvm-external-guest.json'


def main():
    if '--static' not in sys.argv:
        return guest.main(fixtures_path=FIXTURES)
    sys.argv.remove('--static')
    import function_static_guest_diff as static
    data = json.loads(FIXTURES.read_text())
    for f in data['cases']:
        f['missing_targets'] = [0x2000]
        f['boundary_kind'] = 1
    with tempfile.TemporaryDirectory(prefix='dragon-external-static-') as folder:
        path = Path(folder) / 'fixtures.json'
        path.write_text(json.dumps(data))
        return guest.main(fixtures_path=path, compile_factory=static.compile_static,
                          adapter_factory=static.StaticAdapter)


if __name__ == '__main__':
    sys.exit(main())
