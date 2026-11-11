#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class History { Phi, Load, Freeze, Calculation, Pointer, Narrow };
enum class Effect { None, Store, Call, Volatile, Atomic, Fence };
struct Config {
  History history = History::Load;
  Effect effect = Effect::None;
  unsigned bodies = 2;
  unsigned bridges = 1;
  bool outer = false;
  bool duplicate = false;
  bool distinct = false;
  bool unknownNext = false;
  bool ordinaryOldOperand = false;
  bool downstreamSelfCarry = false;
  bool profitable = true;
  bool flaggedCondition = false;
  bool flaggedHistory = false;
};

void mark(llvm::Instruction *op, const char *name) {
  op->setMetadata(name, llvm::MDNode::get(op->getContext(), {}));
}

struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"future-region-state-contexts", context};
  llvm::Function *function;
  llvm::BasicBlock *entry, *head, *join, *table, *guardFailure, *boundsFailure;
  llvm::BasicBlock *defaultExit, *outerExit;
  std::vector<llvm::BasicBlock *> bridges, bodies;
  std::vector<llvm::BranchInst *> bodyBranches;
  std::vector<llvm::StoreInst *> bodyStores;
  llvm::PHINode *state, *carry, *otherCarry, *joinedState, *joinedFlags;
  llvm::Value *history, *otherHistory;
  llvm::LoadInst *historicalLoad, *targetLoad;
  llvm::FreezeInst *historicalFreeze;
  llvm::Instruction *historicalCalculation;
  llvm::BranchInst *headBranch, *boundsBranch;
  llvm::SwitchInst *targetSwitch;
  Config config;

  explicit Fixture(Config chosen = {}) : config(chosen) {
    module.setDataLayout("e-p:64:64");
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *ptr = llvm::PointerType::getUnqual(context);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {i64, i64, i64, ptr, llvm::Type::getInt1Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage, "future_history", module);
    for (unsigned n : {0u, 1u, 2u, 4u}) function->getArg(n)->addAttr(llvm::Attribute::NoUndef);
    auto block = [&](const char *name) {return llvm::BasicBlock::Create(context, name, function);};
    entry = block("entry"); head = block("guard");
    for (unsigned n = 0; n < config.bridges; ++n) bridges.push_back(block("bridge"));
    join = block("checked.join"); table = block("actual.table");
    guardFailure = block("guard.failure"); boundsFailure = block("bounds.failure");
    defaultExit = block("unknown.target"); outerExit = block("outer.failure");
    for (unsigned n = 0; n < config.bodies; ++n) bodies.push_back(block("body"));
    const bool entryAtJoin = config.ordinaryOldOperand || config.downstreamSelfCarry;
    Builder b(entry); b.CreateBr(entryAtJoin ? join : head);
    auto byte = [&](llvm::Value *offset) {return b.CreateGEP(b.getInt8Ty(), function->getArg(3), offset);};
    auto i64Value = [&](llvm::Value *value) -> llvm::Value * {
      if (value->getType()->isPointerTy()) return b.CreatePtrToInt(value, i64);
      if (value->getType() != i64) return b.CreateZExt(value, i64);
      return value;
    };
    llvm::Type *historyType = i64;
    if (config.history == History::Pointer) historyType = ptr;
    if (config.history == History::Narrow) historyType = b.getInt16Ty();
    b.SetInsertPoint(head);
    state = b.CreatePHI(i64, config.bodies + !entryAtJoin, "state");
    carry = b.CreatePHI(historyType, config.bodies + !entryAtJoin, "history");
    otherCarry = (config.duplicate || config.distinct)
        ? b.CreatePHI(historyType, config.bodies + !entryAtJoin, "other.history") : nullptr;
    if (!entryAtJoin) {
      state->addIncoming(function->getArg(0), entry);
      llvm::Value *seed = historyType->isPointerTy() ? function->getArg(3)
                         : historyType == i64 ? function->getArg(1)
                         : b.CreateTrunc(function->getArg(1), historyType);
      // Entry conversion belongs on the incoming edge, not after these PHIs.
      if (auto *conversion = llvm::dyn_cast<llvm::Instruction>(seed)) conversion->moveBefore(entry->getTerminator()->getIterator());
      carry->addIncoming(seed, entry);
      if (otherCarry) otherCarry->addIncoming(seed, entry);
    }
    auto *headFreeze = llvm::cast<llvm::FreezeInst>(b.CreateFreeze(carry, "head.history.freeze"));
    mark(headFreeze, "future.history.use");
    llvm::Value *flags = b.CreateXor(state, i64Value(headFreeze), "flags");
    if (otherCarry) {
      auto *freeze = llvm::cast<llvm::FreezeInst>(b.CreateFreeze(otherCarry, "head.other.freeze"));
      mark(freeze, "future.other.use"); flags = b.CreateXor(flags, i64Value(freeze));
    }
    auto *ordinaryUse = llvm::cast<llvm::Instruction>(b.CreateXor(flags, b.getInt64(0), "ordinary.old.use"));
    mark(ordinaryUse, "future.ordinary.use");
    auto *condition = llvm::cast<llvm::ICmpInst>(b.CreateICmpUGT(state, b.getInt64(50)));
    if (config.flaggedCondition) condition->setSameSign();
    headBranch = b.CreateCondBr(condition, guardFailure, bridges.front());
    for (unsigned n = 0; n < bridges.size(); ++n) {
      b.SetInsertPoint(bridges[n]); b.CreateBr(n + 1 < bridges.size() ? bridges[n + 1] : join);
    }
    b.SetInsertPoint(join);
    joinedState = config.profitable ? b.CreatePHI(i64, 2 + entryAtJoin, "joined.state") : nullptr;
    if (joinedState) joinedState->addIncoming(state, bridges.back());
    joinedFlags = b.CreatePHI(i64, 2 + entryAtJoin, "joined.flags");
    joinedFlags->addIncoming(ordinaryUse, bridges.back());
    if (entryAtJoin) {
      if (joinedState) joinedState->addIncoming(function->getArg(0), entry);
      joinedFlags->addIncoming(function->getArg(1), entry);
    }
    if (config.downstreamSelfCarry) {
      auto *old = b.CreatePHI(i64, 3, "downstream.self");
      old->addIncoming(function->getArg(1), entry); old->addIncoming(old, bridges.back());
      old->addIncoming(old, table);
    }
    auto *index = b.CreateAdd(b.CreateShl(joinedState ? static_cast<llvm::Value *>(joinedState)
                                                   : function->getArg(0), b.getInt64(3)),
                              b.getInt64(0x400), "table.index");
    auto *size = function->getArg(2);
    auto *fits = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
        b.CreateICmpULE(index, b.CreateAdd(size, b.getInt64(-8))));
    boundsBranch = b.CreateCondBr(fits, table, boundsFailure); mark(boundsBranch, "future.bounds");
    b.SetInsertPoint(table);
    auto *historicalPhi = b.CreatePHI(i64, 1, "table.history.phi");
    historicalPhi->addIncoming(joinedState ? static_cast<llvm::Value *>(joinedState) : function->getArg(0), join);
    mark(historicalPhi, "future.history.phi");
    historicalLoad = b.CreateLoad(i64, byte(b.getInt64(0x100)), "table.history.load");
    historicalLoad->setAlignment(llvm::Align(1)); mark(historicalLoad, "future.history.load");
    historicalFreeze = llvm::cast<llvm::FreezeInst>(b.CreateFreeze(historicalLoad, "table.history.freeze"));
    mark(historicalFreeze, "future.history.freeze");
    historicalCalculation = llvm::cast<llvm::Instruction>(b.CreateAdd(historicalLoad, historicalFreeze, "table.history.add"));
    mark(historicalCalculation, "future.history.calculation");
    if (config.flaggedHistory) llvm::cast<llvm::BinaryOperator>(historicalCalculation)->setHasNoUnsignedWrap(true);
    auto *pointer = llvm::cast<llvm::Instruction>(byte(historicalPhi)); mark(pointer, "future.history.pointer");
    auto *narrow = llvm::cast<llvm::Instruction>(b.CreateTrunc(historicalCalculation, b.getInt16Ty())); mark(narrow, "future.history.narrow");
    history = config.history == History::Phi ? static_cast<llvm::Value *>(historicalPhi)
            : config.history == History::Freeze ? static_cast<llvm::Value *>(historicalFreeze)
            : config.history == History::Calculation ? historicalCalculation
            : config.history == History::Pointer ? pointer
            : config.history == History::Narrow ? narrow : historicalLoad;
    otherHistory = config.distinct ? historicalCalculation : history;
    if (config.ordinaryOldOperand) ordinaryUse->setOperand(1, historicalLoad);
    if (config.effect == Effect::Store) b.CreateStore(historicalPhi, byte(b.getInt64(0x108)))->setAlignment(llvm::Align(1));
    if (config.effect == Effect::Call) {
      auto call = module.getOrInsertFunction("unknown_writer", llvm::FunctionType::get(b.getVoidTy(), {ptr}, false));
      b.CreateCall(call, {function->getArg(3)});
    }
    if (config.effect == Effect::Volatile) historicalLoad->setVolatile(true);
    if (config.effect == Effect::Atomic) {
      historicalLoad->setAtomic(llvm::AtomicOrdering::Acquire); historicalLoad->setAlignment(llvm::Align(8));
    }
    if (config.effect == Effect::Fence) b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    auto *address = llvm::cast<llvm::Instruction>(byte(index)); mark(address, "future.table.address");
    targetLoad = b.CreateLoad(i64, address, "actual.target"); targetLoad->setAlignment(llvm::Align(1));
    mark(targetLoad, "future.table.load");
    auto *next = b.CreateXor(historicalPhi, b.getInt64(1));
    auto *unknownNext = b.CreateAdd(historicalPhi, b.getInt64(1), "unknown.next.state");
    if (joinedState) joinedState->addIncoming(next, table);
    joinedFlags->addIncoming(joinedFlags, table);
    targetSwitch = b.CreateSwitch(targetLoad, defaultExit, config.bodies + 1);
    for (unsigned n = 0; n < bodies.size(); ++n) targetSwitch->addCase(b.getInt64(0x1000 + n), bodies[n]);
    targetSwitch->addCase(b.getInt64(0x2000), join); mark(targetSwitch, "future.target.switch");
    for (unsigned n = 0; n < bodies.size(); ++n) {
      b.SetInsertPoint(bodies[n]);
      auto *write = b.CreateStore(b.CreateAdd(historicalLoad, b.getInt64(n + 1)), byte(b.getInt64(0x100)));
      write->setAlignment(llvm::Align(1)); bodyStores.push_back(write);
      bodyBranches.push_back(config.outer ? b.CreateCondBr(function->getArg(4), head, outerExit) : b.CreateBr(head));
      state->addIncoming(config.unknownNext ? unknownNext : static_cast<llvm::Value *>(b.getInt64(n == 0 ? 16 : n == 1 ? 23 : n + 1)), bodies[n]);
      carry->addIncoming(history, bodies[n]);
      if (otherCarry) otherCarry->addIncoming(otherHistory, bodies[n]);
    }
    auto result = [&](llvm::BasicBlock *where, llvm::BasicBlock *from, llvm::Value *value) {
      b.SetInsertPoint(where); auto *out = b.CreatePHI(i64, 1, "outside.value");
      out->addIncoming(value, from); b.CreateRet(b.CreateXor(out, function->getArg(1)));
    };
    result(guardFailure, head, ordinaryUse); result(boundsFailure, join, joinedFlags);
    result(defaultExit, table, historicalLoad);
    b.SetInsertPoint(outerExit); b.CreateRet(function->getArg(1));
  }
};

