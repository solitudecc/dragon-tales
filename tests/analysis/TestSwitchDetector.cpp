#include <catch2/catch_test_macros.hpp>

#include <dragon/Architecture.hpp>
#include <dragon/Configuration.hpp>
#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/assembler/Assembler.hpp>
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

TEST_CASE("SwitchDetector - code reachable from a case is decoded too",
          "[switch][e2e][cfg]") {
  // Case 0 branches on: 0x1100 test edi, edi ; je 0x1180 ; ret / 0x1180 ret.
  // Resolution decoded only the first block of each target, so 0x1180 (and
  // the je fall-through) became block starts with no instructions.
  auto bytes = nonPicDispatch();
  const std::vector<std::uint8_t> case0 = {0x85, 0xff, 0x74, 0x7c, 0xc3};
  std::copy(case0.begin(), case0.end(), bytes.begin() + (kCase0 - kBase));
  bytes[0x1180 - kBase] = 0xc3;

  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g{dragon::Architecture::X86_64, cfg};
  dis.disassemble(bytes, kBase, g);

  for (std::uint64_t a: {std::uint64_t{0x1104}, std::uint64_t{0x1180}}) {
    INFO("block 0x" << std::hex << a);
    CHECK(g.get(a) != nullptr);
    CHECK(g.getIGNILBlock(a) != nullptr);
  }
  for (std::uint64_t b: g.validBlockAddresses()) {
    INFO("empty block 0x" << std::hex << b);
    CHECK(g.get(b) != nullptr);
  }
}

// ---------------------------------------------------------------------------
// Tables of offsets.
//
// Position-independent x86-64 code and gcc's AArch64 output dispatch through
// a table of signed offsets, not pointers - none of them was recognised (0 of
// 34 tables in an x86-64 mingw Lua build, 0 of 8 in an aarch64 gcc one).
// ---------------------------------------------------------------------------

namespace {

std::set<std::uint64_t> jumpSuccessors(dragon::Architecture arch,
                                       const std::vector<std::uint8_t> &bytes,
                                       std::uint64_t jumpBlock) {
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(arch, cfg);
  dragon::Graph g{arch, cfg};
  dis.disassemble(bytes, kBase, g);
  const dragon::BasicBlock bb{&g, jumpBlock};
  REQUIRE(bb.isValid());
  return bb.successors();
}

} // namespace

TEST_CASE("SwitchDetector - x86-64 PIC table of 32-bit offsets",
          "[switch][e2e][relative]") {
  //   cmp edi, 6 ; ja default ; mov edi, edi ; lea rdx, [rip + table] ;
  //   movsxd rax, dword ptr [rdx + rdi*4] ; add rax, rdx ; jmp rax
  // entry[i] = case_i - table
  std::vector<std::uint8_t> code = {0x83, 0xff, 0x06,                   //
                                    0x0f, 0x87, 0, 0, 0, 0,             //
                                    0x89, 0xff,                         //
                                    0x48, 0x8d, 0x15, 0, 0, 0, 0,       //
                                    0x48, 0x63, 0x04, 0xba,             //
                                    0x48, 0x01, 0xd0,                   //
                                    0xff, 0xe0};
  putLE(code, 5, kDefault - 0x1009, 4);
  putLE(code, 14, kTable - 0x1012, 4);
  std::vector<std::uint8_t> bytes(kTable - kBase + 11 * 4, 0xcc);
  std::copy(code.begin(), code.end(), bytes.begin());
  std::set<std::uint64_t> cases;
  for (unsigned i = 0; i < 7; ++i) {
    const std::uint64_t c = kCase0 + 0x10 * i;
    cases.insert(c);
    bytes[c - kBase] = 0xc3;
    putLE(bytes, kTable - kBase + 4 * i, c - kTable, 4);
  }
  bytes[kDefault - kBase] = 0xc3;
  for (unsigned i = 7; i < 11; ++i) // decoys past the bound
    putLE(bytes, kTable - kBase + 4 * i, kCase0 + 0x10 * i + 8 - kTable, 4);

  CHECK(jumpSuccessors(dragon::Architecture::X86_64, bytes, 0x1009) == cases);
}

