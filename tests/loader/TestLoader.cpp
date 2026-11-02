#include <catch2/catch_test_macros.hpp>

#include <dragon/loader/ImageDisassembler.hpp>
#include <dragon/loader/Loader.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "fixtures/fixtures.hpp"

using namespace dragon;
using namespace dragon::loader;

namespace {

std::vector<std::uint8_t> toVec(const unsigned char *p, std::size_t n) {
  return std::vector<std::uint8_t>(p, p + n);
}

/// The tiny ELF with its section header table removed (e_shoff, e_shnum and
/// e_shstrndx zeroed), the way VMProtect and similar rewriters emit it: only
/// the PT_LOAD program headers say what gets mapped.
std::vector<std::uint8_t> tinyElfWithoutSections() {
  std::vector<std::uint8_t> v = toVec(kTinyElfX64, sizeof(kTinyElfX64));
  std::fill(v.begin() + 0x28, v.begin() + 0x30, 0); // e_shoff
  std::fill(v.begin() + 0x3C, v.begin() + 0x40, 0); // e_shnum, e_shstrndx
  return v;
}

const Section *findSection(const LoadedImage &img, const std::string &name) {
  for (const Section &s : img.sections())
    if (s.name == name)
      return &s;
  return nullptr;
}

} // namespace

TEST_CASE("loader - stripped ELF metadata supplies recursive roots", "[loader][metadata-entries]") {
  auto bytes = toVec(kMetadataElfX64, sizeof(kMetadataElfX64));
  bool hasFrameName = true;
  SECTION("all metadata") {}
  SECTION("header works without an eh_frame section name") {
    const std::string name = ".eh_frame";
    auto at = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
    REQUIRE(at != bytes.end());
    *at = '_';
    hasFrameName = false;
  }
  auto image = LoadedImage::loadBytes(bytes);
  REQUIRE(image);
  Configuration config;
  config.disassemblyStrategy = DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  auto graph = disassembleImage(*image, config);
  if (hasFrameName) CHECK(graph.get(0x400110) != nullptr);
  for (uint64_t address : {0x400120,0x400130,0x400140,0x400150}) {
    INFO(std::hex << address);
    CHECK(graph.get(address) != nullptr);
    CHECK(graph.isValidFunction(address));
  }
}

TEST_CASE("loader - stripped PE unwind and TLS metadata supplies roots", "[loader][metadata-entries]") {
  auto image = LoadedImage::loadBytes(toVec(kMetadataPeX64, sizeof(kMetadataPeX64)));
  REQUIRE(image);
  Configuration config;
  config.disassemblyStrategy = DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  auto graph = disassembleImage(*image, config);
  for (uint64_t address : {0x140001010ull,0x140001020ull}) {
    CHECK(graph.get(address) != nullptr);
    CHECK(graph.isValidFunction(address));
  }
}

TEST_CASE("loader - address lookups reject wrapping ranges", "[loader][metadata-entries]") {
  Section section;
  section.address = UINT64_MAX - 7;
  section.virtualSize = 16;
  CHECK(section.contains(UINT64_MAX));
  CHECK_FALSE(section.contains(0));
  auto image = LoadedImage::loadBytes(toVec(kMetadataElfX64, sizeof(kMetadataElfX64)));
  REQUIRE(image);
  CHECK_FALSE(image->readVirtual(0x400100, SIZE_MAX));
}

TEST_CASE("loader - malformed relocation tables cannot invoke LLVM fatal readers", "[loader][metadata-entries]") {
  auto bytes = toVec(kMetadataElfX64, sizeof(kMetadataElfX64));
  uint64_t table = 0;
  for (unsigned byte=0; byte<8; ++byte) table |= uint64_t(bytes[40+byte]) << (8*byte);
  // Section 6 is .rela.dyn. A huge size was the fuzzed getType() abort.
  std::fill_n(bytes.begin()+table+6*64+32,8,0xff);
  auto image = LoadedImage::loadBytes(bytes);
  REQUIRE(image);
  Configuration configuration;
  configuration.disassemblyStrategy = DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  auto graph = disassembleImage(*image,configuration);
  CHECK(graph.get(0x400100) != nullptr);
}

