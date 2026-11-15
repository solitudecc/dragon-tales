"""Freeze an exact Image TU and make only independently recorded test overlays."""
import hashlib
import json
from pathlib import Path
import sys

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def method(source, name):
    start = source.index('  std::optional<Range> ' + name + '(', source.index('class LoopRanges {'))
    brace = source.index('{', start)
    debt = 0
    for n in range(brace, len(source)):
        debt += source[n] == '{'
        debt -= source[n] == '}'
        if debt == 0:
            return source[start:n + 1] + '\n'
    raise RuntimeError('unclosed method')

def main():
    root = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'lib/LLVMImageTableContext.cpp'
    raw = source.read_bytes()
    text = raw.decode()
    wanted = '2a3f4b0ccae176414f9562061d11a1142e134d469f06da34b17184ee4d957ba8'
    assert sha(raw) == wanted, 'freeze changed: never follow an unreviewed core'
    (out / 'LLVMImageTableContext.original.cpp').write_bytes(raw)
    headers = ['LLVMImageTableContext.hpp', 'LLVMPhiLowBits.hpp',
               'LLVMPathValueRanges.hpp', 'LLVMPhiConstant.hpp',
               'LLVMOpaqueIntegerAdmission.hpp']
    header_pins = {}
    for name in headers:
        payload = (root / 'lib' / name).read_bytes()
        (out / name).write_bytes(payload)
        header_pins[name] = dict(size=len(payload), sha256=sha(payload))
    for name in ['ImageTableContext.hpp', 'DispatchCorridor.hpp', 'Execution.h']:
        rel = Path('include/dragon/lifters/llvm') / name
        payload = (root / rel).read_bytes()
        (out / rel).parent.mkdir(parents=True, exist_ok=True)
        (out / rel).write_bytes(payload)
        header_pins[str(rel)] = dict(size=len(payload), sha256=sha(payload))
    marker = 'class LoopRanges {\n'
    assert text.count(marker) == 1
    access = '''#ifdef DRAGON_LOOP_OBSERVATION_TEST_ACCESS
struct LoopRangesObservationTestAccess;
#endif
#ifdef DRAGON_LOOP_OBSERVATION_PUBLICATION_TESTING
class LoopRanges;
void observationLoadPublishedForTesting(LoopRanges &, LoadInst *);
#endif
class LoopRanges {
#ifdef DRAGON_LOOP_OBSERVATION_TEST_ACCESS
  friend struct LoopRangesObservationTestAccess;
#endif
'''
    overlay = text.replace(marker, access, 1)
    publication = '    transaction.keep = true;\n'
    hook = '''    transaction.keep = true;
#ifdef DRAGON_LOOP_OBSERVATION_PUBLICATION_TESTING
    observationLoadPublishedForTesting(*this, load);
#endif
'''
    assert overlay.count(publication) == 1
    overlay = overlay.replace(publication, hook, 1)
    preamble = '''// Independent validation-only source overlay: public Image names cannot interpose.
#define runImageTableCheckedContext observationCacheRunImageTableCheckedContext
#define runClosedImageTableContext observationCacheRunClosedImageTableContext
#define runLoopImageTableContext observationCacheRunLoopImageTableContext
'''
    epilogue = '''
#undef runImageTableCheckedContext
#undef runClosedImageTableContext
#undef runLoopImageTableContext
'''
    overlay = preamble + overlay + epilogue
    recovered = overlay.removeprefix(preamble).removesuffix(epilogue)
    recovered = recovered.replace(access, marker, 1).replace(hook, publication, 1)
    assert recovered == text, 'unauthorized overlay difference'
    (out / 'LLVMImageTableContext.overlay.cpp').write_text(overlay)
    rollback = '      if (!keep) owner.loads.swap(entryLoads);'
    assert overlay.count(rollback) == 1
    mutant = overlay.replace(rollback,
        '      if (!keep && trial.load->getName() != "parent") owner.loads.swap(entryLoads);', 1)
    (out / 'LLVMImageTableContext.no-parent-rollback.cpp').write_text(mutant)
    manifest = dict(schema='actual-d-body-cache-test-overlay-v1',
        original_path=str(source), original_sha256=sha(raw), original_bytes=len(raw),
        methods={name:sha(method(text,name).encode()) for name in
                 ['loadRange', 'independentLoadRange', 'visit']},
        recovered_original_byte_equal=True,
        permitted_changes=['test-only friend/forward declarations',
            'macro-only public Image symbol rename',
            'test-only callback after actual successful cache publication'],
        mutant_change='only skip loads.swap on failed transaction named parent',
        overlays={name:dict(size=(out/name).stat().st_size,
                          sha256=sha((out/name).read_bytes())) for name in
            ['LLVMImageTableContext.overlay.cpp',
             'LLVMImageTableContext.no-parent-rollback.cpp']},
        headers=header_pins, compiled=False, guest_native=0, installed_Image_execution=False)
    (out / 'OVERLAY-MANIFEST.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    print(json.dumps(manifest,indent=2,sort_keys=True))

if __name__ == '__main__':
    main()
