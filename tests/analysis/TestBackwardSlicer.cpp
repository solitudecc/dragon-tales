#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/disassembler/Disassembler.hpp>

#include <memory>
#include <optional>

#include <iostream>

namespace x86 = dragon::x86;

// ---------------------------------------------------------------------------
// Helper: register an IGNIL block and mark it valid in the graph.
// ---------------------------------------------------------------------------

static void addValidBlock(dragon::Graph &g, dragon::ignil::Block block) {
  std::uint64_t addr = block.startAddress();
  g.addIGNILBlock(std::move(block));
  g.addBlock(addr);
}

// ---------------------------------------------------------------------------
// Test 1: constant folded through a single COPY in one block
// ---------------------------------------------------------------------------

TEST_CASE("BackwardSlicer - constant propagation through COPY",
          "[slicer][ignil]") {
  constexpr std::uint64_t baseAddr = 0x1000;
  dragon::ignil::Block block{baseAddr};

  // COPY(rax, 42:i64)  ← simulates  mov rax, 42
  block.append({dragon::ignil::Opcode::COPY,
                dragon::ignil::Value{x86::RAX},
                {dragon::ignil::Value{dragon::ignil::Const{42, 64}}},
                baseAddr});
  block.append({dragon::ignil::Opcode::RET, std::nullopt, {}, baseAddr + 7});

  dragon::Graph g{dragon::Architecture::X86_64, dragon::Configuration{}};
  addValidBlock(g, std::move(block));

  dragon::BackwardSlicer slicer;
  dragon::slice::Expr result =
      slicer.slice(dragon::ignil::Value{x86::RAX}, baseAddr, g);

  std::cout << "Slice result: " << dragon::slice::toString(result) << "\n";

  REQUIRE(std::holds_alternative<dragon::slice::Const>(result.data));
  const auto &c = std::get<dragon::slice::Const>(result.data);
  REQUIRE(c.value == 42);
  REQUIRE(c.width == 64);
}

// ---------------------------------------------------------------------------
// Test 2: ADD expression - result is represented as BinOp(ADD, rax, rbx)
// ---------------------------------------------------------------------------

TEST_CASE("BackwardSlicer - ADD produces BinOp expression", "[slicer][ignil]") {
  constexpr std::uint64_t baseAddr = 0x2000;
  dragon::ignil::Block block{baseAddr};

  // t0 = ADD(rax, rbx)
  dragon::ignil::Temp t0{0, 64};
  block.append({dragon::ignil::Opcode::ADD,
                dragon::ignil::Value{t0},
                {dragon::ignil::Value{x86::RAX}, dragon::ignil::Value{x86::RBX}},
                baseAddr});
  // rax = COPY(t0)
  block.append({dragon::ignil::Opcode::COPY,
                dragon::ignil::Value{x86::RAX},
                {dragon::ignil::Value{t0}},
                baseAddr + 3});
  block.append({dragon::ignil::Opcode::RET, std::nullopt, {}, baseAddr + 6});

  dragon::Graph g{dragon::Architecture::X86_64, dragon::Configuration{}};
  addValidBlock(g, std::move(block));

  dragon::BackwardSlicer slicer;
  dragon::slice::Expr result =
      slicer.slice(dragon::ignil::Value{x86::RAX}, baseAddr, g);

  std::cout << "Slice result: " << dragon::slice::toString(result) << "\n";

  // Should be BinOp{ADD, SymReg{rax}, SymReg{rbx}}
  REQUIRE(std::holds_alternative<dragon::slice::BinOp>(result.data));
  const auto &b = std::get<dragon::slice::BinOp>(result.data);
  REQUIRE(b.op == dragon::ignil::Opcode::ADD);
  REQUIRE(std::holds_alternative<dragon::slice::SymReg>(b.left->data));
  REQUIRE(std::holds_alternative<dragon::slice::SymReg>(b.right->data));
  REQUIRE(std::get<dragon::slice::SymReg>(b.left->data).reg.offset ==
          x86::RAX.offset);
  REQUIRE(std::get<dragon::slice::SymReg>(b.right->data).reg.offset ==
          x86::RBX.offset);
}

// ---------------------------------------------------------------------------
// Test 3: cross-block propagation - rax defined in predecessor block
// ---------------------------------------------------------------------------

