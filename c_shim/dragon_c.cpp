#include "dragon_c.h"

#include <dragon/Architecture.hpp>
#ifdef DRAGON_WITH_Z3
#include <dragon/analysis/SliceExprZ3.hpp>
#include <dragon/analysis/ValueEnumerator.hpp>
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#endif
#include <dragon/assembler/Assembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <dragon/passes/FiniteDispatch.hpp>
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/Configuration.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Function.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/loader/ImageDisassembler.hpp>
#include <dragon/loader/Loader.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/ignil/IGNILSimplify.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/disassembler/Instruction.hpp>
#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/sigs/Signature.hpp>
#include <dragon/sigs/SignatureDB.hpp>
#include <dragon/sigs/SignatureMatcher.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// -- helpers to convert C++ types back to C enum values
static dragon_syntax_t from_syntax(dragon::Syntax s) {
  return s == dragon::Syntax::ATT ? DRAGON_SYNTAX_ATT : DRAGON_SYNTAX_INTEL;
}
static dragon_strategy_t from_strategy(dragon::DisassemblyStrategy s) {
  return s == dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL
                 ? DRAGON_STRATEGY_RECURSIVE_TRAVERSAL
                 : DRAGON_STRATEGY_LINEAR_SWEEP;
}

// -- enum mappings
static dragon::Architecture to_arch(dragon_architecture_t a) {
  switch (a) {
    case DRAGON_ARCH_X86_32:
      return dragon::Architecture::X86_32;
    case DRAGON_ARCH_X86_64:
      return dragon::Architecture::X86_64;
    case DRAGON_ARCH_ARM32:
      return dragon::Architecture::ARM32;
    case DRAGON_ARCH_ARM64:
      return dragon::Architecture::ARM64;
    case DRAGON_ARCH_RISCV32:
      return dragon::Architecture::RISCV32;
    case DRAGON_ARCH_RISCV64:
      return dragon::Architecture::RISCV64;
    default:
      return dragon::Architecture::Unknown;
  }
}

static dragon_architecture_t from_arch(dragon::Architecture a) {
  switch (a) {
    case dragon::Architecture::X86_32:
      return DRAGON_ARCH_X86_32;
    case dragon::Architecture::X86_64:
      return DRAGON_ARCH_X86_64;
    case dragon::Architecture::ARM32:
      return DRAGON_ARCH_ARM32;
    case dragon::Architecture::ARM64:
      return DRAGON_ARCH_ARM64;
    case dragon::Architecture::RISCV32:
      return DRAGON_ARCH_RISCV32;
    case dragon::Architecture::RISCV64:
      return DRAGON_ARCH_RISCV64;
    default:
      return DRAGON_ARCH_UNKNOWN;
  }
}

static dragon::Syntax to_syntax(dragon_syntax_t s) {
  return s == DRAGON_SYNTAX_ATT ? dragon::Syntax::ATT : dragon::Syntax::INTEL;
}

// -- dragon_config - full definition
struct dragon_config {
  dragon::Configuration config;
};

dragon_config_t *dragon_config_create(dragon_syntax_t syntax,
                                      dragon_strategy_t strategy) {
  dragon::Configuration cfg;
  cfg.syntax = to_syntax(syntax);
  cfg.disassemblyStrategy =
          (strategy == DRAGON_STRATEGY_RECURSIVE_TRAVERSAL)
                  ? dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL
                  : dragon::DisassemblyStrategy::LINEAR_SWEEP;
  return new dragon_config{cfg};
}

void dragon_config_destroy(dragon_config_t *c) { delete c; }

dragon_syntax_t dragon_config_syntax(const dragon_config_t *c) {
  return from_syntax(c->config.syntax);
}

dragon_strategy_t dragon_config_strategy(const dragon_config_t *c) {
  return from_strategy(c->config.disassemblyStrategy);
}

void dragon_config_set_cpu(dragon_config_t *c, const char *cpu) {
  if (c)
    c->config.cpu = cpu ? cpu : "";
}

const char *dragon_config_cpu(const dragon_config_t *c) {
  return c ? c->config.cpu.c_str() : "";
}

void dragon_config_set_features(dragon_config_t *c, const char *features) {
  if (c)
    c->config.features = features ? features : "";
}

const char *dragon_config_features(const dragon_config_t *c) {
  return c ? c->config.features.c_str() : "";
}

void dragon_config_set_logging(dragon_config_t *c, int enabled) {
  if (c)
    c->config.logging = enabled != 0;
}

int dragon_config_logging(const dragon_config_t *c) {
  return c && c->config.logging ? 1 : 0;
}

static_assert(static_cast<int>(dragon::CallingConvention::Default) == DRAGON_CC_DEFAULT);
static_assert(static_cast<int>(dragon::CallingConvention::SysV) == DRAGON_CC_SYSV);
static_assert(static_cast<int>(dragon::CallingConvention::Win64) == DRAGON_CC_WIN64);

int dragon_config_set_calling_convention(dragon_config_t *c,
                                         dragon_calling_convention_t cc) {
  if (!c || (cc != DRAGON_CC_DEFAULT && cc != DRAGON_CC_SYSV && cc != DRAGON_CC_WIN64))
    return 0;
  c->config.callingConvention = static_cast<dragon::CallingConvention>(cc);
  return 1;
}

dragon_calling_convention_t dragon_config_calling_convention(const dragon_config_t *c) {
  return c ? static_cast<dragon_calling_convention_t>(c->config.callingConvention)
           : DRAGON_CC_DEFAULT;
}

struct dragon_analysis_hints {
  dragon::Disassembler::AnalysisHints hints;
};

dragon_analysis_hints_t *dragon_analysis_hints_create(void) {
  try { return new dragon_analysis_hints{}; }
  catch (...) { return nullptr; }
}

void dragon_analysis_hints_destroy(dragon_analysis_hints_t *h) { delete h; }

int dragon_analysis_hints_set_entries(dragon_analysis_hints_t *h,
                                       const uint64_t *entries, size_t count) {
  if (!h || (!entries && count)) return 0;
  try {
    std::vector<uint64_t> copy;
    if (count) copy.assign(entries, entries + count);
    h->hints.entryPoints = std::move(copy);
    return 1;
  } catch (...) { return 0; }
}

size_t dragon_analysis_hints_entry_count(const dragon_analysis_hints_t *h) {
  return h ? h->hints.entryPoints.size() : 0;
}

int dragon_analysis_hints_entry(const dragon_analysis_hints_t *h,
                                 size_t index, uint64_t *out) {
  if (!h || !out || index >= h->hints.entryPoints.size()) return 0;
  *out = h->hints.entryPoints[index];
  return 1;
}

int dragon_analysis_hints_set_memory_reader(dragon_analysis_hints_t *h,
                                            dragon_memory_reader_t reader,
                                            void *user) {
  if (!h) return 0;
  try {
    if (reader)
      h->hints.readMemory = [reader, user](uint64_t address, std::span<uint8_t> out) {
        return reader(address, out.data(), out.size(), user) == 1;
      };
    else h->hints.readMemory = {};
    return 1;
  } catch (...) { return 0; }
}

int dragon_analysis_hints_set_known_memory_reader(dragon_analysis_hints_t *h,
    dragon_known_memory_reader_t reader, void *user) {
  if (!h) return 0;
  try {
    if (reader)
      h->hints.readKnownMemory = [reader, user](
          const dragon::Disassembler::MemoryReadSite &site,
          uint64_t address, std::span<uint8_t> out) {
        const int result = reader(site.blockAddress, site.opIndex, site.instructionAddress,
                                  address, out.data(), out.size(), user);
        if (result < 0) throw std::runtime_error("known memory reader failed");
        return result == 1;
      };
    else h->hints.readKnownMemory = {};
    return 1;
  } catch (...) { return 0; }
}

// -- dragon_asm_t - full definition
struct dragon_asm {
  dragon::Assembler assembler;
};

dragon_asm_t *dragon_asm_create(dragon_architecture_t arch,
                                const dragon_config_t *config) {
  dragon::Configuration cfg = config ? config->config : dragon::Configuration{};
  return new dragon_asm{dragon::Assembler(to_arch(arch), cfg)};
}

void dragon_asm_destroy(dragon_asm_t *a) { delete a; }

int dragon_asm_valid(const dragon_asm_t *a) {
  return a && a->assembler.isValid() ? 1 : 0;
}

uint8_t *dragon_asm_assemble(dragon_asm_t *a, const char *source,
                             size_t *out_len) {
  if (!a || !source || !out_len)
    return nullptr;
  auto bytes = a->assembler.assemble(source);
  if (bytes.empty()) {
    *out_len = 0;
    return nullptr;
  }
  auto *buf = new uint8_t[bytes.size()];
  std::copy(bytes.begin(), bytes.end(), buf);
  *out_len = bytes.size();
  return buf;
}

void dragon_bytes_free(uint8_t *bytes) { delete[] bytes; }

// -- dragon_disasm_t - full definition
struct dragon_disasm {
  dragon::Architecture arch;
  dragon::Configuration config;
  dragon::Disassembler disassembler;
};

// -- cast helpers
// --------------------------------------------------------------

static dragon::Graph *as_graph(dragon_graph_t *g) {
  return reinterpret_cast<dragon::Graph *>(g);
}
static const dragon::Graph *as_graph(const dragon_graph_t *g) {
  return reinterpret_cast<const dragon::Graph *>(g);
}
static const dragon::Instruction *as_insn(const dragon_instruction_t *i) {
  return reinterpret_cast<const dragon::Instruction *>(i);
}
static const dragon::BasicBlock *as_block(const dragon_block_t *b) {
  return reinterpret_cast<const dragon::BasicBlock *>(b);
}
static const dragon::Function *as_fn(const dragon_function_t *f) {
  return reinterpret_cast<const dragon::Function *>(f);
}

// -- Graph
// ---------------------------------------------------------------------

dragon_graph_t *dragon_graph_create(dragon_architecture_t arch) {
  dragon::Configuration config;
  auto *g = new dragon::Graph(to_arch(arch), config);
  return reinterpret_cast<dragon_graph_t *>(g);
}

void dragon_graph_destroy(dragon_graph_t *g) { delete as_graph(g); }

size_t dragon_graph_size(const dragon_graph_t *g) {
  return as_graph(g)->size();
}
int dragon_graph_empty(const dragon_graph_t *g) {
  return as_graph(g)->empty() ? 1 : 0;
}

const dragon_instruction_t *
dragon_graph_get_instruction(const dragon_graph_t *g, uint64_t address) {
  const dragon::Instruction *i = as_graph(g)->get(address);
  return reinterpret_cast<const dragon_instruction_t *>(i);
}

dragon_block_t *dragon_graph_get_block(const dragon_graph_t *g,
                                       uint64_t address) {
  auto *b = new dragon::BasicBlock(as_graph(g), address);
  return reinterpret_cast<dragon_block_t *>(b);
}

dragon_function_t *dragon_graph_get_function(const dragon_graph_t *g,
                                             uint64_t address) {
  auto *f = new dragon::Function(as_graph(g), address);
  return reinterpret_cast<dragon_function_t *>(f);
}

void dragon_graph_each_instruction(const dragon_graph_t *g,
                                   void (*cb)(const dragon_instruction_t *,
                                              void *),
                                   void *user) {
  for (const auto &[addr, insn]: as_graph(g)->listing())
    cb(reinterpret_cast<const dragon_instruction_t *>(&insn), user);
}

void dragon_graph_each_block_address(const dragon_graph_t *g,
                                     void (*cb)(uint64_t, void *), void *user) {
  for (uint64_t addr: as_graph(g)->validBlockAddresses())
    cb(addr, user);
}

void dragon_graph_each_function_address(const dragon_graph_t *g,
                                        void (*cb)(uint64_t, void *),
                                        void *user) {
  for (uint64_t addr: as_graph(g)->validFunctionAddresses())
    cb(addr, user);
}

// -- Disassembler
// --------------------------------------------------------------

