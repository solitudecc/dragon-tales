#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;

// These are valid no-STORE paths, not malformed CFG or unreachable padding.
// Four hubs plus the final checked join/success fit the existing six-block
// corridor cap. At 512 predecessors per hub, recursive failed proofs can
// exhaust the module edge budget before a later original direct opportunity.
llvm::Function *addHubPressure(llvm::Module &module, unsigned incoming) {
  auto &context = module.getContext();
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, i64, llvm::Type::getInt32Ty(context), llvm::Type::getInt1Ty(context)}, false),
      llvm::GlobalValue::ExternalLinkage, "earlier_hub_pressure", module);
  for (auto &arg : function->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto block = [&](const char *name) {return llvm::BasicBlock::Create(context, name, function);};
  auto *entry = block("entry");
  std::array<llvm::BasicBlock *, 4> hubs;
  for (auto &hub : hubs) hub = block("hub");
  auto *bypass = block("unknown.direct");
  auto *join = block("checked.join"), *success = block("checked.success"), *failure = block("failure");
  auto *done = block("done");
  Builder builder(entry);
  auto *base = builder.CreateLoad(ptr, function->getArg(1), "guest.base");
  base->setAlignment(llvm::Align(1));
  auto *route = builder.CreateSwitch(function->getArg(3), bypass,
                                     4 * incoming - 3);
  std::array<std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>>, 4> values;
  unsigned caseIndex = 0;
  for (unsigned hubIndex = 0; hubIndex != hubs.size(); ++hubIndex) {
    const unsigned leaves = incoming - (hubIndex != 0);
    for (unsigned n = 0; n != leaves; ++n) {
      auto *leaf = block("incoming");
      route->addCase(builder.getInt32(caseIndex++), leaf);
      builder.SetInsertPoint(leaf);
      auto *index = builder.CreateAdd(function->getArg(2), builder.getInt64(caseIndex), "incoming.index");
      builder.CreateBr(hubs[hubIndex]);
      values[hubIndex].emplace_back(index, leaf);
    }
  }
  llvm::PHINode *last = nullptr;
  for (unsigned n = 0; n != hubs.size(); ++n) {
    builder.SetInsertPoint(hubs[n]);
    auto *index = builder.CreatePHI(i64, incoming, "hub.index");
    if (last) index->addIncoming(last, hubs[n - 1]);
    for (auto [value, predecessor] : values[n]) index->addIncoming(value, predecessor);
    builder.CreateBr(n + 1 == hubs.size() ? join : hubs[n + 1]);
    last = index;
  }
  builder.SetInsertPoint(bypass); builder.CreateBr(join);
  builder.SetInsertPoint(join);
  auto *index = builder.CreatePHI(i64, 2, "joined.index");
  index->addIncoming(last, hubs.back()); index->addIncoming(function->getArg(2), bypass);
  builder.CreateCondBr(function->getArg(4), success, failure);
  builder.SetInsertPoint(success);
  auto *address = builder.CreateGEP(builder.getInt8Ty(), base, index);
  auto *load = builder.CreateLoad(i64, address, "actual.guest.load"); load->setAlignment(llvm::Align(1));
  builder.CreateBr(done);
  builder.SetInsertPoint(done); builder.CreateRet(load);
  builder.SetInsertPoint(failure); builder.CreateRet(builder.getInt64(0));
  for (auto *hub : hubs) CHECK(llvm::pred_size(hub) == incoming);
  return function;
}

struct DirectView {
  llvm::BasicBlock *known, *unknown, *join, *success;
  llvm::LoadInst *load;
};

DirectView addLaterDirect(llvm::Module &module) {
  auto &context = module.getContext();
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, i64, llvm::Type::getInt1Ty(context), llvm::Type::getInt1Ty(context)}, false),
      llvm::GlobalValue::ExternalLinkage, "later_legacy_direct_store", module);
  for (auto &arg : function->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto block = [&](const char *name) {return llvm::BasicBlock::Create(context, name, function);};
  auto *entry = block("entry"), *known = block("known.store"), *unknown = block("unknown");
  auto *join = block("checked.join"), *success = block("checked.success"), *failure = block("failure");
  auto *done = block("done");
  Builder builder(entry);
  auto *base = builder.CreateLoad(ptr, function->getArg(1), "guest.base"); base->setAlignment(llvm::Align(1));
  auto *frame = builder.CreateAdd(function->getArg(2), builder.getInt64(-8), "known.frame");
  auto *storedIndex = builder.CreateAdd(frame, builder.getInt64(-120), "stored.index");
  auto *slot = builder.CreateGEP(builder.getInt8Ty(), base, storedIndex);
  builder.CreateCondBr(function->getArg(3), known, unknown);
  builder.SetInsertPoint(known);
  builder.CreateStore(builder.getInt64(50), slot)->setAlignment(llvm::Align(1)); builder.CreateBr(join);
  builder.SetInsertPoint(unknown); builder.CreateBr(join);
  builder.SetInsertPoint(join);
  auto *joinedFrame = builder.CreatePHI(i64, 2, "joined.frame");
  joinedFrame->addIncoming(frame, known); joinedFrame->addIncoming(function->getArg(2), unknown);
  auto *loadedIndex = builder.CreateAdd(joinedFrame, builder.getInt64(-120), "loaded.index");
  builder.CreateCondBr(function->getArg(4), success, failure);
  builder.SetInsertPoint(success);
  auto *address = builder.CreateGEP(builder.getInt8Ty(), base, loadedIndex);
  auto *load = builder.CreateLoad(i64, address, "actual.guest.load"); load->setAlignment(llvm::Align(1));
  builder.CreateBr(done);
  builder.SetInsertPoint(done); builder.CreateRet(load);
  builder.SetInsertPoint(failure); builder.CreateRet(builder.getInt64(0));
  return {known, unknown, join, success, load};
}

std::string text(llvm::Function &function) {
  std::string result; llvm::raw_string_ostream output(result); function.print(output); return result;
}
} // namespace

TEST_CASE("Guest memory hub searches preserve later module legacy direct opportunities",
          "[llvm][guest-memory-ssa][guest-memory-hub-priority]") {
  for (unsigned incoming : {16u, 512u}) {
    DYNAMIC_SECTION("hub incoming count " << incoming) {
      llvm::LLVMContext context;
      llvm::Module module("guest-memory-hub-priority", context);
      module.setDataLayout("e-p:64:64");
      auto *pressure = addHubPressure(module, incoming);
      const auto before = text(*pressure);
      auto later = addLaterDirect(module);
      REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
      MemoryForwardOptions options; options.runMem2Reg = false;
      const auto stats = detail::runGuestMemorySSA(module, options);
      INFO("incoming=" << incoming << " forwarded=" << stats.loadsForwarded
           << " budget=" << stats.budgetExhausted << " complete=" << stats.complete);
      CHECK(stats.available);
      CHECK(stats.changed);
      CHECK(stats.loadsForwarded == 1);
      CHECK(later.known->getTerminator()->getSuccessor(0) != later.join);
      CHECK(later.unknown->getTerminator()->getSuccessor(0) == later.join);
      CHECK(later.load->getParent() == later.success);
      CHECK(text(*pressure) == before);
      REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
    }
  }
}
