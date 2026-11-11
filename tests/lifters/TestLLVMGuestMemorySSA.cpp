#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/CallEffectSummary.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
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

enum class IrreducibleAccess {
  Exact, Narrow, UnknownCall, UnknownAlias, Fence, AtomicWrite,
  VolatileLoad, VolatileStore, Partial, OtherGuestBase, CrossBlock,
};

// Both cycle headers have an independent entry edge. Neither header dominates
// the other, so the predecessor scheduler cannot treat either edge as a natural
// backedge. The fourth argument chooses whether to continue around the cycle.
void irreducibleMemory(llvm::Module &module, IrreducibleAccess access) {
  auto &context = module.getContext();
  module.setDataLayout("e-p:64:64");
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                      {ptr, ptr, i1, i1}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                         "irreducible_memory", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *exit = llvm::BasicBlock::Create(context, "exit", function);
  llvm::IRBuilder<> builder(entry);
  auto *data = builder.CreateLoad(ptr, function->getArg(1));
  auto *otherData = access == IrreducibleAccess::OtherGuestBase
      ? builder.CreateLoad(ptr, function->getArg(1)) : nullptr;
  auto *slot = builder.CreateGEP(builder.getInt8Ty(), data, builder.getInt64(8));
  if (access == IrreducibleAccess::CrossBlock)
    builder.CreateStore(builder.getInt64(1), slot);
  builder.CreateCondBr(function->getArg(2), left, right);
  for (unsigned index = 0; index != 2; ++index) {
    builder.SetInsertPoint(index ? right : left);
    const auto value = index ? 0x8877665544332211ULL : 0x1122334455667788ULL;
    if (access != IrreducibleAccess::CrossBlock || !index) {
      auto *store = access == IrreducibleAccess::Partial
          ? builder.CreateStore(builder.getInt8(0x88), slot)
          : builder.CreateStore(builder.getInt64(value), slot);
      if (access == IrreducibleAccess::VolatileStore) store->setVolatile(true);
    }
    switch (access) {
    case IrreducibleAccess::UnknownCall: {
      auto callee = module.getOrInsertFunction(
          "unknown_writer", llvm::FunctionType::get(builder.getVoidTy(),
                                                     {ptr}, false));
      builder.CreateCall(callee, {data});
      break;
    }
    case IrreducibleAccess::UnknownAlias:
      builder.CreateStore(builder.getInt64(3), function->getArg(0));
      break;
    case IrreducibleAccess::Fence:
      builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case IrreducibleAccess::AtomicWrite:
      builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, slot, builder.getInt64(1),
                              llvm::MaybeAlign(1),
                              llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case IrreducibleAccess::OtherGuestBase: {
      auto *otherSlot = builder.CreateGEP(builder.getInt8Ty(), otherData,
                                          builder.getInt64(8));
      builder.CreateStore(builder.getInt64(0), otherSlot);
      break;
    }
    default: break;
    }
    if (access != IrreducibleAccess::CrossBlock || index) {
      const bool narrow = access == IrreducibleAccess::Narrow ||
                          access == IrreducibleAccess::OtherGuestBase;
      auto *address = narrow
          ? builder.CreateGEP(builder.getInt8Ty(), data, builder.getInt64(11))
          : slot;
      auto *load = builder.CreateLoad(narrow ? builder.getInt16Ty()
                                             : builder.getInt64Ty(), address);
      if (access == IrreducibleAccess::VolatileLoad) load->setVolatile(true);
    }
    builder.CreateCondBr(function->getArg(3), index ? left : right, exit);
  }
  builder.SetInsertPoint(exit);
  builder.CreateRetVoid();
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

TEST_CASE("S2 canonicalizes nested typed and selected Guest addresses",
          "[llvm][guest-memory-ssa]") {
  llvm::LLVMContext context;
  llvm::Module module{"guest-memory-address-keys", context};
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                       {i1, ptr, ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                          "address_keys", module);
  auto args = function->arg_begin();
  ++args;
  auto *memory = &*args++;
  auto *condition = &*function->arg_begin();
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::IRBuilder<> builder(entry);
  auto *data = builder.CreateLoad(ptr, memory);

  // The first store uses a typed GEP. Its index is in i64 elements, so the
  // actual Guest byte offset is 8, not the literal index 1.
  auto *typedStore = builder.CreateGEP(builder.getInt64Ty(), data,
                                       builder.getInt64(1));
  builder.CreateStore(builder.getInt64(9), typedStore);
  auto *byteLoad = builder.CreateGEP(builder.getInt8Ty(), data,
                                     builder.getInt64(8));
  builder.CreateLoad(builder.getInt64Ty(), byteLoad);

  // The second store is behind a nested constant GEP. Both paths name the
  // same Guest slot after recursively accumulating their byte offsets.
  auto *nestedBase = builder.CreateGEP(builder.getInt8Ty(), data,
                                       builder.getInt64(0x4000));
  auto *nestedStore = builder.CreateGEP(builder.getInt64Ty(), nestedBase,
                                        builder.getInt64(0));
  builder.CreateStore(builder.getInt64(11), nestedStore);
  auto *directLoad = builder.CreateGEP(builder.getInt8Ty(), data,
                                       builder.getInt64(0x4000));
  builder.CreateLoad(builder.getInt64Ty(), directLoad);

  // Both select arms are the same proven Guest address. The condition itself
  // remains unknown; only the address relation is used.
  auto *left = builder.CreateGEP(builder.getInt8Ty(), data,
                                 builder.getInt64(0x5000));
  auto *right = builder.CreateGEP(builder.getInt8Ty(), data,
                                  builder.getInt64(0x5000));
  auto *selected = builder.CreateSelect(condition, left, right);
  builder.CreateStore(builder.getInt64(13), selected);
  auto *selectedLoad = builder.CreateGEP(builder.getInt8Ty(), data,
                                         builder.getInt64(0x5000));
  builder.CreateLoad(builder.getInt64Ty(), selectedLoad);
  builder.CreateRetVoid();

  const auto before = integerLoads(module);
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(
      module, MemoryForwardOptions{65536, false});
  CHECK(stats.available);
  CHECK(stats.complete);
  CHECK(stats.loadsForwarded >= 3);
  CHECK(integerLoads(module) < before);
  CHECK(stats.aliasBarriers == 0);
  verify(module);
}

TEST_CASE("S4 consumes explicit read-only call summaries but rejects writes",
          "[llvm][guest-memory-ssa][call-effects]") {
  auto makeModule = [](std::optional<llvmlifter::CallEffectSummary> summary) {
    auto *context = new llvm::LLVMContext();
    auto *module = new llvm::Module{"guest-memory-summary", *context};
    auto *ptr = llvm::PointerType::getUnqual(*context);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                         {ptr, ptr, ptr, ptr}, false);
    auto *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "summary_case", *module);
    auto *callee = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "unknown_summary_target", *module);
    auto args = function->arg_begin();
    ++args;
    auto *memory = &*args++;
    auto *entry = llvm::BasicBlock::Create(*context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    auto *data = builder.CreateLoad(ptr, memory);
    auto *slot = builder.CreateGEP(builder.getInt8Ty(), data,
                                   builder.getInt64(0x4000));
    builder.CreateStore(builder.getInt64(7), slot);
    auto *call = builder.CreateCall(
        callee, {function->getArg(0), function->getArg(1), function->getArg(2),
                 function->getArg(3)});
    if (summary) REQUIRE(attachCallEffectSummary(*call, *summary));
    builder.CreateLoad(builder.getInt64Ty(), slot);
    builder.CreateRetVoid();
    return std::pair{module, context};
  };

  {
    auto [module, context] = makeModule(std::nullopt);
    MemoryForwardOptions options;
    options.runMem2Reg = false;
    options.consumeCallEffectSummaries = true;
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    CHECK(stats.callBarriers >= 1);
    CHECK(stats.summaryCallsRejected == 0);
    CHECK(stats.loadsForwarded == 0);
    verify(*module);
    delete module;
    delete context;
  }

  {
    CallEffectSummary summary;
    summary.proven = true;
    summary.memory = CallMemoryEffectKind::ReadOnly;
    auto [module, context] = makeModule(summary);
    MemoryForwardOptions options;
    options.runMem2Reg = false;
    options.consumeCallEffectSummaries = true;
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    CHECK(stats.summaryCallsPreserved >= 1);
    CHECK(stats.summaryCallsRejected == 0);
    CHECK(stats.loadsForwarded >= 1);
    CHECK(integerLoads(*module) == 0);
    verify(*module);
    delete module;
    delete context;
  }

  {
    CallEffectSummary summary;
    summary.proven = true;
    summary.memory = CallMemoryEffectKind::KnownRanges;
    summary.writes.push_back(
        {CallMemoryRange::Base::GuestMemory, 0x4000, 8});
    summary.memoryConstants.push_back(
        {{CallMemoryRange::Base::GuestMemory, 0x4000, 8}, 42, true});
    auto [module, context] = makeModule(summary);
    MemoryForwardOptions options;
    options.runMem2Reg = false;
    options.consumeCallEffectSummaries = true;
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    CHECK(stats.summaryCallsPreserved >= 1);
    CHECK(stats.summaryCallsRejected == 0);
    CHECK(stats.loadsForwarded >= 1);
    CHECK(integerLoads(*module) == 0);
    verify(*module);
    delete module;
    delete context;
  }

  {
    CallEffectSummary summary;
    summary.proven = true;
    summary.memory = CallMemoryEffectKind::KnownRanges;
    summary.writes.push_back(
        {CallMemoryRange::Base::GuestMemory, 0x5000, 8});
    auto [module, context] = makeModule(summary);
    MemoryForwardOptions options;
    options.runMem2Reg = false;
    options.consumeCallEffectSummaries = true;
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    CHECK(stats.summaryCallsPreserved >= 1);
    CHECK(stats.summaryCallsRejected == 0);
    CHECK(stats.loadsForwarded >= 1);
    CHECK(integerLoads(*module) == 0);
    verify(*module);
    delete module;
    delete context;
  }
}