dragon_disasm_t *dragon_disasm_create(dragon_architecture_t arch,
                                      const dragon_config_t *config) {
  dragon::Configuration cfg = config ? config->config : dragon::Configuration{};
  dragon::Architecture a = to_arch(arch);
  return new dragon_disasm{a, cfg, dragon::Disassembler(a, cfg)};
}

void dragon_disasm_destroy(dragon_disasm_t *d) { delete d; }

int dragon_disasm_valid(const dragon_disasm_t *d) {
  return d && d->disassembler.isValid() ? 1 : 0;
}

dragon_graph_t *dragon_disasm_disassemble(dragon_disasm_t *d,
                                          const uint8_t *bytes, size_t len,
                                          uint64_t base_address) {
  if (!d || !bytes)
    return nullptr;
  auto instrs = d->disassembler.disassemble({bytes, len}, base_address);
  auto *g = new dragon::Graph(d->arch, d->config);
  for (auto &insn: instrs)
    g->addInstruction(std::move(insn));
  return reinterpret_cast<dragon_graph_t *>(g);
}

dragon_graph_t *dragon_disasm_analyze(dragon_disasm_t *d, const uint8_t *bytes,
                                      size_t len, uint64_t base_address) {
  if (!d || !bytes)
    return nullptr;
  auto *g = new dragon::Graph(d->arch, d->config);
  d->disassembler.disassemble({bytes, len}, base_address, *g);
  return reinterpret_cast<dragon_graph_t *>(g);
}

// -- Loader
// ---------------------------------------------------------------

static dragon::loader::LoadedImage *as_loader(dragon_loader_t *l) {
  return reinterpret_cast<dragon::loader::LoadedImage *>(l);
}
static const dragon::loader::LoadedImage *as_loader(const dragon_loader_t *l) {
  return reinterpret_cast<const dragon::loader::LoadedImage *>(l);
}

static void set_err(char *err, size_t cap, const std::string &msg) {
  if (!err || cap == 0)
    return;
  size_t n = std::min(cap - 1, msg.size());
  std::memcpy(err, msg.data(), n);
  err[n] = '\0';
}

dragon_graph_t *dragon_disasm_analyze_with_hints(dragon_disasm_t *d,
    const uint8_t *bytes, size_t len, uint64_t base_address,
    const dragon_analysis_hints_t *hints, char *err, size_t err_cap) {
  set_err(err, err_cap, "");
  if (!d || !bytes || !d->disassembler.isValid()) {
    set_err(err, err_cap, "invalid disassembler or bytes");
    return nullptr;
  }
  try {
    auto g = std::make_unique<dragon::Graph>(d->arch, d->config);
    // Snapshot before entering user code: a callback may mutate/close hints.
    const auto snapshot = hints ? hints->hints : dragon::Disassembler::AnalysisHints{};
    d->disassembler.disassemble({bytes, len}, base_address, *g,
                                snapshot);
    return reinterpret_cast<dragon_graph_t *>(g.release());
  } catch (const std::exception &e) {
    set_err(err, err_cap, e.what());
  } catch (...) { set_err(err, err_cap, "analysis failed"); }
  return nullptr;
}

dragon_loader_t *dragon_load_file(const char *path, char *err, size_t cap) {
  if (!path) {
    set_err(err, cap, "null path");
    return nullptr;
  }
  std::string e;
  auto img = dragon::loader::LoadedImage::loadFile(path, &e);
  if (!img) {
    set_err(err, cap, e);
    return nullptr;
  }
  return reinterpret_cast<dragon_loader_t *>(
      new dragon::loader::LoadedImage(std::move(*img)));
}

dragon_loader_t *dragon_load_bytes(const uint8_t *bytes, size_t len, char *err,
                                   size_t cap) {
  if (!bytes) {
    set_err(err, cap, "null bytes");
    return nullptr;
  }
  std::string e;
  auto img = dragon::loader::LoadedImage::loadBytes(
      std::vector<std::uint8_t>(bytes, bytes + len), &e);
  if (!img) {
    set_err(err, cap, e);
    return nullptr;
  }
  return reinterpret_cast<dragon_loader_t *>(
      new dragon::loader::LoadedImage(std::move(*img)));
}

void dragon_loader_destroy(dragon_loader_t *l) { delete as_loader(l); }

dragon_format_t dragon_loader_format(const dragon_loader_t *l) {
  switch (as_loader(l)->format()) {
    case dragon::loader::Format::ELF:
      return DRAGON_FORMAT_ELF;
    case dragon::loader::Format::PE:
      return DRAGON_FORMAT_PE;
    default:
      return DRAGON_FORMAT_UNKNOWN;
  }
}

dragon_architecture_t dragon_loader_architecture(const dragon_loader_t *l) {
  return from_arch(as_loader(l)->architecture());
}

int dragon_loader_is_64bit(const dragon_loader_t *l) {
  return as_loader(l)->is64Bit() ? 1 : 0;
}

int dragon_loader_has_entry(const dragon_loader_t *l) {
  return as_loader(l)->entryPoint().has_value() ? 1 : 0;
}

uint64_t dragon_loader_entry(const dragon_loader_t *l) {
  return as_loader(l)->entryPoint().value_or(0);
}

uint64_t dragon_loader_image_base(const dragon_loader_t *l) {
  return as_loader(l)->imageBase();
}

namespace {

// Sections and segments share one representation; these back both families.
const char *describe_region(const std::vector<dragon::loader::Section> &v,
                            size_t index, uint64_t *out_addr,
                            uint64_t *out_vsize, int *out_r, int *out_w,
                            int *out_x) {
  if (index >= v.size())
    return nullptr;
  const auto &s = v[index];
  if (out_addr)
    *out_addr = s.address;
  if (out_vsize)
    *out_vsize = s.virtualSize;
  if (out_r)
    *out_r = s.readable ? 1 : 0;
  if (out_w)
    *out_w = s.writable ? 1 : 0;
  if (out_x)
    *out_x = s.executable ? 1 : 0;
  return s.name.c_str();
}

const uint8_t *region_bytes(const std::vector<dragon::loader::Section> &v,
                            size_t index, size_t *out_len) {
  if (index >= v.size()) {
    if (out_len)
      *out_len = 0;
    return nullptr;
  }
  if (out_len)
    *out_len = v[index].bytes.size();
  return v[index].bytes.data();
}

} // namespace

size_t dragon_loader_section_count(const dragon_loader_t *l) {
  return as_loader(l)->sections().size();
}

const char *dragon_loader_section(const dragon_loader_t *l, size_t index,
                                  uint64_t *out_addr, uint64_t *out_vsize,
                                  int *out_r, int *out_w, int *out_x) {
  return describe_region(as_loader(l)->sections(), index, out_addr, out_vsize,
                         out_r, out_w, out_x);
}

const uint8_t *dragon_loader_section_bytes(const dragon_loader_t *l,
                                           size_t index, size_t *out_len) {
  return region_bytes(as_loader(l)->sections(), index, out_len);
}

size_t dragon_loader_segment_count(const dragon_loader_t *l) {
  return as_loader(l)->segments().size();
}

const char *dragon_loader_segment(const dragon_loader_t *l, size_t index,
                                  uint64_t *out_addr, uint64_t *out_vsize,
                                  int *out_r, int *out_w, int *out_x) {
  return describe_region(as_loader(l)->segments(), index, out_addr, out_vsize,
                         out_r, out_w, out_x);
}

const uint8_t *dragon_loader_segment_bytes(const dragon_loader_t *l,
                                           size_t index, size_t *out_len) {
  return region_bytes(as_loader(l)->segments(), index, out_len);
}

size_t dragon_loader_symbol_count(const dragon_loader_t *l) {
  return as_loader(l)->symbols().size();
}

const char *dragon_loader_symbol(const dragon_loader_t *l, size_t index,
                                 uint64_t *out_addr, uint64_t *out_size,
                                 dragon_symbol_kind_t *out_kind,
                                 int *out_imported, int *out_exported) {
  const auto &syms = as_loader(l)->symbols();
  if (index >= syms.size())
    return nullptr;
  const auto &s = syms[index];
  if (out_addr)
    *out_addr = s.address;
  if (out_size)
    *out_size = s.size;
  if (out_kind) {
    switch (s.kind) {
      case dragon::loader::SymbolKind::Function:
        *out_kind = DRAGON_SYMBOL_FUNCTION;
        break;
      case dragon::loader::SymbolKind::Object:
        *out_kind = DRAGON_SYMBOL_OBJECT;
        break;
      case dragon::loader::SymbolKind::Section:
        *out_kind = DRAGON_SYMBOL_SECTION;
        break;
      case dragon::loader::SymbolKind::File:
        *out_kind = DRAGON_SYMBOL_FILE;
        break;
      default:
        *out_kind = DRAGON_SYMBOL_UNKNOWN;
        break;
    }
  }
  if (out_imported)
    *out_imported = s.imported ? 1 : 0;
  if (out_exported)
    *out_exported = s.exported ? 1 : 0;
  return s.name.c_str();
}

size_t dragon_loader_read_virtual(const dragon_loader_t *l, uint64_t addr,
                                  uint8_t *out, size_t cap) {
  if (!out || cap == 0)
    return 0;
  auto bytes = as_loader(l)->readVirtual(addr, cap);
  if (!bytes)
    return 0;
  std::memcpy(out, bytes->data(), bytes->size());
  return bytes->size();
}

dragon_graph_t *dragon_loader_disassemble(const dragon_loader_t *l,
                                          const dragon_config_t *config) {
  return dragon_loader_disassemble_with_hints(l, config, nullptr);
}

dragon_graph_t *dragon_loader_disassemble_with_hints(const dragon_loader_t *l,
    const dragon_config_t *config, const dragon_analysis_hints_t *hints) {
  if (!l) return nullptr;
  try {
    const auto cfg = config ? config->config : dragon::Configuration{};
    const auto snapshot = hints ? hints->hints : dragon::Disassembler::AnalysisHints{};
    auto graph = std::make_unique<dragon::Graph>(
        dragon::loader::disassembleImage(*as_loader(l), cfg, snapshot));
    return reinterpret_cast<dragon_graph_t *>(graph.release());
  } catch (...) { return nullptr; }
}

// -- Instruction
// ---------------------------------------------------------------

uint64_t dragon_instruction_address(const dragon_instruction_t *i) {
  return as_insn(i)->address();
}
uint8_t dragon_instruction_size(const dragon_instruction_t *i) {
  return as_insn(i)->size();
}
const char *dragon_instruction_mnemonic(const dragon_instruction_t *i) {
  return as_insn(i)->mnemonic().data();
}
const char *dragon_instruction_operands(const dragon_instruction_t *i) {
  return as_insn(i)->operands().data();
}
const char *dragon_instruction_disassembly(const dragon_instruction_t *i) {
  return as_insn(i)->disassembly().data();
}
const char *dragon_instruction_pattern(const dragon_instruction_t *i) {
  return as_insn(i)->pattern().data();
}

const uint8_t *dragon_instruction_bytes(const dragon_instruction_t *i,
                                        size_t *out_len) {
  const auto &b = as_insn(i)->bytes();
  if (out_len)
    *out_len = b.size();
  return b.data();
}

const uint8_t *dragon_instruction_chromosome_mask(const dragon_instruction_t *i,
                                                  size_t *out_len) {
  const auto &m = as_insn(i)->chromosomeMask();
  if (out_len)
    *out_len = m.size();
  return m.data();
}