void verify(const llvm::Module &module) {
  std::string error; llvm::raw_string_ostream out(error);
  const bool invalid = llvm::verifyModule(module, &out); INFO(error); REQUIRE_FALSE(invalid);
}

std::string ir(const llvm::Module &module) {
  std::string text; llvm::raw_string_ostream out(text); module.print(out, nullptr); return text;
}

std::vector<llvm::Instruction *> marked(llvm::Function &function, const char *name) {
  std::vector<llvm::Instruction *> result;
  for (auto &block : function) for (auto &op : block) if (op.getMetadata(name)) result.push_back(&op);
  return result;
}

llvm::BasicBlock *copiedHead(Fixture &fixture, llvm::BasicBlock *body) {
  auto *block = body->getTerminator()->getSuccessor(0);
  for (unsigned step = 0; step != 3 && block != fixture.head; ++step) {
    for (auto &op : *block) if (op.getMetadata("future.history.use")) return block;
    auto *branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch || !branch->isUnconditional()) return nullptr;
    block = branch->getSuccessor(0);
  }
  return nullptr;
}

std::set<std::uint64_t> targetIndexes(Fixture &fixture) {
  // Fold on the disposable fixture only after SSA identity assertions.
  for (unsigned round = 0; round != 8; ++round) {
    bool changed = false;
    for (auto &block : *fixture.function) for (auto it = block.begin(); it != block.end();) {
      auto &op = *it++;
      if (op.isTerminator() || op.getType()->isVoidTy() || llvm::isa<llvm::PHINode>(op)) continue;
      if (auto *constant = llvm::ConstantFoldInstruction(&op, fixture.module.getDataLayout())) {
        op.replaceAllUsesWith(constant); op.eraseFromParent(); changed = true;
      }
    }
    if (!changed) break;
  }
  std::set<std::uint64_t> result;
  for (auto *op : marked(*fixture.function, "future.table.load")) {
    auto *load = llvm::cast<llvm::LoadInst>(op);
    auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
    if (gep) if (auto *value = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1))) result.insert(value->getZExtValue());
  }
  return result;
}

