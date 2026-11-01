#!/usr/bin/env python3
"""Generate minimal, valid ELF64 and PE32+ images as a C++ byte-array header.

The loader tests embed their inputs as byte arrays (the same convention as
tests/obfush/TestObfush.cpp), so they need no toolchain or on-disk fixture at
test time. Both images are hand-built here with just enough structure for
llvm::object to parse them the way a real executable would:

  * tiny ELF64 x86-64 ET_EXEC: one PT_LOAD, a .text with `xor eax,eax; ret`,
    an entry point into it, and a .symtab with one STT_FUNC symbol "myfunc".
  * tiny PE32+ x86-64: DOS stub, PE header, PE32+ optional header with an
    ImageBase and entry RVA, and one executable ".text" section.

  * dynsym-only ELF64: .dynsym/.dynstr and no .symtab (a stripped binary),
    with an export, an import and a weak import.
  * PE32+ with import and export directories and no COFF symbol table (a
    stripped / MSVC release build).

Re-run to regenerate fixtures.hpp after changing any image.
"""
import struct

TEXT = bytes([0x55, 0x48, 0x89, 0xE5, 0x31, 0xC0, 0x5D, 0xC3])  # push/mov/xor/pop/ret


def align(n, a):
    return (n + a - 1) & ~(a - 1)


def build_elf():
    EM_X86_64 = 62
    base = 0x400000

    ehdr_size, phdr_size, shdr_size, sym_size = 64, 56, 64, 24

    off_phdr = ehdr_size
    off_text = off_phdr + phdr_size
    text_addr = base + off_text

    shstrtab = b"\x00.text\x00.symtab\x00.strtab\x00.shstrtab\x00"
    name_text = shstrtab.index(b".text\x00")
    name_symtab = shstrtab.index(b".symtab\x00")
    name_strtab = shstrtab.index(b".strtab\x00")
    name_shstr = shstrtab.index(b".shstrtab\x00")

    strtab = b"\x00myfunc\x00"
    name_myfunc = strtab.index(b"myfunc\x00")

    off_shstr = off_text + len(TEXT)
    off_strtab = off_shstr + len(shstrtab)
    off_symtab = align(off_strtab + len(strtab), 8)

    # Section index 1 == .text, used as st_shndx.
    symtab = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)  # null symbol
    st_info = (1 << 4) | 2  # STB_GLOBAL, STT_FUNC
    symtab += struct.pack("<IBBHQQ", name_myfunc, st_info, 0, 1, text_addr, len(TEXT))

    off_shdr = align(off_symtab + len(symtab), 8)
    e_entry = text_addr

    def shdr(name, typ, flags, addr, off, size, link, info, align_, entsize):
        return struct.pack("<IIQQQQIIQQ", name, typ, flags, addr, off, size,
                           link, info, align_, entsize)

    sections = b""
    sections += shdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)                      # 0 null
    sections += shdr(name_text, 1, 0x6, text_addr, off_text, len(TEXT),  # 1 .text
                     0, 0, 1, 0)
    sections += shdr(name_symtab, 2, 0, 0, off_symtab, len(symtab),      # 2 .symtab
                     3, 1, 8, sym_size)
    sections += shdr(name_strtab, 3, 0, 0, off_strtab, len(strtab),      # 3 .strtab
                     0, 0, 1, 0)
    sections += shdr(name_shstr, 3, 0, 0, off_shstr, len(shstrtab),      # 4 .shstrtab
                     0, 0, 1, 0)
    e_shnum, e_shstrndx = 5, 4

    ehdr = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\x00" * 8  # ident: 64-bit LE SysV
    ehdr += struct.pack("<HHIQQQIHHHHHH",
                        2, EM_X86_64, 1, e_entry, off_phdr, off_shdr, 0,
                        ehdr_size, phdr_size, 1, shdr_size, e_shnum, e_shstrndx)

    filesz = off_shdr + len(sections)
    # PT_LOAD mapping the whole file at `base`, R+X.
    phdr = struct.pack("<IIQQQQQQ", 1, 0x5, 0, base, base, filesz, filesz, 0x1000)

    img = bytearray(filesz)
    img[0:len(ehdr)] = ehdr
    img[off_phdr:off_phdr + len(phdr)] = phdr
    img[off_text:off_text + len(TEXT)] = TEXT
    img[off_shstr:off_shstr + len(shstrtab)] = shstrtab
    img[off_strtab:off_strtab + len(strtab)] = strtab
    img[off_symtab:off_symtab + len(symtab)] = symtab
    img[off_shdr:off_shdr + len(sections)] = sections
    return bytes(img), text_addr


