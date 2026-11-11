#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;
using namespace dragon::passes;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Kind { Integer, Pointer, Float, Vector, ConstantFloat, ConstantVector,
                  Undef, Poison, UndefAddress, PoisonAddress, OtherSpace };
enum class Effect { None, Volatile, Atomic, Call, Fence };
struct Config {
  unsigned width = 64;
  unsigned bodies = 1;
  unsigned bridges = 1;
  Kind kind = Kind::Integer;
  Effect effect = Effect::None;
  bool outer = false;
  bool flaggedCondition = false;
  bool bridgeStore = false;
};
void tag(llvm::Instruction *op, const char *name) {
  op->setMetadata(name, llvm::MDNode::get(op->getContext(), {}));
}
std::vector<llvm::Instruction *> marked(llvm::Function &f, const char *name) {
  std::vector<llvm::Instruction *> result;
  for (auto &block : f) for (auto &op : block) if (op.getMetadata(name)) result.push_back(&op);
  return result;
}
void verify(llvm::Module &m) {
  std::string error; llvm::raw_string_ostream out(error);
  const bool invalid = llvm::verifyModule(m, &out); INFO(error); REQUIRE_FALSE(invalid);
}
std::string text(llvm::Module &m) {
  std::string result; llvm::raw_string_ostream out(result); m.print(out, nullptr); return result;
}
struct Fixture {
  LLVMLifter lifter{Architecture::X86_64, "store-state-contexts"};
  llvm::Module &module = lifter.module();
  llvm::LLVMContext &context = module.getContext();
  llvm::Function *function;
  llvm::BasicBlock *known, *unknown, *head, *join, *table, *failure, *fallback, *outerFailure;
  llvm::PHINode *state, *history, *joinedState, *joinedFlags;
  llvm::BranchInst *headBranch, *boundsBranch, *knownBranch;
  llvm::SwitchInst *targetSwitch;
  llvm::StoreInst *firstStore, *secondStore;
  llvm::LoadInst *beforeStore, *afterStore, *tableLoad;
  std::vector<llvm::BasicBlock *> bodies;
  std::vector<llvm::BasicBlock *> bridges;
  Config config;