void expectCompleteContexts(Fixture &fixture) {
  verify(fixture.module); const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.changed); CHECK(stats.complete); CHECK(stats.branchesRewritten >= fixture.bodies.size()); verify(fixture.module);
  CHECK(fixture.head->getTerminator() == fixture.headBranch); CHECK(fixture.headBranch->isConditional());
  CHECK(fixture.join->getTerminator() == fixture.boundsBranch); CHECK(fixture.boundsBranch->isConditional());
  CHECK(fixture.table->getTerminator() == fixture.targetSwitch); CHECK(fixture.targetSwitch->getDefaultDest() == fixture.defaultExit);
  CHECK(fixture.entry->getTerminator()->getSuccessor(0) == fixture.head);
  CHECK(marked(*fixture.function, "future.table.load").size() == 1 + fixture.bodies.size());
  CHECK(marked(*fixture.function, "future.history.load").size() == 1 + fixture.bodies.size());
  llvm::DominatorTree dom(*fixture.function);
  for (unsigned n = 0; n < fixture.bodies.size(); ++n) {
    CAPTURE(n);
    auto *copy = copiedHead(fixture, fixture.bodies[n]); REQUIRE(copy);
    auto *entry = fixture.bodies[n]->getTerminator()->getSuccessor(0);
    // The first edge still carries direct S definitions and needs a snapshot.
    // Its repair can make later edges' historical values available as external
    // predecessor PHIs, which may be used directly without a second snapshot.
    if (n == 0) REQUIRE(entry != copy);
    if (entry != copy) {
      CHECK(llvm::pred_size(entry) == 1); CHECK(*llvm::pred_begin(entry) == fixture.bodies[n]);
      const auto expected = fixture.config.distinct ? 2u : 1u;
      CHECK(static_cast<unsigned>(std::distance(entry->phis().begin(), entry->phis().end())) == expected);
      for (auto &phi : entry->phis()) {
        CHECK(phi.getNumIncomingValues() == 1); CHECK(phi.getIncomingBlock(0) == fixture.bodies[n]);
        auto *incoming = llvm::dyn_cast<llvm::Instruction>(phi.getIncomingValue(0)); REQUIRE(incoming);
        CHECK(dom.dominates(incoming, fixture.bodies[n]->getTerminator()));
        CHECK(incoming->getParent() != copy); CHECK(incoming->getParent() != entry);
      }
    }
    llvm::FreezeInst *freeze = nullptr, *other = nullptr;
    for (auto &op : *copy) {
      if (op.getMetadata("future.history.use")) freeze = llvm::cast<llvm::FreezeInst>(&op);
      if (op.getMetadata("future.other.use")) other = llvm::cast<llvm::FreezeInst>(&op);
    }
    REQUIRE(freeze);
    auto *historical = llvm::dyn_cast<llvm::Instruction>(freeze->getOperand(0)); REQUIRE(historical);
    if (entry != copy) {
      auto *snapshot = llvm::dyn_cast<llvm::PHINode>(historical); REQUIRE(snapshot);
      CHECK(snapshot->getParent() == entry);
    } else {
      CHECK(dom.dominates(historical, fixture.bodies[n]->getTerminator()));
      CHECK(historical->getParent() != copy);
    }
    CHECK(dom.dominates(historical, freeze));
    if (fixture.config.duplicate) {REQUIRE(other); CHECK(other->getOperand(0) == historical);}
    if (fixture.config.distinct) {REQUIRE(other); CHECK(other->getOperand(0) != historical);}
    CHECK(fixture.bodyStores[n]->getParent() == fixture.bodies[n]);
    if (fixture.config.outer) {
      CHECK(fixture.bodies[n]->getTerminator() == fixture.bodyBranches[n]);
      CHECK(fixture.bodyBranches[n]->getCondition() == fixture.function->getArg(4));
      CHECK(fixture.bodyBranches[n]->getSuccessor(1) == fixture.outerExit);
    }
  }
  for (auto *op : marked(*fixture.function, "future.target.switch")) {
    auto *sw = llvm::cast<llvm::SwitchInst>(op);
    llvm::LoadInst *selector = nullptr;
    for (auto &instruction : *sw->getParent())
      if (instruction.getMetadata("future.table.load")) selector = llvm::cast<llvm::LoadInst>(&instruction);
    REQUIRE(selector); CHECK(sw->getCondition() == selector);
    CHECK(sw->getDefaultDest() == fixture.defaultExit);
    CHECK(sw->getNumCases() == fixture.targetSwitch->getNumCases());
    for (auto item : sw->cases()) if (item.getCaseValue()->getZExtValue() == 0x2000) {
      CHECK(item.getCaseSuccessor() == fixture.join);
      CHECK(fixture.joinedFlags->getBasicBlockIndex(sw->getParent()) >= 0);
    }
  }
  const auto indexes = targetIndexes(fixture);
  CHECK(indexes.contains(0x400 + 16 * 8)); CHECK(indexes.contains(0x400 + 23 * 8)); verify(fixture.module);
}
} // namespace