int dragon_instruction_is_block_start(const dragon_instruction_t *i) {
  return as_insn(i)->isBlockStart() ? 1 : 0;
}
int dragon_instruction_is_function_start(const dragon_instruction_t *i) {
  return as_insn(i)->isFunctionStart() ? 1 : 0;
}
int dragon_instruction_is_prologue(const dragon_instruction_t *i) {
  return as_insn(i)->isPrologue() ? 1 : 0;
}
int dragon_instruction_is_return(const dragon_instruction_t *i) {
  return as_insn(i)->isReturn() ? 1 : 0;
}
int dragon_instruction_is_call(const dragon_instruction_t *i) {
  return as_insn(i)->isCall() ? 1 : 0;
}
int dragon_instruction_is_jump(const dragon_instruction_t *i) {
  return as_insn(i)->isJump() ? 1 : 0;
}
int dragon_instruction_is_conditional(const dragon_instruction_t *i) {
  return as_insn(i)->isConditional() ? 1 : 0;
}
int dragon_instruction_is_trap(const dragon_instruction_t *i) {
  return as_insn(i)->isTrap() ? 1 : 0;
}
int dragon_instruction_has_indirect_target(const dragon_instruction_t *i) {
  return as_insn(i)->hasIndirectTarget() ? 1 : 0;
}
size_t dragon_instruction_edges(const dragon_instruction_t *i) {
  return as_insn(i)->edges();
}

void dragon_instruction_each_branch(const dragon_instruction_t *i,
                                    void (*cb)(uint64_t, void *), void *user) {
  for (uint64_t addr: as_insn(i)->branches())
    cb(addr, user);
}

void dragon_instruction_each_callee(const dragon_instruction_t *i,
                                    void (*cb)(uint64_t, void *), void *user) {
  for (uint64_t addr: as_insn(i)->callees())
    cb(addr, user);
}

// -- BasicBlock
void dragon_block_destroy(dragon_block_t *b) {
  delete reinterpret_cast<dragon::BasicBlock *>(b);
}
int dragon_block_valid(const dragon_block_t *b) {
  return as_block(b)->isValid() ? 1 : 0;
}
uint64_t dragon_block_start_address(const dragon_block_t *b) {
  return as_block(b)->startAddress();
}
uint64_t dragon_block_terminator_address(const dragon_block_t *b) {
  return as_block(b)->terminatorAddress();
}
size_t dragon_block_size(const dragon_block_t *b) {
  return as_block(b)->size();
}
size_t dragon_block_byte_size(const dragon_block_t *b) {
  return as_block(b)->byteSize();
}

void dragon_block_each_instruction(const dragon_block_t *b,
                                   void (*cb)(const dragon_instruction_t *,
                                              void *),
                                   void *user) {
  for (const dragon::Instruction *i: as_block(b)->instructions())
    cb(reinterpret_cast<const dragon_instruction_t *>(i), user);
}

void dragon_block_each_successor(const dragon_block_t *b,
                                 void (*cb)(uint64_t, void *), void *user) {
  for (const uint64_t addr: as_block(b)->successors())
    cb(addr, user);
}

// -- Function
// ------------------------------------------------------------------

void dragon_function_destroy(dragon_function_t *f) {
  delete reinterpret_cast<dragon::Function *>(f);
}
int dragon_function_valid(const dragon_function_t *f) {
  return as_fn(f)->isValid() ? 1 : 0;
}
uint64_t dragon_function_start_address(const dragon_function_t *f) {
  return as_fn(f)->startAddress();
}
size_t dragon_function_num_blocks(const dragon_function_t *f) {
  return as_fn(f)->numBlocks();
}
size_t dragon_function_num_instructions(const dragon_function_t *f) {
  return as_fn(f)->numInstructions();
}
size_t dragon_function_edges(const dragon_function_t *f) {
  return as_fn(f)->edges();
}
size_t dragon_function_byte_size(const dragon_function_t *f) {
  return as_fn(f)->byteSize();
}
size_t dragon_function_cyclomatic_complexity(const dragon_function_t *f) {
  return as_fn(f)->cyclomaticComplexity();
}

void dragon_function_each_block(const dragon_function_t *f,
                                void (*cb)(const dragon_block_t *, void *),
                                void *user) {
  for (const auto &[addr, block]: as_fn(f)->blocks())
    cb(reinterpret_cast<const dragon_block_t *>(&block), user);
}

// =============================================================================
// String helper
// =============================================================================

void dragon_cstr_free(char *s) { delete[] s; }

static char *dup_string(const std::string &s) {
  char *buf = new char[s.size() + 1];
  s.copy(buf, s.size());
  buf[s.size()] = '\0';
  return buf;
}

char *dragon_graph_finite_values(const dragon_graph_t *g,
    const uint64_t *entries,size_t count,uint64_t query_block,
    uint32_t query_offset,uint8_t query_width,uint32_t max_values,
    uint32_t max_blocks,uint32_t max_operations,uint32_t max_facts) {
  return dragon_graph_finite_values_with_calls(g,entries,count,query_block,
      query_offset,query_width,max_values,max_blocks,max_operations,max_facts,0);
}
char *dragon_graph_finite_values_with_calls(const dragon_graph_t *g,
    const uint64_t *entries,size_t count,uint64_t query_block,
    uint32_t query_offset,uint8_t query_width,uint32_t max_values,
    uint32_t max_blocks,uint32_t max_operations,uint32_t max_facts,uint8_t prove_leaf_calls) {
  if(!g || (count && !entries) || query_width>64 || prove_leaf_calls>1)return nullptr;
  try {
    std::set<std::uint64_t> roots;
    if(count)roots.insert(entries,entries+count);
    std::vector<dragon::analysis::FiniteValueQuery> queries;
    if(query_width)queries.push_back({query_block,dragon::ignil::Reg{query_offset,query_width,{}}});
    return dup_string(dragon::analysis::finiteValues(*as_graph(g),roots,queries,
      {max_values,max_blocks,max_operations,max_facts,prove_leaf_calls!=0}).toJSON());
  } catch(...) {return nullptr;}
}
static_assert(static_cast<int>(dragon::analysis::ValueSetKind::Unreachable)==DRAGON_VALUE_UNREACHABLE);
static_assert(static_cast<int>(dragon::analysis::ValueSetKind::Finite)==DRAGON_VALUE_FINITE);
static_assert(static_cast<int>(dragon::analysis::ValueSetKind::Unknown)==DRAGON_VALUE_UNKNOWN);

namespace {
// Bound allocation before dereferencing caller arrays. The native analyser
// independently charges/validates all constraints against its own budgets.
constexpr size_t initialInputCap = 262144;
bool copyInitialStates(const dragon_finite_entry_state_t *states,size_t count,
                       std::vector<dragon::analysis::FiniteEntryState> &out) {
  if ((count && !states) || count > initialInputCap) return false;
  size_t used=count;
  auto charge=[&](size_t n) {
    if(n>initialInputCap-used)return false;
    used+=n;return true;
  };
  out.reserve(count);
  for(size_t i=0;i<count;++i) {
    const auto &s=states[i];
    if((s.register_count && !s.registers) || (s.memory_count && !s.memory) ||
       !charge(s.register_count) || !charge(s.memory_count))return false;
    dragon::analysis::FiniteEntryState entry{s.entry,{},{}};
    entry.registers.reserve(s.register_count);
    for(size_t j=0;j<s.register_count;++j) {
      const auto &r=s.registers[j];
      if(!charge((size_t(r.width)+7)/8))return false;
      entry.registers.push_back({r.offset,r.width,r.value});
    }
    entry.memory.reserve(s.memory_count);
    for(size_t j=0;j<s.memory_count;++j) {
      const auto &m=s.memory[j];
      if((m.size && !m.bytes) || !charge(m.size))return false;
      dragon::analysis::FiniteMemoryFact memory{m.address,{}};
      if(m.size)memory.bytes.assign(m.bytes,m.bytes+m.size);
      entry.memory.push_back(std::move(memory));
    }
    out.push_back(std::move(entry));
  }
  return true;
}
}

char *dragon_graph_finite_values_with_initial_state(const dragon_graph_t *g,
    const uint64_t *entries,size_t count,uint64_t query_block,
    uint32_t query_offset,uint8_t query_width,uint32_t max_values,
    uint32_t max_blocks,uint32_t max_operations,uint32_t max_facts,
    uint8_t prove_leaf_calls,const dragon_finite_entry_state_t *states,size_t state_count) {
  if(!g || (count && !entries) || count>initialInputCap ||
     query_width>64 || prove_leaf_calls>1)return nullptr;
  try {
    std::vector<dragon::analysis::FiniteEntryState> initial;
    if(!copyInitialStates(states,state_count,initial))return nullptr;
    std::set<std::uint64_t> roots;
    if(count)roots.insert(entries,entries+count);
    std::vector<dragon::analysis::FiniteValueQuery> queries;
    if(query_width)queries.push_back({query_block,dragon::ignil::Reg{query_offset,query_width,{}}});
    return dup_string(dragon::analysis::finiteValuesWithInitialState(*as_graph(g),roots,
      queries,initial,{max_values,max_blocks,max_operations,max_facts,prove_leaf_calls!=0}).toJSON());
  } catch(...) {return nullptr;}
}

char *dragon_graph_finite_memory_values_with_initial_state(const dragon_graph_t *g,
    const uint64_t *entries,size_t count,const dragon_finite_memory_query_t *queries,
    size_t query_count,uint32_t max_values,uint32_t max_blocks,uint32_t max_operations,
    uint32_t max_facts,uint8_t prove_leaf_calls,
    const dragon_finite_entry_state_t *states,size_t state_count) {
  if(!g || (count && !entries) || count>initialInputCap ||
     (query_count && !queries) || query_count>initialInputCap || prove_leaf_calls>1)return nullptr;
  try {
    std::vector<dragon::analysis::FiniteEntryState> initial;
    if(!copyInitialStates(states,state_count,initial))return nullptr;
    std::set<std::uint64_t> roots;
    if(count)roots.insert(entries,entries+count);
    std::vector<dragon::analysis::FiniteMemoryQuery> native;
    native.reserve(query_count);
    for(size_t i=0;i<query_count;++i) {
      const auto &q=queries[i];
      native.push_back({q.block,q.before_op_index,q.address,q.width});
    }
    return dup_string(dragon::analysis::finiteMemoryValuesWithInitialState(*as_graph(g),roots,
      native,initial,{max_values,max_blocks,max_operations,max_facts,prove_leaf_calls!=0}).toJSON());
  } catch(...) {return nullptr;}
}

void dragon_u64_array_free(uint64_t *arr) { delete[] arr; }

// Never returns nullptr, even for an empty vector - callers rely on getting
// a real (if zero-length) allocation back to free unconditionally.
static uint64_t *dup_u64_array(const std::vector<std::uint64_t> &v) {
  auto *buf = new uint64_t[v.size()];
  std::copy(v.begin(), v.end(), buf);
  return buf;
}

// =============================================================================
// IGNIL opcodes
// =============================================================================