TEST_CASE("BackwardSlicer - cross-block constant propagation",
          "[slicer][ignil]") {
  // Block A (0x3000): COPY(rax, 99); JMP(0x3010)
  // Block B (0x3010): RET            <-- slice rax here
  constexpr std::uint64_t addrA = 0x3000;
  constexpr std::uint64_t addrB = 0x3010;

  dragon::ignil::Block blockA{addrA};
  blockA.append({dragon::ignil::Opcode::COPY,
                 dragon::ignil::Value{x86::RAX},
                 {dragon::ignil::Value{dragon::ignil::Const{99, 64}}},
                 addrA});
  blockA.append({dragon::ignil::Opcode::JMP,
                 std::nullopt,
                 {dragon::ignil::Value{dragon::ignil::Const{addrB, 64}}},
                 addrA + 7});

  dragon::ignil::Block blockB{addrB};
  blockB.append({dragon::ignil::Opcode::RET, std::nullopt, {}, addrB});

  dragon::Graph g{dragon::Architecture::X86_64, dragon::Configuration{}};
  addValidBlock(g, std::move(blockA));
  addValidBlock(g, std::move(blockB));

  dragon::BackwardSlicer slicer{5};
  dragon::slice::Expr result =
      slicer.slice(dragon::ignil::Value{x86::RAX}, addrB, g);

  std::cout << "Slice result (cross-block): " << dragon::slice::toString(result)
            << "\n";

  REQUIRE(std::holds_alternative<dragon::slice::Const>(result.data));
  const auto &c = std::get<dragon::slice::Const>(result.data);
  REQUIRE(c.value == 99);
  REQUIRE(c.width == 64);
}

// ---------------------------------------------------------------------------
// Test 4: join node stops propagation - rax left as SymReg
// ---------------------------------------------------------------------------

TEST_CASE("BackwardSlicer - join node stops at SymReg",
          "[slicer][ignil]") {
  // Block A (0x4000): JMP(0x4020)
  // Block B (0x4010): JMP(0x4020)
  // Block C (0x4020): RET  <-- slice rax here; has two predecessors → join
  constexpr std::uint64_t addrA = 0x4000;
  constexpr std::uint64_t addrB = 0x4010;
  constexpr std::uint64_t addrC = 0x4020;

  dragon::ignil::Block blockA{addrA};
  blockA.append({dragon::ignil::Opcode::COPY,
                 dragon::ignil::Value{x86::RAX},
                 {dragon::ignil::Value{dragon::ignil::Const{1, 64}}},
                 addrA});
  blockA.append({dragon::ignil::Opcode::JMP,
                 std::nullopt,
                 {dragon::ignil::Value{dragon::ignil::Const{addrC, 64}}},
                 addrA + 7});

  dragon::ignil::Block blockB{addrB};
  blockB.append({dragon::ignil::Opcode::COPY,
                 dragon::ignil::Value{x86::RAX},
                 {dragon::ignil::Value{dragon::ignil::Const{2, 64}}},
                 addrB});
  blockB.append({dragon::ignil::Opcode::JMP,
                 std::nullopt,
                 {dragon::ignil::Value{dragon::ignil::Const{addrC, 64}}},
                 addrB + 7});

  dragon::ignil::Block blockC{addrC};
  blockC.append({dragon::ignil::Opcode::RET, std::nullopt, {}, addrC});

  dragon::Graph g{dragon::Architecture::X86_64, dragon::Configuration{}};
  addValidBlock(g, std::move(blockA));
  addValidBlock(g, std::move(blockB));
  addValidBlock(g, std::move(blockC));

  dragon::BackwardSlicer slicer{5};
  dragon::slice::Expr result =
      slicer.slice(dragon::ignil::Value{x86::RAX}, addrC, g);

  std::cout << "Slice result (join): " << dragon::slice::toString(result) << "\n";

  // C has two predecessors → join node → SymReg, not a concrete constant.
  REQUIRE(std::holds_alternative<dragon::slice::SymReg>(result.data));
  REQUIRE(std::get<dragon::slice::SymReg>(result.data).reg.offset ==
          x86::RAX.offset);
}

// ---------------------------------------------------------------------------
// Sub-register aliasing on real lifter output.
//
// The register space is byte-addressed: RAX/EAX/AX/AL share offset 0, AH is
// byte 1. A write to any of them is a definition of the others. These used to
// be skipped (only an exact offset+width match counted), so the slicer
// answered with an *older* value - `mov rax, 0x100; mov eax, 5` sliced RAX to
// 0x100.
// ---------------------------------------------------------------------------

