#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMDeflattening.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <string>

using namespace dragon::llvmlifter;

namespace {
void verifySameFunction(const llvm::Module &module) {
  std::string message;
  llvm::raw_string_ostream stream(message);
  const bool invalid = llvm::verifyModule(module, &stream);
  INFO(stream.str());
  CHECK_FALSE(invalid);
}
}

TEST_CASE("Same-function deflattening preserves dispatcher live-out PHIs",
          "[llvm][dispatch-deflatten][same-function]") {
  llvm::LLVMContext context;
  llvm::Module module("state-and-payload", context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *type = llvm::FunctionType::get(i32, {llvm::Type::getInt1Ty(context)}, false);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "payload", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(right);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i32, 2, "state");
  state->addIncoming(builder.getInt32(1), left);
  state->addIncoming(builder.getInt32(2), right);
  auto *payload = builder.CreatePHI(i32, 2, "payload");
  payload->addIncoming(builder.getInt32(10), left);
  payload->addIncoming(builder.getInt32(20), right);
  auto *switchInst = builder.CreateSwitch(state, fallback, 2);
  switchInst->addCase(builder.getInt32(1), taken);
  switchInst->addCase(builder.getInt32(2), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRet(payload);
  builder.SetInsertPoint(fallback);
  builder.CreateRet(builder.getInt32(0));
  verifySameFunction(module);

  const auto stats = runProvableDeflattening(module);
  CHECK_FALSE(stats.changed);
  CHECK(stats.edgesRewritten == 0);
  CHECK(stats.unresolvedDispatchers == 1);
  REQUIRE(llvm::isa<llvm::SwitchInst>(dispatch->getTerminator()));
  CHECK(left->getTerminator()->getSuccessor(0) == dispatch);
  CHECK(right->getTerminator()->getSuccessor(0) == dispatch);
  auto *result = llvm::cast<llvm::ReturnInst>(taken->getTerminator());
  REQUIRE(llvm::isa<llvm::PHINode>(result->getReturnValue()));
  auto *retained = llvm::cast<llvm::PHINode>(result->getReturnValue());
  CHECK(retained->getIncomingValueForBlock(left) == builder.getInt32(10));
  CHECK(retained->getIncomingValueForBlock(right) == builder.getInt32(20));
  verifySameFunction(module);
}

TEST_CASE("Same-function deflattening retains observable selector loads",
          "[llvm][dispatch-deflatten][same-function]") {
  for (const bool atomic : {false, true}) {
    INFO("atomic=" << atomic);
    llvm::LLVMContext context;
    llvm::Module module("observable-selector", context);
    auto *i32 = llvm::Type::getInt32Ty(context);
    auto *type = llvm::FunctionType::get(
        i32, {llvm::PointerType::getUnqual(context)}, false);
    auto *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "observable", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
    auto *taken = llvm::BasicBlock::Create(context, "taken", function);
    auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
    llvm::IRBuilder<> builder(entry);
    builder.CreateStore(builder.getInt32(1), function->getArg(0));
    builder.CreateBr(dispatch);
    builder.SetInsertPoint(dispatch);
    auto *state = builder.CreateLoad(i32, function->getArg(0));
    state->setAlignment(llvm::Align(4));
    if (atomic) state->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    else state->setVolatile(true);
    auto *switchInst = builder.CreateSwitch(state, fallback, 1);
    switchInst->addCase(builder.getInt32(1), taken);
    builder.SetInsertPoint(taken);
    builder.CreateRet(builder.getInt32(10));
    builder.SetInsertPoint(fallback);
    builder.CreateRet(builder.getInt32(20));
    verifySameFunction(module);

    const auto stats = runProvableDeflattening(module);
    CHECK_FALSE(stats.changed);
    CHECK(stats.edgesRewritten == 0);
    CHECK(stats.unresolvedDispatchers == 1);
    CHECK(entry->getTerminator()->getSuccessor(0) == dispatch);
    CHECK(llvm::isa<llvm::SwitchInst>(dispatch->getTerminator()));
    verifySameFunction(module);
  }
}

TEST_CASE("Same-function deflattening retains indirect branch destinations",
          "[llvm][dispatch-deflatten][same-function]") {
  llvm::LLVMContext context;
  llvm::Module module("indirect-predecessor", context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(i32, {}, false),
      llvm::GlobalValue::ExternalLinkage, "indirect_predecessor", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  auto *address = llvm::BlockAddress::get(function, dispatch);
  auto *branch = builder.CreateIndirectBr(address, 1);
  branch->addDestination(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i32, 1, "state");
  state->addIncoming(builder.getInt32(1), entry);
  auto *switchInst = builder.CreateSwitch(state, fallback, 1);
  switchInst->addCase(builder.getInt32(1), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRet(builder.getInt32(10));
  builder.SetInsertPoint(fallback);
  builder.CreateRet(builder.getInt32(20));
  verifySameFunction(module);

  const auto stats = runProvableDeflattening(module);
  CHECK_FALSE(stats.changed);
  CHECK(stats.edgesRewritten == 0);
  CHECK(stats.unresolvedDispatchers == 1);
  CHECK(branch->getAddress() == address);
  CHECK(branch->getSuccessor(0) == dispatch);
  verifySameFunction(module);
}

TEST_CASE("Same-function deflattening threads proved edges beside unknown states",
          "[llvm][dispatch-deflatten][same-function]") {
  for (const bool loadBacked : {false, true}) {
    INFO("loadBacked=" << loadBacked);
    llvm::LLVMContext context;
    llvm::Module module("mixed-state-edges", context);
    auto *i32 = llvm::Type::getInt32Ty(context);
    auto *type = llvm::FunctionType::get(
        i32, {llvm::Type::getInt1Ty(context), i32}, false);
    auto *function = llvm::Function::Create(
        type, llvm::GlobalValue::ExternalLinkage, "mixed", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *known = llvm::BasicBlock::Create(context, "known", function);
    auto *unknown = llvm::BasicBlock::Create(context, "unknown", function);
    auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
    auto *taken = llvm::BasicBlock::Create(context, "taken", function);
    auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
    llvm::IRBuilder<> builder(entry);
    auto *slot = loadBacked ? builder.CreateAlloca(i32) : nullptr;
    builder.CreateCondBr(function->getArg(0), known, unknown);
    builder.SetInsertPoint(known);
    if (slot) builder.CreateStore(builder.getInt32(1), slot);
    builder.CreateBr(dispatch);
    builder.SetInsertPoint(unknown);
    if (slot) builder.CreateStore(function->getArg(1), slot);
    builder.CreateBr(dispatch);
    builder.SetInsertPoint(dispatch);
    llvm::Value *state;
    if (slot) state = builder.CreateLoad(i32, slot);
    else {
      auto *phi = builder.CreatePHI(i32, 2, "state");
      phi->addIncoming(builder.getInt32(1), known);
      phi->addIncoming(function->getArg(1), unknown);
      state = phi;
    }
    auto *switchInst = builder.CreateSwitch(state, fallback, 1);
    switchInst->addCase(builder.getInt32(1), taken);
    builder.SetInsertPoint(taken);
    builder.CreateRet(builder.getInt32(10));
    builder.SetInsertPoint(fallback);
    builder.CreateRet(builder.getInt32(20));
    verifySameFunction(module);

    const auto stats = runProvableDeflattening(module);
    CHECK(stats.changed);
    CHECK(stats.edgesRewritten == 1);
    CHECK(stats.dispatchersRewritten == 1);
    CHECK(stats.unresolvedDispatchers == 1);
    CHECK(known->getTerminator()->getSuccessor(0) == taken);
    CHECK(unknown->getTerminator()->getSuccessor(0) == dispatch);
    CHECK(llvm::isa<llvm::SwitchInst>(dispatch->getTerminator()));
    verifySameFunction(module);
    const auto repeat = runProvableDeflattening(module);
    CHECK_FALSE(repeat.changed);
    CHECK(repeat.edgesRewritten == 0);
    CHECK(repeat.unresolvedDispatchers == 1);
    verifySameFunction(module);
  }
}