  explicit Fixture(Config selected = {}) : config(selected) {
    module.setDataLayout("e-p:64:64-p1:64:64");
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *destination = llvm::PointerType::get(context, config.kind == Kind::OtherSpace ? 1 : 0);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {ptr, destination, i64, i64, llvm::Type::getInt1Ty(context), i64}, false),
        llvm::GlobalValue::ExternalLinkage, "store_state", module);
    for (auto &arg : function->args()) arg.addAttr(llvm::Attribute::NoUndef);
    auto block = [&](const char *name) {return llvm::BasicBlock::Create(context, name, function);};
    auto *entry = block("entry"); known = block("known"); unknown = block("unknown");
    head = block("store.guard");
    for (unsigned n = 0; n != config.bridges; ++n) bridges.push_back(block("bridge"));
    join = block("checked.join"); table = block("actual.table");
    failure = block("failure"); fallback = block("target.default"); outerFailure = block("outer.failure");
    auto *guardFailure = block("state.failure"); auto *hit = block("payload");
    for (unsigned n = 0; n != config.bodies; ++n) bodies.push_back(block("body"));
    Builder b(entry); b.CreateCondBr(b.CreateICmpEQ(function->getArg(5), b.getInt64(0)), known, unknown);
    b.SetInsertPoint(known);
    knownBranch = config.outer ? b.CreateCondBr(function->getArg(4), head, outerFailure) : b.CreateBr(head);
    b.SetInsertPoint(unknown); b.CreateBr(head);
    b.SetInsertPoint(head);
    state = b.CreatePHI(i64, config.bodies + 2, "state");
    history = b.CreatePHI(i64, config.bodies + 2, "old.history");
    state->addIncoming(b.getInt64(50), known); state->addIncoming(function->getArg(2), unknown);
    history->addIncoming(function->getArg(3), known); history->addIncoming(function->getArg(3), unknown);
    beforeStore = b.CreateLoad(i64, function->getArg(0), "read.before.store");
    beforeStore->setAlignment(llvm::Align(1)); tag(beforeStore, "store.before.load");
    auto *fresh = b.CreateFreeze(history, "per.execution.freeze");
    tag(llvm::cast<llvm::Instruction>(fresh), "store.history.freeze");
    llvm::Value *value = b.CreateZExtOrTrunc(state, b.getIntNTy(config.width));
    if (config.kind == Kind::Pointer) value = function->getArg(0);
    if (config.kind == Kind::Float) value = b.CreateBitCast(state, b.getDoubleTy());
    if (config.kind == Kind::Vector)
      value = b.CreateInsertElement(llvm::Constant::getNullValue(llvm::FixedVectorType::get(i64, 2)), state, b.getInt32(0));
    if (config.kind == Kind::ConstantFloat) value = llvm::ConstantFP::get(b.getDoubleTy(), 2.5);
    if (config.kind == Kind::ConstantVector) value = llvm::Constant::getNullValue(llvm::FixedVectorType::get(i64, 2));
    if (config.kind == Kind::Undef) value = llvm::UndefValue::get(value->getType());
    if (config.kind == Kind::Poison) value = llvm::PoisonValue::get(value->getType());
    llvm::Value *address = function->getArg(1);
    if (config.kind == Kind::UndefAddress) address = llvm::UndefValue::get(address->getType());
    if (config.kind == Kind::PoisonAddress) address = llvm::PoisonValue::get(address->getType());
    if (config.effect == Effect::Call) {
      auto helper = module.getOrInsertFunction("unknown_writer", llvm::FunctionType::get(b.getVoidTy(), {ptr}, false));
      b.CreateCall(helper, {function->getArg(0)});
    }
    if (config.effect == Effect::Fence) b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    firstStore = b.CreateStore(value, address); firstStore->setAlignment(llvm::Align(1)); tag(firstStore, "store.first");
    if (config.effect == Effect::Volatile) firstStore->setVolatile(true);
    if (config.effect == Effect::Atomic) {
      firstStore->setAtomic(llvm::AtomicOrdering::Release); firstStore->setAlignment(llvm::Align(8));
    }
    afterStore = b.CreateLoad(i64, function->getArg(0), "read.after.store");
    afterStore->setAlignment(llvm::Align(1)); tag(afterStore, "store.after.load");
    auto *flags = b.CreateXor(fresh, b.CreateXor(beforeStore, afterStore), "flags");
    secondStore = b.CreateStore(flags, function->getArg(0)); secondStore->setAlignment(llvm::Align(1)); tag(secondStore, "store.second");
    llvm::Value *conditionState = state;
    if (config.flaggedCondition) {
      auto *bad = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(state, b.getInt64(~std::uint64_t{0})));
      bad->setHasNoUnsignedWrap(true); conditionState = b.CreateFreeze(bad);
    }
    headBranch = b.CreateCondBr(b.CreateICmpUGT(conditionState, b.getInt64(50)), guardFailure, bridges.front());
    for (unsigned n = 0; n != bridges.size(); ++n) {
      b.SetInsertPoint(bridges[n]);
      if (config.bridgeStore && n == 0) {
        auto *store = b.CreateStore(state, function->getArg(0)); store->setAlignment(llvm::Align(1)); tag(store, "store.bridge");
      }
      b.CreateBr(n + 1 == bridges.size() ? join : bridges[n + 1]);
    }
    b.SetInsertPoint(join);
    joinedState = b.CreatePHI(i64, 2, "joined.state"); joinedState->addIncoming(state, bridges.back());
    joinedFlags = b.CreatePHI(i64, 2, "joined.flags"); joinedFlags->addIncoming(flags, bridges.back());
    auto *index = b.CreateAdd(b.CreateShl(joinedState, b.getInt64(3)), b.getInt64(0x400));
    auto *fits = b.CreateAnd(b.CreateICmpUGT(function->getArg(3), b.getInt64(7)),
        b.CreateICmpULE(index, b.CreateAdd(function->getArg(3), b.getInt64(-8))));
    boundsBranch = b.CreateCondBr(fits, table, failure);
    b.SetInsertPoint(table);
    auto *tableAddress = b.CreateGEP(b.getInt8Ty(), function->getArg(0), index);
    tableLoad = b.CreateLoad(i64, tableAddress, "actual.target"); tableLoad->setAlignment(llvm::Align(1)); tag(tableLoad, "store.table.load");
    auto *reentryState = b.CreateXor(joinedState, b.getInt64(1));
    targetSwitch = b.CreateSwitch(tableLoad, fallback, config.bodies + 2);
    targetSwitch->addCase(b.getInt64(0x1000), hit); targetSwitch->addCase(b.getInt64(0x2000), join);
    joinedState->addIncoming(reentryState, table);
    joinedFlags->addIncoming(joinedFlags, table);
    for (unsigned n = 0; n != bodies.size(); ++n) {
      targetSwitch->addCase(b.getInt64(0x3000 + n), bodies[n]);
      b.SetInsertPoint(bodies[n]);
      auto *write = b.CreateStore(b.CreateAdd(tableLoad, b.getInt64(n + 1)), function->getArg(0)); write->setAlignment(llvm::Align(1));
      state->addIncoming(b.getInt64(n % 2 ? 23 : 16), bodies[n]);
      history->addIncoming(tableLoad, bodies[n]); b.CreateBr(head);
    }
    b.SetInsertPoint(guardFailure); b.CreateRet(flags);
    b.SetInsertPoint(failure); b.CreateRet(joinedFlags);
    b.SetInsertPoint(fallback); b.CreateRet(tableLoad);
    b.SetInsertPoint(hit); b.CreateRet(b.CreateAdd(tableLoad, joinedFlags));
    b.SetInsertPoint(outerFailure); b.CreateRet(function->getArg(3));
  }
  Report run(Options options = {{"allow_plain_stores", std::int64_t{1}}}) {
    PassPipeline pipeline; const bool added = pipeline.add("dispatch-specialize", std::move(options));
    INFO(pipeline.lastError()); REQUIRE(added); return pipeline.run(lifter);
  }
};
llvm::BasicBlock *copiedHead(Fixture &f, llvm::BasicBlock *pred) {
  auto *block = pred->getTerminator()->getSuccessor(0);
  for (unsigned n = 0; n != 3 && block != f.head; ++n) {
    for (auto &op : *block) if (op.getMetadata("store.first")) return block;
    auto *br = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!br || !br->isUnconditional()) return nullptr;
    block = br->getSuccessor(0);
  }
  return nullptr;
}
std::set<std::uint64_t> targetIndexes(Fixture &f) {
  for (unsigned round = 0; round != 8; ++round) {
    bool changed = false;
    for (auto &block : *f.function) for (auto it = block.begin(); it != block.end();) {
      auto &op = *it++;
      if (op.isTerminator() || op.getType()->isVoidTy()) continue;
      if (auto *constant = llvm::ConstantFoldInstruction(&op, f.module.getDataLayout())) {
        op.replaceAllUsesWith(constant); op.eraseFromParent(); changed = true;
      }
    }
    if (!changed) break;
  }
  std::set<std::uint64_t> result;
  for (auto *op : marked(*f.function, "store.table.load")) {
    auto *load = llvm::cast<llvm::LoadInst>(op);
    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand()))
      if (auto *index = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1))) result.insert(index->getZExtValue());
  }
  return result;
}
void checkCopies(Fixture &f) {
  verify(f.module); const auto report = f.run(); REQUIRE(report.ok); CHECK(report.changed); verify(f.module);
  auto *copy = copiedHead(f, f.known); REQUIRE(copy);
  std::vector<std::string> order;
  llvm::StoreInst *first = nullptr, *second = nullptr; llvm::LoadInst *before = nullptr, *after = nullptr;
  for (auto &op : *copy) {
    for (auto name : {"store.before.load", "store.first", "store.after.load", "store.second"})
      if (op.getMetadata(name)) order.emplace_back(name);
    if (op.getMetadata("store.first")) first = llvm::cast<llvm::StoreInst>(&op);
    if (op.getMetadata("store.second")) second = llvm::cast<llvm::StoreInst>(&op);
    if (op.getMetadata("store.before.load")) before = llvm::cast<llvm::LoadInst>(&op);
    if (op.getMetadata("store.after.load")) after = llvm::cast<llvm::LoadInst>(&op);
  }
  CHECK((order == std::vector<std::string>{"store.before.load", "store.first", "store.after.load", "store.second"}));
  REQUIRE(first); REQUIRE(second); REQUIRE(before); REQUIRE(after);
  CHECK(first->getValueOperand()->getType() == f.firstStore->getValueOperand()->getType());
  CHECK(first->getAlign() == f.firstStore->getAlign()); CHECK(second->getAlign() == f.secondStore->getAlign());
  CHECK(first->isSimple()); CHECK(second->isSimple()); CHECK(before != after);
  CHECK(before->getPointerOperand() == after->getPointerOperand());
  CHECK(first->getPointerOperand() == f.firstStore->getPointerOperand());
  CHECK(f.head->getTerminator() == f.headBranch); CHECK(f.headBranch->isConditional());
  CHECK(f.join->getTerminator() == f.boundsBranch); CHECK(f.boundsBranch->isConditional());
  CHECK(f.table->getTerminator() == f.targetSwitch); CHECK(f.targetSwitch->getDefaultDest() == f.fallback);
  CHECK(f.unknown->getTerminator()->getSuccessor(0) == f.head);
  if (f.config.outer) {CHECK(f.known->getTerminator() == f.knownBranch); CHECK(f.knownBranch->getSuccessor(1) == f.outerFailure);}
  for (auto *op : marked(*f.function, "store.table.load")) {
    auto *sw = llvm::dyn_cast<llvm::SwitchInst>(op->getParent()->getTerminator()); REQUIRE(sw);
    CHECK(sw->getCondition() == op); CHECK(sw->getDefaultDest() == f.fallback);
    CHECK(sw->getNumCases() == f.targetSwitch->getNumCases());
    CHECK(sw->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt64Ty(f.context), 0x2000))->getCaseSuccessor() == f.join);
  }
  const auto indexes = targetIndexes(f); CHECK(indexes.contains(0x400 + 50 * 8)); verify(f.module);
}
} // namespace