// Verify every enum value matches ignil::Opcode at compile time (exhaustive,
// not spot-checked) - a mid-enum insertion on either side desyncs this
// mirror silently otherwise, which is exactly what happened before (see the
// comment on the float opcodes in IGNIL.hpp). Keep this list in the same
// order as the C++ enum and dragon_ignil_opcode_t so a missing entry here
// is easy to spot by eye.
static_assert(static_cast<int>(dragon::ignil::Opcode::COPY)   == DRAGON_IGNIL_COPY);
// Last value: guards every opcode above it against a mid-enum insertion.
static_assert(static_cast<int>(dragon::ignil::Opcode::OPAQUE) == DRAGON_IGNIL_OPAQUE);
static_assert(static_cast<int>(dragon::ignil::Opcode::LOAD)   == DRAGON_IGNIL_LOAD);
static_assert(static_cast<int>(dragon::ignil::Opcode::STORE) == DRAGON_IGNIL_STORE);
static_assert(static_cast<int>(dragon::ignil::Opcode::ADD)   == DRAGON_IGNIL_ADD);
static_assert(static_cast<int>(dragon::ignil::Opcode::SUB)   == DRAGON_IGNIL_SUB);
static_assert(static_cast<int>(dragon::ignil::Opcode::NEG)   == DRAGON_IGNIL_NEG);
static_assert(static_cast<int>(dragon::ignil::Opcode::MUL)   == DRAGON_IGNIL_MUL);
static_assert(static_cast<int>(dragon::ignil::Opcode::SDIV)  == DRAGON_IGNIL_SDIV);
static_assert(static_cast<int>(dragon::ignil::Opcode::UDIV)  == DRAGON_IGNIL_UDIV);
static_assert(static_cast<int>(dragon::ignil::Opcode::SMOD)  == DRAGON_IGNIL_SMOD);
static_assert(static_cast<int>(dragon::ignil::Opcode::UMOD)  == DRAGON_IGNIL_UMOD);
static_assert(static_cast<int>(dragon::ignil::Opcode::AND)   == DRAGON_IGNIL_AND);
static_assert(static_cast<int>(dragon::ignil::Opcode::OR)    == DRAGON_IGNIL_OR);
static_assert(static_cast<int>(dragon::ignil::Opcode::XOR)   == DRAGON_IGNIL_XOR);
static_assert(static_cast<int>(dragon::ignil::Opcode::NOT)   == DRAGON_IGNIL_NOT);
static_assert(static_cast<int>(dragon::ignil::Opcode::SHL)   == DRAGON_IGNIL_SHL);
static_assert(static_cast<int>(dragon::ignil::Opcode::SHR)   == DRAGON_IGNIL_SHR);
static_assert(static_cast<int>(dragon::ignil::Opcode::SAR)   == DRAGON_IGNIL_SAR);
static_assert(static_cast<int>(dragon::ignil::Opcode::ZEXT)  == DRAGON_IGNIL_ZEXT);
static_assert(static_cast<int>(dragon::ignil::Opcode::SEXT)  == DRAGON_IGNIL_SEXT);
static_assert(static_cast<int>(dragon::ignil::Opcode::TRUNC) == DRAGON_IGNIL_TRUNC);
static_assert(static_cast<int>(dragon::ignil::Opcode::EQ)    == DRAGON_IGNIL_EQ);
static_assert(static_cast<int>(dragon::ignil::Opcode::NEQ)   == DRAGON_IGNIL_NEQ);
static_assert(static_cast<int>(dragon::ignil::Opcode::ULT)   == DRAGON_IGNIL_ULT);
static_assert(static_cast<int>(dragon::ignil::Opcode::ULE)   == DRAGON_IGNIL_ULE);
static_assert(static_cast<int>(dragon::ignil::Opcode::UGT)   == DRAGON_IGNIL_UGT);
static_assert(static_cast<int>(dragon::ignil::Opcode::UGE)   == DRAGON_IGNIL_UGE);
static_assert(static_cast<int>(dragon::ignil::Opcode::SLT)   == DRAGON_IGNIL_SLT);
static_assert(static_cast<int>(dragon::ignil::Opcode::SLE)   == DRAGON_IGNIL_SLE);
static_assert(static_cast<int>(dragon::ignil::Opcode::SGT)   == DRAGON_IGNIL_SGT);
static_assert(static_cast<int>(dragon::ignil::Opcode::SGE)   == DRAGON_IGNIL_SGE);
static_assert(static_cast<int>(dragon::ignil::Opcode::JMP)   == DRAGON_IGNIL_JMP);
static_assert(static_cast<int>(dragon::ignil::Opcode::CJMP)  == DRAGON_IGNIL_CJMP);
static_assert(static_cast<int>(dragon::ignil::Opcode::CALL)  == DRAGON_IGNIL_CALL);
static_assert(static_cast<int>(dragon::ignil::Opcode::RET)   == DRAGON_IGNIL_RET);
static_assert(static_cast<int>(dragon::ignil::Opcode::UNDEF) == DRAGON_IGNIL_UNDEF);
static_assert(static_cast<int>(dragon::ignil::Opcode::FADD)   == DRAGON_IGNIL_FADD);
static_assert(static_cast<int>(dragon::ignil::Opcode::FSUB)   == DRAGON_IGNIL_FSUB);
static_assert(static_cast<int>(dragon::ignil::Opcode::FMUL)   == DRAGON_IGNIL_FMUL);
static_assert(static_cast<int>(dragon::ignil::Opcode::FDIV)   == DRAGON_IGNIL_FDIV);
static_assert(static_cast<int>(dragon::ignil::Opcode::SITOFP) == DRAGON_IGNIL_SITOFP);
static_assert(static_cast<int>(dragon::ignil::Opcode::FPTOSI) == DRAGON_IGNIL_FPTOSI);
static_assert(static_cast<int>(dragon::ignil::Opcode::FLT)    == DRAGON_IGNIL_FLT);
static_assert(static_cast<int>(dragon::ignil::Opcode::FEQ)    == DRAGON_IGNIL_FEQ);
static_assert(static_cast<int>(dragon::ignil::Opcode::FUNO)   == DRAGON_IGNIL_FUNO);
static_assert(static_cast<int>(dragon::ignil::Opcode::FADD32) == DRAGON_IGNIL_FADD32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FSUB32) == DRAGON_IGNIL_FSUB32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FMUL32) == DRAGON_IGNIL_FMUL32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FDIV32) == DRAGON_IGNIL_FDIV32);
static_assert(static_cast<int>(dragon::ignil::Opcode::SITOFP32) == DRAGON_IGNIL_SITOFP32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FPTOSI32) == DRAGON_IGNIL_FPTOSI32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FLT32) == DRAGON_IGNIL_FLT32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FEQ32) == DRAGON_IGNIL_FEQ32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FUNO32) == DRAGON_IGNIL_FUNO32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FPEXT) == DRAGON_IGNIL_FPEXT);
static_assert(static_cast<int>(dragon::ignil::Opcode::FPTRUNC) == DRAGON_IGNIL_FPTRUNC);
static_assert(static_cast<int>(dragon::ignil::Opcode::UITOFP) == DRAGON_IGNIL_UITOFP);
static_assert(static_cast<int>(dragon::ignil::Opcode::UITOFP32) == DRAGON_IGNIL_UITOFP32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FPTOUI) == DRAGON_IGNIL_FPTOUI);
static_assert(static_cast<int>(dragon::ignil::Opcode::FPTOUI32) == DRAGON_IGNIL_FPTOUI32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FSQRT) == DRAGON_IGNIL_FSQRT);
static_assert(static_cast<int>(dragon::ignil::Opcode::FSQRT32) == DRAGON_IGNIL_FSQRT32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FFLOOR) == DRAGON_IGNIL_FFLOOR);
static_assert(static_cast<int>(dragon::ignil::Opcode::FFLOOR32) == DRAGON_IGNIL_FFLOOR32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FCEIL) == DRAGON_IGNIL_FCEIL);
static_assert(static_cast<int>(dragon::ignil::Opcode::FCEIL32) == DRAGON_IGNIL_FCEIL32);
static_assert(static_cast<int>(dragon::ignil::Opcode::FTRUNC) == DRAGON_IGNIL_FTRUNC);
static_assert(static_cast<int>(dragon::ignil::Opcode::FTRUNC32) == DRAGON_IGNIL_FTRUNC32);
static_assert(static_cast<int>(dragon::ignil::Opcode::LABEL) == DRAGON_IGNIL_LABEL);
static_assert(static_cast<int>(dragon::ignil::Opcode::LOCALJMP) == DRAGON_IGNIL_LOCALJMP);
static_assert(static_cast<int>(dragon::ignil::Opcode::LOCALCJMP) == DRAGON_IGNIL_LOCALCJMP);

const char *dragon_ignil_opcode_name(dragon_ignil_opcode_t op) {
  return dragon::ignil::opcodeName(
             static_cast<dragon::ignil::Opcode>(op))
      .data();
}

// =============================================================================
// IGNIL block - borrowed from graph via reinterpret_cast
// =============================================================================

static const dragon::ignil::Block *
as_ignil_block(const dragon_ignil_block_t *b) {
  return reinterpret_cast<const dragon::ignil::Block *>(b);
}

const dragon_ignil_block_t *
dragon_graph_get_ignil_block(const dragon_graph_t *g, uint64_t addr) {
  const dragon::ignil::Block *b = as_graph(g)->getIGNILBlock(addr);
  return reinterpret_cast<const dragon_ignil_block_t *>(b);
}

size_t dragon_ignil_block_size(const dragon_ignil_block_t *b) {
  return as_ignil_block(b)->size();
}

uint64_t dragon_ignil_block_address(const dragon_ignil_block_t *b) {
  return as_ignil_block(b)->startAddress();
}

char *dragon_ignil_block_to_string(const dragon_ignil_block_t *b) {
  return dup_string(dragon::ignil::toString(*as_ignil_block(b)));
}

char *dragon_ignil_block_op_to_string(const dragon_ignil_block_t *b,
                                      size_t index) {
  const auto &ops = as_ignil_block(b)->ops();
  if (index >= ops.size())
    return nullptr;
  return dup_string(dragon::ignil::toString(ops[index]));
}

size_t dragon_ignil_block_unlifted(const dragon_ignil_block_t *b,
                                   uint64_t *out, size_t cap) {
  if (!b)
    return 0;
  const auto &unlifted = as_ignil_block(b)->unlifted();
  for (size_t i = 0; out && i < cap && i < unlifted.size(); ++i)
    out[i] = unlifted[i];
  return unlifted.size();
}

void dragon_ignil_block_each_op(
    const dragon_ignil_block_t *b,
    void (*cb)(uint64_t, dragon_ignil_opcode_t, size_t, void *), void *user) {
  const auto &ops = as_ignil_block(b)->ops();
  for (size_t i = 0; i < ops.size(); ++i) {
    cb(ops[i].address,
       static_cast<dragon_ignil_opcode_t>(
           static_cast<int>(ops[i].opcode)),
       i, user);
  }
}

// =============================================================================
// Disassembler: trace lifting - produces an OWNED ignil block (unlike
// dragon_graph_get_ignil_block's borrowed one), reusing all the accessors
// above since it's the same underlying dragon::ignil::Block type.
// =============================================================================

dragon_ignil_block_t *dragon_disasm_lift_trace(
    const dragon_disasm_t *d, const dragon_instruction_t *const *instructions,
    size_t count) {
  if (!d || (!instructions && count > 0))
    return nullptr;
  std::vector<dragon::Instruction> trace;
  trace.reserve(count);
  for (size_t i = 0; i < count; ++i)
    trace.push_back(*as_insn(instructions[i]));
  auto *block = new dragon::ignil::Block(d->disassembler.liftTrace(trace));
  return reinterpret_cast<dragon_ignil_block_t *>(block);
}

void dragon_ignil_block_destroy(dragon_ignil_block_t *b) {
  delete reinterpret_cast<dragon::ignil::Block *>(b);
}

// =============================================================================
// IGNIL peephole simplifier
// =============================================================================

dragon_ignil_block_t *dragon_ignil_simplify_block(
        const dragon_ignil_block_t *b, int max_iterations,
        size_t *out_constants_folded, size_t *out_identities_applied,
        size_t *out_ops_removed, size_t *out_iterations) {
  if (!b)
    return nullptr;
  dragon::ignil::SimplifyStats stats;
  const int iterations = max_iterations > 0 ? max_iterations : 8;
  auto *block = new dragon::ignil::Block(dragon::ignil::simplifyBlock(
          *as_ignil_block(b), stats, iterations));
  if (out_constants_folded)
    *out_constants_folded = stats.constantsFolded;
  if (out_identities_applied)
    *out_identities_applied = stats.identitiesApplied;
  if (out_ops_removed)
    *out_ops_removed = stats.opsRemoved;
  if (out_iterations)
    *out_iterations = stats.iterations;
  return reinterpret_cast<dragon_ignil_block_t *>(block);
}

// =============================================================================
// IGNIL dead-store elimination
// =============================================================================

namespace {

// The maximal registers for `arch`, so a 32-bit write's zero-extend range is
// resolved exactly. An unknown/unsupported architecture yields an empty file,
// which makes deadStoreEliminate fall back to assuming an 8-byte parent.
std::vector<dragon::ignil::RegExtent> register_file_for(
        dragon_architecture_t arch) {
  switch (arch) {
    case DRAGON_ARCH_X86_32:
    case DRAGON_ARCH_X86_64:
      return dragon::x86::registerFile();
    case DRAGON_ARCH_ARM64:
      return dragon::arm::registerFile();
    default:
      return {};
  }
}

} // namespace