TEST_CASE("S4 records syscall ABI summaries without trusting unknown syscalls",
          "[llvm][guest-memory-ssa][call-effects]") {
  auto makeModule = [](bool attachSummary) {
    auto *context = new llvm::LLVMContext();
    auto *module = new llvm::Module{"guest-syscall-summary", *context};
    auto *ptr = llvm::PointerType::getUnqual(*context);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                         {ptr, ptr, ptr, ptr}, false);
    auto *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "syscall_case", *module);
    auto *callee = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "unknown_syscall", *module);
    auto args = function->arg_begin();
    ++args;
    auto *memory = &*args++;
    auto *entry = llvm::BasicBlock::Create(*context, "entry", function);
    llvm::IRBuilder<> builder(entry);
    auto *data = builder.CreateLoad(ptr, memory);
    auto *slot = builder.CreateGEP(builder.getInt8Ty(), data,
                                   builder.getInt64(0x5000));
    builder.CreateStore(builder.getInt64(9), slot);
    auto *call = builder.CreateCall(
        callee, {function->getArg(0), function->getArg(1), function->getArg(2),
                 function->getArg(3)});
    if (attachSummary) {
      CallEffectSummary summary;
      summary.boundary = CallBoundaryKind::SystemCall;
      summary.hasSyscallNumber = true;
      summary.syscallNumber = 0; // Linux x86-64 read; number zero is valid metadata.
      summary.proven = true;
      summary.memory = CallMemoryEffectKind::NoMemory;
      summary.values.push_back({CallValueEffect::Location::ReturnRegister, 0,
                                64, true, 123});
      REQUIRE(attachCallEffectSummary(*call, summary));
    }
    builder.CreateLoad(builder.getInt64Ty(), slot);
    builder.CreateRetVoid();
    return std::pair{module, context};
  };

  {
    auto [module, context] = makeModule(false);
    MemoryForwardOptions options;
    options.runMem2Reg = false;
    options.consumeCallEffectSummaries = true;
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    CHECK(stats.callBarriers >= 1);
    CHECK(stats.loadsForwarded == 0);
    verify(*module);
    delete module;
    delete context;
  }

  {
    auto [module, context] = makeModule(true);
    auto &function = *module->getFunction("syscall_case");
    const auto *call = llvm::dyn_cast<llvm::CallBase>(
        &*std::next(function.front().begin(), 3));
    REQUIRE(call != nullptr);
    const auto parsed = getCallEffectSummary(*call);
    REQUIRE(parsed.has_value());
    CHECK(parsed->boundary == CallBoundaryKind::SystemCall);
    CHECK(parsed->hasSyscallNumber);
    CHECK(parsed->syscallNumber == 0);
    CHECK(parsed->values.size() == 1);

    MemoryForwardOptions options;
    options.runMem2Reg = false;
    options.consumeCallEffectSummaries = true;
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    CHECK(stats.summaryCallsPreserved >= 1);
    CHECK(stats.callBarriers == 0);
    CHECK(stats.loadsForwarded >= 1);
    CHECK(integerLoads(*module) == 0);
    verify(*module);
    delete module;
    delete context;
  }
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

