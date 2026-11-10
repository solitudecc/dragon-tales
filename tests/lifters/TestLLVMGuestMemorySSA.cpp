#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/IRBuilder.h>
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

std::size_t phiNodes(const llvm::Module &module) {
  std::size_t count = 0;
  for (const auto &function : module)
    for (const auto &block : function)
      for (const auto &instruction : block)
        if (llvm::isa<llvm::PHINode>(&instruction)) ++count;
  return count;
}

void verify(const llvm::Module &module) {
  std::string error;
  llvm::raw_string_ostream stream{error};
  const bool invalid = llvm::verifyModule(module, &stream);
  stream.flush();
  INFO(error);
  CHECK_FALSE(invalid);
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

TEST_CASE("S3 preserves Guest facts across an IR readnone call",
          "[llvm][guest-memory-ssa]") {
  llvm::LLVMContext context;
  llvm::Module module{"guest-memory-readnone", context};
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {ptr, ptr, ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                          "readnone_case", module);
  auto *pureType = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false);
  auto *pure = llvm::Function::Create(pureType, llvm::Function::ExternalLinkage,
                                      "pure_helper", module);
  pure->setDoesNotAccessMemory();
  auto args = function->arg_begin();
  ++args;
  auto *memory = &*args++;
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *data = builder.CreateLoad(ptr, memory);
  auto *slot = builder.CreateGEP(builder.getInt8Ty(), data, builder.getInt64(0x4000));
  builder.CreateStore(builder.getInt64(7), slot);
  builder.CreateCall(pure);
  builder.CreateLoad(builder.getInt64Ty(), slot);
  builder.CreateRetVoid();

  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(
      module, MemoryForwardOptions{65536, false});
  CHECK(stats.available);
  CHECK(stats.loadsForwarded >= 1);
  CHECK(integerLoads(module) == 0);
  verify(module);
}

TEST_CASE("S3 preserves Guest facts across a proven internal leaf call",
          "[llvm][guest-memory-ssa]") {
  // A one-instruction RET graph is enough to prove that the explicitly
  // annotated internal target has no Guest-memory writes.  The IR callee is
  // kept as a declaration: the proof source is the frozen disassembly graph,
  // never the function/import spelling.
  Configuration config;
  config.disassemblyStrategy = DisassemblyStrategy::LINEAR_SWEEP;
  Graph graph{Architecture::X86_64, config};
  Disassembler dis{Architecture::X86_64, config};
  const Bytes leafCode{0xc3};
  dis.disassemble(leafCode, 0x2000, graph);
  REQUIRE(Function{&graph, 0x2000}.isValid());

  llvm::LLVMContext context;
  llvm::Module module{"guest-memory-leaf-call", context};
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {ptr, ptr, ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                          "leaf_call_case", module);
  auto *callee = llvm::Function::Create(
      type, llvm::Function::ExternalLinkage, "arbitrary_internal_name", module);
  auto args = function->arg_begin();
  ++args;
  auto *memory = &*args++;
  auto *entryBlock = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entryBlock);
  auto *data = builder.CreateLoad(ptr, memory);
  auto *slot = builder.CreateGEP(builder.getInt8Ty(), data,
                                 builder.getInt64(0x4000));
  builder.CreateStore(builder.getInt64(7), slot);
  auto *call = builder.CreateCall(callee, {function->getArg(0), function->getArg(1),
                                          function->getArg(2), function->getArg(3)});
  call->setMetadata(
      "dragon.guest.call.target",
      llvm::MDNode::get(context, llvm::ConstantAsMetadata::get(
          llvm::ConstantInt::get(builder.getInt64Ty(), 0x2000))));
  builder.CreateLoad(builder.getInt64Ty(), slot);
  builder.CreateRetVoid();

  MemoryForwardOptions options;
  options.runMem2Reg = false;
  options.proveLeafCalls = true;
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(
      module, options, &graph);
  CHECK(stats.available);
  CHECK(stats.leafCallsPreserved >= 1);
  CHECK(stats.callBarriers == 0);
  CHECK(stats.loadsForwarded >= 1);
  CHECK(integerLoads(module) == 0);
  verify(module);
}

