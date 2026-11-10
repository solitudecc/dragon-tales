#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t entry = 0x1000;

Graph decode(const Bytes &bytes) {
  Configuration config;
  config.disassemblyStrategy = DisassemblyStrategy::LINEAR_SWEEP;
  Graph graph{Architecture::X86_64, config};
  Disassembler dis{Architecture::X86_64, config};
  dis.disassemble(bytes, entry, graph);
  return graph;
}

LLVMLifter lift(Graph &graph) {
  const Function function{&graph, entry};
  REQUIRE(function.isValid());
  LLVMLifter lifter{Architecture::X86_64};
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
  REQUIRE(lifter.lift(function, graph));
  return lifter;
}

std::size_t integerLoads(const llvm::Module &module) {
  std::size_t count = 0;
  for (const auto &function : module)
    for (const auto &block : function)
      for (const auto &instruction : block)
        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
          if (load->getType()->isIntegerTy()) ++count;
  return count;
}

void verify(const llvm::Module &module) {
  std::string error;
  llvm::raw_string_ostream stream{error};
  INFO(error);
  CHECK_FALSE(llvm::verifyModule(module, &stream));
}
} // namespace

TEST_CASE("S2 forwards exact Guest stores and reuses exact Guest loads",
          "[llvm][guest-memory-ssa]") {
  // mov rbx, 0x4000; mov rax, 7; mov [rbx], rax;
  // mov rcx, [rbx]; mov rdx, [rbx]; ret
  auto graph = decode({0x48,0xc7,0xc3,0x00,0x40,0x00,0x00,
                       0x48,0xc7,0xc0,0x07,0x00,0x00,0x00,
                       0x48,0x89,0x03,0x48,0x8b,0x0b,
                       0x48,0x8b,0x13,0xc3});
  auto lifter = lift(graph);
  lifter.runMem2Reg();
  const auto before = integerLoads(lifter.module());
  const auto stats = lifter.runGuestMemorySSA({65536, false});
  const auto after = integerLoads(lifter.module());
  CHECK(stats.available);
  CHECK(stats.complete);
  CHECK(stats.guestStores >= 1);
  CHECK(stats.guestLoads >= 2);
  CHECK(stats.loadsForwarded >= 2);
  CHECK(after < before);
  verify(lifter.module());
}

TEST_CASE("S2 reuses a prior exact Guest load when no store intervenes",
          "[llvm][guest-memory-ssa]") {
  auto graph = decode({0x48,0xc7,0xc3,0x00,0x40,0x00,0x00,
                       0x48,0x8b,0x0b,0x48,0x8b,0x13,0xc3});
  auto lifter = lift(graph);
  const auto stats = lifter.runGuestMemorySSA();
  CHECK(stats.loadsReused >= 1);
  CHECK(stats.loadsForwarded >= 1);
  verify(lifter.module());
}

TEST_CASE("S2 does not forward across a call or an overlapping partial write",
          "[llvm][guest-memory-ssa]") {
  // mov rbx, 0x4000; mov rax, 7; mov [rbx], rax;
  // call +0; mov rcx, [rbx]; ret
  auto graph = decode({0x48,0xc7,0xc3,0x00,0x40,0x00,0x00,
                       0x48,0xc7,0xc0,0x07,0x00,0x00,0x00,
                       0x48,0x89,0x03,0xe8,0x00,0x00,0x00,0x00,
                       0x48,0x8b,0x0b,0xc3});
  auto lifter = lift(graph);
  const auto stats = lifter.runGuestMemorySSA();
  CHECK(stats.available);
  CHECK(stats.callBarriers >= 1);
  CHECK(stats.loadsForwarded == 0);
  verify(lifter.module());
}

TEST_CASE("S2 treats overlapping concrete slots as an alias barrier",
          "[llvm][guest-memory-ssa]") {
  // mov rbx, 0x4000; mov rax, 7; mov [rbx], rax;
  // mov ecx, [rbx+4]; ret
  auto graph = decode({0x48,0xc7,0xc3,0x00,0x40,0x00,0x00,
                       0x48,0xc7,0xc0,0x07,0x00,0x00,0x00,
                       0x48,0x89,0x03,0x8b,0x4b,0x04,0xc3});
  auto lifter = lift(graph);
  const auto stats = lifter.runGuestMemorySSA();
  CHECK(stats.available);
  CHECK(stats.partialOverlaps >= 1);
  CHECK(stats.loadsForwarded == 0);
  verify(lifter.module());
}
