#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/CallEffectSummary.hpp>
#include <dragon/lifters/llvm/LLVMDeflattening.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

namespace {
using namespace dragon::llvmlifter;
using Builder = llvm::IRBuilder<>;

llvm::Function *function(llvm::Module &module, const char *name, bool local) {
  auto &context = module.getContext();
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
      {llvm::PointerType::getUnqual(context)}, false);
  return llvm::Function::Create(type, local ? llvm::GlobalValue::InternalLinkage
                                          : llvm::GlobalValue::ExternalLinkage,
                                name, module);
}

void postcondition(llvm::Module &module) {
  auto &context = module.getContext();
  auto *callee = function(module, "postcondition_callee", true);
  auto *entry = llvm::BasicBlock::Create(context, "entry", callee);
  auto *fortyTwo = llvm::BasicBlock::Create(context, "forty_two", callee);
  auto *other = llvm::BasicBlock::Create(context, "other", callee);
  Builder b(entry);
  auto *load = b.CreateLoad(b.getInt64Ty(), callee->getArg(0));
  auto *sw = b.CreateSwitch(load, other, 1);
  sw->addCase(b.getInt64(42), fortyTwo);
  b.SetInsertPoint(fortyTwo);
  b.CreateStore(b.getInt64(42), callee->getArg(0));
  b.CreateRet(b.getInt64(99));
  b.SetInsertPoint(other);
  b.CreateStore(b.getInt64(42), callee->getArg(0));
  b.CreateRet(b.getInt64(17));
  auto *caller = function(module, "postcondition_probe", false);
  auto *callerEntry = llvm::BasicBlock::Create(context, "entry", caller);
  b.SetInsertPoint(callerEntry);
  auto *call = b.CreateCall(callee, {caller->getArg(0)});
  CallEffectSummary summary;
  summary.version = 2;
  summary.proven = true;
  summary.memory = CallMemoryEffectKind::KnownRanges;
  const CallMemoryRange range{CallMemoryRange::Base::GuestState, 0, 8};
  summary.reads.push_back(range);
  summary.writes.push_back(range);
  summary.memoryConstants.push_back({range, 42, true});
  // Correct contract: EVERY return path has written 42, independently of the
  // selector's entry value. It says nothing about the callee's precondition.
  REQUIRE(attachCallEffectSummary(*call, summary));
  b.CreateRet(call);
}

void aliasBases(llvm::Module &module) {
  auto &context = module.getContext();
  auto *callee = function(module, "distinct_bases_callee", true);
  auto *entry = llvm::BasicBlock::Create(context, "entry", callee);
  auto *one = llvm::BasicBlock::Create(context, "one", callee);
  auto *other = llvm::BasicBlock::Create(context, "other", callee);
  Builder b(entry);
  auto *a = b.CreateLoad(b.getPtrTy(), callee->getArg(0));
  auto *slotA = b.CreateGEP(b.getInt8Ty(), a, b.getInt64(8));
  auto *load = b.CreateLoad(b.getInt64Ty(), slotA);
  auto *sw = b.CreateSwitch(load, other, 1);
  sw->addCase(b.getInt64(1), one);
  b.SetInsertPoint(one);
  b.CreateRet(b.getInt64(111));
  b.SetInsertPoint(other);
  b.CreateRet(b.getInt64(222));
  auto *caller = function(module, "distinct_bases_probe", false);
  auto *callerEntry = llvm::BasicBlock::Create(context, "entry", caller);
  b.SetInsertPoint(callerEntry);
  auto *bPointerSlot = b.CreateGEP(b.getInt8Ty(), caller->getArg(0), b.getInt64(8));
  auto *baseB = b.CreateLoad(b.getPtrTy(), bPointerSlot);
  auto *slotB = b.CreateGEP(b.getInt8Ty(), baseB, b.getInt64(8));
  b.CreateStore(b.getInt64(1), slotB);
  b.CreateRet(b.CreateCall(callee, {caller->getArg(0)}));
}

void nonentry(llvm::Module &module) {
  auto &context = module.getContext();
  auto *callee = function(module, "nonentry_callee", true);
  auto *entry = llvm::BasicBlock::Create(context, "entry", callee);
  auto *middle = llvm::BasicBlock::Create(context, "middle", callee);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", callee);
  auto *one = llvm::BasicBlock::Create(context, "one", callee);
  auto *other = llvm::BasicBlock::Create(context, "other", callee);
  Builder b(entry);
  b.CreateStore(b.getInt64(2), callee->getArg(0));
  b.CreateBr(middle);
  b.SetInsertPoint(middle);
  b.CreateBr(dispatch);
  b.SetInsertPoint(dispatch);
  auto *load = b.CreateLoad(b.getInt64Ty(), callee->getArg(0));
  auto *sw = b.CreateSwitch(load, other, 1);
  sw->addCase(b.getInt64(1), one);
  b.SetInsertPoint(one);
  b.CreateRet(b.getInt64(111));
  b.SetInsertPoint(other);
  b.CreateRet(b.getInt64(222));
  auto *caller = function(module, "nonentry_probe", false);
  auto *callerEntry = llvm::BasicBlock::Create(context, "entry", caller);
  b.SetInsertPoint(callerEntry);
  b.CreateStore(b.getInt64(1), caller->getArg(0));
  b.CreateRet(b.CreateCall(callee, {caller->getArg(0)}));
}

using FixtureBuilder = void (*)(llvm::Module &);

void checkIsolated(FixtureBuilder fixture) {
  for (const bool consume : {false, true}) {
    INFO("consumeCallEffectSummaries=" << consume);
    llvm::LLVMContext context;
    llvm::Module module("cross-call-isolation", context);
    fixture(module);
    REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
    dragon::llvmlifter::DeflatteningOptions options;
    options.consumeCallEffectSummaries = consume;
    const auto stats = dragon::llvmlifter::runProvableDeflattening(module, options);
    CHECK(stats.available);
    CHECK_FALSE(stats.changed);
    CHECK(stats.dispatchersVisited == 1);
    CHECK(stats.dispatchersRewritten == 0);
    CHECK(stats.edgesRewritten == 0);
    CHECK(stats.crossCallDispatchersRewritten == 0);
    CHECK(stats.crossCallSummariesConsumed == 0);
    CHECK(stats.crossCallDirectStoresConsumed == 0);
    CHECK(stats.unresolvedDispatchers == 1);
    unsigned switches = 0;
    for (const auto &function : module)
      for (const auto &block : function)
        switches += llvm::isa<llvm::SwitchInst>(block.getTerminator());
    CHECK(switches == 1);
    CHECK_FALSE(llvm::verifyModule(module, &llvm::errs()));
  }
}
} // namespace

TEST_CASE("Cross-call effect postconditions do not prove entry selectors",
          "[llvm][dispatch-deflatten][cross-call-isolation]") {
  checkIsolated(postcondition);
}

TEST_CASE("Cross-call state facts keep distinct nested pointer objects separate",
          "[llvm][dispatch-deflatten][cross-call-isolation]") {
  checkIsolated(aliasBases);
}

TEST_CASE("Caller state does not prove a later callee dispatcher value",
          "[llvm][dispatch-deflatten][cross-call-isolation]") {
  checkIsolated(nonentry);
}