TEST_CASE("S2 visits irreducible cycle blocks using only local exact stores",
          "[llvm][guest-memory-ssa][guest-memory-irreducible]") {
  llvm::LLVMContext context;
  llvm::Module module{"irreducible-exact", context};
  irreducibleMemory(module, IrreducibleAccess::Exact);
  REQUIRE(integerLoads(module) == 2);
  verify(module);
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {65536, false});
  CHECK(stats.complete);
  CHECK(stats.changed);
  CHECK(stats.blocksVisited == 4);
  CHECK(stats.loadsForwarded == 2);
  CHECK(stats.loadsReused == 0);
  CHECK(stats.phisInserted == 0);
  CHECK(integerLoads(module) == 0);
  verify(module);
}

TEST_CASE("S2 rebuilds local Guest bytes in irreducible cycle blocks",
          "[llvm][guest-memory-ssa][guest-memory-irreducible]") {
  llvm::LLVMContext context;
  llvm::Module module{"irreducible-bytes", context};
  irreducibleMemory(module, IrreducibleAccess::Narrow);
  REQUIRE(integerLoads(module) == 2);
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {65536, false});
  CHECK(stats.complete);
  CHECK(stats.blocksVisited == 4);
  CHECK(stats.loadsForwarded == 2);
  CHECK(stats.byteRebuilds == 2);
  CHECK(stats.phisInserted == 0);
  CHECK(integerLoads(module) == 0);
  verify(module);
}

