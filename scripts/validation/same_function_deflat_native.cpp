#include <dragon/lifters/llvm/LLVMDeflattening.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <string>

namespace {
void phiPayload(llvm::Module &module, bool loadBacked) {
  auto &context = module.getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *type = llvm::FunctionType::get(i32, {llvm::Type::getInt1Ty(context)}, false);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage,
      loadBacked ? "load_phi_payload" : "phi_payload", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  auto *slot = loadBacked ? builder.CreateAlloca(i32) : nullptr;
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  if (slot) builder.CreateStore(builder.getInt32(1), slot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(right);
  if (slot) builder.CreateStore(builder.getInt32(2), slot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *payload = builder.CreatePHI(i32, 2, "payload");
  payload->addIncoming(builder.getInt32(10), left);
  payload->addIncoming(builder.getInt32(20), right);
  llvm::Value *state = nullptr;
  if (slot) {
    state = builder.CreateLoad(i32, slot);
  } else {
    auto *phi = builder.CreatePHI(i32, 2, "state");
    phi->addIncoming(builder.getInt32(1), left);
    phi->addIncoming(builder.getInt32(2), right);
    state = phi;
  }
  auto *sw = builder.CreateSwitch(state, fallback, 2);
  sw->addCase(builder.getInt32(1), taken);
  sw->addCase(builder.getInt32(2), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRet(payload);
  builder.SetInsertPoint(fallback);
  builder.CreateRet(builder.getInt32(0));
}

void mixedState(llvm::Module &module, bool loadBacked) {
  auto &context = module.getContext();
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *type = llvm::FunctionType::get(
      i64, {llvm::Type::getInt1Ty(context), i64}, false);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage,
      loadBacked ? "load_mixed_state" : "mixed_state", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *known = llvm::BasicBlock::Create(context, "known", function);
  auto *unknown = llvm::BasicBlock::Create(context, "unknown", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *one = llvm::BasicBlock::Create(context, "one", function);
  auto *two = llvm::BasicBlock::Create(context, "two", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  auto *slot = loadBacked ? builder.CreateAlloca(i64) : nullptr;
  builder.CreateCondBr(function->getArg(0), known, unknown);
  builder.SetInsertPoint(known);
  if (slot) builder.CreateStore(builder.getInt64(1), slot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(unknown);
  if (slot) builder.CreateStore(function->getArg(1), slot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  llvm::Value *state = nullptr;
  if (slot) {
    state = builder.CreateLoad(i64, slot);
  } else {
    auto *phi = builder.CreatePHI(i64, 2, "state");
    phi->addIncoming(builder.getInt64(1), known);
    phi->addIncoming(function->getArg(1), unknown);
    state = phi;
  }
  auto *sw = builder.CreateSwitch(state, fallback, 2);
  sw->addCase(builder.getInt64(1), one);
  sw->addCase(builder.getInt64(2), two);
  builder.SetInsertPoint(one);
  builder.CreateRet(builder.getInt64(11));
  builder.SetInsertPoint(two);
  builder.CreateRet(builder.getInt64(22));
  builder.SetInsertPoint(fallback);
  builder.CreateRet(builder.getInt64(33));
}

void dump(const llvm::Module &module, const std::string &path) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_Text);
  if (error) {
    llvm::errs() << error.message() << '\n';
    std::exit(2);
  }
  module.print(stream, nullptr);
}
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  llvm::LLVMContext context;
  llvm::Module module("same-function-deflat-actual", context);
  phiPayload(module, false);
  phiPayload(module, true);
  mixedState(module, false);
  mixedState(module, true);
  if (llvm::verifyModule(module, &llvm::errs())) return 3;
  dump(module, std::string(argv[1]) + "/before.ll");
  dragon::llvmlifter::DeflatteningOptions options;
  options.consumeCallEffectSummaries = false;
  const auto stats = dragon::llvmlifter::runProvableDeflattening(module, options);
  const bool invalid = llvm::verifyModule(module, &llvm::errs());
  dump(module, std::string(argv[1]) + "/after.ll");
  llvm::outs() << "changed=" << stats.changed
               << " dispatchers=" << stats.dispatchersVisited
               << " rewritten=" << stats.dispatchersRewritten
               << " edges=" << stats.edgesRewritten
               << " unresolved=" << stats.unresolvedDispatchers
               << " verify_invalid=" << invalid << '\n';
  return invalid ? 3 : 0;
}