TEST_CASE("S2 keeps a partially uncovered concrete slot conservative",
          "[llvm][guest-memory-ssa]") {
  // mov rbx, 0x4000; mov rax, 7; mov [rbx], rax;
  // mov rcx, [rbx+4]; ret (bytes 4..11 are only partially covered)
  auto graph = decode({0x48,0xc7,0xc3,0x00,0x40,0x00,0x00,
                       0x48,0xc7,0xc0,0x07,0x00,0x00,0x00,
                       0x48,0x89,0x03,0x48,0x8b,0x4b,0x04,0xc3});
  auto lifter = lift(graph);
  const auto stats = lifter.runGuestMemorySSA();
  CHECK(stats.available);
  CHECK(stats.partialOverlaps >= 1);
  CHECK(stats.loadsForwarded == 0);
  verify(lifter.module());
}

TEST_CASE("S2 rebuilds a fully-covered narrower Guest load by bytes",
          "[llvm][guest-memory-ssa]") {
  // mov rbx, 0x4000; mov rax, 7; mov [rbx], rax;
  // mov ecx, [rbx+4]; ret (bytes 4..7 are fully covered)
  auto graph = decode({0x48,0xc7,0xc3,0x00,0x40,0x00,0x00,
                       0x48,0xc7,0xc0,0x07,0x00,0x00,0x00,
                       0x48,0x89,0x03,0x8b,0x4b,0x04,0xc3});
  auto lifter = lift(graph);
  const auto before = integerLoads(lifter.module());
  const auto stats = lifter.runGuestMemorySSA();
  CHECK(stats.available);
  CHECK(stats.loadsForwarded >= 1);
  CHECK(integerLoads(lifter.module()) < before);
  verify(lifter.module());
}

TEST_CASE("S2 merges differing acyclic paths with a Guest-memory PHI",
          "[llvm][guest-memory-ssa]") {
  llvm::LLVMContext context;
  llvm::Module module{"guest-memory-phi", context};
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {ptr, ptr, ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                          "phi_case", module);
  auto args = function->arg_begin();
  auto *condition = &*args++;
  auto *memory = &*args++;
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *join = llvm::BasicBlock::Create(context, "join", function);
  llvm::IRBuilder<> builder(entry);
  auto *data = builder.CreateLoad(ptr, memory);
  auto *isNull = builder.CreateIsNull(condition);
  builder.CreateCondBr(isNull, left, right);
  for (auto *block : {left, right}) {
    llvm::IRBuilder<> arm(block);
    auto *slot = arm.CreateGEP(arm.getInt8Ty(), data, arm.getInt64(0x4000));
    arm.CreateStore(arm.getInt64(block == left ? 1 : 2), slot);
    arm.CreateBr(join);
  }
  llvm::IRBuilder<> tail(join);
  auto *slot = tail.CreateGEP(tail.getInt8Ty(), data, tail.getInt64(0x4000));
  tail.CreateLoad(tail.getInt64Ty(), slot);
  tail.CreateRetVoid();

  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(
      module, MemoryForwardOptions{65536, false});
  CHECK(stats.available);
  CHECK(stats.loadsForwarded >= 1);
  CHECK(phiNodes(module) >= 1);
  CHECK(integerLoads(module) == 0);
  verify(module);
}