dragon_ignil_block_t *dragon_ignil_dead_store_eliminate(
        const dragon_ignil_block_t *b, dragon_architecture_t arch,
        const dragon_ignil_reg_t *live_out, size_t live_out_count,
        size_t *out_ops_removed) {
  if (!b)
    return nullptr;

  dragon::ignil::DeadStoreOptions opts;
  opts.registerFile = register_file_for(arch);

  std::optional<std::vector<dragon::ignil::Reg>> live;
  if (live_out) {
    std::vector<dragon::ignil::Reg> regs;
    regs.reserve(live_out_count);
    for (size_t i = 0; i < live_out_count; ++i)
      regs.push_back(dragon::ignil::Reg{live_out[i].offset, live_out[i].width,
                                        std::string{}});
    live = std::move(regs);
  }

  dragon::ignil::SimplifyStats stats;
  auto *block = new dragon::ignil::Block(dragon::ignil::deadStoreEliminate(
          *as_ignil_block(b), live, stats, opts));
  if (out_ops_removed)
    *out_ops_removed = stats.opsRemoved;
  return reinterpret_cast<dragon_ignil_block_t *>(block);
}

// =============================================================================
// SliceExpr
// =============================================================================

struct dragon_slice_expr {
  std::shared_ptr<dragon::slice::Expr> expr;
};

static dragon_slice_expr_t *
wrap_expr(std::shared_ptr<dragon::slice::Expr> ptr) {
  if (!ptr)
    return nullptr;
  return new dragon_slice_expr{std::move(ptr)};
}

void dragon_slice_expr_destroy(dragon_slice_expr_t *e) { delete e; }

dragon_slice_type_t dragon_slice_expr_type(const dragon_slice_expr_t *e) {
  if (!e)
    return DRAGON_SLICE_UNDEF;
  return static_cast<dragon_slice_type_t>(e->expr->data.index());
}

char *dragon_slice_expr_to_string(const dragon_slice_expr_t *e) {
  if (!e)
    return dup_string("undef");
  return dup_string(dragon::slice::toString(*e->expr));
}

uint64_t dragon_slice_expr_const_value(const dragon_slice_expr_t *e) {
  if (!e)
    return 0;
  const auto *c = std::get_if<dragon::slice::Const>(&e->expr->data);
  return c ? c->value : 0;
}

uint8_t dragon_slice_expr_const_width(const dragon_slice_expr_t *e) {
  if (!e)
    return 0;
  const auto *c = std::get_if<dragon::slice::Const>(&e->expr->data);
  return c ? c->width : 0;
}

uint32_t dragon_slice_expr_symreg_offset(const dragon_slice_expr_t *e) {
  if (!e)
    return 0;
  const auto *r = std::get_if<dragon::slice::SymReg>(&e->expr->data);
  return r ? r->reg.offset : 0;
}

uint8_t dragon_slice_expr_symreg_width(const dragon_slice_expr_t *e) {
  if (!e)
    return 0;
  const auto *r = std::get_if<dragon::slice::SymReg>(&e->expr->data);
  return r ? r->reg.width : 0;
}

const char *dragon_slice_expr_symreg_name(const dragon_slice_expr_t *e) {
  if (!e)
    return "";
  const auto *r = std::get_if<dragon::slice::SymReg>(&e->expr->data);
  return r ? r->reg.name.c_str() : "";
}

dragon_ignil_opcode_t
dragon_slice_expr_opcode(const dragon_slice_expr_t *e) {
  if (!e)
    return DRAGON_IGNIL_UNDEF;
  if (const auto *b = std::get_if<dragon::slice::BinOp>(&e->expr->data))
    return static_cast<dragon_ignil_opcode_t>(static_cast<int>(b->op));
  if (const auto *u = std::get_if<dragon::slice::UnaryOp>(&e->expr->data))
    return static_cast<dragon_ignil_opcode_t>(static_cast<int>(u->op));
  return DRAGON_IGNIL_UNDEF;
}

dragon_slice_expr_t *dragon_slice_expr_left(const dragon_slice_expr_t *e) {
  if (!e)
    return nullptr;
  const auto *b = std::get_if<dragon::slice::BinOp>(&e->expr->data);
  return b ? wrap_expr(b->left) : nullptr;
}

dragon_slice_expr_t *dragon_slice_expr_right(const dragon_slice_expr_t *e) {
  if (!e)
    return nullptr;
  const auto *b = std::get_if<dragon::slice::BinOp>(&e->expr->data);
  return b ? wrap_expr(b->right) : nullptr;
}

dragon_slice_expr_t *dragon_slice_expr_operand(const dragon_slice_expr_t *e) {
  if (!e)
    return nullptr;
  const auto *u = std::get_if<dragon::slice::UnaryOp>(&e->expr->data);
  return u ? wrap_expr(u->operand) : nullptr;
}

dragon_slice_expr_t *
dragon_slice_expr_load_addr(const dragon_slice_expr_t *e) {
  if (!e)
    return nullptr;
  const auto *l = std::get_if<dragon::slice::Load>(&e->expr->data);
  return l ? wrap_expr(l->addr) : nullptr;
}

uint8_t dragon_slice_expr_load_width(const dragon_slice_expr_t *e) {
  if (!e)
    return 0;
  const auto *l = std::get_if<dragon::slice::Load>(&e->expr->data);
  return l ? l->width : 0;
}

#ifdef DRAGON_WITH_Z3
char *dragon_slice_expr_to_z3_string(const dragon_slice_expr_t *e) {
  if (!e)
    return dup_string("undef");
  z3::context ctx;
  dragon::slice::SliceExprZ3 lowerer{ctx};
  return dup_string(lowerer.lower(*e->expr).to_string());
}

uint64_t *dragon_slice_expr_enumerate_values(const dragon_slice_expr_t *e,
                                              size_t max_values, int sorted,
                                              size_t *out_count) {
  if (!e) {
    if (out_count)
      *out_count = 0;
    return nullptr;
  }
  z3::context ctx;
  dragon::slice::EnumeratorConfig cfg;
  if (max_values > 0)
    cfg.maxValues = max_values;
  cfg.sorted = sorted != 0;
  std::vector<std::uint64_t> values =
      dragon::slice::enumeratedValues(*e->expr, ctx, {}, cfg);
  if (out_count)
    *out_count = values.size();
  return dup_u64_array(values);
}

int dragon_slice_expr_solve_unique(const dragon_slice_expr_t *e,
                                    uint64_t *out_value) {
  if (!e)
    return 0;
  z3::context ctx;
  std::optional<std::uint64_t> value =
      dragon::slice::solveUnique(*e->expr, ctx, {});
  if (!value)
    return 0;
  if (out_value)
    *out_value = *value;
  return 1;
}
#endif

// =============================================================================
// BackwardSlicer
// =============================================================================

struct dragon_slicer {
  dragon::BackwardSlicer slicer;
};

dragon_slicer_t *dragon_slicer_create(int max_blocks) {
  return new dragon_slicer{dragon::BackwardSlicer{max_blocks > 0 ? max_blocks : 5}};
}

// =============================================================================
// LLVMLifter
// =============================================================================

struct dragon_llvm_lifter {
  dragon::llvmlifter::LLVMLifter lifter;
};
static_assert(static_cast<int>(dragon::llvmlifter::ExecutionMode::StaticAnalysis)==DRAGON_LLVM_STATIC_ANALYSIS);
static_assert(static_cast<int>(dragon::llvmlifter::StaticEntryPolicy::Resumable)==DRAGON_LLVM_ENTRY_RESUMABLE);
static_assert(static_cast<int>(dragon::llvmlifter::StaticEntryPolicy::FunctionStart)==DRAGON_LLVM_ENTRY_FUNCTION_START);

dragon_llvm_lifter_t *dragon_llvm_lifter_create(dragon_architecture_t arch,
                                                const char *module_name) {
  std::string name = (module_name && *module_name) ? module_name : "lifted";
  return new dragon_llvm_lifter{
          dragon::llvmlifter::LLVMLifter(to_arch(arch), name)};
}

void dragon_llvm_lifter_destroy(dragon_llvm_lifter_t *l) { delete l; }

int dragon_llvm_lifter_set_execution_mode(dragon_llvm_lifter_t *l, dragon_llvm_execution_mode_t mode) {
  return l && l->lifter.setExecutionMode(static_cast<dragon::llvmlifter::ExecutionMode>(mode));
}
int dragon_llvm_lifter_set_static_entry_policy(dragon_llvm_lifter_t *l, dragon_llvm_static_entry_policy_t policy) {
  return l && l->lifter.setStaticEntryPolicy(static_cast<dragon::llvmlifter::StaticEntryPolicy>(policy));
}
void dragon_llvm_execute_v2(dragon_guest_state_v2_t *state, dragon_guest_memory_v2_t *memory,
    dragon_exec_context_v2_t *context, dragon_exec_exit_v2_t *exit, uint64_t root_exit_pc) {
  if (!exit) return;
  if (!state || !memory || !context) {
    *exit = {}; exit->reason=DRAGON_EXIT_UNSUPPORTED; exit->detail=DRAGON_DETAIL_INVALID_STATE; return;
  }
  dragon::llvmlifter::executeV2(*state,*memory,*context,*exit,root_exit_pc);
}


int dragon_llvm_lifter_lift(dragon_llvm_lifter_t *l,
                             const dragon_function_t *fn,
                             const dragon_graph_t *g) {
  if (!l || !fn || !g)
    return 0;
  return l->lifter.lift(*as_fn(fn), *as_graph(g)) ? 1 : 0;
}

int dragon_llvm_lifter_lift_static_region(dragon_llvm_lifter_t *l,
    const dragon_function_t *fn,const dragon_graph_t *g,
    const std::uint64_t *blocks,std::size_t count,
    std::uint32_t maxBlocks,std::uint32_t maxDispatchCases,int rebuildDecodedBlocks) {
  if(!l || !fn || !g || (count && !blocks) || count>4096 ||
     (rebuildDecodedBlocks!=0 && rebuildDecodedBlocks!=1))return 0;
  try {
    return l->lifter.liftStaticRegion(*as_fn(fn),*as_graph(g),{blocks,count},
        {maxBlocks,maxDispatchCases,rebuildDecodedBlocks!=0}) ? 1 : 0;
  } catch(...) {return 0;}
}

char *dragon_llvm_lifter_ir(const dragon_llvm_lifter_t *l) {
  if (!l)
    return dup_string("");
  return dup_string(l->lifter.ir());
}

void dragon_llvm_lifter_run_mem2reg(dragon_llvm_lifter_t *l) {
  if (l)
    l->lifter.runMem2Reg();
}

void dragon_llvm_lifter_run_inst_combine(dragon_llvm_lifter_t *l) {
  if (l)
    l->lifter.runInstCombine();
}

void dragon_llvm_lifter_run_gvn(dragon_llvm_lifter_t *l) {
  if (l)
    l->lifter.runGVN();
}

void dragon_llvm_lifter_run_dce(dragon_llvm_lifter_t *l) {
  if (l)
    l->lifter.runDCE();
}

int dragon_llvm_lifter_fold_predicates(dragon_llvm_lifter_t *l,
    uint32_t timeout_ms, uint32_t max_queries, uint32_t max_expression_nodes,
    uint32_t resource_limit,
    dragon_predicate_fold_stats_t *out) {
  if (out) *out = {};
  if (!l || !out) return 0;
  const auto stats = l->lifter.runPredicateFolding(
      {timeout_ms, max_queries, max_expression_nodes, resource_limit});
  out->available = stats.available;
  out->conditions_visited = stats.conditionsVisited;
  out->conditions_folded = stats.conditionsFolded;
  out->solver_queries = stats.solverQueries;
  out->unknown_queries = stats.unknownQueries;
  out->unsupported_conditions = stats.unsupportedConditions;
  out->budget_skipped = stats.budgetSkipped;
  return 1;
}

