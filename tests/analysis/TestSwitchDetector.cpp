#include <catch2/catch_test_macros.hpp>

#include <dragon/Architecture.hpp>
#include <dragon/Configuration.hpp>
#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/disassembler/Instruction.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>

#include <llvm/IR/Verifier.h>

#include <cstring>
#include <set>

// Namespace aliases to disambiguate slice::Const from ignil::Const.
namespace slice = dragon::slice;
namespace ignil = dragon::ignil;
using namespace dragon::slice;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static dragon::Graph emptyGraph() {
  return dragon::Graph{dragon::Architecture::X86_64, dragon::Configuration{}};
}

// Build LOAD(ADD(Const{base}, MUL(SymReg{reg}, Const{scale})))
static Expr makeJumpTableExpr(std::uint64_t base, const ignil::Reg &reg,
                               std::uint64_t scale) {
  auto baseE  = std::make_shared<Expr>(slice::Const{base, 64});
  auto idxE   = std::make_shared<Expr>(SymReg{reg});
  auto scaleE = std::make_shared<Expr>(slice::Const{scale, 64});
  auto mul    = std::make_shared<Expr>(BinOp{ignil::Opcode::MUL, idxE, scaleE});
  auto add    = std::make_shared<Expr>(BinOp{ignil::Opcode::ADD, baseE, mul});
  return Expr{Load{add, 64}};
}

// ---------------------------------------------------------------------------
// Phase 1 - structural pattern matching (empty graph, no bounds extraction)
// ---------------------------------------------------------------------------

TEST_CASE("SwitchDetector -- detects 8-byte jump-table pattern", "[switch]") {
  auto g  = emptyGraph();
  ignil::Reg rax{0, 64, "RAX"};
  Expr expr = makeJumpTableExpr(0x4000, rax, 8);

  auto sw = detectPossibleSwitch(expr, 0x2000, g);

  REQUIRE(sw.has_value());
  CHECK(sw->tableBase == 0x4000);
  CHECK(sw->entrySize == 8);
  CHECK(sw->numCases == 0); // empty graph → no predecessor found
  CHECK(sw->indexReg.offset == rax.offset);
  CHECK(sw->indexReg.width == rax.width);
}

TEST_CASE("SwitchDetector -- detects 4-byte jump-table pattern", "[switch]") {
  auto g  = emptyGraph();
  ignil::Reg rcx{8, 64, "RCX"};
  Expr expr = makeJumpTableExpr(0x8000, rcx, 4);

  auto sw = detectPossibleSwitch(expr, 0x3000, g);

  REQUIRE(sw.has_value());
  CHECK(sw->tableBase == 0x8000);
  CHECK(sw->entrySize == 4);
  CHECK(sw->indexReg.offset == rcx.offset);
}

TEST_CASE("SwitchDetector -- commutative outer ADD (MUL before base)", "[switch]") {
  // ADD(MUL(idx, scale), base) - base is the right operand
  auto g   = emptyGraph();
  ignil::Reg rax{0, 64, "RAX"};
  auto baseE  = std::make_shared<Expr>(slice::Const{0x5000, 64});
  auto idxE   = std::make_shared<Expr>(SymReg{rax});
  auto scaleE = std::make_shared<Expr>(slice::Const{8, 64});
  auto mul    = std::make_shared<Expr>(BinOp{ignil::Opcode::MUL, idxE, scaleE});
  Expr expr{Load{std::make_shared<Expr>(BinOp{ignil::Opcode::ADD, mul, baseE}), 64}};

  auto sw = detectPossibleSwitch(expr, 0x1000, g);

  REQUIRE(sw.has_value());
  CHECK(sw->tableBase == 0x5000);
  CHECK(sw->entrySize == 8);
}

TEST_CASE("SwitchDetector -- commutative inner MUL (scale before index)", "[switch]") {
  // LOAD(ADD(base, MUL(scale, idx))) - scale is the left MUL operand
  auto g   = emptyGraph();
  ignil::Reg rdx{24, 64, "RDX"};
  auto baseE  = std::make_shared<Expr>(slice::Const{0x6000, 64});
  auto idxE   = std::make_shared<Expr>(SymReg{rdx});
  auto scaleE = std::make_shared<Expr>(slice::Const{8, 64});
  auto mul    = std::make_shared<Expr>(BinOp{ignil::Opcode::MUL, scaleE, idxE});
  auto add    = std::make_shared<Expr>(BinOp{ignil::Opcode::ADD, baseE, mul});
  Expr expr{Load{add, 64}};

  auto sw = detectPossibleSwitch(expr, 0x1000, g);

  REQUIRE(sw.has_value());
  CHECK(sw->tableBase == 0x6000);
  CHECK(sw->indexReg.offset == rdx.offset);
}