// -----------------------------------------------------------------------
// ELF
// -----------------------------------------------------------------------

TEST_CASE("loader - parses a minimal ELF64 x86-64 image", "[loader][elf]") {
  std::string err;
  auto img = LoadedImage::loadBytes(toVec(kTinyElfX64, sizeof(kTinyElfX64)), &err);
  REQUIRE(img.has_value());
  CHECK(err.empty());

  CHECK(img->format() == Format::ELF);
  CHECK(img->architecture() == Architecture::X86_64);
  CHECK(img->is64Bit());
  CHECK(img->hasVirtualLayout());

  REQUIRE(img->entryPoint().has_value());
  CHECK(*img->entryPoint() == kTinyElfX64Entry);

  SECTION("the .text section is executable and carries the real bytes") {
    const Section *text = findSection(*img, ".text");
    REQUIRE(text != nullptr);
    CHECK(text->executable);
    CHECK(text->address == kTinyElfX64Entry);
    // push rbp; mov rbp,rsp; xor eax,eax; pop rbp; ret
    REQUIRE(text->bytes.size() == 8);
    CHECK(text->bytes[0] == 0x55);
    CHECK(text->bytes.back() == 0xC3);
  }

  SECTION("the function symbol is recovered at the entry") {
    const Symbol *fn = img->functionAt(kTinyElfX64Entry);
    REQUIRE(fn != nullptr);
    CHECK(fn->name == "myfunc");
    CHECK(fn->kind == SymbolKind::Function);
    CHECK_FALSE(fn->imported);
  }

  SECTION("readVirtual spans the entry and returns the first instruction") {
    auto bytes = img->readVirtual(kTinyElfX64Entry, 4);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == 4);
    CHECK((*bytes)[0] == 0x55);
  }

  SECTION("readVirtual rejects an unmapped address") {
    CHECK_FALSE(img->readVirtual(0xDEAD0000, 4).has_value());
  }
}

TEST_CASE("loader - disassembleImage turns an ELF into a populated Graph",
          "[loader][elf][pipeline]") {
  auto img = LoadedImage::loadBytes(toVec(kTinyElfX64, sizeof(kTinyElfX64)));
  REQUIRE(img.has_value());

  Graph g = disassembleImage(*img);
  CHECK(g.architecture() == Architecture::X86_64);
  // Five instructions: push/mov/xor/pop/ret, all inside .text.
  CHECK(g.size() == 5);
  CHECK(g.get(kTinyElfX64Entry) != nullptr);
  CHECK(g.isValidFunction(kTinyElfX64Entry));
}

TEST_CASE("loader - ELF image base is the lowest PT_LOAD address",
          "[loader][elf]") {
  auto img = LoadedImage::loadBytes(toVec(kTinyElfX64, sizeof(kTinyElfX64)));
  REQUIRE(img.has_value());

  // The fixture's PT_LOAD maps the file from 0x400000; .text only starts at
  // 0x400078, so a section-based guess would get the base wrong.
  CHECK(img->imageBase() == 0x400000ULL);

  REQUIRE(img->segments().size() == 1);
  const Section &seg = img->segments().front();
  CHECK(seg.address == 0x400000ULL);
  CHECK(seg.readable);
  CHECK(seg.executable);
  CHECK_FALSE(seg.writable);
}