namespace {

// Concrete value of a slice made only of constants, or nullopt.
std::optional<std::uint64_t> evalConst(const dragon::slice::Expr &e) {
  using namespace dragon::slice;
  using Opc = dragon::ignil::Opcode;
  auto mask = [](std::uint64_t v, unsigned w) {
    return w >= 64 ? v : v & ((std::uint64_t{1} << w) - 1);
  };
  if (const auto *c = std::get_if<Const>(&e.data))
    return mask(c->value, c->width);
  if (const auto *u = std::get_if<UnaryOp>(&e.data)) {
    auto v = evalConst(*u->operand);
    if (!v)
      return std::nullopt;
    if (u->op == Opc::ZEXT || u->op == Opc::TRUNC)
      return mask(*v, u->resultWidth);
    return std::nullopt;
  }
  if (const auto *b = std::get_if<BinOp>(&e.data)) {
    auto l = evalConst(*b->left), r = evalConst(*b->right);
    if (!l || !r)
      return std::nullopt;
    switch (b->op) {
      case Opc::AND: return *l & *r;
      case Opc::OR:  return *l | *r;
      case Opc::ADD: return *l + *r;
      case Opc::SHL: return *r >= 64 ? 0 : *l << *r;
      case Opc::SHR: return *r >= 64 ? 0 : *l >> *r;
      default:       return std::nullopt;
    }
  }
  return std::nullopt;
}

// Lift `code` at 0x1000 and slice `reg` at the end of the entry block.
dragon::slice::Expr sliceEntry(dragon::Architecture arch,
                               const std::vector<std::uint8_t> &code,
                               const dragon::ignil::Reg &reg) {
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
  dragon::Disassembler dis(arch, cfg);
  dragon::Graph g{arch, cfg};
  dis.disassemble(code, 0x1000, g);
  return dragon::BackwardSlicer{5}.slice(dragon::ignil::Value{reg}, 0x1000, g);
}

const std::vector<std::uint8_t> kMovRax12345678 = {0x48, 0xc7, 0xc0, 0x78,
                                                   0x56, 0x34, 0x12};

std::vector<std::uint8_t> withRaxPrefix(std::vector<std::uint8_t> tail) {
  std::vector<std::uint8_t> code = kMovRax12345678;
  code.insert(code.end(), tail.begin(), tail.end());
  code.push_back(0xc3); // ret
  return code;
}

} // namespace

TEST_CASE("BackwardSlicer - sub-register writes define the full register",
          "[slicer][ignil]") {
  using dragon::Architecture;
  struct Case {
    const char *what;
    std::vector<std::uint8_t> tail;
    std::uint64_t rax;
  };
  const Case cases[] = {
          {"mov eax, 5 (zero-extends)", {0xb8, 0x05, 0x00, 0x00, 0x00}, 0x5},
          {"mov ax, 5 (merges)", {0x66, 0xb8, 0x05, 0x00}, 0x12340005},
          {"mov al, 5 (merges)", {0xb0, 0x05}, 0x12345605},
          {"mov ah, 0x99 (merges byte 1)", {0xb4, 0x99}, 0x12349978},
          {"mov al, 5; mov ah, 0x99", {0xb0, 0x05, 0xb4, 0x99}, 0x12349905},
  };
  for (const Case &c: cases) {
    INFO(c.what);
    auto e = sliceEntry(Architecture::X86_64, withRaxPrefix(c.tail), x86::RAX);
    INFO(dragon::slice::toString(e));
    auto v = evalConst(e);
    REQUIRE(v.has_value());
    CHECK(*v == c.rax);
  }
}

TEST_CASE("BackwardSlicer - narrow reads of a wider write", "[slicer][ignil]") {
  using dragon::Architecture;
  const auto code = withRaxPrefix({});
  struct Case {
    dragon::ignil::Reg reg;
    std::uint64_t value;
  };
  const Case cases[] = {{x86::EAX, 0x12345678},
                        {x86::AX, 0x5678},
                        {x86::AL, 0x78},
                        {x86::AH, 0x56}};
  for (const Case &c: cases) {
    INFO(c.reg.name);
    auto e = sliceEntry(Architecture::X86_64, code, c.reg);
    INFO(dragon::slice::toString(e));
    auto v = evalConst(e);
    REQUIRE(v.has_value());
    CHECK(*v == c.value);
  }
}

TEST_CASE("BackwardSlicer - partial write over an unknown register stays "
          "symbolic",
          "[slicer][ignil]") {
  // mov al, 5 ; ret - the upper 56 bits of RAX are the caller's.
  auto e = sliceEntry(dragon::Architecture::X86_64, {0xb0, 0x05, 0xc3},
                      x86::RAX);
  INFO(dragon::slice::toString(e));
  CHECK_FALSE(evalConst(e).has_value());
  CHECK(dragon::slice::toString(e).find("RAX") != std::string::npos);
}

TEST_CASE("BackwardSlicer - AArch64 W write zero-extends into X",
          "[slicer][ignil]") {
  // movn x0, #0 (x0 = -1) ; mov w0, #5 ; ret
  const std::vector<std::uint8_t> code = {0x00, 0x00, 0x80, 0x92, //
                                          0xa0, 0x00, 0x80, 0x52, //
                                          0xc0, 0x03, 0x5f, 0xd6};
  auto e = sliceEntry(dragon::Architecture::ARM64, code,
                      dragon::ignil::Reg{dragon::arm::X0.offset, 64, "X0"});
  INFO(dragon::slice::toString(e));
  auto v = evalConst(e);
  REQUIRE(v.has_value());
  CHECK(*v == 5);
}