TEST_CASE("SwitchDetector -- rejects plain register target", "[switch]") {
  auto g = emptyGraph();
  Expr expr{SymReg{ignil::Reg{0, 64, "RAX"}}};
  CHECK_FALSE(detectPossibleSwitch(expr, 0x1000, g).has_value());
}

TEST_CASE("SwitchDetector -- rejects constant target", "[switch]") {
  auto g = emptyGraph();
  Expr expr{slice::Const{0x4000, 64}};
  CHECK_FALSE(detectPossibleSwitch(expr, 0x1000, g).has_value());
}

TEST_CASE("SwitchDetector -- rejects wrong scale (3 bytes)", "[switch]") {
  auto g = emptyGraph();
  Expr expr = makeJumpTableExpr(0x4000, ignil::Reg{0, 64, "RAX"}, 3);
  CHECK_FALSE(detectPossibleSwitch(expr, 0x1000, g).has_value());
}

TEST_CASE("SwitchDetector -- rejects ADD without Load wrapper", "[switch]") {
  auto g      = emptyGraph();
  auto baseE  = std::make_shared<Expr>(slice::Const{0x4000, 64});
  auto idxE   = std::make_shared<Expr>(SymReg{ignil::Reg{0, 64, "RAX"}});
  auto scaleE = std::make_shared<Expr>(slice::Const{8, 64});
  auto mul    = std::make_shared<Expr>(BinOp{ignil::Opcode::MUL, idxE, scaleE});
  Expr expr{BinOp{ignil::Opcode::ADD, baseE, mul}};
  CHECK_FALSE(detectPossibleSwitch(expr, 0x1000, g).has_value());
}

// ---------------------------------------------------------------------------
// Phase 2 - bounds extraction from a CJMP predecessor block
// ---------------------------------------------------------------------------

TEST_CASE("SwitchDetector -- extracts numCases from UGE predecessor",
          "[switch]") {
  dragon::Configuration cfg;
  dragon::Graph g{dragon::Architecture::X86_64, cfg};

  // IGNIL predecessor block at 0x1000:
  //   t0 = UGE(RAX, 5)           ; idx >= 5 means out-of-bounds
  //   CJMP(t0, 0x3000, 0x1005)   ; taken=default, not-taken=switch block
  ignil::Block predBlock{0x1000};
  predBlock.append({ignil::Opcode::UGE,
                    ignil::Value{ignil::Temp{0, 1}},
                    {ignil::Value{ignil::Reg{0, 64, "RAX"}},
                     ignil::Value{ignil::Const{5, 64}}},
                    0x1000});
  predBlock.append({ignil::Opcode::CJMP,
                    std::nullopt,
                    {ignil::Value{ignil::Temp{0, 1}},
                     ignil::Value{ignil::Const{0x3000, 64}},
                     ignil::Value{ignil::Const{0x1005, 64}}},
                    0x1003});

  // Instruction at 0x1000: JAE to default, size 5 so fall-through = 0x1005
  dragon::Instruction predInsn{
      0x1000, 5, "jae", "0x3000",
      std::vector<std::uint8_t>{0x0F, 0x83, 0x00, 0x00, 0x00},
      false, false, true, true, false, false};
  predInsn.addBranch(0x3000);
  predInsn.setBlockStart(true);

  g.addInstruction(std::move(predInsn));
  g.addBlock(0x1000);
  g.addIGNILBlock(std::move(predBlock));

  ignil::Reg rax{0, 64, "RAX"};
  Expr jumpTarget = makeJumpTableExpr(0x4000, rax, 8);

  auto sw = detectPossibleSwitch(jumpTarget, 0x1005, g);

  REQUIRE(sw.has_value());
  CHECK(sw->tableBase == 0x4000);
  CHECK(sw->entrySize == 8);
  CHECK(sw->numCases == 5);
  CHECK(sw->indexReg.offset == rax.offset);
}

// ---------------------------------------------------------------------------
// readTableTargets
// ---------------------------------------------------------------------------

TEST_CASE("readTableTargets -- returns empty when numCases is 0", "[switch]") {
  SwitchCandidate sw{0x4000, 8, 0, ignil::Reg{0, 64, "RAX"}};
  std::vector<std::uint8_t> bytes(64, 0);
  CHECK(readTableTargets(sw, bytes, 0x4000).empty());
}

