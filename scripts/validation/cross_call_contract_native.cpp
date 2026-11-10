#include <dragon/lifters/llvm/CallEffectSummary.hpp>
#include <dragon/lifters/llvm/LLVMDeflattening.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <string>

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
  if (!attachCallEffectSummary(*call, summary)) std::exit(4);
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

void dump(llvm::Module &module, const std::string &path) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_Text);
  if (error) std::exit(2);
  module.print(stream, nullptr);
}
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  llvm::LLVMContext context;
  llvm::Module module("cross-call-contract-audit", context);
  postcondition(module);
  aliasBases(module);
  nonentry(module);
  if (llvm::verifyModule(module, &llvm::errs())) return 3;
  dump(module, std::string(argv[1]) + "/before.ll");
  dragon::llvmlifter::DeflatteningOptions options;
  // Compatibility switches must not reactivate experimental cross-call proofs.
  options.consumeCallEffectSummaries = true;
  const auto stats = dragon::llvmlifter::runProvableDeflattening(module, options);
  const bool invalid = llvm::verifyModule(module, &llvm::errs());
  dump(module, std::string(argv[1]) + "/after.ll");
  llvm::outs() << "visited=" << stats.dispatchersVisited
               << " rewritten=" << stats.dispatchersRewritten
               << " edges=" << stats.edgesRewritten
               << " cross=" << stats.crossCallDispatchersRewritten
               << " summaries=" << stats.crossCallSummariesConsumed
               << " direct=" << stats.crossCallDirectStoresConsumed
               << " unresolved=" << stats.unresolvedDispatchers
               << " invalid=" << invalid << '\n';
  return invalid ? 3 : 0;
}