TEST_CASE("SwitchDetector - AArch64 gcc table of 16-bit offsets",
          "[switch][e2e][relative]") {
  //   0x1000 cmp w0, #6 ; b.hi default ; adr x1, table ;
  //   0x100c ldrh w0, [x1, w0, uxtw #1] ; adr x2, L ;
  //   0x1014 add x0, x2, w0, sxth #2 ; br x0 ; 0x101c L:
  // entry[i] = (case_i - L) / 4, as gcc emits it.
  dragon::Assembler as{dragon::Architecture::ARM64, dragon::Configuration{}};
  std::vector<std::uint8_t> code;
  for (const char *line: {"cmp w0, #6", "b.hi #0x1fc", "adr x1, #0x2f8",
                          "ldrh w0, [x1, w0, uxtw #1]", "adr x2, #0xc",
                          "add x0, x2, w0, sxth #2", "br x0"}) {
    auto insn = as.assemble(line);
    REQUIRE(insn.size() == 4);
    code.insert(code.end(), insn.begin(), insn.end());
  }
  constexpr std::uint64_t kLabel = 0x101c;
  const std::uint8_t ret[] = {0xc0, 0x03, 0x5f, 0xd6};
  std::vector<std::uint8_t> bytes(kTable - kBase + 11 * 2, 0);
  std::copy(code.begin(), code.end(), bytes.begin());
  std::set<std::uint64_t> cases;
  for (unsigned i = 0; i < 7; ++i) {
    const std::uint64_t c = kCase0 + 0x10 * i;
    cases.insert(c);
    std::copy(std::begin(ret), std::end(ret), bytes.begin() + (c - kBase));
    putLE(bytes, kTable - kBase + 2 * i, (c - kLabel) / 4, 2);
  }
  std::copy(std::begin(ret), std::end(ret),
            bytes.begin() + (kDefault - kBase));
  for (unsigned i = 7; i < 11; ++i)
    putLE(bytes, kTable - kBase + 2 * i, (kCase0 + 0x10 * i + 8 - kLabel) / 4,
          2);
  CHECK(jumpSuccessors(dragon::Architecture::ARM64, bytes, 0x1008) == cases);
}

TEST_CASE("readTableTargets - signed, shifted offsets", "[switch][relative]") {
  SwitchCandidate sw{0x2000, 2, 3, ignil::Reg{0, 64, "X0"}};
  sw.relative = true;
  sw.targetBase = 0x1800;
  sw.entryShift = 2;
  sw.entryBits = 16;
  sw.entrySigned = true;
  // entries 0x0010, 0xfff0 (-16), 0x0000
  const std::vector<std::uint8_t> table = {0x10, 0x00, 0xf0, 0xff, 0x00, 0x00};
  auto t = readTableTargets(sw, table, 0x2000);
  REQUIRE(t.size() == 3);
  CHECK(t[0] == 0x1840);
  CHECK(t[1] == 0x17c0);
  CHECK(t[2] == 0x1800);
}

namespace {

// Hoist the table base before a dispatch loop. Case blocks are only reachable
// after resolving the dispatch; linear sweep also sees an unreachable incoming
// edge with an unknown R13. A case may deliberately invalidate the hypothesis.
std::vector<std::uint8_t> loopDispatch(bool clobberBase) {
  std::vector<std::uint8_t> bytes(kTable - kBase + 3 * 4, 0xcc);
  auto emit = [&](std::uint64_t at, std::initializer_list<std::uint8_t> code) {
    std::copy(code.begin(), code.end(), bytes.begin() + (at - kBase));
  };
  emit(0x1000, {0x4c, 0x8d, 0x2d, 0, 0, 0, 0, 0xeb, 0x17}); // lea r13,T; jmp loop
  putLE(bytes, 3, kTable - 0x1007, 4);
  emit(0x1020, {0x8b, 0x06,                         // mov eax,[rsi]
                0x83, 0xf8, 0x02,                   // cmp eax,2
                0x0f, 0x87, 0, 0, 0, 0,             // ja default
                0x49, 0x63, 0x44, 0x85, 0x00,       // movsxd rax,[r13+rax*4]
                0x4c, 0x01, 0xe8, 0xff, 0xe0});     // add rax,r13; jmp rax
  putLE(bytes, 0x1027 - kBase, kDefault - 0x102b, 4);
  emit(0x1070, {0x75, 0x0e}); // makes 0x1080 a block under linear sweep
  emit(0x1080, {0xe9, 0, 0, 0, 0}); // unreachable predecessor of loop
  putLE(bytes, 0x1081 - kBase, 0x1020 - 0x1085, 4);
  for (unsigned i = 0; i < 3; ++i) {
    const std::uint64_t at = kCase0 + 0x10 * i;
    putLE(bytes, kTable - kBase + 4 * i, at - kTable, 4);
    if (clobberBase && i == 1) {
      emit(at, {0x49, 0x83, 0xc5, 0x04, 0xe9, 0, 0, 0, 0}); // add r13,4
      putLE(bytes, at + 5 - kBase, 0x1020 - (at + 9), 4);
    } else {
      emit(at, {0xe9, 0, 0, 0, 0});
      putLE(bytes, at + 1 - kBase, 0x1020 - (at + 5), 4);
    }
  }
  bytes[kDefault - kBase] = 0xc3;
  return bytes;
}

} // namespace

