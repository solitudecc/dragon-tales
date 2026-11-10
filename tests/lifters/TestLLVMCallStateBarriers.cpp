#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace dragon;
using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Slot { HostLocal, HostEscaped, GuestFixed, GuestStack, GuestRegister };
struct Fixture {
  llvm::Function *machine;
  llvm::Function *opaque;
};

// A real, single-function state loop: 10 -> opaque call -> 20 -> 30 -> return.
// It does not inspect a Guest entry PC or rely on summaries of another function.
// The opaque body is absent during optimization and linked by the native oracle.
Fixture fixture(LLVMLifter &lifter, Slot storage, bool writeAfterCall,
                bool falseReadonlyControl = false) {
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *opaque = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                             {ptr, ptr, ptr, i32}, false),
      llvm::GlobalValue::ExternalLinkage, "opaque_writer", lifter.module());
  if (falseReadonlyControl) opaque->setOnlyReadsMemory();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(i32, {ptr, ptr, i32}, false),
      llvm::GlobalValue::ExternalLinkage, "machine", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "cff.dispatch", function);
  auto *first = llvm::BasicBlock::Create(context, "cff.first", function);
  auto *second = llvm::BasicBlock::Create(context, "cff.second", function);
  auto *done = llvm::BasicBlock::Create(context, "cff.done", function);
  auto *fallback = llvm::BasicBlock::Create(context, "cff.default", function);
  Builder builder(entry);
  auto constant = [&](unsigned value) { return llvm::ConstantInt::get(i32, value); };
  auto *data = builder.CreateLoad(ptr, function->getArg(1), "guest.data");
  llvm::Value *stateSlot = nullptr;
  if (storage == Slot::HostLocal || storage == Slot::HostEscaped) {
    stateSlot = builder.CreateAlloca(i32, nullptr, "host.state");
  } else if (storage == Slot::GuestRegister) {
    // Byte 16 is an ordinary register snapshot slot, never RIP/PC (128).
    stateSlot = builder.CreateGEP(builder.getInt8Ty(), function->getArg(0),
                                 builder.getInt64(16), "register.state");
  } else {
    llvm::Value *offset = builder.getInt64(0x100);
    if (storage == Slot::GuestStack) {
      auto *index = builder.CreateZExt(
          builder.CreateAnd(function->getArg(2), constant(7)), builder.getInt64Ty());
      offset = builder.CreateAdd(builder.getInt64(0x4000),
                                 builder.CreateMul(index, builder.getInt64(4)));
    }
    stateSlot = builder.CreateGEP(builder.getInt8Ty(), data, offset, "guest.state");
  }
  builder.CreateStore(constant(10), stateSlot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreateLoad(i32, stateSlot, "cff.state");
  auto *selector = builder.CreateSwitch(state, fallback, 3);
  selector->addCase(constant(10), first);
  selector->addCase(constant(20), second);
  selector->addCase(constant(30), done);
  builder.SetInsertPoint(first);
  if (!writeAfterCall) builder.CreateStore(constant(20), stateSlot);
  auto *exposed = storage == Slot::HostLocal
      ? static_cast<llvm::Value *>(llvm::ConstantPointerNull::get(ptr)) : stateSlot;
  builder.CreateCall(opaque, {function->getArg(0), function->getArg(1), exposed,
                              function->getArg(2)});
  if (writeAfterCall) builder.CreateStore(constant(20), stateSlot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(second);
  auto *output = builder.CreateGEP(builder.getInt8Ty(), data,
                                  builder.getInt64(0x200));
  builder.CreateStore(builder.CreateXor(function->getArg(2), constant(0x13579bdf)),
                       output);
  builder.CreateStore(constant(30), stateSlot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(done);
  builder.CreateRet(builder.CreateAdd(function->getArg(2), constant(7)));
  builder.SetInsertPoint(fallback);
  builder.CreateRet(builder.CreateXor(function->getArg(2), constant(0xbad)));
  return {function, opaque};
}

void verify(LLVMLifter &lifter) {
  std::string error;
  llvm::raw_string_ostream output(error);
  const bool invalid = llvm::verifyModule(lifter.module(), &output);
  INFO(error);
  REQUIRE_FALSE(invalid);
}

unsigned allocas(llvm::Function &function) {
  unsigned count = 0;
  for (auto &block : function)
    for (auto &instruction : block)
      count += llvm::isa<llvm::AllocaInst>(instruction);
  return count;
}

unsigned stateLoads(llvm::Function &function) {
  unsigned count = 0;
  for (auto &block : function)
    for (auto &instruction : block)
      count += llvm::isa<llvm::LoadInst>(instruction) &&
               instruction.getName() == "cff.state";
  return count;
}

unsigned callsTo(llvm::Function &function, llvm::Function *callee) {
  unsigned count = 0;
  for (auto &block : function)
    for (auto &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
        count += call->getCalledFunction() == callee;
  return count;
}

void snapshot(LLVMLifter &lifter, const std::string &name) {
  if (const auto *directory = std::getenv("DRAGON_CALL_STATE_FIXTURE_DIR")) {
    std::filesystem::create_directories(directory);
    std::ofstream output{std::filesystem::path(directory) / name};
    output << lifter.ir();
    REQUIRE(output.good());
  }
}

void runCase(Slot slot, bool after, const char *name, bool canRecover) {
  LLVMLifter lifter{Architecture::X86_64};
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  auto data = fixture(lifter, slot, after);
  verify(lifter);
  snapshot(lifter, std::string(name) + "-before.ll");
  MemoryForwardOptions options;
  options.runMem2Reg = false;
  const auto memory = lifter.runGuestMemorySSA(options);
  REQUIRE(memory.available);
  CHECK(memory.callBarriers >= 1);
  CHECK(callsTo(*data.machine, data.opaque) == 1);
  // Promotion is legal for a nonescaping host alloca. Guest memory/Guest stack
  // remain externally visible even though their source language called it a local.
  lifter.runMem2Reg();
  if (slot == Slot::HostLocal) {
    CHECK(allocas(*data.machine) == 0);
    CHECK(stateLoads(*data.machine) == 0);
  } else if (slot == Slot::HostEscaped) {
    CHECK(allocas(*data.machine) == 1);
    CHECK(stateLoads(*data.machine) == 1);
  } else {
    CHECK(allocas(*data.machine) == 0);
    CHECK(stateLoads(*data.machine) == 1);
  }
  const auto deflat = lifter.runProvableDeflattening();
  CHECK(deflat.changed);
  CHECK(deflat.edgesRewritten == (canRecover ? 3 : 2));
  CHECK(deflat.unresolvedDispatchers == (canRecover ? 0 : 1));
  llvm::BasicBlock *first = nullptr;
  llvm::BasicBlock *second = nullptr;
  llvm::BasicBlock *done = nullptr;
  llvm::BasicBlock *dispatch = nullptr;
  for (auto &block : *data.machine) {
    if (block.getName() == "cff.first") first = &block;
    if (block.getName() == "cff.second") second = &block;
    if (block.getName() == "cff.done") done = &block;
    if (block.getName() == "cff.dispatch") dispatch = &block;
  }
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(done);
  REQUIRE(dispatch);
  CHECK(data.machine->getEntryBlock().getTerminator()->getSuccessor(0) == first);
  CHECK(second->getTerminator()->getSuccessor(0) == done);
  if (canRecover) CHECK(first->getTerminator()->getSuccessor(0) == second);
  if (!canRecover) {
    // Only the post-CALL edge remains unknown. The initial and second-block
    // stores are independent proofs, so they may bypass the dispatcher.
    CHECK(first->getTerminator()->getSuccessor(0) == dispatch);
    CHECK(llvm::isa<llvm::SwitchInst>(dispatch->getTerminator()));
  }
  CHECK(deflat.crossCallDispatchersRewritten == 0);
  CHECK(deflat.crossCallSummariesConsumed == 0);
  CHECK(deflat.crossCallDirectStoresConsumed == 0);
  CHECK(callsTo(*data.machine, data.opaque) == 1);
  verify(lifter);
  lifter.runControlFlowSimplification();
  verify(lifter);
  CHECK(callsTo(*data.machine, data.opaque) == 1);
  snapshot(lifter, std::string(name) + "-after.ll");
}
} // namespace

TEST_CASE("Same-function CFF rewrites a Guest state assigned after unknown CALL",
          "[llvm][call-state-barriers]") {
  runCase(Slot::GuestFixed, true, "guest-after", true);
  runCase(Slot::GuestStack, true, "stack-after", true);
}

TEST_CASE("Same-function CFF keeps pre-CALL Guest and register state unknown",
          "[llvm][call-state-barriers]") {
  runCase(Slot::GuestFixed, false, "guest-before", false);
  runCase(Slot::GuestStack, false, "stack-before", false);
  runCase(Slot::GuestRegister, false, "register-before", false);
}

TEST_CASE("Same-function CFF distinguishes unescaped host locals from Guest stack",
          "[llvm][call-state-barriers]") {
  runCase(Slot::HostLocal, false, "host-local", true);
  runCase(Slot::HostEscaped, false, "host-escaped", false);
  // This intentionally false attribute is exported only as an oracle control.
  // Linking a real writer afterwards must expose the resulting stale-state bug.
  if (std::getenv("DRAGON_CALL_STATE_FIXTURE_DIR")) {
    LLVMLifter lifter{Architecture::X86_64};
    auto control = fixture(lifter, Slot::GuestFixed, false, true);
    (void)control;
    lifter.runMem2Reg();
    lifter.runGVN();
    lifter.runControlFlowSimplification();
    snapshot(lifter, "guest-before-false-readonly.ll");
  }
}