TEST_CASE("Store state contexts require explicit opt in and retain the original options layout",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  static_assert(sizeof(DispatcherSpecializationOptions) == 2 * sizeof(std::size_t));
  for (unsigned mode = 0; mode != 3; ++mode) {
    Fixture f; verify(f.module); const auto before = text(f.module);
    if (mode == 0) {const auto report = f.run({}); CHECK(report.ok); CHECK_FALSE(report.changed);}
    else if (mode == 1) {const auto stats = f.lifter.runDispatcherSpecialization(); CHECK_FALSE(stats.changed);}
    else {const auto stats = runDispatcherSpecialization(f.module, {}); CHECK_FALSE(stats.changed);}
    CHECK(text(f.module) == before); verify(f.module);
  }
}

TEST_CASE("Store state explicit zero executes the legacy behavior byte for byte",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  Fixture f; verify(f.module); const auto before = text(f.module);
  const auto report = f.run({{"allow_plain_stores", std::int64_t{0}}});
  CHECK(report.ok); CHECK_FALSE(report.changed); CHECK(text(f.module) == before); verify(f.module);
}

TEST_CASE("Store state contexts copy supported integer writes in the original event order",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  for (unsigned width : {8u, 16u, 32u, 64u, 128u}) {
    DYNAMIC_SECTION("STORE width " << width) {Config c; c.width = width; Fixture f(c); checkCopies(f);}
  }
}