TEST_CASE("Future region contexts snapshot prior table PHI LOAD freeze and calculation inputs",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  for (auto history : {History::Phi, History::Load, History::Freeze, History::Calculation}) {
    DYNAMIC_SECTION("historical value " << static_cast<int>(history)) {
      Config config; config.history = history; Fixture fixture(config); expectCompleteContexts(fixture);
    }
  }
}

TEST_CASE("Future region snapshots keep pointer and narrow historical identities",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  for (auto history : {History::Pointer, History::Narrow}) {
    DYNAMIC_SECTION("historical type " << static_cast<int>(history)) {
      Config config; config.history = history; Fixture fixture(config); expectCompleteContexts(fixture);
    }
  }
}

TEST_CASE("Future region snapshots deduplicate equal inputs and retain distinct definitions",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  for (bool distinct : {false, true}) {
    DYNAMIC_SECTION("distinct definitions " << distinct) {
      Config config; config.duplicate = !distinct; config.distinct = distinct; Fixture fixture(config);
      expectCompleteContexts(fixture);
    }
  }
}

TEST_CASE("Future region contexts retain outside writes outer guards and fresh reentries",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  Config config; config.outer = true; Fixture fixture(config); expectCompleteContexts(fixture);
}

TEST_CASE("Future region snapshots fit six blocks and truncate without inventing a reload",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  SECTION("Snapshot and full context fit six blocks") {
    Config config; config.bridges = 2; Fixture fixture(config); expectCompleteContexts(fixture);
  }
  SECTION("An uncopied future block no longer needs a snapshot") {
    Config config; config.bridges = 3; Fixture fixture(config); verify(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0); verify(fixture.module);
    CHECK(marked(*fixture.function, "future.table.load").size() == 1);
    for (auto *body : fixture.bodies) {
      auto *next = body->getTerminator()->getSuccessor(0);
      if (next != fixture.head) CHECK(next->phis().empty());
    }
  }
}

