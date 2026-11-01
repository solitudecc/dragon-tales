#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/ArgumentRecovery.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Function.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>

#include <cstdint>
#include <vector>

using namespace dragon;
namespace x86 = dragon::x86;

namespace {

// Disassemble x86-64 bytes at 0x1000 into a fully analysed graph:
// instructions, blocks, functions and IGNIL.
Graph disassemble(const std::vector<std::uint8_t> &bytes) {
  Configuration cfg;
  Disassembler dis(Architecture::X86_64, cfg);
  Graph g(Architecture::X86_64, cfg);
  dis.disassemble(bytes, 0x1000, g);
  return g;
}

// Every function below opens with the push rbp / mov rbp, rsp prologue the
// x86-64 function-start heuristic requires.
const std::uint8_t kProlog[] = {0x55, 0x48, 0x89, 0xE5};

std::vector<std::uint8_t> withProlog(std::vector<std::uint8_t> body) {
  std::vector<std::uint8_t> out(std::begin(kProlog), std::end(kProlog));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

const analysis::RecoveredArgument *
argAt(const analysis::FunctionSignature &sig, unsigned index) {
  for (const auto &a : sig.arguments)
    if (a.index == index)
      return &a;
  return nullptr;
}

} // namespace

// -----------------------------------------------------------------------
// Two integer arguments, returns a value
// -----------------------------------------------------------------------

TEST_CASE("ArgumentRecovery - two int args in RDI/RSI, returns via RAX",
          "[argrec][ignil]") {
  //   mov eax, edi      89 f8
  //   add eax, esi      01 f0
  //   pop rbp ; ret     5d c3
  Graph g = disassemble(withProlog({0x89, 0xF8, 0x01, 0xF0, 0x5D, 0xC3}));
  Function fn(&g, 0x1000);
  REQUIRE(fn.isValid());

  SECTION("liveness exposes the argument registers, not the return register") {
    auto liveIn = analysis::liveInRegisters(fn, g);
    CHECK(liveIn.count(x86::kOffRDI) == 1);
    CHECK(liveIn.count(x86::kOffRSI) == 1);
    // EAX is written before it is read, so RAX is not an input.
    CHECK(liveIn.count(x86::kOffRAX) == 0);
  }

  auto sig = analysis::recoverSignature(fn, g, Architecture::X86_64);
  REQUIRE(sig.arguments.size() == 2);
  CHECK(argAt(sig, 0)->reg.offset == x86::kOffRDI);
  CHECK(argAt(sig, 1)->reg.offset == x86::kOffRSI);
  CHECK(argAt(sig, 0)->reg.width == 32); // read as edi -> i32
  REQUIRE_FALSE(sig.returnsVoid());
  CHECK(sig.returnValue->offset == x86::kOffRAX);

  CHECK(analysis::toString(sig, 0x1000) ==
        "i32 sub_1000(i32 RDI, i32 RSI)");
}

// -----------------------------------------------------------------------
// One argument, no return value (void)
// -----------------------------------------------------------------------

TEST_CASE("ArgumentRecovery - one arg, writes memory, returns void",
          "[argrec][ignil]") {
  //   mov [rbp-4], edi   89 7d fc
  //   pop rbp ; ret      5d c3
  Graph g = disassemble(withProlog({0x89, 0x7D, 0xFC, 0x5D, 0xC3}));
  Function fn(&g, 0x1000);
  REQUIRE(fn.isValid());

  auto sig = analysis::recoverSignature(fn, g, Architecture::X86_64);
  REQUIRE(sig.arguments.size() == 1);
  CHECK(argAt(sig, 0)->reg.offset == x86::kOffRDI);
  CHECK(sig.returnsVoid()); // RAX never written
  CHECK(analysis::toString(sig, 0x1000) == "void sub_1000(i32 RDI)");
}

// -----------------------------------------------------------------------
// No arguments, returns a constant
// -----------------------------------------------------------------------

TEST_CASE("ArgumentRecovery - no args, returns a constant", "[argrec][ignil]") {
  //   mov eax, 7        b8 07 00 00 00
  //   pop rbp ; ret     5d c3
  Graph g =
          disassemble(withProlog({0xB8, 0x07, 0x00, 0x00, 0x00, 0x5D, 0xC3}));
  Function fn(&g, 0x1000);
  REQUIRE(fn.isValid());

  auto sig = analysis::recoverSignature(fn, g, Architecture::X86_64);
  CHECK(sig.arguments.empty());
  REQUIRE_FALSE(sig.returnsVoid());
  CHECK(sig.returnValue->offset == x86::kOffRAX);
  CHECK(analysis::toString(sig, 0x1000) == "i32 sub_1000(void)");
}

// -----------------------------------------------------------------------
// Three arguments, recovered across a branch (exercises the dataflow)
// -----------------------------------------------------------------------

TEST_CASE("ArgumentRecovery - three args found across two paths",
          "[argrec][ignil]") {
  //   cmp edi, 10       83 ff 0a
  //   jle L             7e 04
  //   mov eax, esi      89 f0       (uses RSI on the fall-through path)
  //   jmp E             eb 02
  // L:mov eax, edx      89 d0       (uses RDX on the taken path)
  // E:pop rbp ; ret     5d c3
  Graph g = disassemble(withProlog({0x83, 0xFF, 0x0A, 0x7E, 0x04, 0x89, 0xF0,
                                    0xEB, 0x02, 0x89, 0xD0, 0x5D, 0xC3}));
  Function fn(&g, 0x1000);
  REQUIRE(fn.isValid());
  REQUIRE(fn.numBlocks() >= 3); // the branch really did split the CFG

  auto sig = analysis::recoverSignature(fn, g, Architecture::X86_64);
  REQUIRE(sig.arguments.size() == 3);
  CHECK(argAt(sig, 0)->reg.offset == x86::kOffRDI);
  CHECK(argAt(sig, 1)->reg.offset == x86::kOffRSI);
  CHECK(argAt(sig, 2)->reg.offset == x86::kOffRDX);
  REQUIRE_FALSE(sig.returnsVoid());
}

// -----------------------------------------------------------------------
// The lifter records the recovered prototype as a function attribute
// -----------------------------------------------------------------------

TEST_CASE("ArgumentRecovery - LLVMLifter records a dragon.signature attribute",
          "[argrec][llvm]") {
  Graph g = disassemble(withProlog({0x89, 0xF8, 0x01, 0xF0, 0x5D, 0xC3}));
  Function fn(&g, 0x1000);
  REQUIRE(fn.isValid());

  llvmlifter::LLVMLifter lifter(Architecture::X86_64, "argrec");
  REQUIRE(lifter.lift(fn, g));

  CHECK(lifter.recoveredSignatureString(fn, g) ==
        "i32 sub_1000(i32 RDI, i32 RSI)");
  const std::string ir = lifter.ir();
  CHECK(ir.find("\"dragon.signature\"=\"i32 sub_1000(i32 RDI, i32 RSI)\"") !=
        std::string::npos);
}

// -----------------------------------------------------------------------
// Microsoft x64: arguments in RCX, RDX, R8, R9.
//
// The x86-64 convention was hard-wired to System V, so a Windows function
// taking (a, b) in RCX/RDX was reported with four arguments RDI..RCX.
// -----------------------------------------------------------------------

TEST_CASE("ArgumentRecovery - Win64 convention reads RCX/RDX",
          "[analysis][signature]") {
  // push rbp; mov rbp, rsp; mov eax, ecx; add eax, edx; pop rbp; ret
  const auto bytes = withProlog({0x89, 0xC8, 0x01, 0xD0, 0x5D, 0xC3});
  auto sigFor = [&](CallingConvention cc) {
    Configuration cfg;
    cfg.callingConvention = cc;
    Disassembler dis(Architecture::X86_64, cfg);
    Graph g(Architecture::X86_64, cfg);
    dis.disassemble(bytes, 0x1000, g);
    const Function fn{&g, 0x1000};
    REQUIRE(fn.isValid());
    return analysis::recoverSignature(fn, g, Architecture::X86_64);
  };

  auto win = sigFor(CallingConvention::Win64);
  REQUIRE(win.arguments.size() == 2);
  CHECK(win.arguments[0].reg.offset == x86::RCX.offset);
  CHECK(win.arguments[1].reg.offset == x86::RDX.offset);

  // Read as System V the same body is RDX (#2) and RCX (#3): four arguments.
  CHECK(sigFor(CallingConvention::Default).arguments.size() == 4);
}
