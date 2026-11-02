#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dragon/Architecture.hpp>

/// Container-format loading: turn an ELF or PE file on disk (or a blob of its
/// bytes) into a LoadedImage the rest of dragon can disassemble and analyse,
/// without shelling out to another tool first.
///
/// The parse is delegated to LLVM's `llvm::object` library (already a
/// dependency of libdragon, via the `Object` component), so no new third-party
/// code is pulled in. Everything LLVM hands back is *copied out* into the owned
/// std structures below, so a LoadedImage carries no LLVM types in its public
/// surface and outlives the ObjectFile it was built from. That is what lets the
/// C shim and the Python API wrap it without exposing any LLVM header.
///
/// A LoadedImage models an *image* - sections placed at their virtual
/// addresses, an entry point, an image base - which is what an executable or a
/// shared object/DLL is. A relocatable object file (`.o`/`.obj`) loads too, but
/// its sections sit at address 0 and it has no entry point; callers that need a
/// real address space should check hasVirtualLayout().
namespace dragon::loader {

/// The container format a LoadedImage was parsed from.
enum class Format : std::uint8_t {
  Unknown = 0,
  ELF, ///< ELF executable, shared object, or relocatable
  PE,  ///< PE/COFF image (.exe/.dll) or COFF object
};

/// Human-readable name ("elf", "pe", "unknown").
[[nodiscard]] std::string_view formatName(Format f);

/// One section of the image, with its virtual placement, permissions, and the
/// raw bytes the file provides for it.
///
/// `bytes` holds the on-disk contents. For a BSS-style section it is empty even
/// though `virtualSize` is non-zero; readVirtual() zero-fills the gap, matching
/// what the loader would map at run time.
struct Section {
  std::string name;
  std::uint64_t address{};     ///< virtual address (image base already applied)
  std::uint64_t virtualSize{}; ///< size when mapped, in bytes
  std::vector<std::uint8_t> bytes; ///< file-provided contents (<= virtualSize)

  bool readable{};
  bool writable{};
  bool executable{};
  /// Part of the loaded image. False for ELF sections without SHF_ALLOC
  /// (.symtab, .strtab, .comment, debug info): their sh_addr is 0 and they
  /// are not in the address space, so no address lookup may land in them.
  bool mapped{true};

  /// A section that occupies address space but has no file bytes (.bss / PE
  /// uninitialised data). `bytes` is empty for these.
  [[nodiscard]] bool isBss() const { return bytes.empty() && virtualSize != 0; }

  [[nodiscard]] bool contains(std::uint64_t addr) const {
    return mapped && addr >= address && addr - address < virtualSize;
  }
};

/// What a symbol names.
enum class SymbolKind : std::uint8_t {
  Unknown = 0,
  Function,
  Object, ///< data object (variable)
  Section,
  File,
};

/// A symbol-table entry, flattened to the handful of facts an analysis needs.
///
/// `imported` is a symbol the image expects the loader to supply (a libc/DLL
/// import). For ELF it comes from .symtab and .dynsym (undefined entries) and
/// its `address` is 0. For PE it is one Import Address Table entry and its
/// `address` is that IAT slot's virtual address - the location an indirect
/// `call [X]` reads; an import by ordinal is named "#<ordinal>".
///
/// `exported` is a defined symbol visible to other modules: for ELF a global
/// or weak .dynsym/.symtab definition, for PE an export-directory entry
/// (forwarders are skipped). A symbol can be neither (a local). A linked PE's
/// COFF symbol table, if present, is debug information and contributes
/// neither imports nor exports.
struct Symbol {
  std::string name;
  std::uint64_t address{};
  std::uint64_t size{};
  SymbolKind kind{SymbolKind::Unknown};
  bool imported{};
  bool exported{};
};

class LoadedImage {
  public:
  /// Load from a path. Returns nullopt on any failure (not found, unreadable,
  /// not an object file, truncated); when `error` is non-null it receives a
  /// one-line reason.
  [[nodiscard]] static std::optional<LoadedImage>
  loadFile(const std::string &path, std::string *error = nullptr);