TEST_CASE("Future region context batches retain unknown paths at the module copy cap",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  Config config; config.bodies = 20; Fixture fixture(config); verify(fixture.module);
  const auto before = fixture.function->size(); const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.changed); CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0); verify(fixture.module);
  const auto count = marked(*fixture.function, "future.table.load").size(); CHECK(count > 1); CHECK(count < 21);
  CHECK(fixture.function->size() - before <= 64);
  CHECK(fixture.entry->getTerminator()->getSuccessor(0) == fixture.head);
}

TEST_CASE("Future region planning respects zero public budgets without partial state",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  for (bool conditions : {false, true}) {
    Fixture fixture; verify(fixture.module); const auto before = ir(fixture.module);
    DispatcherSpecializationOptions options;
    if (conditions) options.maxConditions = 0; else options.maxExpressionNodes = 0;
    const auto stats = runDispatcherSpecialization(fixture.module, options);
    CHECK_FALSE(stats.changed); CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0);
    CHECK(ir(fixture.module) == before); verify(fixture.module);
  }
}

TEST_CASE("Future region effect barriers keep actual table loads in their original blocks",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  for (auto effect : {Effect::Store, Effect::Call, Effect::Volatile, Effect::Atomic, Effect::Fence}) {
    CAPTURE(static_cast<int>(effect)); Config config; config.effect = effect; Fixture fixture(config);
    verify(fixture.module); runDispatcherSpecialization(fixture.module); verify(fixture.module);
    CHECK(marked(*fixture.function, "future.table.load").size() == 1);
    CHECK(fixture.targetLoad->getParent() == fixture.table);
    CHECK(fixture.historicalLoad->getParent() == fixture.table);
    CHECK(fixture.table->getTerminator() == fixture.targetSwitch);
  }
}