TEST_CASE("loader - a section-less ELF is mapped from its PT_LOAD segments",
          "[loader][elf]") {
  std::string err;
  auto img = LoadedImage::loadBytes(tinyElfWithoutSections(), &err);
  REQUIRE(img.has_value());
  CHECK(err.empty());

  CHECK(img->format() == Format::ELF);
  CHECK(img->architecture() == Architecture::X86_64);
  CHECK(img->sections().empty());
  CHECK(img->hasVirtualLayout());
  CHECK(img->imageBase() == 0x400000ULL);
  REQUIRE(img->entryPoint().has_value());
  CHECK(*img->entryPoint() == kTinyElfX64Entry);

  SECTION("readVirtual reaches code and headers through the segment") {
    auto code = img->readVirtual(kTinyElfX64Entry, 8);
    REQUIRE(code.has_value());
    CHECK(*code == std::vector<std::uint8_t>{0x55, 0x48, 0x89, 0xE5, 0x31,
                                             0xC0, 0x5D, 0xC3});
    auto magic = img->readVirtual(0x400000ULL, 4);
    REQUIRE(magic.has_value());
    CHECK(*magic == std::vector<std::uint8_t>{0x7F, 'E', 'L', 'F'});
    CHECK_FALSE(img->readVirtual(0x500000ULL, 1).has_value());
  }

  SECTION("the executable segment stands in for the missing .text") {
    auto exec = img->executableSections();
    REQUIRE(exec.size() == 1);
    CHECK(exec.front()->address == 0x400000ULL);
    CHECK(exec.front()->contains(kTinyElfX64Entry));
  }

  SECTION("disassembleImage decodes the entry function") {
    Graph g = disassembleImage(*img);
    CHECK(g.architecture() == Architecture::X86_64);
    CHECK(g.get(kTinyElfX64Entry) != nullptr);
    CHECK(g.isValidFunction(kTinyElfX64Entry));
  }
}

// Regression: ELF sections without SHF_ALLOC (.symtab, .strtab, .comment)
// have sh_addr 0 and were treated as mapped there. In a PIE or shared object
// (base 0) every address in a gap between allocated sections - the ELF
// header, padding - then read .symtab/.comment bytes instead of what the
// PT_LOAD maps (seen on clang-built aarch64 PIEs: readVirtual(0x10) returned
// the .comment string). They are listed but take no part in address lookups.
TEST_CASE("loader - non-SHF_ALLOC ELF sections are not in the address space",
          "[loader][elf]") {
  auto img = LoadedImage::loadBytes(toVec(kTinyElfX64, sizeof(kTinyElfX64)));
  REQUIRE(img.has_value());
  const Section *symtab = findSection(*img, ".symtab");
  REQUIRE(symtab != nullptr);
  CHECK(symtab->address == 0);
  CHECK_FALSE(symtab->bytes.empty());
  CHECK_FALSE(symtab->mapped);
  CHECK_FALSE(symtab->contains(0));
  CHECK(img->sectionContaining(0) == nullptr);
  CHECK_FALSE(img->readVirtual(0, 4).has_value()); // nothing is mapped at 0
  const Section *text = findSection(*img, ".text");
  REQUIRE(text != nullptr);
  CHECK(text->mapped);
  CHECK(img->sectionContaining(kTinyElfX64Entry) == text);
}

// -----------------------------------------------------------------------
// PE
// -----------------------------------------------------------------------

TEST_CASE("loader - parses a minimal PE32+ x86-64 image", "[loader][pe]") {
  std::string err;
  auto img = LoadedImage::loadBytes(toVec(kTinyPeX64, sizeof(kTinyPeX64)), &err);
  REQUIRE(img.has_value());
  CHECK(err.empty());

  CHECK(img->format() == Format::PE);
  CHECK(img->architecture() == Architecture::X86_64);
  CHECK(img->is64Bit());
  CHECK(img->imageBase() == 0x140000000ULL);

  REQUIRE(img->entryPoint().has_value());
  CHECK(*img->entryPoint() == kTinyPeX64Entry); // imageBase + 0x1000

  const Section *text = findSection(*img, ".text");
  REQUIRE(text != nullptr);
  CHECK(text->executable);
  CHECK(text->readable);
  CHECK(text->address == kTinyPeX64Entry);
}