def build_pe():
    IMAGE_BASE = 0x140000000
    entry_rva = 0x1000
    sect_align, file_align = 0x1000, 0x200

    e_lfanew = 0x40
    dos = bytearray(e_lfanew)
    dos[0:2] = b"MZ"
    dos[0x3C:0x40] = struct.pack("<I", e_lfanew)

    # COFF file header: Machine=AMD64, 1 section, PE32+ optional header (0xF0).
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0xF0, 0x0022)

    num_dirs = 16
    headers_end = e_lfanew + 4 + len(coff) + 0xF0 + 40
    size_headers = align(headers_end, file_align)
    raw_ptr = size_headers
    raw_size = align(len(TEXT), file_align)
    size_image = align(entry_rva + max(len(TEXT), sect_align), sect_align)

    # PE32+ optional header (magic 0x20B). ImageBase is 8 bytes; no BaseOfData.
    opt = struct.pack("<HBBIIII", 0x20B, 14, 0, raw_size, 0, 0, entry_rva)
    opt += struct.pack("<IQ", 0, IMAGE_BASE)           # BaseOfCode, ImageBase
    opt += struct.pack("<IIHHHHHH", sect_align, file_align, 6, 0, 0, 0, 6, 0)
    opt += struct.pack("<IIII", 0, size_image, size_headers, 0)  # ...,SizeOfImage,SizeOfHeaders,CheckSum
    opt += struct.pack("<HH", 3, 0)                    # Subsystem=console, DllCharacteristics
    opt += struct.pack("<QQQQ", 0x100000, 0x1000, 0x100000, 0x1000)  # stack/heap
    opt += struct.pack("<II", 0, num_dirs)             # LoaderFlags, NumberOfRvaAndSizes
    opt += b"\x00" * (num_dirs * 8)                    # data directories
    assert len(opt) == 0xF0, len(opt)

    name = b".text".ljust(8, b"\x00")
    sect = name + struct.pack("<IIIIIIHHI", len(TEXT), entry_rva, raw_size,
                              raw_ptr, 0, 0, 0, 0, 0x60000020)

    img = bytearray(raw_ptr + raw_size)
    img[0:len(dos)] = dos
    p = e_lfanew
    img[p:p + 4] = b"PE\x00\x00"; p += 4
    img[p:p + len(coff)] = coff; p += len(coff)
    img[p:p + len(opt)] = opt; p += len(opt)
    img[p:p + len(sect)] = sect
    img[raw_ptr:raw_ptr + len(TEXT)] = TEXT
    return bytes(img), IMAGE_BASE + entry_rva


