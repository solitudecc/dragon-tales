"""Robustness of the Python API on names that are not UTF-8.

Section and symbol names come straight from the binary and need not be
UTF-8 (found by fuzzing the loader: damaged files, but also legitimate
ones). LoadedImage.sections()/symbols() decoded them strictly and raised
UnicodeDecodeError, losing the whole list. They now decode with
replacement characters. Run directly; exits non-zero on failure.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'python'))
import dragon  # noqa: E402


def tiny_elf(bad_name):
    """ELF64 x86-64 executable: .text (ret) and a section named bad_name."""
    text = b'\xc3'
    shstr = b'\0.text\0' + bad_name + b'\0.shstrtab\0'
    off_text = 64
    off_shstr = off_text + len(text)
    off_sh = (off_shstr + len(shstr) + 7) & ~7
    names = {'text': 1, 'bad': 7, 'shstrtab': 7 + len(bad_name) + 1}

    def sh(name, typ, flags, addr, off, size):
        return struct.pack('<IIQQQQIIQQ', name, typ, flags, addr, off, size, 0, 0, 1, 0)

    shdrs = (sh(0, 0, 0, 0, 0, 0) +
             sh(names['text'], 1, 6, 0x401000, off_text, len(text)) +
             sh(names['bad'], 1, 2, 0x402000, off_text, len(text)) +
             sh(names['shstrtab'], 3, 0, 0, off_shstr, len(shstr)))
    ehdr = struct.pack('<4sBBBBB7xHHIQQQIHHHHHH', b'\x7fELF', 2, 1, 1, 0, 0,
                       2, 0x3e, 1, 0x401000, 0, off_sh, 0, 64, 0, 0, 64, 4, 3)
    body = ehdr + text + shstr
    return body + b'\0' * (off_sh - len(body)) + shdrs


img = dragon.LoadedImage.load_bytes(tiny_elf(b'\xff\xfe.x'))
names = [s.name for s in img.sections()]   # used to raise UnicodeDecodeError
img.symbols()
assert '.text' in names, names
assert any('�' in n for n in names), names
print('ok', names)