TEST_CASE("S2 forwards a loop-invariant Guest slot without guessing iterations",
          "[llvm][guest-memory-ssa]") {
  llvm::LLVMContext context;
  llvm::Module module{"guest-memory-loop", context};
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {ptr, ptr, ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                          "loop_case", module);
  auto args = function->arg_begin();
  auto *condition = &*args++;
  auto *memory = &*args++;
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *header = llvm::BasicBlock::Create(context, "header", function);
  auto *body = llvm::BasicBlock::Create(context, "body", function);
  auto *exit = llvm::BasicBlock::Create(context, "exit", function);
  llvm::IRBuilder<> entryBuilder(entry);
  auto *data = entryBuilder.CreateLoad(ptr, memory);
  auto *slot = entryBuilder.CreateGEP(entryBuilder.getInt8Ty(), data,
                                      entryBuilder.getInt64(0x4000));
  entryBuilder.CreateStore(entryBuilder.getInt64(1), slot);
  entryBuilder.CreateBr(header);
  llvm::IRBuilder<> headerBuilder(header);
  headerBuilder.CreateCondBr(headerBuilder.CreateIsNull(condition), body, exit);
  llvm::IRBuilder<> bodyBuilder(body);
  auto *bodySlot = bodyBuilder.CreateGEP(bodyBuilder.getInt8Ty(), data,
                                         bodyBuilder.getInt64(0x4000));
  bodyBuilder.CreateStore(bodyBuilder.getInt64(1), bodySlot);
  bodyBuilder.CreateBr(header);
  llvm::IRBuilder<> exitBuilder(exit);
  auto *exitSlot = exitBuilder.CreateGEP(exitBuilder.getInt8Ty(), data,
                                         exitBuilder.getInt64(0x4000));
  exitBuilder.CreateLoad(exitBuilder.getInt64Ty(), exitSlot);
  exitBuilder.CreateRetVoid();

  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(
      module, MemoryForwardOptions{65536, false});
  CHECK(stats.available);
  CHECK(stats.loopBarriers >= 1);
  CHECK(stats.loadsForwarded >= 1);
  CHECK(stats.phisInserted == 0);
  CHECK(integerLoads(module) == 0);
  verify(module);
}

TEST_CASE("S2 uses a loop PHI when a canonical loop changes a Guest slot",
          "[llvm][guest-memory-ssa]") {
  llvm::LLVMContext context;
  llvm::Module module{"guest-memory-loop-phi", context};
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {ptr, ptr, ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                          "loop_phi_case", module);
  auto args = function->arg_begin();
  auto *condition = &*args++;
  auto *memory = &*args++;
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *header = llvm::BasicBlock::Create(context, "header", function);
  auto *body = llvm::BasicBlock::Create(context, "body", function);
  auto *exit = llvm::BasicBlock::Create(context, "exit", function);
  llvm::IRBuilder<> entryBuilder(entry);
  auto *data = entryBuilder.CreateLoad(ptr, memory);
  auto *slot = entryBuilder.CreateGEP(entryBuilder.getInt8Ty(), data,
                                      entryBuilder.getInt64(0x4000));
  entryBuilder.CreateStore(entryBuilder.getInt64(1), slot);
  entryBuilder.CreateBr(header);
  llvm::IRBuilder<> headerBuilder(header);
  headerBuilder.CreateCondBr(headerBuilder.CreateIsNull(condition), body, exit);
  llvm::IRBuilder<> bodyBuilder(body);
  auto *bodySlot = bodyBuilder.CreateGEP(bodyBuilder.getInt8Ty(), data,
                                         bodyBuilder.getInt64(0x4000));
  bodyBuilder.CreateStore(bodyBuilder.getInt64(2), bodySlot);
  bodyBuilder.CreateBr(header);
  llvm::IRBuilder<> exitBuilder(exit);
  auto *exitSlot = exitBuilder.CreateGEP(exitBuilder.getInt8Ty(), data,
                                         exitBuilder.getInt64(0x4000));
  exitBuilder.CreateLoad(exitBuilder.getInt64Ty(), exitSlot);
  exitBuilder.CreateRetVoid();

  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(
      module, MemoryForwardOptions{65536, false});
  CHECK(stats.available);
  CHECK(stats.phisInserted >= 1);
  CHECK(stats.loadsForwarded >= 1);
  CHECK(integerLoads(module) == 0);
  verify(module);
}
