"""Make only a friend declaration and public-symbol renames on exact frozen TUs."""
from pathlib import Path
import hashlib
import json
import sys

EXPECTED = {
    'b6': 'b6b18cc70504e690c1eb3671858309237a06b48451ff9ace8561f6ada91dc01e',
    'df': 'df7746ffbf9b5fc9b90ce6c5667a938658dea6c3532228c87e5116cf0ec5a8a7',
    'opaque': '17badf6503b87724228fad78ee3b424f94683e24afff1532017f3789b76cc544',
}
PREAMBLE = '''// Independent original-TU accounting validation; never an installed Image implementation.
#define runImageTableCheckedContext budgetProbeRunImageTableCheckedContext
#define runClosedImageTableContext budgetProbeRunClosedImageTableContext
#define runLoopImageTableContext budgetProbeRunLoopImageTableContext
'''
EPILOGUE = '''
#undef runImageTableCheckedContext
#undef runClosedImageTableContext
#undef runLoopImageTableContext
'''
MARKER = 'class LoopRanges {\n'
ACCESS = 'struct LoopRangeBudgetTestAccess;\nclass LoopRanges {\n  friend struct LoopRangeBudgetTestAccess;\n'

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def main():
    root = Path(sys.argv[1]).resolve()
    frozen = root / 'frozen-v1'
    frozen.mkdir(exist_ok=False)
    manifest = {}
    for mode, expected in EXPECTED.items():
        original = root / ('LLVMImageTableContext.' + mode + '.cpp')
        raw = original.read_bytes()
        assert sha(raw) == expected
        text = raw.decode()
        assert text.count(MARKER) == 1
        overlay = PREAMBLE + text.replace(MARKER, ACCESS, 1) + EPILOGUE
        recovered = overlay.removeprefix(PREAMBLE).removesuffix(EPILOGUE).replace(ACCESS, MARKER, 1)
        assert recovered.encode() == raw
        target = frozen / ('LLVMImageTableContext.' + mode + '.overlay.cpp')
        target.write_text(overlay)
        manifest[mode] = dict(original=str(original), original_sha256=expected,
                              original_bytes=len(raw), overlay=str(target),
                              overlay_sha256=sha(overlay.encode()),
                              recovered_original_byte_equal=True)
    result = dict(schema='loop-original-TU-budget-overlay-v1', sources=manifest,
                  changes=['one test-only friend declaration', 'three public Image symbol macro renames'],
                  mutation_or_publication_hooks=False, guest_native=0,
                  installed_Image_execution=False)
    (frozen / 'OVERLAY-MANIFEST.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    print(json.dumps(result, indent=2, sort_keys=True))

if __name__ == '__main__':
    main()