TEST_CASE("Future region ordinary old operands and downstream self carried PHIs stay conservative",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  for (bool ordinary : {false, true}) {
    Config config; config.ordinaryOldOperand = ordinary; config.downstreamSelfCarry = !ordinary;
    Fixture fixture(config); verify(fixture.module); runDispatcherSpecialization(fixture.module); verify(fixture.module);
    CHECK(marked(*fixture.function, "future.table.load").size() == 1);
    CHECK(fixture.targetLoad->getParent() == fixture.table);
  }
}

TEST_CASE("Future region proof rejects unknown historical state and poison sensitive conditions",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  for (bool flagged : {false, true}) {
    Config config; config.flaggedCondition = flagged; config.unknownNext = !flagged;
    Fixture fixture(config); verify(fixture.module); const auto before = ir(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK_FALSE(stats.changed); CHECK(stats.branchesRewritten == 0); CHECK(ir(fixture.module) == before);
    verify(fixture.module);
  }
}

TEST_CASE("Future region snapshots preserve flagged liveouts without proving their values",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  Config config; config.history = History::Calculation; config.flaggedHistory = true; Fixture fixture(config);
  expectCompleteContexts(fixture);
  for (auto *op : marked(*fixture.function, "future.history.calculation")) CHECK(llvm::cast<llvm::BinaryOperator>(op)->hasNoUnsignedWrap());
}

TEST_CASE("Future region snapshots require an actual downstream constant context",
          "[llvm][dispatch-specialize][future-region-state-contexts]") {
  Config config; config.profitable = false; Fixture fixture(config); verify(fixture.module);
  runDispatcherSpecialization(fixture.module); verify(fixture.module);
  // The historical one-block fallback may still fold one guard; it must not
  // create an unprofitable extended context or snapshot an uncopied S value.
  CHECK(marked(*fixture.function, "future.table.load").size() == 1);
  CHECK(fixture.table->getTerminator() == fixture.targetSwitch);
  for (auto *body : fixture.bodies) {
    auto *next = body->getTerminator()->getSuccessor(0);
    if (next != fixture.head) CHECK(next->phis().empty());
  }
}