TEST_CASE("S2 keeps irreducible blocks independent of incomplete incoming facts",
          "[llvm][guest-memory-ssa][guest-memory-irreducible]") {
  llvm::LLVMContext context;
  llvm::Module module{"irreducible-cross-block", context};
  irreducibleMemory(module, IrreducibleAccess::CrossBlock);
  REQUIRE(integerLoads(module) == 1);
  SECTION("The ordinary fact walk still imports no incomplete loop facts") {
    MemoryForwardOptions options{65536, false};
    options.enablePhi = false; // Disable the separate CFG-copy fallback.
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, options);
    CHECK(stats.complete);
    CHECK(stats.blocksVisited == 4);
    CHECK_FALSE(stats.changed);
    CHECK(stats.loadsForwarded == 0);
    CHECK(stats.phisInserted == 0);
    CHECK(integerLoads(module) == 1);
    verify(module);
  }
  SECTION("A proved incoming edge can copy the load without a loop invariant") {
    auto &function = *module.begin();
    auto *entry = &function.getEntryBlock();
    auto *left = entry->getTerminator()->getSuccessor(0);
    auto *right = entry->getTerminator()->getSuccessor(1);
    auto *exit = right->getTerminator()->getSuccessor(1);
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {65536, false});
    CHECK(stats.complete);
    CHECK(stats.blocksVisited == 5);
    CHECK(stats.changed);
    CHECK(stats.loadsForwarded == 1);
    CHECK(stats.phisInserted == 0);
    CHECK(integerLoads(module) == 1);
    // One of the two explicit reaching stores authorizes its own copied edge.
    // The original right-hand LOAD must still serve the remaining edge.
    unsigned originalEdges = 0;
    llvm::BasicBlock *copy = nullptr;
    for (auto *pred : {entry, left}) {
      const unsigned edge = pred == entry ? 1 : 0;
      auto *successor = pred->getTerminator()->getSuccessor(edge);
      if (successor == right) ++originalEdges;
      else copy = successor;
    }
    CHECK(originalEdges == 1);
    REQUIRE(copy);
    CHECK(copy != right);
    CHECK(copy->getParent() == &function);
    auto *branch = llvm::dyn_cast<llvm::BranchInst>(copy->getTerminator());
    REQUIRE(branch);
    REQUIRE(branch->isConditional());
    CHECK(branch->getCondition() == function.getArg(3));
    CHECK(branch->getSuccessor(0) == left);
    CHECK(branch->getSuccessor(1) == exit);
    CHECK(std::any_of(right->begin(), right->end(), [](const llvm::Instruction &op) {
      const auto *load = llvm::dyn_cast<llvm::LoadInst>(&op);
      return load && load->getType()->isIntegerTy(64);
    }));
    verify(module);
  }
}