def build_elf_dynsym_only():
    """A stripped-style ELF64: .dynsym/.dynstr but no .symtab - the shape of
    every distribution binary. dynsym: a defined global export, an undefined
    global import and an undefined *weak* import."""
    base = 0x400000
    ehdr_size, phdr_size, shdr_size, sym_size = 64, 56, 64, 24
    off_text = ehdr_size + phdr_size
    text_addr = base + off_text
    dynstr = b"\x00dyn_export\x00puts\x00__gmon_start__\x00"
    n_export = dynstr.index(b"dyn_export\x00")
    n_puts = dynstr.index(b"puts\x00")
    n_gmon = dynstr.index(b"__gmon_start__\x00")
    dynsym = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    dynsym += struct.pack("<IBBHQQ", n_export, 0x12, 0, 1, text_addr, len(TEXT))  # GLOBAL FUNC .text
    dynsym += struct.pack("<IBBHQQ", n_puts, 0x12, 0, 0, 0, 0)                    # GLOBAL FUNC UND
    dynsym += struct.pack("<IBBHQQ", n_gmon, 0x20, 0, 0, 0, 0)                    # WEAK NOTYPE UND
    shstrtab = b"\x00.text\x00.dynsym\x00.dynstr\x00.shstrtab\x00"
    off_dynsym = align(off_text + len(TEXT), 8)
    off_dynstr = off_dynsym + len(dynsym)
    off_shstr = off_dynstr + len(dynstr)
    off_shdr = align(off_shstr + len(shstrtab), 8)
    total = off_shdr + 5 * shdr_size

    ehdr = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\x00" * 8
    ehdr += struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, text_addr, ehdr_size, off_shdr,
                        0, ehdr_size, phdr_size, 1, shdr_size, 5, 4)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, 0, base, base, off_shdr, off_shdr, 0x1000)

    def shdr(name, typ, flags, addr, off, size, link, info, align_, entsize):
        return struct.pack("<IIQQQQIIQQ", name, typ, flags, addr, off, size,
                           link, info, align_, entsize)
    sh = shdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    sh += shdr(shstrtab.index(b".text\x00"), 1, 6, text_addr, off_text, len(TEXT), 0, 0, 16, 0)
    sh += shdr(shstrtab.index(b".dynsym\x00"), 11, 2, base + off_dynsym, off_dynsym,
               len(dynsym), 3, 1, 8, sym_size)
    sh += shdr(shstrtab.index(b".dynstr\x00"), 3, 2, base + off_dynstr, off_dynstr,
               len(dynstr), 0, 0, 1, 0)
    sh += shdr(shstrtab.index(b".shstrtab\x00"), 3, 0, 0, off_shstr, len(shstrtab), 0, 0, 1, 0)

    img = bytearray(total)
    img[0:len(ehdr)] = ehdr
    img[ehdr_size:ehdr_size + len(phdr)] = phdr
    img[off_text:off_text + len(TEXT)] = TEXT
    img[off_dynsym:off_dynsym + len(dynsym)] = dynsym
    img[off_dynstr:off_dynstr + len(dynstr)] = dynstr
    img[off_shstr:off_shstr + len(shstrtab)] = shstrtab
    img[off_shdr:off_shdr + len(sh)] = sh
    return bytes(img), text_addr