TEST_CASE("Store state contexts preserve predecessor guards historical inputs and reentries",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  Config c; c.outer = true; c.bodies = 2; Fixture f(c); checkCopies(f);
  llvm::DominatorTree dom(*f.function);
  for (auto *body : f.bodies) {
    auto *copy = copiedHead(f, body); REQUIRE(copy);
    llvm::FreezeInst *freeze = nullptr;
    for (auto &op : *copy) if (op.getMetadata("store.history.freeze")) freeze = llvm::cast<llvm::FreezeInst>(&op);
    REQUIRE(freeze); auto *history = llvm::dyn_cast<llvm::Instruction>(freeze->getOperand(0)); REQUIRE(history);
    CHECK(dom.dominates(history, freeze)); CHECK(history->getParent() != copy);
  }
}

TEST_CASE("Store state contexts retain writes inside a bounded downstream context",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  Config c; c.bridgeStore = true; Fixture f(c); checkCopies(f);
  CHECK(marked(*f.function, "store.bridge").size() >= 2);
}

TEST_CASE("Store state contexts keep unsupported values addresses and synchronization as barriers",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  for (auto kind : {Kind::Pointer, Kind::Float, Kind::Vector, Kind::ConstantFloat, Kind::ConstantVector,
                    Kind::Undef, Kind::Poison, Kind::UndefAddress, Kind::PoisonAddress, Kind::OtherSpace}) {
    DYNAMIC_SECTION("unsupported STORE kind " << static_cast<int>(kind)) {
      Config c; c.kind = kind; Fixture f(c); verify(f.module); const auto before = text(f.module);
      const auto report = f.run(); CHECK(report.ok); CHECK_FALSE(report.changed); CHECK(text(f.module) == before); verify(f.module);
    }
  }
  for (unsigned width : {1u, 24u}) {
    DYNAMIC_SECTION("unsupported STORE width " << width) {
      Config c; c.width = width; Fixture f(c); verify(f.module); const auto before = text(f.module);
      const auto report = f.run(); CHECK(report.ok); CHECK_FALSE(report.changed); CHECK(text(f.module) == before); verify(f.module);
    }
  }
  for (auto effect : {Effect::Volatile, Effect::Atomic, Effect::Call, Effect::Fence}) {
    DYNAMIC_SECTION("effect barrier " << static_cast<int>(effect)) {
      Config c; c.effect = effect; Fixture f(c); verify(f.module); const auto before = text(f.module);
      const auto report = f.run(); CHECK(report.ok); CHECK_FALSE(report.changed); CHECK(text(f.module) == before); verify(f.module);
    }
  }
}