int dragon_llvm_lifter_simplify_mba(dragon_llvm_lifter_t *l,
    uint32_t timeout_ms, uint32_t max_queries, uint32_t max_expression_nodes,
    uint32_t max_variables, uint32_t resource_limit, dragon_mba_simplify_stats_t *out) {
  if (out) *out = {};
  if (!l || !out) return 0;
  const auto stats = l->lifter.runMBASimplification(
      {timeout_ms, max_queries, max_expression_nodes, max_variables, resource_limit});
  out->available = stats.available;
  out->expressions_visited = stats.expressionsVisited;
  out->candidates = stats.candidates;
  out->expressions_simplified = stats.expressionsSimplified;
  out->solver_queries = stats.solverQueries;
  out->unknown_queries = stats.unknownQueries;
  out->rejected_candidates = stats.rejectedCandidates;
  out->budget_skipped = stats.budgetSkipped;
  return 1;
}

struct dragon_pass_pipeline {
  dragon::passes::PassPipeline pipeline;
  std::string error;
};
static_assert(static_cast<int>(dragon::passes::Stage::Graph)==DRAGON_PASS_GRAPH);
static_assert(static_cast<int>(dragon::passes::Stage::LLVM)==DRAGON_PASS_LLVM);
static_assert(static_cast<int>(dragon::passes::Status::Error)==DRAGON_PASS_ERROR);
namespace {
dragon::passes::Result invokePass(dragon_pass_callback_t fn,void *userdata,
    void *target,dragon::passes::AnalysisCache &cache,const dragon::passes::Options &options) {
  const auto json=dragon::passes::optionsJSON(options);
  dragon_pass_result_t result{DRAGON_PASS_ERROR,0,"callback did not produce a result","{}"};
  fn(target,cache.revision(),json.c_str(),userdata,&result);
  return {static_cast<dragon::passes::Status>(result.status),result.changed!=0,
      result.message?result.message:"",dragon::passes::parseCountersJSON(result.counters_json?result.counters_json:"{}")};
}
}
dragon_pass_pipeline_t *dragon_pass_pipeline_create(int builtins) {
  try {return new dragon_pass_pipeline{dragon::passes::PassPipeline{builtins!=0},{}};} catch(...) {return nullptr;}
}
void dragon_pass_pipeline_destroy(dragon_pass_pipeline_t *p) {
  if(p && p->pipeline.isRunning()) {p->error="cannot destroy a running pipeline";return;}
  delete p;
}
int dragon_pass_pipeline_add(dragon_pass_pipeline_t *p,const char *name,const char *options) {
  if(!p || !name) return 0;
  try {p->error.clear();return p->pipeline.add(name,dragon::passes::parseOptionsJSON(options?options:"{}"));}
  catch(const std::exception &e) {p->error=e.what();return 0;}
}
int dragon_pass_pipeline_clear(dragon_pass_pipeline_t *p) {return p && p->pipeline.clear();}
int dragon_pass_pipeline_register(dragon_pass_pipeline_t *p,const char *name,
    dragon_pass_stage_t stage,dragon_pass_callback_t callback,void *userdata,int readOnly,const char *dependencies) {
  if(!p || !name || !callback || (stage!=DRAGON_PASS_GRAPH && stage!=DRAGON_PASS_LLVM)) return 0;
  try {
    p->error.clear();auto prior=dragon::passes::parseNamesJSON(dependencies?dependencies:"[]");
    if(stage==DRAGON_PASS_GRAPH) return p->pipeline.registerGraph(name,[=](dragon::passes::GraphContext &c,const auto &o){return invokePass(callback,userdata,c.bindingTarget,c.analyses,o);},readOnly!=0,std::move(prior));
    return p->pipeline.registerLLVM(name,[=](dragon::passes::LLVMContext &c,const auto &o){return invokePass(callback,userdata,c.bindingTarget,c.analyses,o);},readOnly!=0,std::move(prior));
  } catch(const std::exception &e) {p->error=e.what();return 0;}
}
int dragon_pass_pipeline_register_finite_dispatch(dragon_pass_pipeline_t *p,const char *name,
    const uint8_t *code,size_t length,uint64_t base,const uint64_t *entries,size_t count) {
  return dragon_pass_pipeline_register_finite_dispatch_with_hints(
      p,name,code,length,base,entries,count,nullptr);
}
int dragon_pass_pipeline_register_finite_dispatch_with_hints(dragon_pass_pipeline_t *p,const char *name,
    const uint8_t *code,size_t length,uint64_t base,const uint64_t *entries,size_t count,
    const dragon_analysis_hints_t *hints) {
  if(!p || !name || !code || !length || (count && !entries))return 0;
  try {
    p->error.clear();
    const auto snapshot = hints ? hints->hints : dragon::Disassembler::AnalysisHints{};
    return p->pipeline.registerGraph(name,
      dragon::passes::FiniteDispatchPass{{code,length},base,{entries,count},snapshot},
      false,{},dragon::passes::FiniteDispatchPass::validator());
  } catch(const std::exception &e) {p->error=e.what();return 0;}
  catch(...) {p->error="finite dispatch registration failed";return 0;}
}
int dragon_pass_pipeline_override(dragon_pass_pipeline_t *p,const char *name,
    dragon_pass_stage_t stage,dragon_pass_callback_t callback,void *userdata) {
  if(!p || !name || !callback) return 0;
  p->error.clear();
  if(stage==DRAGON_PASS_GRAPH) return p->pipeline.overrideGraph(name,[=](dragon::passes::GraphContext &c,const auto &o){return invokePass(callback,userdata,c.bindingTarget,c.analyses,o);});
  if(stage==DRAGON_PASS_LLVM) return p->pipeline.overrideLLVM(name,[=](dragon::passes::LLVMContext &c,const auto &o){return invokePass(callback,userdata,c.bindingTarget,c.analyses,o);});
  return 0;
}
char *dragon_pass_pipeline_describe(const dragon_pass_pipeline_t *p) {return dup_string(p?p->pipeline.describeJSON():"[]");}
char *dragon_pass_pipeline_error(const dragon_pass_pipeline_t *p) {return dup_string(!p?"null pipeline":!p->error.empty()?p->error:p->pipeline.lastError());}
char *dragon_pass_pipeline_run_llvm(dragon_pass_pipeline_t *p,dragon_llvm_lifter_t *l,
    uint32_t iterations,uint32_t runs,int verify) {
  if(!p || !l) return nullptr;
  try {return dup_string(p->pipeline.run(l->lifter,{iterations,runs,verify!=0},l).toJSON());}
  catch(const std::exception &e) {p->error=e.what();return nullptr;}
}
char *dragon_pass_pipeline_run_graph(dragon_pass_pipeline_t *p,dragon_graph_t *g,
    uint32_t iterations,uint32_t runs) {
  if(!p || !g) return nullptr;
  try {return dup_string(p->pipeline.run(*as_graph(g),{iterations,runs,true},g).toJSON());}
  catch(const std::exception &e) {p->error=e.what();return nullptr;}
}

void dragon_slicer_destroy(dragon_slicer_t *s) { delete s; }

dragon_slice_expr_t *dragon_slicer_slice_reg(const dragon_slicer_t *s,
                                              const dragon_graph_t *g,
                                              uint64_t block_addr,
                                              uint32_t reg_offset,
                                              uint8_t reg_width,
                                              const char *reg_name) {
  if (!s || !g)
    return nullptr;
  std::string name = (reg_name && *reg_name) ? reg_name : "";
  dragon::ignil::Value reg{
      dragon::ignil::Reg{reg_offset, reg_width, std::move(name)}};
  dragon::slice::Expr result =
      s->slicer.slice(reg, block_addr, *as_graph(g));
  return new dragon_slice_expr{
      std::make_shared<dragon::slice::Expr>(std::move(result))};
}

// =============================================================================
// SwitchDetector
// =============================================================================

int dragon_detect_possible_switch(const dragon_slice_expr_t *jump_target,
                                   uint64_t block_addr,
                                   const dragon_graph_t *g,
                                   dragon_switch_candidate_t *out) {
  if (!jump_target || !g || !out)
    return 0;
  std::optional<dragon::slice::SwitchCandidate> sw =
      dragon::slice::detectPossibleSwitch(*jump_target->expr, block_addr,
                                           *as_graph(g));
  if (!sw)
    return 0;
  out->table_base = sw->tableBase;
  out->entry_size = sw->entrySize;
  out->num_cases = sw->numCases;
  out->index_reg_offset = sw->indexReg.offset;
  out->index_reg_width = sw->indexReg.width;
  out->relative = sw->relative ? 1 : 0;
  out->entry_shift = sw->entryShift;
  out->entry_bits = sw->entryBits;
  out->entry_signed = sw->entrySigned ? 1 : 0;
  out->target_base = sw->targetBase;
  return 1;
}

uint64_t *dragon_read_table_targets(const dragon_switch_candidate_t *sw,
                                     const uint8_t *bytes, size_t bytes_len,
                                     uint64_t base_addr, size_t *out_count) {
  if (!sw || (!bytes && bytes_len > 0)) {
    if (out_count)
      *out_count = 0;
    return nullptr;
  }
  dragon::slice::SwitchCandidate candidate{
      sw->table_base, sw->entry_size, sw->num_cases,
      dragon::ignil::Reg{sw->index_reg_offset, sw->index_reg_width, ""}};
  candidate.relative = sw->relative != 0;
  candidate.targetBase = sw->target_base;
  candidate.entryShift = sw->entry_shift;
  candidate.entryBits = sw->entry_bits;
  candidate.entrySigned = sw->entry_signed != 0;
  std::vector<std::uint64_t> targets = dragon::slice::readTableTargets(
      candidate, std::span<const std::uint8_t>(bytes, bytes_len), base_addr);
  if (out_count)
    *out_count = targets.size();
  return dup_u64_array(targets);
}

// =============================================================================
// FLIRT signatures (dragon::sigs)
// =============================================================================

struct dragon_sigdb {
  dragon::sigs::SignatureDB db;
};

dragon_sigdb_t *dragon_sigdb_create() { return new dragon_sigdb{}; }

void dragon_sigdb_destroy(dragon_sigdb_t *db) { delete db; }

void dragon_sigdb_load_directory(dragon_sigdb_t *db, const char *dir) {
  if (db && dir)
    db->db.loadDirectory(dir);
}

void dragon_sigdb_load_file(dragon_sigdb_t *db, const char *path) {
  if (db && path)
    db->db.loadFile(path);
}

size_t dragon_sigdb_signature_count(const dragon_sigdb_t *db) {
  return db ? db->db.signatureCount() : 0;
}

uint16_t dragon_sigs_crc16(const uint8_t *data, size_t len) {
  return dragon::sigs::SignatureMatcher::computeCRC16({data, len});
}

int dragon_sigdb_match_at(const dragon_sigdb_t *db, const uint8_t *bytes,
                          size_t len, uint64_t address,
                          uint64_t *out_address, char **out_name,
                          uint32_t *out_length) {
  if (!db || !bytes)
    return 0;
  auto match = dragon::sigs::SignatureMatcher::matchAt(db->db, {bytes, len},
                                                        address);
  if (!match)
    return 0;
  if (out_address)
    *out_address = match->address;
  if (out_name)
    *out_name = dup_string(match->name);
  if (out_length)
    *out_length = match->length;
  return 1;
}

void dragon_sigdb_match_entries(
    const dragon_sigdb_t *db, const uint8_t *image, size_t image_len,
    uint64_t image_base, const uint64_t *entry_points,
    size_t num_entry_points,
    void (*cb)(uint64_t, const char *, uint32_t, void *), void *user) {
  if (!db || !image || !cb || (!entry_points && num_entry_points > 0))
    return;
  std::vector<std::uint64_t> entries(entry_points,
                                     entry_points + num_entry_points);
  auto matches = dragon::sigs::SignatureMatcher::matchEntries(
      db->db, {image, image_len}, image_base, entries);
  for (const auto &m: matches)
    cb(m.address, m.name.c_str(), m.length, user);
}