TEST_CASE("SwitchDetector - iteratively proves a loop invariant table base",
          "[switch][e2e][iterative]") {
  for (auto strategy: {dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL,
                       dragon::DisassemblyStrategy::LINEAR_SWEEP}) {
    INFO("strategy " << static_cast<int>(strategy));
    dragon::Configuration cfg;
    cfg.disassemblyStrategy = strategy;
    dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
    dragon::Graph g{dragon::Architecture::X86_64, cfg};
    dis.disassemble(loopDispatch(false), kBase, g);
    REQUIRE(g.get(0x1033));
    CHECK(g.get(0x1033)->branches() ==
          std::set<std::uint64_t>{0x1100, 0x1110, 0x1120});
    const dragon::Function fn{&g, kBase};
    CHECK(fn.block(0x1100) != nullptr);
    CHECK(fn.block(0x1120) != nullptr);
  }
}

TEST_CASE("SwitchDetector - rolls back a base modified by a tentative case",
          "[switch][e2e][iterative]") {
  for (auto strategy: {dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL,
                       dragon::DisassemblyStrategy::LINEAR_SWEEP}) {
    INFO("strategy " << static_cast<int>(strategy));
    dragon::Configuration cfg;
    cfg.disassemblyStrategy = strategy;
    dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
    dragon::Graph g{dragon::Architecture::X86_64, cfg};
    dis.disassemble(loopDispatch(true), kBase, g);
    REQUIRE(g.get(0x1033));
    CHECK(g.get(0x1033)->branches().empty());
    CHECK(g.get(0x1033)->edges() == 1); // original unresolved jump metadata
    CHECK_FALSE(g.isValidBlock(kCase0));
    CHECK(g.getIGNILBlock(kCase0) == nullptr);
    if (strategy == dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL)
      CHECK(g.get(kCase0) == nullptr);
  }
}

TEST_CASE("SwitchDetector - rechecks earlier tables after dependent resolution",
          "[switch][e2e][iterative]") {
  auto bytes = loopDispatch(false);
  bytes.resize(0x1328 - kBase, 0xcc);
  auto emit = [&](std::uint64_t at, std::initializer_list<std::uint8_t> code) {
    std::copy(code.begin(), code.end(), bytes.begin() + (at - kBase));
  };
  emit(0x1100, {0xe9, 0, 0, 0, 0}); // case 0 reaches another dispatch
  putLE(bytes, 0x1101 - kBase, 0x1140 - 0x1105, 4);
  emit(0x1140, {0x8b, 0x4e, 0x04,             // mov ecx,[rsi+4]
                0x83, 0xf9, 0x01,             // cmp ecx,1
                0x0f, 0x87, 0, 0, 0, 0,       // ja default
                0x48, 0x8d, 0x15, 0, 0, 0, 0, // lea rdx,table2
                0x48, 0x63, 0x04, 0x8a,       // movsxd rax,[rdx+rcx*4]
                0x48, 0x01, 0xd0, 0xff, 0xe0});
  putLE(bytes, 0x1148 - kBase, kDefault - 0x114c, 4);
  putLE(bytes, 0x114f - kBase, 0x1320 - 0x1153, 4);
  putLE(bytes, 0x1320 - kBase, 0x1180 - 0x1320, 4);
  putLE(bytes, 0x1324 - kBase, 0x1190 - 0x1320, 4);
  emit(0x1180, {0x49, 0x83, 0xc5, 0x04, 0xe9, 0, 0, 0, 0}); // add r13,4
  putLE(bytes, 0x1185 - kBase, 0x1020 - 0x1189, 4);
  bytes[0x1190 - kBase] = 0xc3;

  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g{dragon::Architecture::X86_64, cfg};
  dis.disassemble(bytes, kBase, g);
  REQUIRE(g.get(0x1033));
  CHECK(g.get(0x1033)->branches().empty());
  CHECK(g.get(0x1140) == nullptr);
  CHECK(g.get(0x1180) == nullptr);
  CHECK_FALSE(g.isValidBlock(0x1140));
  CHECK(g.getIGNILBlock(0x1140) == nullptr);
}

