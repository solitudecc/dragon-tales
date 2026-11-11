#include <catch2/catch_test_macros.hpp>
#include "../../lib/GuestMemoryEdges.hpp"
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

using namespace dragon::llvmlifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class History { Add, Load, Freeze };
struct Config {
  History history = History::Add;
  unsigned hubs = 1;
  unsigned padding = 0;
  bool mismatch = false;
  bool effect = false;
  bool ordinaryOldUse = false;
  bool poisonedAddress = false;
  bool downstreamSelf = false;
  bool freshAddress = false;
};
void mark(llvm::Instruction *op, const char *key) {
  op->setMetadata(key, llvm::MDNode::get(op->getContext(), {}));
}
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"guest-history", context};
  llvm::Function *function;
  llvm::BasicBlock *state, *checked, *body, *unknown, *done;
  std::vector<llvm::BasicBlock *> hubs;
  llvm::Value *base, *fresh;
  llvm::PHINode *frame, *history;
  llvm::LoadInst *target;
  llvm::StoreInst *store;
  explicit Fixture(Config config = {}) {
    module.setDataLayout("e-p:64:64");
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *ptr = llvm::PointerType::getUnqual(context);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {ptr, ptr, i64, i64, llvm::Type::getInt1Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage, "history_checked", module);
    auto block = [&](const char *n) { return llvm::BasicBlock::Create(context, n, function); };
    auto *entry = block("entry"); state = block("state"); checked = block("checked");
    body = block("known.store"); unknown = block("unknown"); done = block("done");
    for (unsigned n = 0; n < config.hubs; ++n) hubs.push_back(block("hub"));
    std::vector<llvm::BasicBlock *> other;
    for (unsigned n = 0; n < config.hubs; ++n) other.push_back(block("other"));
    Builder b(entry);
    base = b.CreateLoad(ptr, function->getArg(1), "guest.base");
    llvm::cast<llvm::LoadInst>(base)->setAlignment(llvm::Align(1));
    b.CreateBr(state);
    b.SetInsertPoint(state);
    frame = b.CreatePHI(i64, 2, "frame"); history = b.CreatePHI(i64, 2, "history");
    frame->addIncoming(function->getArg(2), entry); history->addIncoming(b.getInt64(0), entry);
    auto *index = b.CreateAdd(frame, b.getInt64(16), "index");
    if (config.poisonedAddress) llvm::cast<llvm::BinaryOperator>(index)->setHasNoUnsignedWrap();
    auto *freshIndex = b.CreateAdd(frame, b.getInt64(32), "fresh.index");
    auto *freshPointer = b.CreateGEP(b.getInt8Ty(), base, freshIndex, "fresh.ptr");
    if (config.history == History::Add) fresh = b.CreateAdd(frame, function->getArg(3), "fresh");
    else {
      auto *value = b.CreateLoad(i64, freshPointer, "fresh.load");
      value->setAlignment(llvm::Align(1)); mark(value, "history.fresh.load");
      fresh = config.history == History::Freeze ? b.CreateFreeze(value, "fresh.freeze") : value;
    }
    mark(llvm::cast<llvm::Instruction>(fresh), "history.fresh");
    if (config.freshAddress) index = fresh;
    for (unsigned n = 0; n < config.padding; ++n) b.CreateXor(fresh, b.getInt64(n), "padding");
    b.CreateCondBr(b.CreateICmpULT(index, function->getArg(3)), checked, done);
    b.SetInsertPoint(checked);
    auto *slot = b.CreateGEP(b.getInt8Ty(), base, index, "slot");
    target = b.CreateLoad(i64, slot, "target"); target->setAlignment(llvm::Align(1));
    mark(target, "history.target");
    b.CreateCondBr(function->getArg(4), body, unknown);
    b.SetInsertPoint(body);
    b.CreateStore(b.getInt64(99), freshPointer)->setAlignment(llvm::Align(1));
    llvm::Value *storedIndex = index;
    if (config.mismatch) storedIndex = b.CreateAdd(index, b.getInt64(8), "wrong.index");
    auto *storedPointer = b.CreateGEP(b.getInt8Ty(), base, storedIndex, "stored.ptr");
    store = b.CreateStore(b.getInt64(14), storedPointer); store->setAlignment(llvm::Align(1));
    mark(store, "history.store");
    b.CreateBr(hubs.front());
    b.SetInsertPoint(unknown);
    auto *choose = b.CreateSwitch(function->getArg(3), other.front(), other.size() - 1);
    for (unsigned n = 1; n < other.size(); ++n) choose->addCase(b.getInt64(n), other[n]);
    std::vector<llvm::PHINode *> frames, histories;
    for (unsigned n = 0; n < hubs.size(); ++n) {
      b.SetInsertPoint(other[n]);
      auto *changedFrame = b.CreateAdd(frame, b.getInt64(8), "other.frame");
      b.CreateBr(hubs[n]);
      b.SetInsertPoint(hubs[n]);
      auto *f = b.CreatePHI(i64, 2, "hub.frame");
      auto *h = b.CreatePHI(i64, 2, "hub.history");
      f->addIncoming(n ? static_cast<llvm::Value *>(frames.back()) : frame, n ? hubs[n-1] : body);
      h->addIncoming(n ? static_cast<llvm::Value *>(histories.back()) : fresh, n ? hubs[n-1] : body);
      f->addIncoming(changedFrame, other[n]); h->addIncoming(b.getInt64(7), other[n]);
      if (config.ordinaryOldUse && n == 0) {
        auto *old = b.CreateAdd(fresh, b.getInt64(1), "ordinary.old");
        h = b.CreatePHI(i64, 2, "unused.history");
        h->moveBefore(f->getIterator());
        h->addIncoming(b.getInt64(0), body); h->addIncoming(b.getInt64(0), other[n]);
        mark(llvm::cast<llvm::Instruction>(old), "history.ordinary.old");
      }
      if (config.effect && n == 0)
        b.CreateStore(b.getInt64(23), freshPointer)->setAlignment(llvm::Align(1));
      frames.push_back(f); histories.push_back(h);
      b.CreateBr(n + 1 < hubs.size() ? hubs[n+1] : state);
    }
    frame->addIncoming(frames.back(), hubs.back()); history->addIncoming(histories.back(), hubs.back());
    if (config.downstreamSelf) frame->setIncomingValue(frame->getBasicBlockIndex(hubs.back()), frame);
    b.SetInsertPoint(done); b.CreateRet(b.CreateXor(history, fresh));
  }
};
void verify(Fixture &f) { REQUIRE_FALSE(llvm::verifyModule(f.module, &llvm::errs())); }
unsigned marked(Fixture &f, const char *key) {
  unsigned count = 0; for (auto &b : *f.function) for (auto &i : b) count += i.getMetadata(key) != nullptr; return count;
}
std::string text(Fixture &f) {std::string s; llvm::raw_string_ostream out(s); f.module.print(out,nullptr);return s;}
MemoryForwardStats run(Fixture &f, detail::CheckedMemoryEdgeBudget &budget) {
  MemoryForwardStats stats;
  detail::forwardCheckedMemoryEdges(*f.function, {f.base}, f.module.getDataLayout(),
      MemoryForwardOptions{65536, false}, stats, budget);
  return stats;
}
void positive(Config config) {
  Fixture f(config); verify(f); const auto size = f.function->size();
  detail::CheckedMemoryEdgeBudget budget; const auto stats = run(f, budget); verify(f);
  REQUIRE(stats.loadsForwarded == 1); CHECK(stats.changed); CHECK(stats.complete);
  auto *snapshot = f.body->getTerminator()->getSuccessor(0);
  REQUIRE(snapshot != f.hubs.front()); REQUIRE(snapshot->getName().contains("guest.inputs"));
  CHECK(snapshot->phis().begin() != snapshot->phis().end());
  CHECK(f.function->size() - size == config.hubs + 3);
  CHECK(marked(f, "history.target") == 1); CHECK(marked(f, "history.store") == 1);
  CHECK(marked(f, "history.fresh") == 2);
  if (config.history != History::Add) CHECK(marked(f, "history.fresh.load") == 2);
  CHECK(f.target->getParent() == f.checked); CHECK(f.store->getParent() == f.body);
  CHECK(f.hubs.front()->phis().begin()->getBasicBlockIndex(f.body) == -1);
  llvm::DominatorTree dom(*f.function);
  for (auto &phi : snapshot->phis()) {
    REQUIRE(phi.getNumIncomingValues() == 1); CHECK(phi.getIncomingBlock(0) == f.body);
    auto *old = llvm::dyn_cast<llvm::Instruction>(phi.getIncomingValue(0)); REQUIRE(old);
    CHECK(dom.dominates(old, f.body->getTerminator())); CHECK(!old->getParent()->getName().contains("guest.edge"));
  }
}
}
TEST_CASE("Checked Guest history snapshots retain future frame and fresh observations", "[llvm][guest-memory-ssa][guest-memory-history]") {
  for (auto history : {History::Add, History::Load, History::Freeze})
    for (unsigned hubs : {1u, 2u}) DYNAMIC_SECTION("history " << static_cast<int>(history) << " hubs " << hubs) {
      Config config; config.history = history; config.hubs = hubs; positive(config);
    }
}
TEST_CASE("Checked Guest history retains mismatched addresses and effect barriers", "[llvm][guest-memory-ssa][guest-memory-history]") {
  for (unsigned kind = 0; kind < 7; ++kind) DYNAMIC_SECTION("negative " << kind) {
    Config config; config.mismatch = kind == 0; config.effect = kind == 1;
    config.ordinaryOldUse = kind == 2; config.poisonedAddress = kind == 3; config.downstreamSelf = kind == 4;
    config.freshAddress = kind >= 5;
    if (kind >= 5) config.history = kind == 5 ? History::Load : History::Freeze;
    Fixture f(config); verify(f); const auto before = text(f);
    detail::CheckedMemoryEdgeBudget budget; auto stats = run(f,budget); verify(f);
    CHECK(stats.loadsForwarded == 0); CHECK_FALSE(stats.changed); CHECK(text(f) == before);
  }
}
TEST_CASE("Checked Guest history snapshots obey block instruction and repair caps", "[llvm][guest-memory-ssa][guest-memory-history]") {
  for (unsigned kind = 0; kind < 4; ++kind) DYNAMIC_SECTION("budget " << kind) {
    Config config; config.hubs = kind == 0 ? 4 : 1; config.padding = kind == 1 ? 240 : 0;
    Fixture f(config); verify(f); const auto before = text(f);
    detail::CheckedMemoryEdgeBudget budget;
    if (kind == 2) budget.clonedInstructions = 16383;
    if (kind == 3) budget.repairCells = 262144;
    auto stats = run(f,budget); verify(f);
    CHECK(stats.loadsForwarded == 0); CHECK_FALSE(stats.changed); CHECK_FALSE(stats.complete);
    CHECK(stats.budgetExhausted > 0); CHECK(text(f) == before);
  }
}