// -----------------------------------------------------------------------
// Errors
// -----------------------------------------------------------------------

TEST_CASE("loader - rejects non-object bytes with an error", "[loader]") {
  std::string err;
  auto img = LoadedImage::loadBytes({0x00, 0x01, 0x02, 0x03, 0x04}, &err);
  CHECK_FALSE(img.has_value());
  CHECK_FALSE(err.empty());
}

TEST_CASE("loader - loadFile on a missing path fails cleanly", "[loader]") {
  std::string err;
  auto img = LoadedImage::loadFile("/no/such/file/at/all", &err);
  CHECK_FALSE(img.has_value());
  CHECK_FALSE(err.empty());
}

// -----------------------------------------------------------------------
// Imports and exports of stripped images.
//
// Regression: symbols came only from obj->symbols() - .symtab for ELF, the
// COFF symbol table for PE. A stripped ELF (every distribution binary;
// /bin/ls gave 0 symbols) has only .dynsym, and a PE's imports and exports
// live in its import/export directories, which were never read: a stripped
// or MSVC-built PE had no imports or exports at all, and a mingw PE with
// COFF symbols reported linker leftovers (`__end__`) as DLL imports.
// -----------------------------------------------------------------------

namespace {
const Symbol *findSymbol(const LoadedImage &img, const std::string &name) {
  for (const Symbol &s : img.symbols())
    if (s.name == name)
      return &s;
  return nullptr;
}
} // namespace

TEST_CASE("Loader - ELF without .symtab: imports and exports from .dynsym",
          "[loader]") {
  auto img = LoadedImage::loadBytes(toVec(kDynElfX64, sizeof(kDynElfX64)));
  REQUIRE(img.has_value());

  const Symbol *exp = findSymbol(*img, "dyn_export");
  REQUIRE(exp != nullptr);
  CHECK(exp->exported);
  CHECK_FALSE(exp->imported);
  CHECK(exp->address == kDynElfX64Export);
  CHECK(exp->kind == SymbolKind::Function);

  const Symbol *puts = findSymbol(*img, "puts");
  REQUIRE(puts != nullptr);
  CHECK(puts->imported);
  CHECK_FALSE(puts->exported);

  // Weak undefined is still an import (glibc's __gmon_start__ & co).
  const Symbol *gmon = findSymbol(*img, "__gmon_start__");
  REQUIRE(gmon != nullptr);
  CHECK(gmon->imported);

  CHECK(img->imports().size() == 2);
  CHECK(img->exports().size() == 1);
}

TEST_CASE("loader - disassembleImage reads an x86-64 PE with the Win64 "
          "calling convention",
          "[loader][pe]") {
  auto img = LoadedImage::loadBytes(toVec(kImpExpPeX64, sizeof(kImpExpPeX64)));
  REQUIRE(img.has_value());
  Graph g = disassembleImage(*img);
  CHECK(g.configuration().callingConvention ==
        dragon::CallingConvention::Win64);
}

TEST_CASE("Loader - PE imports and exports come from the directories",
          "[loader]") {
  auto img = LoadedImage::loadBytes(toVec(kImpExpPeX64, sizeof(kImpExpPeX64)));
  REQUIRE(img.has_value());
  REQUIRE(img->imageBase() == kImpExpPeX64Base);

  // Imports: one per IAT slot, address = the slot (what `call [X]` reads).
  const Symbol *byName = findSymbol(*img, "GetTickCount");
  REQUIRE(byName != nullptr);
  CHECK(byName->imported);
  CHECK(byName->address == kImpExpPeX64Base + 0x2060);
  const Symbol *byOrdinal = findSymbol(*img, "#5");
  REQUIRE(byOrdinal != nullptr);
  CHECK(byOrdinal->imported);
  CHECK(byOrdinal->address == kImpExpPeX64Base + 0x2068);
  CHECK(img->imports().size() == 2);

  // Exports, including an ordinal-only one.
  const Symbol *named = findSymbol(*img, "exported_fn");
  REQUIRE(named != nullptr);
  CHECK(named->exported);
  CHECK_FALSE(named->imported);
  CHECK(named->address == kImpExpPeX64Base + 0x1000);
  CHECK(named->kind == SymbolKind::Function);
  const Symbol *ordinalOnly = findSymbol(*img, "#2");
  REQUIRE(ordinalOnly != nullptr);
  CHECK(ordinalOnly->exported);
  CHECK(ordinalOnly->address == kImpExpPeX64Base + 0x1004);
  CHECK(img->exports().size() == 2);
}