def build_pe_import_export():
    """PE32+ with an import directory (KERNEL32.dll: GetTickCount by name,
    ordinal 5) and an export directory (exported_fn, plus ordinal-only #2),
    and no COFF symbol table - the shape of a stripped/MSVC release build."""
    IMAGE_BASE = 0x180000000
    sect_align, file_align = 0x1000, 0x200
    e_lfanew = 0x40
    dos = bytearray(e_lfanew)
    dos[0:2] = b"MZ"
    dos[0x3C:0x40] = struct.pack("<I", e_lfanew)

    # .idata at RVA 0x2000
    idata = bytearray(0x200)
    ilt = [0x2080, (1 << 63) | 5, 0]
    struct.pack_into("<IIIII", idata, 0x00, 0x2040, 0, 0, 0x2100, 0x2060)  # descriptor
    for i, v in enumerate(ilt):
        struct.pack_into("<Q", idata, 0x40 + 8 * i, v)                     # ILT
        struct.pack_into("<Q", idata, 0x60 + 8 * i, v)                     # IAT
    idata[0x80:0x80 + 15] = struct.pack("<H", 0) + b"GetTickCount\x00"
    idata[0x100:0x10d] = b"KERNEL32.dll\x00"
    # .edata at RVA 0x3000
    edata = bytearray(0x200)
    struct.pack_into("<IIHHIIIIIII", edata, 0, 0, 0, 0, 0, 0x3060, 1, 2, 1,
                     0x3028, 0x3030, 0x3034)
    struct.pack_into("<II", edata, 0x28, 0x1000, 0x1004)   # EAT
    struct.pack_into("<I", edata, 0x30, 0x3040)            # name pointers
    struct.pack_into("<H", edata, 0x34, 0)                 # name ordinals
    edata[0x40:0x40 + 12] = b"exported_fn\x00"
    edata[0x60:0x60 + 9] = b"tiny.dll\x00"

    sections = [(b".text", 0x1000, TEXT, 0x60000020),
                (b".idata", 0x2000, bytes(idata[:0x110]), 0xC0000040),
                (b".edata", 0x3000, bytes(edata[:0x70]), 0x40000040)]
    coff = struct.pack("<HHIIIHH", 0x8664, len(sections), 0, 0, 0, 0xF0, 0x2022)
    headers_end = e_lfanew + 4 + len(coff) + 0xF0 + 40 * len(sections)
    size_headers = align(headers_end, file_align)
    size_image = 0x4000
    dirs = [(0, 0)] * 16
    dirs[0] = (0x3000, 0x70)    # export
    dirs[1] = (0x2000, 0x28)    # import
    dirs[12] = (0x2060, 0x18)   # IAT
    opt = struct.pack("<HBBIIII", 0x20B, 14, 0, 0x200, 0x400, 0, 0x1000)
    opt += struct.pack("<IQ", 0x1000, IMAGE_BASE)
    opt += struct.pack("<IIHHHHHH", sect_align, file_align, 6, 0, 0, 0, 6, 0)
    opt += struct.pack("<IIII", 0, size_image, size_headers, 0)
    opt += struct.pack("<HH", 3, 0)
    opt += struct.pack("<QQQQ", 0x100000, 0x1000, 0x100000, 0x1000)
    opt += struct.pack("<II", 0, 16)
    opt += b"".join(struct.pack("<II", *d) for d in dirs)
    assert len(opt) == 0xF0, len(opt)

    raw = size_headers
    shdrs = b""
    body = b""
    for name, rva, data, chars in sections:
        raw_size = align(len(data), file_align)
        shdrs += name.ljust(8, b"\x00") + struct.pack("<IIIIIIHHI", len(data), rva, raw_size,
                                                     raw + len(body), 0, 0, 0, 0, chars)
        body += data.ljust(raw_size, b"\x00")
    img = bytearray(size_headers) + body
    img[0:len(dos)] = dos
    p = e_lfanew
    img[p:p + 4] = b"PE\x00\x00"; p += 4
    img[p:p + len(coff)] = coff; p += len(coff)
    img[p:p + len(opt)] = opt; p += len(opt)
    img[p:p + len(shdrs)] = shdrs
    return bytes(img), IMAGE_BASE


def emit(name, data):
    out = f"static const unsigned char {name}[] = {{\n"
    for i in range(0, len(data), 12):
        row = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
        out += f"    {row},\n"
    out += "};\n"
    return out


def main():
    elf, elf_entry = build_elf()
    pe, pe_entry = build_pe()
    with open("fixtures.hpp", "w") as f:
        f.write("// Generated by make_fixtures.py - do not edit by hand.\n")
        f.write("// Minimal ELF64 and PE32+ x86-64 images for the loader tests.\n")
        f.write("#pragma once\n\n")
        f.write(f"// Entry point of the ELF image / address of symbol \"myfunc\".\n")
        f.write(f"static constexpr unsigned long long kTinyElfX64Entry = 0x{elf_entry:x}ULL;\n")
        f.write(f"static constexpr unsigned long long kTinyPeX64Entry  = 0x{pe_entry:x}ULL;\n\n")
        f.write(emit("kTinyElfX64", elf))
        f.write("\n")
        f.write(emit("kTinyPeX64", pe))
        elf_dyn, elf_dyn_text = build_elf_dynsym_only()
        pe_ie, pe_ie_base = build_pe_import_export()
        f.write("\n// ELF with .dynsym but no .symtab; address of \"dyn_export\".\n")
        f.write(f"static constexpr unsigned long long kDynElfX64Export = 0x{elf_dyn_text:x}ULL;\n")
        f.write(emit("kDynElfX64", elf_dyn))
        f.write("\n// PE32+ with import and export directories, no COFF symbols; ImageBase.\n")
        f.write(f"static constexpr unsigned long long kImpExpPeX64Base = 0x{pe_ie_base:x}ULL;\n")
        f.write(emit("kImpExpPeX64", pe_ie))
    print(f"ELF: {len(elf)} bytes, entry 0x{elf_entry:x}")
    print(f"PE:  {len(pe)} bytes, entry 0x{pe_entry:x}")


if __name__ == "__main__":
    main()