TEST_CASE("SwitchDetector - rolls back a case that bypasses the index guard",
          "[switch][e2e][iterative]") {
  auto bytes = nonPicDispatch();
  // Case 0 can install an out-of-bounds index and re-enter after the guard.
  const std::uint8_t bypass[] = {0xbf, 0xff, 0x00, 0x00, 0x00, // mov edi,255
                                  0xe9, 0, 0, 0, 0};
  std::copy(std::begin(bypass), std::end(bypass),
            bytes.begin() + (kCase0 - kBase));
  putLE(bytes, kCase0 + 6 - kBase, 0x1009 - (kCase0 + 10), 4);
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g{dragon::Architecture::X86_64, cfg};
  dis.disassemble(bytes, kBase, g);
  REQUIRE(g.get(0x100b));
  CHECK(g.get(0x100b)->branches().empty());
  CHECK(g.get(kCase0) == nullptr);
}

TEST_CASE("SwitchDetector - explicit entries exclude speculative function roots",
          "[switch][e2e][iterative]") {
  auto bytes = loopDispatch(false);
  // The case after a padded RET is heuristically tagged as another function
  // by linear sweep. It is reached only through the table, with R13 intact.
  bytes[0x1210 - kBase] = 0xe9;
  putLE(bytes, 0x1211 - kBase, 0x1020 - 0x1215, 4);
  putLE(bytes, kTable - kBase, 0x1210 - kTable, 4);
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::LINEAR_SWEEP;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g{dragon::Architecture::X86_64, cfg};
  dragon::Disassembler::AnalysisHints hints;
  hints.entryPoints = {kBase};
  dis.disassemble(bytes, kBase, g, hints);
  REQUIRE(g.get(0x1033));
  CHECK(g.get(0x1033)->branches() ==
        std::set<std::uint64_t>{0x1210, 0x1110, 0x1120});
}

TEST_CASE("SwitchDetector - 32-bit index writes remain bounded across joins",
          "[switch][e2e][join-width]") {
  // Like str_pack: EBX is written before a branching loop, then a 32-bit
  // guard protects a table indexed by RBX. The join must keep RBX[63:32]=0
  // while leaving its low bits unknown. A case that sets bit 32 invalidates
  // that proof even though its low 32 bits still pass the guard.
  for (auto strategy: {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                       dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    for (bool clobber: {false, true}) {
      INFO("strategy " << static_cast<int>(strategy) << " clobber " << clobber);
      std::vector<std::uint8_t> bytes(kTable - kBase + 3 * 4, 0xcc);
      auto emit = [&](std::uint64_t at,
                       std::initializer_list<std::uint8_t> code) {
        std::copy(code.begin(), code.end(), bytes.begin() + (at - kBase));
      };
      emit(0x1000, {0x4c, 0x8d, 0x2d, 0, 0, 0, 0, // lea r13,table
                    0x8b, 0x1e,                     // mov ebx,[rsi]
                    0x85, 0xff, 0x74, 0x13,          // test edi,edi; je 1020
                    0xe9, 0, 0, 0, 0});              // jmp 1040
      putLE(bytes, 3, kTable - 0x1007, 4);
      putLE(bytes, 0x100e - kBase, 0x1040 - 0x1012, 4);
      emit(0x1020, {0xe9, 0, 0, 0, 0});
      putLE(bytes, 0x1021 - kBase, 0x1040 - 0x1025, 4);
      emit(0x1040, {0x83, 0xfb, 0x02,             // cmp ebx,2
                    0x0f, 0x87, 0, 0, 0, 0,       // ja default
                    0x49, 0x63, 0x44, 0x9d, 0x00, // movsxd rax,[r13+rbx*4]
                    0x4c, 0x01, 0xe8, 0xff, 0xe0});
      putLE(bytes, 0x1045 - kBase, kDefault - 0x1049, 4);
      for (unsigned i = 0; i < 3; ++i) {
        const auto at = kCase0 + 0x10 * i;
        putLE(bytes, kTable - kBase + i * 4, at - kTable, 4);
        if (clobber && i == 1) {
          emit(at, {0x48, 0xbb, 0, 0, 0, 0, 1, 0, 0, 0, // mov rbx,1<<32
                      0xe9, 0, 0, 0, 0});
          putLE(bytes, at + 11 - kBase, 0x1040 - (at + 15), 4);
        } else {
          emit(at, {0xe9, 0, 0, 0, 0});
          putLE(bytes, at + 1 - kBase, 0x1007 - (at + 5), 4);
        }
      }
      bytes[kDefault - kBase] = 0xc3;
      dragon::Configuration cfg;
      cfg.disassemblyStrategy = strategy;
      dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
      dragon::Graph g{dragon::Architecture::X86_64, cfg};
      dis.disassemble(bytes, kBase, g);
      REQUIRE(g.get(0x1051));
      if (clobber)
        CHECK(g.get(0x1051)->branches().empty());
      else
        CHECK(g.get(0x1051)->branches() ==
              std::set<std::uint64_t>{0x1100, 0x1110, 0x1120});
    }
  }
}