TEST_CASE("Loader - an undefined symbol is never also exported", "[loader]") {
  for (const auto &bytes : {toVec(kTinyElfX64, sizeof(kTinyElfX64)),
                            toVec(kDynElfX64, sizeof(kDynElfX64)),
                            toVec(kImpExpPeX64, sizeof(kImpExpPeX64))}) {
    auto img = LoadedImage::loadBytes(bytes);
    REQUIRE(img.has_value());
    for (const Symbol &s : img->symbols()) {
      INFO(s.name);
      CHECK_FALSE((s.imported && s.exported));
    }
  }
}

// -----------------------------------------------------------------------
// Damaged / unusual import directories (found by fuzzing the loader).
//
// LLVM's ImportDirectoryEntryRef::imported_symbols() resolves the import
// lookup table with cantFail(): an RVA outside the image crashed the loader
// with a null dereference, and so did a lookup-table RVA of 0, which is a
// valid PE that only has an IAT.
// -----------------------------------------------------------------------

namespace {
std::uint32_t rd32(const std::vector<std::uint8_t> &b, std::size_t o) {
  return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | std::uint32_t{b[o + 3]} << 24;
}
void wr32(std::vector<std::uint8_t> &b, std::size_t o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
// File offset of the first import descriptor of a PE32+ image.
std::size_t firstImportDescriptor(const std::vector<std::uint8_t> &pe) {
  const std::size_t nt = rd32(pe, 0x3c);
  const std::size_t sections = pe[nt + 6] | pe[nt + 7] << 8;
  const std::size_t optSize = pe[nt + 20] | pe[nt + 21] << 8;
  const std::size_t opt = nt + 24;
  const std::uint32_t importRva = rd32(pe, opt + 112 + 8); // data dir 1
  for (std::size_t i = 0; i < sections; ++i) {
    const std::size_t sh = opt + optSize + i * 40;
    const std::uint32_t va = rd32(pe, sh + 12), raw = rd32(pe, sh + 20),
                        size = std::max(rd32(pe, sh + 8), rd32(pe, sh + 16));
    if (importRva >= va && importRva < va + size)
      return raw + (importRva - va);
  }
  return 0;
}
} // namespace

TEST_CASE("Loader - an import lookup table outside the image does not crash",
          "[loader][pe][robustness]") {
  auto pe = toVec(kImpExpPeX64, sizeof(kImpExpPeX64));
  const std::size_t desc = firstImportDescriptor(pe);
  REQUIRE(desc != 0);
  wr32(pe, desc + 0, 0x7fff0000); // ImportLookupTableRVA
  auto img = LoadedImage::loadBytes(pe);
  REQUIRE(img.has_value()); // and no crash
  CHECK(img->imports().empty());
}

TEST_CASE("Loader - imports are read from the IAT without a lookup table",
          "[loader][pe][robustness]") {
  auto pe = toVec(kImpExpPeX64, sizeof(kImpExpPeX64));
  const std::size_t desc = firstImportDescriptor(pe);
  REQUIRE(desc != 0);
  wr32(pe, desc + 0, 0); // ImportLookupTableRVA = 0: IAT only
  auto img = LoadedImage::loadBytes(pe);
  REQUIRE(img.has_value());
  CHECK(img->imports().size() == 2);
}