#ifdef DRAGON_WITH_Z3
// =============================================================================
// IGNIL symbolic execution
// =============================================================================

namespace {

namespace sym = dragon::ignil;

struct SymExecImpl {
  sym::SymbolicExecutor exec;
  explicit SymExecImpl(sym::SymbolicOptions o) : exec(std::move(o)) {}
};

// A z3::expr does not own the context it was built in, so every expression
// handed across this boundary has to keep its executor alive by itself.
// Callers cannot be relied on to destroy expressions before the executor:
// Python finalizes objects in an order the caller does not control at
// interpreter shutdown, and a destroyed context makes ~expr a use-after-free.
// So the executor handle is a shared_ptr and every expression holds one -
// dragon_symexec_destroy drops the caller's reference, and the executor is
// really freed once the last expression is gone too.
using SymExecHandle = std::shared_ptr<SymExecImpl>;

struct SymExprImpl {
  SymExecHandle owner;
  z3::expr e;
};

struct SymStateImpl {
  SymExecHandle owner; // the state is full of z3::exprs; same rule as above
  sym::SymbolicState state;
};

struct SymDiffImpl {
  SymExecHandle owner; // ditto, for the values handed out from it
  std::vector<sym::SymbolicState::Delta> deltas;
};

struct SymRunImpl {
  SymExecHandle owner; // for the successor conditions handed out from it
  sym::RunResult result;
};

struct SolutionImpl {
  // Plain values only - no Z3 handles - so this one needs no owner.
  sym::Solution solution;
};

SymExecHandle *as_handle(dragon_symexec_t *e) {
  return reinterpret_cast<SymExecHandle *>(e);
}
const SymExecHandle *as_handle(const dragon_symexec_t *e) {
  return reinterpret_cast<const SymExecHandle *>(e);
}
SymExecImpl *as_exec(dragon_symexec_t *e) { return as_handle(e)->get(); }
const SymExecImpl *as_exec(const dragon_symexec_t *e) {
  return as_handle(e)->get();
}
const SymExprImpl *as_expr(const dragon_symexpr_t *x) {
  return reinterpret_cast<const SymExprImpl *>(x);
}
const SymRunImpl *as_run(const dragon_symrun_t *r) {
  return reinterpret_cast<const SymRunImpl *>(r);
}
const SymStateImpl *as_state(const dragon_symstate_t *s) {
  return reinterpret_cast<const SymStateImpl *>(s);
}
const SymDiffImpl *as_diff(const dragon_symdiff_t *d) {
  return reinterpret_cast<const SymDiffImpl *>(d);
}
const SolutionImpl *as_solution(const dragon_solution_t *s) {
  return reinterpret_cast<const SolutionImpl *>(s);
}

dragon_symexpr_t *wrap_sym(SymExecHandle owner, const z3::expr &e) {
  return reinterpret_cast<dragon_symexpr_t *>(
          new SymExprImpl{std::move(owner), e});
}

sym::Reg make_reg(std::uint32_t offset, std::uint8_t width) {
  return sym::Reg{offset, width, ""};
}

/// 0 keeps the library default; anything else is an explicit yes/no.
bool tri_state(int value, bool fallback) {
  if (value == 0)
    return fallback;
  return value > 0;
}

int tri_result(std::optional<bool> v) {
  if (!v)
    return -1;
  return *v ? 1 : 0;
}

} // namespace

dragon_symexec_t *dragon_symexec_create(const dragon_symexec_options_t *opts) {
  sym::SymbolicOptions o;
  if (opts) {
    if (opts->solver_timeout_ms)
      o.solverTimeoutMs = opts->solver_timeout_ms;
    if (opts->max_address_solutions)
      o.maxAddressSolutions = opts->max_address_solutions;
    if (opts->max_branch_targets)
      o.maxBranchTargets = opts->max_branch_targets;
    o.zeroExtendOn32BitWrite =
            tri_state(opts->zero_extend_on_32bit_write, o.zeroExtendOn32BitWrite);
    o.symbolizeUnmappedReads =
            tri_state(opts->symbolize_unmapped_reads, o.symbolizeUnmappedReads);
    o.stopAtTerminator = tri_state(opts->stop_at_terminator, o.stopAtTerminator);
  }
  return reinterpret_cast<dragon_symexec_t *>(
          new SymExecHandle(std::make_shared<SymExecImpl>(std::move(o))));
}

void dragon_symexec_destroy(dragon_symexec_t *e) {
  // Drops the caller's reference. Any expression still alive holds one of its
  // own, and the executor goes away with the last of them.
  delete as_handle(e);
}

void dragon_symexec_reset(dragon_symexec_t *e) {
  if (e)
    as_exec(e)->exec.reset();
}

void dragon_symexec_set_register(dragon_symexec_t *e, uint32_t offset,
                                 uint8_t width, uint64_t value) {
  if (e)
    as_exec(e)->exec.setRegister(make_reg(offset, width), value);
}

dragon_symexpr_t *dragon_symexec_symbolize_register(dragon_symexec_t *e,
                                                    uint32_t offset,
                                                    uint8_t width,
                                                    const char *name) {
  if (!e)
    return nullptr;
  return wrap_sym(*as_handle(e),
                  as_exec(e)->exec.symbolizeRegister(make_reg(offset, width),
                                                     name ? name : ""));
}

void dragon_symexec_map_memory(dragon_symexec_t *e, uint64_t address,
                               const uint8_t *data, size_t len) {
  if (!e || (!data && len))
    return;
  as_exec(e)->exec.mapMemory(address, std::vector<std::uint8_t>(data, data + len));
}

dragon_symexpr_t *dragon_symexec_symbolize_memory(dragon_symexec_t *e,
                                                  uint64_t address,
                                                  uint64_t size,
                                                  const char *name) {
  if (!e || !name)
    return nullptr;
  return wrap_sym(*as_handle(e),
                  as_exec(e)->exec.symbolizeMemory(address, size, name));
}

void dragon_symexec_assume(dragon_symexec_t *e, const dragon_symexpr_t *c) {
  if (!e || !c || !as_expr(c)->e.is_bool())
    return;
  as_exec(e)->exec.assume(as_expr(c)->e);
}

void dragon_symexec_write_register(dragon_symexec_t *e, uint32_t offset,
                                   uint8_t width, const dragon_symexpr_t *x) {
  if (!e || !x || width == 0 || !as_expr(x)->e.is_bv())
    return;
  as_exec(e)->exec.setRegister(make_reg(offset, width), as_expr(x)->e);
}

void dragon_symexec_write_memory(dragon_symexec_t *e, uint64_t address,
                                 const dragon_symexpr_t *x) {
  if (!e || !x || !as_expr(x)->e.is_bv())
    return;
  as_exec(e)->exec.setMemory(address, as_expr(x)->e);
}

void dragon_symexec_set_fresh_tag(dragon_symexec_t *e, const char *tag) {
  if (e)
    as_exec(e)->exec.setFreshTag(tag ? tag : "");
}

void dragon_symexec_set_concrete_opaque(dragon_symexec_t *e, uint32_t kind,
                                        uint64_t selector,
                                        const uint64_t *results, size_t n) {
  if (!e || (n && !results))
    return;
  as_exec(e)->exec.setConcreteOpaque(static_cast<sym::OpaqueKind>(kind), selector,
                                     std::vector<std::uint64_t>(results, results + n));
}

namespace {

int literal_of(const z3::expr &value, uint64_t *out) {
  const z3::expr folded = value.simplify();
  if (!folded.is_numeral() || !folded.is_bv() ||
      folded.get_sort().bv_size() > 64)
    return 0;
  if (out)
    *out = folded.get_numeral_uint64();
  return 1;
}

} // namespace

int dragon_symexec_concrete_register(const dragon_symexec_t *e, uint32_t offset,
                                     uint8_t width, uint64_t *out) {
  if (!e || width == 0)
    return 0;
  return literal_of(as_exec(e)->exec.readRegister(make_reg(offset, width)), out);
}

int dragon_symexec_concrete_memory(const dragon_symexec_t *e, uint64_t address,
                                   uint8_t width, uint64_t *out) {
  if (!e || width == 0)
    return 0;
  return literal_of(as_exec(e)->exec.readMemory(address, width), out);
}

dragon_symexpr_t *dragon_symexec_const(const dragon_symexec_t *e,
                                       uint64_t value, uint8_t width) {
  if (!e || width == 0 || width > 64)
    return nullptr;
  return wrap_sym(*as_handle(e), as_exec(e)->exec.bv(value, width));
}

dragon_symexpr_t *dragon_symexec_read_register(const dragon_symexec_t *e,
                                               uint32_t offset, uint8_t width) {
  if (!e || width == 0)
    return nullptr;
  return wrap_sym(*as_handle(e),
                  as_exec(e)->exec.readRegister(make_reg(offset, width)));
}

dragon_symexpr_t *dragon_symexec_read_memory(const dragon_symexec_t *e,
                                             uint64_t address, uint8_t width) {
  if (!e || width == 0)
    return nullptr;
  return wrap_sym(*as_handle(e), as_exec(e)->exec.readMemory(address, width));
}

// -- state snapshots ---------------------------------------------------------

dragon_symstate_t *dragon_symexec_snapshot(const dragon_symexec_t *e) {
  if (!e)
    return nullptr;
  return reinterpret_cast<dragon_symstate_t *>(
          new SymStateImpl{*as_handle(e), as_exec(e)->exec.state()});
}

void dragon_symexec_restore(dragon_symexec_t *e, const dragon_symstate_t *s) {
  if (!e || !s)
    return;
  as_exec(e)->exec.setState(as_state(s)->state);
}

void dragon_symstate_destroy(dragon_symstate_t *s) {
  delete reinterpret_cast<SymStateImpl *>(s);
}

dragon_symdiff_t *dragon_symstate_merge(dragon_symstate_t *into,
                                        const dragon_symstate_t *incoming,
                                        const char *tag) {
  if (!into || !incoming)
    return nullptr;
  auto *target = reinterpret_cast<SymStateImpl *>(into);
  return reinterpret_cast<dragon_symdiff_t *>(new SymDiffImpl{
          target->owner,
          target->state.merge(as_state(incoming)->state, tag ? tag : "")});
}

// -- state diffing -----------------------------------------------------------

dragon_symdiff_t *dragon_symexec_diff(const dragon_symexec_t *e,
                                      const dragon_symstate_t *before) {
  if (!e || !before)
    return nullptr;
  return reinterpret_cast<dragon_symdiff_t *>(new SymDiffImpl{
          *as_handle(e), as_exec(e)->exec.diff(as_state(before)->state)});
}

void dragon_symdiff_destroy(dragon_symdiff_t *d) {
  delete reinterpret_cast<SymDiffImpl *>(d);
}

size_t dragon_symdiff_count(const dragon_symdiff_t *d) {
  return d ? as_diff(d)->deltas.size() : 0;
}

int dragon_symdiff_entry(const dragon_symdiff_t *d, size_t index, int *kind,
                         uint64_t *key, uint8_t *width) {
  if (!d || index >= as_diff(d)->deltas.size())
    return 0;
  const auto &delta = as_diff(d)->deltas[index];
  if (kind)
    *kind = delta.kind == sym::SymbolicState::Delta::Kind::Register ? 0 : 1;
  if (key)
    *key = delta.key;
  if (width)
    *width = delta.width;
  return 1;
}

dragon_symexpr_t *dragon_symdiff_value(const dragon_symdiff_t *d, size_t index) {
  if (!d || index >= as_diff(d)->deltas.size())
    return nullptr;
  const auto &diff = *as_diff(d);
  return wrap_sym(diff.owner, diff.deltas[index].value);
}

void dragon_symexpr_destroy(dragon_symexpr_t *x) {
  delete reinterpret_cast<SymExprImpl *>(x);
}

