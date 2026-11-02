#include "FunctionEntries.hpp"
#include <dragon/loader/Loader.hpp>

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/BinaryFormat/ELF.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/DebugInfo/DWARF/DWARFDataExtractor.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugFrame.h>
#include <llvm/Support/DataExtractor.h>

#include <algorithm>
#include <limits>
#include <span>

namespace dragon::loader::detail {
namespace {
using namespace llvm;
using namespace llvm::object;

std::span<const uint8_t> fileBytes(const LoadedImage &image, uint64_t address) {
  auto from = [address](const Section &section) -> std::span<const uint8_t> {
    if (!section.mapped || address < section.address || address - section.address >= section.bytes.size())
      return {};
    return std::span(section.bytes).subspan(address - section.address);
  };
  for (const auto &section : image.sections())
    if (auto bytes = from(section); !bytes.empty()) return bytes;
  for (const auto &segment : image.segments())
    if (auto bytes = from(segment); !bytes.empty()) return bytes;
  return {};
}

StringRef text(std::span<const uint8_t> bytes) {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::optional<uint64_t> integer(std::span<const uint8_t> bytes, unsigned size, bool little) {
  if (bytes.size() < size) return {};
  DataExtractor extractor(text(bytes), little, size);
  uint64_t offset = 0;
  return extractor.getUnsigned(&offset, size);
}

// .eh_frame_hdr follows LSB 5.0 section 10.6. Both relative bases are
// explicit: pcrel is the encoded field's address; datarel is the header start.
std::optional<uint64_t> encodedPointer(const LoadedImage &image, const DataExtractor &data,
    uint64_t &offset, uint8_t encoding, uint64_t sectionAddress, unsigned pointerSize) {
  if (encoding == dwarf::DW_EH_PE_omit) return {};
  const uint64_t start = offset;
  Error error = Error::success();
  uint64_t value = 0;
  switch (encoding & 15) {
    case dwarf::DW_EH_PE_absptr: value = data.getUnsigned(&offset, pointerSize, &error); break;
    case dwarf::DW_EH_PE_uleb128: value = data.getULEB128(&offset, &error); break;
    case dwarf::DW_EH_PE_sleb128: value = data.getSLEB128(&offset, &error); break;
    case dwarf::DW_EH_PE_udata2: value = data.getUnsigned(&offset, 2, &error); break;
    case dwarf::DW_EH_PE_udata4: value = data.getUnsigned(&offset, 4, &error); break;
    case dwarf::DW_EH_PE_udata8: value = data.getUnsigned(&offset, 8, &error); break;
    case dwarf::DW_EH_PE_sdata2:
      if (!data.isValidOffsetForDataOfSize(offset,2)) return {};
      value = data.getSigned(&offset,2); break;
    case dwarf::DW_EH_PE_sdata4:
      if (!data.isValidOffsetForDataOfSize(offset,4)) return {};
      value = data.getSigned(&offset,4); break;
    case dwarf::DW_EH_PE_sdata8:
      if (!data.isValidOffsetForDataOfSize(offset,8)) return {};
      value = data.getSigned(&offset,8); break;
    default: return {};
  }
  if (error) { consumeError(std::move(error)); return {}; }
  if (offset <= start || offset > data.getData().size()) return {};
  switch (encoding & 0x70) {
    case 0: break;
    case dwarf::DW_EH_PE_pcrel: value += sectionAddress + start; break;
    case dwarf::DW_EH_PE_datarel: value += sectionAddress; break;
    default: return {};
  }
  if (pointerSize == 4) value &= 0xffffffffu;
  if (encoding & dwarf::DW_EH_PE_indirect)
    return integer(fileBytes(image, value), pointerSize, data.isLittleEndian());
  return value;
}

// RelocationRef::getType() uses a fatal LLVM reader for malformed relocation
// tables. Stay on ELFFile's checked Expected<ArrayRef<...>> APIs throughout.
template<class ELFT, class Add>
void elfRelocations(const ELFObjectFile<ELFT> &object, const LoadedImage &image, Add add) {
  const auto architecture = image.architecture();
  if (architecture != Architecture::ARM64 && architecture != Architecture::X86_32 &&
      architecture != Architecture::X86_64) return;
  const auto &file = object.getELFFile();
  auto sections = file.sections();
  if (!sections) { consumeError(sections.takeError()); return; }
  for (const auto &section : *sections) {
    if (section.sh_type != ELF::SHT_REL && section.sh_type != ELF::SHT_RELA) continue;
    auto process = [&](const auto &relocations) {
      for (const auto &relocation : relocations) {
        const auto type = relocation.getType(false);
        const bool relative =
            (architecture == Architecture::X86_64 && (type == ELF::R_X86_64_RELATIVE || type == ELF::R_X86_64_IRELATIVE)) ||
            (architecture == Architecture::X86_32 && (type == ELF::R_386_RELATIVE || type == ELF::R_386_IRELATIVE)) ||
            (architecture == Architecture::ARM64 && (type == ELF::R_AARCH64_RELATIVE || type == ELF::R_AARCH64_IRELATIVE));
        const bool absolute =
            (architecture == Architecture::X86_64 && type == ELF::R_X86_64_64) ||
            (architecture == Architecture::X86_32 && type == ELF::R_386_32) ||
            (architecture == Architecture::ARM64 && type == ELF::R_AARCH64_ABS64);
        if (!relative && !absolute) continue;
        uint64_t addend;
        if constexpr (requires { relocation.r_addend; }) addend = relocation.r_addend;
        else {
          auto pointer = integer(fileBytes(image,relocation.r_offset),image.is64Bit() ? 8 : 4,object.isLittleEndian());
          if (!pointer) continue;
          addend = *pointer;
        }
        if (relative) { add(addend); continue; }
        auto table = file.getSection(section.sh_link);
        if (!table) { consumeError(table.takeError()); continue; }
        if ((*table)->sh_type != ELF::SHT_SYMTAB && (*table)->sh_type != ELF::SHT_DYNSYM) continue;
        auto symbols = file.symbols(*table);
        if (!symbols) { consumeError(symbols.takeError()); continue; }
        const auto index = relocation.getSymbol(false);
        if (index < symbols->size() && (*symbols)[index].st_shndx != ELF::SHN_UNDEF)
          add((*symbols)[index].st_value + addend);
      }
    };
    if (section.sh_type == ELF::SHT_RELA) {
      auto relocations = file.relas(section);
      if (relocations) process(*relocations);
      else consumeError(relocations.takeError());
    } else {
      auto relocations = file.rels(section);
      if (relocations) process(*relocations);
      else consumeError(relocations.takeError());
    }
  }
}
}

std::vector<std::uint64_t> collectFunctionEntries(const llvm::object::ObjectFile &object,
                                                 const LoadedImage &image) {
  using namespace llvm;
  using namespace llvm::object;
  std::vector<uint64_t> entries;
  if (!image.hasVirtualLayout()) return entries;
  const unsigned pointerSize = image.is64Bit() ? 8 : 4;
  const bool little = object.isLittleEndian();
  const auto executable = image.executableSections();
  auto valid = [&](uint64_t address) {
    if (image.architecture() == Architecture::ARM64 && address % 4) return false;
    return std::any_of(executable.begin(), executable.end(), [address](const Section *section) {
      return address >= section->address && address - section->address < section->bytes.size();
    });
  };
  auto add = [&](uint64_t address) { if (valid(address)) entries.push_back(address); };

  if (isa<ELFObjectFileBase>(object)) {
    for (const auto &section : image.sections()) {
      if (section.name == ".eh_frame") {
        DWARFDebugFrame frames(object.getArch(), true, section.address);
        if (auto error = frames.parse(DWARFDataExtractor(text(section.bytes), little, pointerSize))) {
          consumeError(std::move(error));
          continue;
        }
        for (const auto &entry : frames.entries())
          if (const auto *fde = dyn_cast<dwarf::FDE>(&entry)) {
            const auto start = fde->getInitialLocation(), size = fde->getAddressRange();
            if (size && start <= UINT64_MAX - (size - 1) && valid(start + size - 1)) add(start);
          }
      } else if (section.name == ".eh_frame_hdr" && section.bytes.size() >= 4 && section.bytes[0] == 1) {
        DataExtractor data(text(section.bytes), little, pointerSize);
        uint64_t offset = 4;
        auto frame = encodedPointer(image, data, offset, section.bytes[1], section.address, pointerSize);
        auto count = encodedPointer(image, data, offset, section.bytes[2], section.address, pointerSize);
        if (!frame || fileBytes(image, *frame).size() < 4 || !count ||
            *count > section.bytes.size() / 2 || *count > (1u << 20)) continue;
        std::vector<uint64_t> table;
        bool complete = true;
        for (uint64_t i = 0; i < *count; ++i) {
          auto start = encodedPointer(image, data, offset, section.bytes[3], section.address, pointerSize);
          auto fde = encodedPointer(image, data, offset, section.bytes[3], section.address, pointerSize);
          if (!start || !fde || fileBytes(image, *fde).size() < 4 ||
              (!table.empty() && *start < table.back())) { complete = false; break; }
          table.push_back(*start);
        }
        if (complete) for (auto address : table) add(address);
      } else if (section.name == ".init_array" || section.name == ".fini_array" ||
                 section.name == ".preinit_array") {
        for (size_t offset = 0; offset + pointerSize <= section.bytes.size(); offset += pointerSize)
          if (auto pointer = integer(std::span(section.bytes).subspan(offset), pointerSize, little)) add(*pointer);
      }
    }
    if (const auto *elf = dyn_cast<ELF64LEObjectFile>(&object)) elfRelocations(*elf,image,add);
    else if (const auto *elf = dyn_cast<ELF64BEObjectFile>(&object)) elfRelocations(*elf,image,add);
    else if (const auto *elf = dyn_cast<ELF32LEObjectFile>(&object)) elfRelocations(*elf,image,add);
    else if (const auto *elf = dyn_cast<ELF32BEObjectFile>(&object)) elfRelocations(*elf,image,add);
  } else if (const auto *coff = dyn_cast<COFFObjectFile>(&object)) {
    auto va = [&](uint64_t rva) -> std::optional<uint64_t> {
      if (rva > UINT64_MAX - image.imageBase()) return {};
      return image.imageBase() + rva;
    };
    // PE x64 RUNTIME_FUNCTION triples (BeginAddress, EndAddress, UnwindInfo).
    // The exception directory is authoritative; .pdata need not keep its name.
    if (image.architecture() == Architecture::X86_64)
      if (const auto *directory = coff->getDataDirectory(COFF::EXCEPTION_TABLE))
        if (auto address = va(directory->RelativeVirtualAddress)) {
          const auto bytes = fileBytes(image, *address);
          const uint64_t size = directory->Size;
          if (size <= bytes.size() && size % 12 == 0)
            for (uint64_t offset = 0; offset < size; offset += 12) {
              const auto begin = *integer(bytes.subspan(offset), 4, true);
              const auto end = *integer(bytes.subspan(offset + 4), 4, true);
              const auto unwind = *integer(bytes.subspan(offset + 8), 4, true);
              auto start = va(begin), last = end ? va(end - 1) : std::nullopt;
              auto info = va(unwind);
              if (begin < end && start && last && valid(*last) && info &&
                  fileBytes(image, *info).size() >= 4) add(*start);
            }
        }
    uint64_t callbacks = 0;
    if (const auto *tls = coff->getTLSDirectory64()) callbacks = tls->AddressOfCallBacks;
    else if (const auto *tls = coff->getTLSDirectory32()) callbacks = tls->AddressOfCallBacks;
    if (callbacks) {
      const auto bytes = fileBytes(image, callbacks);
      const size_t limit = std::min<size_t>(bytes.size() / pointerSize, 1u << 16);
      for (size_t index = 0; index < limit; ++index) {
        const auto pointer = *integer(bytes.subspan(index * pointerSize), pointerSize, true);
        if (!pointer) break;
        add(pointer);
      }
    }
    for (const auto *symbol : image.exports())
      if (symbol->kind == SymbolKind::Function) add(symbol->address);
  }
  std::sort(entries.begin(), entries.end());
  entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
  return entries;
}
}