TEST_CASE("readTableTargets -- returns empty when table base is before buffer",
          "[switch]") {
  SwitchCandidate sw{0x3F00, 8, 4, ignil::Reg{0, 64, "RAX"}};
  std::vector<std::uint8_t> bytes(64, 0); // covers 0x4000 - 0x403F
  CHECK(readTableTargets(sw, bytes, 0x4000).empty());
}

TEST_CASE("readTableTargets -- returns empty when table extends beyond buffer",
          "[switch]") {
  // Table at 0x4038 (offset 56), 4 x 8 = 32 bytes needed, only 8 remain
  SwitchCandidate sw{0x4038, 8, 4, ignil::Reg{0, 64, "RAX"}};
  std::vector<std::uint8_t> bytes(64, 0);
  CHECK(readTableTargets(sw, bytes, 0x4000).empty());
}

TEST_CASE("readTableTargets -- reads 8-byte entries correctly", "[switch]") {
  // Buffer at 0x2000, jump table at 0x2010, three 64-bit entries
  std::vector<std::uint8_t> bytes(0x40, 0);
  const std::uint64_t t0 = 0x3000, t1 = 0x3010, t2 = 0x3020;
  std::memcpy(bytes.data() + 0x10, &t0, 8);
  std::memcpy(bytes.data() + 0x18, &t1, 8);
  std::memcpy(bytes.data() + 0x20, &t2, 8);

  SwitchCandidate sw{0x2010, 8, 3, ignil::Reg{0, 64, "RAX"}};
  auto targets = readTableTargets(sw, bytes, 0x2000);

  REQUIRE(targets.size() == 3);
  CHECK(targets[0] == 0x3000);
  CHECK(targets[1] == 0x3010);
  CHECK(targets[2] == 0x3020);
}

TEST_CASE("readTableTargets -- reads 4-byte entries correctly", "[switch]") {
  // Buffer at 0x3000, jump table at 0x3008, four 32-bit entries
  std::vector<std::uint8_t> bytes(0x40, 0);
  const std::uint32_t t0 = 0x1000, t1 = 0x1010, t2 = 0x1020, t3 = 0x1030;
  std::memcpy(bytes.data() + 0x08, &t0, 4);
  std::memcpy(bytes.data() + 0x0C, &t1, 4);
  std::memcpy(bytes.data() + 0x10, &t2, 4);
  std::memcpy(bytes.data() + 0x14, &t3, 4);

  SwitchCandidate sw{0x3008, 4, 4, ignil::Reg{0, 64, "RDX"}};
  auto targets = readTableTargets(sw, bytes, 0x3000);

  REQUIRE(targets.size() == 4);
  CHECK(targets[0] == 0x1000);
  CHECK(targets[1] == 0x1010);
  CHECK(targets[2] == 0x1020);
  CHECK(targets[3] == 0x1030);
}

// ---------------------------------------------------------------------------
// End to end on real lifter output.
//
// The cases above hand-build LOAD(ADD(base, MUL(idx, scale))), but no lifter
// produces that: the x86 lifter writes a SIB scale as SHL inside a nested ADD,
// the bounds check slices to AND(EQ(ULT(..),0), EQ(EQ(SUB(..),0),0)) rather
// than UGE/UGT, and the i386 dispatch `jmp [eax*4 + table]` jumps through
// memory, which resolveIndirectJumps used to skip. So on compiled code no
// jump table was ever resolved. These run compiler-shaped dispatch sequences
// through Disassembler::disassemble and check every case block is found.
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kBase = 0x1000;
constexpr std::uint64_t kDefault = 0x1200;
constexpr std::uint64_t kTable = 0x1300;
constexpr std::uint64_t kCase0 = 0x1100;