char *dragon_symexpr_to_string(const dragon_symexpr_t *x) {
  if (!x)
    return nullptr;
  return dup_string(as_expr(x)->e.to_string());
}

char *dragon_symexpr_to_smt2(const dragon_symexpr_t *x) {
  if (!x)
    return nullptr;
  const z3::expr &e = as_expr(x)->e;
  z3::context &ctx = e.ctx();
  // The benchmark printer wants a formula, so a bitvector is tied to a result
  // variable the reader can pick the term back out from.
  const z3::expr formula =
          e.is_bool() ? e : (ctx.bv_const("dragon!result", e.get_sort().bv_size()) == e);
  return dup_string(Z3_benchmark_to_smtlib_string(ctx, "dragon", "", "unknown",
                                                  "", 0, nullptr, formula));
}

int dragon_symexpr_as_u64(const dragon_symexpr_t *x, uint64_t *out) {
  if (!x)
    return 0;
  return literal_of(as_expr(x)->e, out);
}

uint8_t dragon_symexpr_width(const dragon_symexpr_t *x) {
  if (!x || !as_expr(x)->e.is_bv())
    return 0;
  return static_cast<uint8_t>(as_expr(x)->e.get_sort().bv_size());
}

int dragon_symexpr_is_bool(const dragon_symexpr_t *x) {
  return x && as_expr(x)->e.is_bool() ? 1 : 0;
}

dragon_symexpr_t *dragon_symexpr_binop(dragon_symop_t op,
                                       const dragon_symexpr_t *a,
                                       const dragon_symexpr_t *b) {
  if (!a || !b)
    return nullptr;
  const SymExecHandle &owner = as_expr(a)->owner;
  const z3::expr &x = as_expr(a)->e;
  const z3::expr &y = as_expr(b)->e;
  // Z3 aborts the process on a cross-context operation, so refuse it here.
  if (&x.ctx() != &y.ctx())
    return nullptr;

  const bool wantBool = op >= DRAGON_SYMOP_BOOL_AND;
  if (wantBool && !(x.is_bool() && y.is_bool()))
    return nullptr;
  if (!wantBool && !(x.is_bv() && y.is_bv()))
    return nullptr;
  if (!wantBool && x.get_sort().bv_size() != y.get_sort().bv_size())
    return nullptr;

  switch (op) {
    case DRAGON_SYMOP_ADD: return wrap_sym(owner, x + y);
    case DRAGON_SYMOP_SUB: return wrap_sym(owner, x - y);
    case DRAGON_SYMOP_MUL: return wrap_sym(owner, x * y);
    case DRAGON_SYMOP_AND: return wrap_sym(owner, x & y);
    case DRAGON_SYMOP_OR: return wrap_sym(owner, x | y);
    case DRAGON_SYMOP_XOR: return wrap_sym(owner, x ^ y);
    case DRAGON_SYMOP_EQ: return wrap_sym(owner, x == y);
    case DRAGON_SYMOP_NE: return wrap_sym(owner, x != y);
    case DRAGON_SYMOP_ULT: return wrap_sym(owner, z3::ult(x, y));
    case DRAGON_SYMOP_ULE: return wrap_sym(owner, z3::ule(x, y));
    case DRAGON_SYMOP_UGT: return wrap_sym(owner, z3::ugt(x, y));
    case DRAGON_SYMOP_UGE: return wrap_sym(owner, z3::uge(x, y));
    case DRAGON_SYMOP_SLT: return wrap_sym(owner, x < y);
    case DRAGON_SYMOP_SLE: return wrap_sym(owner, x <= y);
    case DRAGON_SYMOP_SGT: return wrap_sym(owner, x > y);
    case DRAGON_SYMOP_SGE: return wrap_sym(owner, x >= y);
    case DRAGON_SYMOP_BOOL_AND: return wrap_sym(owner, x && y);
    case DRAGON_SYMOP_BOOL_OR: return wrap_sym(owner, x || y);
    case DRAGON_SYMOP_IMPLIES: return wrap_sym(owner, z3::implies(x, y));
    default: return nullptr;
  }
}

dragon_symexpr_t *dragon_symexpr_not(const dragon_symexpr_t *a) {
  if (!a || !as_expr(a)->e.is_bool())
    return nullptr;
  return wrap_sym(as_expr(a)->owner, !as_expr(a)->e);
}

dragon_symrun_t *dragon_symexec_run(dragon_symexec_t *e,
                                    const dragon_ignil_block_t *b) {
  if (!e || !b)
    return nullptr;
  auto *run = new SymRunImpl{*as_handle(e),
                             as_exec(e)->exec.run(*as_ignil_block(b))};
  return reinterpret_cast<dragon_symrun_t *>(run);
}

void dragon_symrun_destroy(dragon_symrun_t *r) {
  delete reinterpret_cast<SymRunImpl *>(r);
}

dragon_symexec_status_t dragon_symrun_status(const dragon_symrun_t *r) {
  if (!r)
    return DRAGON_SYMEXEC_MALFORMED_OP;
  return static_cast<dragon_symexec_status_t>(as_run(r)->result.status);
}

dragon_symexec_exit_t dragon_symrun_exit(const dragon_symrun_t *r) {
  if (!r)
    return DRAGON_SYMEXIT_ERROR;
  return static_cast<dragon_symexec_exit_t>(as_run(r)->result.exit);
}

int dragon_symrun_complete(const dragon_symrun_t *r) {
  return r && as_run(r)->result.complete() ? 1 : 0;
}

char *dragon_symrun_error(const dragon_symrun_t *r) {
  if (!r || as_run(r)->result.error.empty())
    return nullptr;
  return dup_string(as_run(r)->result.error);
}

void dragon_symrun_stats(const dragon_symrun_t *r, size_t *out_ops_executed,
                         size_t *out_solver_queries,
                         size_t *out_addresses_concretized) {
  if (!r)
    return;
  const auto &s = as_run(r)->result.stats;
  if (out_ops_executed)
    *out_ops_executed = s.opsExecuted;
  if (out_solver_queries)
    *out_solver_queries = s.solverQueries;
  if (out_addresses_concretized)
    *out_addresses_concretized = s.addressesConcretized;
}

size_t dragon_symrun_successor_count(const dragon_symrun_t *r) {
  return r ? as_run(r)->result.successors.size() : 0;
}

int dragon_symrun_successor(const dragon_symrun_t *r, size_t index,
                            uint64_t *out_address, int *out_taken) {
  if (!r || index >= as_run(r)->result.successors.size())
    return 0;
  const auto &s = as_run(r)->result.successors[index];
  if (out_address)
    *out_address = s.address;
  if (out_taken)
    *out_taken = s.taken ? 1 : 0;
  return 1;
}

dragon_symexpr_t *dragon_symrun_successor_condition(const dragon_symrun_t *r,
                                                    size_t index) {
  if (!r || index >= as_run(r)->result.successors.size())
    return nullptr;
  return wrap_sym(as_run(r)->owner,
                  as_run(r)->result.successors[index].condition);
}

size_t dragon_symrun_warning_count(const dragon_symrun_t *r) {
  return r ? as_run(r)->result.warnings.size() : 0;
}

size_t dragon_symrun_unresolved_count(const dragon_symrun_t *r) {
  return r ? as_run(r)->result.unresolved.size() : 0;
}

int dragon_symrun_unresolved(const dragon_symrun_t *r, size_t index,
                             int *out_is_store, uint64_t *out_op_address,
                             uint8_t *out_width) {
  if (!r || index >= as_run(r)->result.unresolved.size())
    return 0;
  const auto &access = as_run(r)->result.unresolved[index];
  if (out_is_store)
    *out_is_store = access.isStore ? 1 : 0;
  if (out_op_address)
    *out_op_address = access.opAddress;
  if (out_width)
    *out_width = access.width;
  return 1;
}

dragon_symexpr_t *dragon_symrun_unresolved_address(const dragon_symrun_t *r,
                                                   size_t index) {
  if (!r || index >= as_run(r)->result.unresolved.size())
    return nullptr;
  return wrap_sym(as_run(r)->owner, as_run(r)->result.unresolved[index].address);
}

dragon_symexpr_t *dragon_symrun_unresolved_value(const dragon_symrun_t *r,
                                                 size_t index) {
  if (!r || index >= as_run(r)->result.unresolved.size())
    return nullptr;
  return wrap_sym(as_run(r)->owner, as_run(r)->result.unresolved[index].value);
}

char *dragon_symrun_warning(const dragon_symrun_t *r, size_t index,
                            uint64_t *out_address) {
  if (!r || index >= as_run(r)->result.warnings.size())
    return nullptr;
  const auto &w = as_run(r)->result.warnings[index];
  if (out_address)
    *out_address = w.address;
  return dup_string(w.message);
}

int dragon_symexec_is_satisfiable(dragon_symexec_t *e,
                                  const dragon_symexpr_t *c) {
  if (!e || !c || !as_expr(c)->e.is_bool())
    return -1;
  return tri_result(as_exec(e)->exec.isSatisfiable(as_expr(c)->e));
}

int dragon_symexec_proves(dragon_symexec_t *e, const dragon_symexpr_t *c) {
  if (!e || !c || !as_expr(c)->e.is_bool())
    return -1;
  return tri_result(as_exec(e)->exec.proves(as_expr(c)->e));
}

int dragon_symexec_evaluate(dragon_symexec_t *e, const dragon_symexpr_t *x,
                            uint64_t *out) {
  if (!e || !x || !as_expr(x)->e.is_bv())
    return 0;
  auto v = as_exec(e)->exec.evaluate(as_expr(x)->e);
  if (!v)
    return 0;
  if (out)
    *out = *v;
  return 1;
}

size_t dragon_symexec_enumerate(dragon_symexec_t *e, const dragon_symexpr_t *x,
                                uint64_t *out, size_t max,
                                int *out_exhaustive) {
  if (out_exhaustive)
    *out_exhaustive = 0;
  if (!e || !x || !out || !max || !as_expr(x)->e.is_bv())
    return 0;

  bool exhaustive = false;
  auto values = as_exec(e)->exec.enumerate(as_expr(x)->e, max, &exhaustive);
  std::copy(values.begin(), values.end(), out);
  if (out_exhaustive)
    *out_exhaustive = exhaustive ? 1 : 0;
  return values.size();
}

dragon_solution_t *dragon_symexec_solve(dragon_symexec_t *e,
                                        const dragon_symexpr_t *goal) {
  if (!e)
    return nullptr;
  if (goal && !as_expr(goal)->e.is_bool())
    return nullptr;

  auto solution = goal ? as_exec(e)->exec.solve(as_expr(goal)->e)
                       : as_exec(e)->exec.solve();
  if (!solution)
    return nullptr;
  return reinterpret_cast<dragon_solution_t *>(
          new SolutionImpl{std::move(*solution)});
}

void dragon_solution_destroy(dragon_solution_t *s) {
  delete reinterpret_cast<SolutionImpl *>(s);
}

int dragon_solution_value(const dragon_solution_t *s, const char *symbol,
                          uint64_t *out) {
  if (!s || !symbol)
    return 0;
  auto v = as_solution(s)->solution.value(symbol);
  if (!v)
    return 0;
  if (out)
    *out = *v;
  return 1;
}

size_t dragon_solution_bytes(const dragon_solution_t *s, const char *region,
                             uint8_t *out, size_t cap) {
  if (!s || !region)
    return 0;
  const auto bytes = as_solution(s)->solution.bytes(region);
  if (out && cap)
    std::copy_n(bytes.begin(), std::min(cap, bytes.size()), out);
  return bytes.size();
}

void dragon_solution_each(const dragon_solution_t *s,
                          void (*cb)(const char *, uint64_t, void *),
                          void *user) {
  if (!s || !cb)
    return;
  for (const auto &[name, value]: as_solution(s)->solution.values())
    cb(name.c_str(), value, user);
}
#endif // DRAGON_WITH_Z3