  /// Load from a blob already in memory - the bytes are copied, so the caller's
  /// buffer need not outlive the call.
  [[nodiscard]] static std::optional<LoadedImage>
  loadBytes(std::vector<std::uint8_t> bytes, std::string *error = nullptr);

  // -- image-level facts --------------------------------------------------

  [[nodiscard]] Format format() const { return format_; }

  /// The architecture detected from the header. Disassembler/Graph take this
  /// directly, so a caller never has to name the architecture by hand.
  /// Architecture::Unknown if the format carried one dragon does not model.
  [[nodiscard]] Architecture architecture() const { return arch_; }

  [[nodiscard]] bool is64Bit() const { return is64_; }

  /// Absolute virtual address of the entry point, or nullopt for a format that
  /// has none (a relocatable object, or a DLL/.so with no entry).
  [[nodiscard]] std::optional<std::uint64_t> entryPoint() const {
    return entry_;
  }

  /// The address the image is based at (PE ImageBase; for ELF the lowest
  /// PT_LOAD address). Zero for a relocatable object.
  [[nodiscard]] std::uint64_t imageBase() const { return imageBase_; }

  /// True when sections or segments carry real virtual addresses (an
  /// executable / shared object / DLL), false for a relocatable object whose
  /// sections sit at 0.
  [[nodiscard]] bool hasVirtualLayout() const { return hasVirtualLayout_; }

  // -- sections -----------------------------------------------------------

  [[nodiscard]] const std::vector<Section> &sections() const {
    return sections_;
  }

  /// The section whose mapped range covers `addr`, or nullptr.
  [[nodiscard]] const Section *sectionContaining(std::uint64_t addr) const;

  /// Executable sections, in address order - the ones worth disassembling.
  /// When the image has none (an ELF whose section headers were stripped),
  /// the executable segments are returned instead.
  [[nodiscard]] std::vector<const Section *> executableSections() const;

  // -- segments -----------------------------------------------------------
  /// ELF PT_LOAD program headers, as Sections named "LOAD0", "LOAD1", ...:
  /// address/virtualSize are p_vaddr/p_memsz and `bytes` the p_filesz file
  /// bytes. This is what the system loader really maps, and the only layout
  /// left when a packer/protector strips the section headers. Empty for PE
  /// and relocatable objects.
  [[nodiscard]] const std::vector<Section> &segments() const {
    return segments_;
  }

  /// Read `size` bytes from the virtual address space, spanning whatever
  /// section covers `addr` (or, failing that, segment) and zero-filling any
  /// BSS tail. Returns nullopt if `addr` is not mapped or the run would leave
  /// the covering section/segment.
  [[nodiscard]] std::optional<std::vector<std::uint8_t>>
  readVirtual(std::uint64_t addr, std::size_t size) const;

  // -- symbols ------------------------------------------------------------

  [[nodiscard]] const std::vector<Symbol> &symbols() const { return symbols_; }

  /// Defined symbols only (imports excluded), in address order.
  [[nodiscard]] std::vector<const Symbol *> definedSymbols() const;

  /// Undefined/external symbols the image imports.
  [[nodiscard]] std::vector<const Symbol *> imports() const;

  /// Defined global symbols the image exports.
  [[nodiscard]] std::vector<const Symbol *> exports() const;

  /// The function-typed symbol defined exactly at `addr`, or nullptr. Used to
  /// seed and name recovered functions.
  [[nodiscard]] const Symbol *functionAt(std::uint64_t addr) const;

  /// Valid file-backed executable addresses from unwind metadata, initializer
  /// arrays, code relocations and PE TLS callbacks. Sorted and deduplicated;
  /// these are analysis seeds, not invented symbols.
  [[nodiscard]] const std::vector<std::uint64_t> &functionEntries() const {
    return functionEntries_;
  }

  private:
  LoadedImage() = default;

  Format format_{Format::Unknown};
  Architecture arch_{Architecture::Unknown};
  bool is64_{};
  bool hasVirtualLayout_{};
  std::optional<std::uint64_t> entry_;
  std::uint64_t imageBase_{};
  std::vector<Section> sections_;
  std::vector<Section> segments_;
  std::vector<Symbol> symbols_;
  std::vector<std::uint64_t> functionEntries_;
};

} // namespace dragon::loader