// ---------------------------------------------------------------------------
// Join nodes: merged when every path gives the same constant.
// ---------------------------------------------------------------------------

namespace {
namespace ig = dragon::ignil;
ig::Op copyOp(const ig::Reg &r, ig::Value v, std::uint64_t at) {
  return {ig::Opcode::COPY, ig::Value{r}, {std::move(v)}, at};
}
ig::Op jmpOp(std::uint64_t to, std::uint64_t at) {
  return {ig::Opcode::JMP, std::nullopt, {ig::Value{ig::Const{to, 64}}}, at};
}
ig::Op cjmpOp(std::uint64_t taken, std::uint64_t fall, std::uint64_t at) {
  return {ig::Opcode::CJMP,
          std::nullopt,
          {ig::Value{x86::ZF}, ig::Value{ig::Const{taken, 64}},
           ig::Value{ig::Const{fall, 64}}},
          at};
}
ig::Op retOp(std::uint64_t at) { return {ig::Opcode::RET, std::nullopt, {}, at}; }
} // namespace

TEST_CASE("BackwardSlicer - a join where every path sets the same constant",
          "[slicer][ignil][join]") {
  // A: cjmp B, C ; B: rax = 5 ; jmp D ; C: rax = 5 ; jmp D ; D: ret
  ig::Block a{0x4000}, b{0x4010}, c{0x4020}, d{0x4030};
  a.append(cjmpOp(0x4010, 0x4020, 0x4000));
  b.append(copyOp(x86::RAX, ig::Const{5, 64}, 0x4010));
  b.append(jmpOp(0x4030, 0x4017));
  c.append(copyOp(x86::RAX, ig::Const{5, 64}, 0x4020));
  c.append(jmpOp(0x4030, 0x4027));
  d.append(retOp(0x4030));
  dragon::Graph g{dragon::Architecture::X86_64, dragon::Configuration{}};
  for (auto *blk: {&a, &b, &c, &d})
    addValidBlock(g, std::move(*blk));
  auto e = dragon::BackwardSlicer{5}.slice(ig::Value{x86::RAX}, 0x4030, g);
  INFO(dragon::slice::toString(e));
  REQUIRE(std::holds_alternative<dragon::slice::Const>(e.data));
  CHECK(std::get<dragon::slice::Const>(e.data).value == 5);
}

TEST_CASE("BackwardSlicer - a value set before a loop is seen inside it",
          "[slicer][ignil][join]") {
  // P: rdx = 0x9000 ; jmp H
  // H: rax = rax + 1 (or rdx = rdx + 8) ; cjmp H, E
  // E: ret                  - slice rdx at E
  auto build = [](bool loopWritesRdx) {
    ig::Block p{0x5000}, h{0x5010}, e{0x5020};
    p.append(copyOp(x86::RDX, ig::Const{0x9000, 64}, 0x5000));
    p.append(jmpOp(0x5010, 0x5007));
    const ig::Reg &w = loopWritesRdx ? x86::RDX : x86::RAX;
    h.append({ig::Opcode::ADD, ig::Value{w},
              {ig::Value{w}, ig::Value{ig::Const{8, 64}}}, 0x5010});
    h.append(cjmpOp(0x5010, 0x5020, 0x5014));
    e.append(retOp(0x5020));
    auto g = std::make_unique<dragon::Graph>(dragon::Architecture::X86_64,
                                             dragon::Configuration{});
    for (auto *blk: {&p, &h, &e})
      addValidBlock(*g, std::move(*blk));
    return g;
  };
  {
    auto g = build(false);
    auto e = dragon::BackwardSlicer{5}.slice(ig::Value{x86::RDX}, 0x5020, *g);
    INFO(dragon::slice::toString(e));
    REQUIRE(std::holds_alternative<dragon::slice::Const>(e.data));
    CHECK(std::get<dragon::slice::Const>(e.data).value == 0x9000);
  }
  {
    // The loop changes rdx each time round: no single value - the slice is
    // ADD(RDX at the loop head, 8), the head's value left unknown.
    auto g = build(true);
    auto e = dragon::BackwardSlicer{5}.slice(ig::Value{x86::RDX}, 0x5020, *g);
    const std::string text = dragon::slice::toString(e);
    INFO(text);
    CHECK_FALSE(std::holds_alternative<dragon::slice::Const>(e.data));
    CHECK(text.find("0x9000") == std::string::npos);
  }
}