TEST_CASE("Store state copies retain dynamic freezes and poison producing arithmetic as written",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  // These are structural checks only. No native execution assigns a chosen
  // value to undef/freeze or to an overflowing poison-producing expression.
  for (bool useFreeze : {false, true}) {
    DYNAMIC_SECTION("undef freeze " << useFreeze) {
      Fixture f; Builder b(f.firstStore);
      llvm::Instruction *value = nullptr;
      if (useFreeze) value = llvm::cast<llvm::Instruction>(b.CreateFreeze(llvm::UndefValue::get(b.getInt64Ty())));
      else {
        auto *add = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(f.state, b.getInt64(~std::uint64_t{0})));
        add->setHasNoUnsignedWrap(true); value = add;
      }
      tag(value, "store.dynamic.value"); f.firstStore->setOperand(0, value);
      verify(f.module); const auto report = f.run(); REQUIRE(report.ok); CHECK(report.changed); verify(f.module);
      auto *copy = copiedHead(f, f.known); REQUIRE(copy);
      llvm::Instruction *fresh = nullptr; llvm::StoreInst *store = nullptr;
      for (auto &op : *copy) {
        if (op.getMetadata("store.dynamic.value")) fresh = &op;
        if (op.getMetadata("store.first")) store = llvm::cast<llvm::StoreInst>(&op);
      }
      REQUIRE(fresh); REQUIRE(store); CHECK(fresh != value); CHECK(store->getValueOperand() == fresh);
      if (useFreeze) {
        REQUIRE(llvm::isa<llvm::FreezeInst>(fresh)); CHECK(llvm::isa<llvm::UndefValue>(fresh->getOperand(0)));
      } else {
        REQUIRE(llvm::isa<llvm::BinaryOperator>(fresh)); CHECK(llvm::cast<llvm::BinaryOperator>(fresh)->hasNoUnsignedWrap());
      }
    }
  }
}

TEST_CASE("Store state contexts keep poison sensitive guard proofs conservative",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  Config c; c.flaggedCondition = true; Fixture f(c); verify(f.module); const auto before = text(f.module);
  const auto report = f.run(); CHECK(report.ok); CHECK_FALSE(report.changed); CHECK(text(f.module) == before); verify(f.module);
}

TEST_CASE("Store state context planning retains unknown and overbudget original paths",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  Config c; c.bodies = 20; Fixture f(c); verify(f.module); const auto blocks = f.function->size();
  const auto report = f.run(); REQUIRE(report.ok); CHECK(report.changed); verify(f.module);
  REQUIRE(report.runs.size() == 1); CHECK(report.runs[0].result.counters.at("budget_exhausted") > 0);
  CHECK(f.function->size() - blocks <= 64); CHECK(f.unknown->getTerminator()->getSuccessor(0) == f.head);
  CHECK(marked(*f.function, "store.table.load").size() > 1); CHECK(marked(*f.function, "store.table.load").size() < 22);
}

TEST_CASE("Store state context zero budgets leave the whole module untouched",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  for (auto name : {"max_conditions", "max_expression_nodes"}) {
    Fixture f; verify(f.module); const auto before = text(f.module);
    auto report = f.run({{"allow_plain_stores", std::int64_t{1}}, {name, std::int64_t{0}}});
    CHECK(report.ok); CHECK_FALSE(report.changed); CHECK(text(f.module) == before); verify(f.module);
    REQUIRE(report.runs.size() == 1); CHECK(report.runs[0].result.counters.at("budget_exhausted") > 0);
  }
}

TEST_CASE("Store state context flag validates exact numeric zero or one before mutation",
          "[llvm][dispatch-specialize][store-state-contexts]") {
  Fixture f; const auto before = text(f.module);
  for (const auto &options : {Options{{"allow_plain_stores", std::int64_t{-1}}},
       Options{{"allow_plain_stores", std::int64_t{2}}}, Options{{"allow_plain_stores", true}},
       Options{{"allow_plain_stores", std::string{"1"}}}}) {
    PassPipeline pipeline; CHECK_FALSE(pipeline.add("dispatch-specialize", options)); CHECK_FALSE(pipeline.lastError().empty());
  }
  for (auto flag : {std::int64_t{0}, std::int64_t{1}}) {
    PassPipeline pipeline; CHECK(pipeline.add("dispatch-specialize", {{"allow_plain_stores", flag}}));
  }
  CHECK(text(f.module) == before);
}