void putLE(std::vector<std::uint8_t> &buf, std::size_t off, std::uint64_t v,
           unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    buf[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// `code` at kBase, a `ret` at each case and at kDefault, the table at kTable
// with `cases` entries of `entry` bytes, then decoy entries a wrong bound
// would read as further cases.
std::vector<std::uint8_t> layout(std::vector<std::uint8_t> code,
                                 unsigned cases, unsigned entry) {
  std::vector<std::uint8_t> buf(kTable - kBase + (cases + 4) * entry, 0xcc);
  std::copy(code.begin(), code.end(), buf.begin());
  for (unsigned i = 0; i < cases; ++i) {
    buf[kCase0 + 0x10 * i - kBase] = 0xc3;
    putLE(buf, kTable - kBase + i * entry, kCase0 + 0x10 * i, entry);
  }
  buf[kDefault - kBase] = 0xc3;
  for (unsigned i = cases; i < cases + 4; ++i) // decoys: inside the buffer
    putLE(buf, kTable - kBase + i * entry, kCase0 + 0x10 * i + 8, entry);
  return buf;
}

std::set<std::uint64_t> blockStarts(dragon::Architecture arch,
                                    const std::vector<std::uint8_t> &bytes) {
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(arch, cfg);
  dragon::Graph g{arch, cfg};
  dis.disassemble(bytes, kBase, g);
  std::set<std::uint64_t> out;
  for (const auto &b: g.blocks())
    out.insert(b.startAddress());
  return out;
}

void requireExactlyCases(const std::set<std::uint64_t> &blocks,
                         unsigned cases) {
  for (unsigned i = 0; i < cases; ++i) {
    INFO("case " << i);
    CHECK(blocks.count(kCase0 + 0x10 * i));
  }
  for (unsigned i = cases; i < cases + 4; ++i) {
    INFO("decoy " << i);
    CHECK_FALSE(blocks.count(kCase0 + 0x10 * i + 8));
  }
  CHECK(blocks.count(kDefault));
}

} // namespace

TEST_CASE("SwitchDetector - i386 gcc dispatch: jmp [eax*4 + table]",
          "[switch][e2e]") {
  // i686-w64-mingw32-gcc -O2, 7 cases:
  //   mov eax, [esp+4] ; cmp eax, 6 ; ja default ; jmp [eax*4 + table]
  std::vector<std::uint8_t> code = {0x8b, 0x44, 0x24, 0x04,             //
                                    0x83, 0xf8, 0x06,                   //
                                    0x0f, 0x87, 0, 0, 0, 0,             //
                                    0xff, 0x24, 0x85, 0, 0, 0, 0};
  putLE(code, 9, kDefault - 0x100d, 4);
  putLE(code, 16, kTable, 4);
  requireExactlyCases(
          blockStarts(dragon::Architecture::X86_32, layout(code, 7, 4)), 7);
}

TEST_CASE("SwitchDetector - x86-64 gcc non-PIC dispatch: mov edi, edi ; "
          "jmp [rdi*8 + table]",
          "[switch][e2e]") {
  //   cmp edi, 6 ; ja default ; mov edi, edi ; jmp [rdi*8 + table]
  std::vector<std::uint8_t> code = {0x83, 0xff, 0x06,       //
                                    0x0f, 0x87, 0, 0, 0, 0, //
                                    0x89, 0xff,             //
                                    0xff, 0x24, 0xfd, 0, 0, 0, 0};
  putLE(code, 5, kDefault - 0x1009, 4);
  putLE(code, 14, kTable, 4);
  requireExactlyCases(
          blockStarts(dragon::Architecture::X86_64, layout(code, 7, 8)), 7);
}

TEST_CASE("SwitchDetector - x86-64 register dispatch with a jae guard",
          "[switch][e2e]") {
  //   cmp edi, 5 ; jae default ; mov eax, edi ; mov rax, [rax*8 + table] ;
  //   jmp rax
  std::vector<std::uint8_t> code = {0x83, 0xff, 0x05,             //
                                    0x0f, 0x83, 0, 0, 0, 0,       //
                                    0x89, 0xf8,                   //
                                    0x48, 0x8b, 0x04, 0xc5, 0, 0, 0, 0, //
                                    0xff, 0xe0};
  putLE(code, 5, kDefault - 0x1009, 4);
  putLE(code, 15, kTable, 4);
  requireExactlyCases(
          blockStarts(dragon::Architecture::X86_64, layout(code, 5, 8)), 5);
}

TEST_CASE("SwitchDetector - a signed guard is not a table bound",
          "[switch][e2e]") {
  // cmp edi, 6 ; jg default - a negative index passes the guard, so nothing
  // bounds the table and no entry is read.
  std::vector<std::uint8_t> code = {0x83, 0xff, 0x06,       //
                                    0x0f, 0x8f, 0, 0, 0, 0, //
                                    0x89, 0xff,             //
                                    0xff, 0x24, 0xfd, 0, 0, 0, 0};
  putLE(code, 5, kDefault - 0x1009, 4);
  putLE(code, 14, kTable, 4);
  auto blocks = blockStarts(dragon::Architecture::X86_64, layout(code, 7, 8));
  CHECK_FALSE(blocks.count(kCase0));
}

TEST_CASE("SwitchDetector - matches the x86 lifter's address shape",
          "[switch]") {
  // LOAD(ADD(ADD(0, SHL(ZEXT(EDI), 3)), table)) - `mov edi, edi` followed by
  // `jmp [rdi*8 + table]`.
  auto g = emptyGraph();
  ignil::Reg edi{56, 32, "EDI"};
  auto c = [](std::uint64_t v, ignil::Width w) {
    return std::make_shared<Expr>(slice::Const{v, w});
  };
  auto zext = std::make_shared<Expr>(
          UnaryOp{ignil::Opcode::ZEXT, std::make_shared<Expr>(SymReg{edi}), 64});
  auto shl = std::make_shared<Expr>(BinOp{ignil::Opcode::SHL, zext, c(3, 8)});
  auto inner = std::make_shared<Expr>(BinOp{ignil::Opcode::ADD, c(0, 64), shl});
  auto addr = std::make_shared<Expr>(
          BinOp{ignil::Opcode::ADD, inner, c(0x402000, 64)});
  auto sw = detectPossibleSwitch(Expr{Load{addr, 64}}, 0x2000, g);
  REQUIRE(sw.has_value());
  CHECK(sw->tableBase == 0x402000);
  CHECK(sw->entrySize == 8);
  CHECK(sw->indexReg.offset == edi.offset);
  CHECK(sw->indexReg.width == 32);
}

// ---------------------------------------------------------------------------
// A resolved table is part of the CFG.
//
// Resolution used to add the case blocks to the graph without any edge to
// them: the jump block's only successor was the (made-up) fall-through, the
// cases belonged to no function, and the LLVM lifter turned the dispatch into
// `unreachable`. Linear sweep did not resolve tables at all, and a table
// outside the analysed bytes (.rdata) could not be read.
// ---------------------------------------------------------------------------

namespace {

// x86-64 non-PIC dispatch, 7 cases: the jump block is at 0x1009.
std::vector<std::uint8_t> nonPicDispatch() {
  std::vector<std::uint8_t> code = {0x83, 0xff, 0x06,       //
                                    0x0f, 0x87, 0, 0, 0, 0, //
                                    0x89, 0xff,             //
                                    0xff, 0x24, 0xfd, 0, 0, 0, 0};
  putLE(code, 5, kDefault - 0x1009, 4);
  putLE(code, 14, kTable, 4);
  return layout(code, 7, 8);
}

} // namespace

TEST_CASE("SwitchDetector - resolved cases are CFG edges of the jump",
          "[switch][e2e][cfg]") {
  const auto bytes = nonPicDispatch();
  std::set<std::uint64_t> cases;
  for (unsigned i = 0; i < 7; ++i)
    cases.insert(kCase0 + 0x10 * i);

  for (auto strategy: {dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL,
                       dragon::DisassemblyStrategy::LINEAR_SWEEP}) {
    INFO("strategy " << static_cast<int>(strategy));
    dragon::Configuration cfg;
    cfg.disassemblyStrategy = strategy;
    dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
    dragon::Graph g{dragon::Architecture::X86_64, cfg};
    dis.disassemble(bytes, kBase, g);

    const dragon::BasicBlock jumpBlock{&g, 0x1009};
    REQUIRE(jumpBlock.isValid());
    CHECK(jumpBlock.successors() == cases);

    const dragon::Function fn{&g, kBase};
    REQUIRE(fn.isValid());
    for (std::uint64_t c: cases) {
      INFO("case 0x" << std::hex << c);
      CHECK(fn.block(c) != nullptr);
    }

    // The LLVM lifter dispatches on the target instead of `unreachable`.
    dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
    REQUIRE(lifter.lift(fn, g));
    REQUIRE_FALSE(llvm::verifyModule(lifter.module(), &llvm::errs()));
    const std::string ir = lifter.ir();
    CHECK(ir.find("switch i64") != std::string::npos);
  }
}

TEST_CASE("SwitchDetector - a table outside the code bytes is read through "
          "AnalysisHints::readMemory",
          "[switch][e2e][cfg]") {
  auto image = nonPicDispatch();
  // Hand the analysis only the code, as loader::disassembleImage does with
  // .text; the table stays reachable through readMemory.
  const std::vector<std::uint8_t> code(image.begin(),
                                       image.begin() + (kTable - kBase));
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g{dragon::Architecture::X86_64, cfg};
  dragon::Disassembler::AnalysisHints hints;
  hints.readMemory = [&image](std::uint64_t addr, std::span<std::uint8_t> out) {
    if (addr < kBase || addr - kBase + out.size() > image.size())
      return false;
    std::memcpy(out.data(), image.data() + (addr - kBase), out.size());
    return true;
  };
  dis.disassemble(code, kBase, g, hints);
  const dragon::BasicBlock jumpBlock{&g, 0x1009};
  REQUIRE(jumpBlock.isValid());
  CHECK(jumpBlock.successors().size() == 7);
}