TEST_CASE("S2 retains memory barriers in conservative cycle coverage",
          "[llvm][guest-memory-ssa][guest-memory-irreducible]") {
  const std::pair<const char *, IrreducibleAccess> cases[] = {
      {"unknown-call", IrreducibleAccess::UnknownCall},
      {"unknown-alias", IrreducibleAccess::UnknownAlias},
      {"fence", IrreducibleAccess::Fence},
      {"atomic-write", IrreducibleAccess::AtomicWrite},
      {"volatile-load", IrreducibleAccess::VolatileLoad},
      {"volatile-store", IrreducibleAccess::VolatileStore},
      {"partial-store", IrreducibleAccess::Partial},
      {"possibly-aliasing-guest-bases", IrreducibleAccess::OtherGuestBase},
  };
  for (const auto &[name, access] : cases) {
    DYNAMIC_SECTION(name) {
      llvm::LLVMContext context;
      llvm::Module module{name, context};
      irreducibleMemory(module, access);
      REQUIRE(integerLoads(module) == 2);
      const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {65536, false});
      CHECK(stats.complete);
      CHECK(stats.blocksVisited == 4);
      CHECK_FALSE(stats.changed);
      CHECK(stats.loadsForwarded == 0);
      CHECK(stats.byteRebuilds == 0);
      CHECK(stats.phisInserted == 0);
      CHECK(integerLoads(module) == 2);
      verify(module);
    }
  }
}

TEST_CASE("S2 bounds rewrites in conservative cycle coverage",
          "[llvm][guest-memory-ssa][guest-memory-irreducible]") {
  llvm::LLVMContext context;
  llvm::Module module{"irreducible-budget", context};
  irreducibleMemory(module, IrreducibleAccess::Exact);
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {1, false});
  CHECK_FALSE(stats.complete);
  CHECK(stats.changed);
  CHECK(stats.loadsForwarded == 1);
  CHECK(stats.budgetExhausted == 1);
  CHECK(stats.phisInserted == 0);
  CHECK(integerLoads(module) == 1);
  verify(module);
}
