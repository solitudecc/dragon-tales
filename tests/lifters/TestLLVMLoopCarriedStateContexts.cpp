#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class OldValue { State, Load, Freeze, Calculation, SelfCarry, External };
enum class Effect { None, Store, Call, Volatile, Atomic, Fence };
struct Config {
  OldValue oldValue = OldValue::Load;
  Effect headEffect = Effect::None;
  unsigned bodies = 2;
  unsigned bridges = 1;
  bool knownEntries = false;
  bool outer = false;
  bool oldStateExpression = false;
  bool profitable = true;
};

void tag(llvm::Instruction *op, const char *name) {
  op->setMetadata(name, llvm::MDNode::get(op->getContext(), {}));
}

struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"loop-carried-state-contexts", context};
  llvm::Function *function;
  llvm::BasicBlock *head, *join, *table, *failure, *defaultExit, *outerExit;
  std::array<llvm::BasicBlock *, 3> initial;
  llvm::BasicBlock *unknown;
  std::vector<llvm::BasicBlock *> bodies;
  std::vector<llvm::BranchInst *> bodyBranches;
  std::vector<llvm::BasicBlock *> bridges;
  llvm::PHINode *state, *carry, *joinedState, *joinedFlags;
  llvm::FreezeInst *oldFreeze;
  llvm::LoadInst *oldLoad, *tableLoad;
  llvm::Value *oldCalculation, *oldNext, *historical;
  llvm::BranchInst *headBranch, *boundsBranch;
  llvm::SwitchInst *targetSwitch;
  Config config;

  explicit Fixture(Config selected = {}) : config(selected) {
    module.setDataLayout("e-p:64:64");
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *ptr = llvm::PointerType::getUnqual(context);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {i64, i64, i64, i64, i64, ptr, ptr, llvm::Type::getInt1Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage, "old_execution_states", module);
    for (unsigned index : {0u, 1u, 2u, 3u, 4u, 7u})
      function->getArg(index)->addAttr(llvm::Attribute::NoUndef);
    auto block = [&](const char *name) {return llvm::BasicBlock::Create(context, name, function);};
    auto *entry = block("entry");
    for (auto &item : initial) item = block("initial");
    unknown = block("unknown.incoming"); head = block("guard");
    for (unsigned n = 0; n < config.bridges; ++n) bridges.push_back(block("bridge"));
    join = block("checked.join"); table = block("actual.table");
    failure = block("bounds.failure"); defaultExit = block("unknown.target");
    outerExit = block("outer.failure");
    auto *guardFailure = block("state.failure");
    for (unsigned n = 0; n < config.bodies; ++n) bodies.push_back(block("body"));
    Builder b(entry);
    auto *routing = b.CreateSwitch(function->getArg(0), unknown, 3);
    for (unsigned n = 0; n < initial.size(); ++n)
      routing->addCase(b.getInt64(n), initial[n]);
    for (auto *item : initial) {b.SetInsertPoint(item); b.CreateBr(head);}
    b.SetInsertPoint(unknown); b.CreateBr(head);
    b.SetInsertPoint(head);
    state = b.CreatePHI(i64, 4 + config.bodies, "state");
    carry = b.CreatePHI(i64, 4 + config.bodies, "historical.input");
    const std::array<std::uint64_t, 3> seeds = {50, 16, 23};
    for (unsigned n = 0; n < initial.size(); ++n) {
      state->addIncoming(config.knownEntries ? static_cast<llvm::Value *>(b.getInt64(seeds[n]))
                                           : function->getArg(n + 1), initial[n]);
      carry->addIncoming(function->getArg(4), initial[n]);
    }
    state->addIncoming(function->getArg(3), unknown);
    carry->addIncoming(function->getArg(4), unknown);
    // This order makes a wrong substitution with this execution's LOAD or
    // calculation an invalid forward reference, not an unobserved identity.
    oldFreeze = llvm::cast<llvm::FreezeInst>(b.CreateFreeze(carry, "old.freeze"));
    tag(oldFreeze, "old-state.freeze");
    oldLoad = b.CreateLoad(i64, function->getArg(5), "old.load");
    oldLoad->setAlignment(llvm::Align(1)); tag(oldLoad, "old-state.load");
    oldCalculation = b.CreateAdd(oldLoad, oldFreeze, "old.calculation");
    tag(llvm::cast<llvm::Instruction>(oldCalculation), "old-state.calculation");
    oldNext = b.CreateAdd(state, b.getInt64(1), "unknown.old.next");
    auto *flags = b.CreateXor(b.CreateXor(state, oldFreeze), oldCalculation, "flags");
    if (config.headEffect == Effect::Store)
      b.CreateStore(state, function->getArg(5))->setAlignment(llvm::Align(1));
    if (config.headEffect == Effect::Call) {
      auto call = module.getOrInsertFunction("unknown_writer", llvm::FunctionType::get(b.getVoidTy(), {ptr}, false));
      b.CreateCall(call, {function->getArg(5)});
    }
    if (config.headEffect == Effect::Volatile) oldLoad->setVolatile(true);
    if (config.headEffect == Effect::Atomic) {
      oldLoad->setAtomic(llvm::AtomicOrdering::Acquire); oldLoad->setAlignment(llvm::Align(8));
    }
    if (config.headEffect == Effect::Fence) b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    headBranch = b.CreateCondBr(b.CreateICmpUGT(state, b.getInt64(50)), guardFailure, bridges.front());
    historical = config.oldValue == OldValue::State ? static_cast<llvm::Value *>(state)
               : config.oldValue == OldValue::Load ? static_cast<llvm::Value *>(oldLoad)
               : config.oldValue == OldValue::Freeze ? static_cast<llvm::Value *>(oldFreeze)
               : config.oldValue == OldValue::Calculation ? oldCalculation
               : config.oldValue == OldValue::SelfCarry ? static_cast<llvm::Value *>(carry)
                                                       : function->getArg(4);
    for (unsigned n = 0; n < bridges.size(); ++n) {
      b.SetInsertPoint(bridges[n]);
      auto *observed = b.CreateLoad(i64, function->getArg(6), "bridge.size");
      observed->setAlignment(llvm::Align(1));
      b.CreateBr(n + 1 < bridges.size() ? bridges[n + 1] : join);
    }
    b.SetInsertPoint(join);
    joinedState = config.profitable ? b.CreatePHI(i64, 2, "joined.state") : nullptr;
    if (joinedState) joinedState->addIncoming(state, bridges.back());
    joinedFlags = b.CreatePHI(i64, 2, "joined.flags");
    joinedFlags->addIncoming(flags, bridges.back());
    auto *size = b.CreateLoad(i64, function->getArg(6), "fresh.size");
    size->setAlignment(llvm::Align(1)); tag(size, "old-state.bounds.load");
    auto *index = b.CreateAdd(b.CreateShl(joinedState ? static_cast<llvm::Value *>(joinedState)
                                                   : function->getArg(3), b.getInt64(3)),
                              b.getInt64(0x400), "table.index");
    auto *fits = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
        b.CreateICmpULE(index, b.CreateAdd(size, b.getInt64(-8))));
    boundsBranch = b.CreateCondBr(fits, table, failure);
    tag(boundsBranch, "old-state.bounds");
    b.SetInsertPoint(table);
    auto *address = llvm::cast<llvm::GetElementPtrInst>(b.CreateGEP(b.getInt8Ty(), function->getArg(5), index, "table.address"));
    tag(address, "old-state.address");
    tableLoad = b.CreateLoad(i64, address, "actual.target");
    tableLoad->setAlignment(llvm::Align(1)); tag(tableLoad, "old-state.table.load");
    auto *nextJoinedState = b.CreateXor(joinedState ? static_cast<llvm::Value *>(joinedState)
                                                 : function->getArg(3), b.getInt64(1));
    targetSwitch = b.CreateSwitch(tableLoad, defaultExit, config.bodies + (joinedState ? 1 : 0));
    for (unsigned n = 0; n < bodies.size(); ++n)
      targetSwitch->addCase(b.getInt64(0x1000 + n), bodies[n]);
    if (joinedState) {
      targetSwitch->addCase(b.getInt64(0x2000), join);
      joinedState->addIncoming(nextJoinedState, table);
      joinedFlags->addIncoming(joinedFlags, table);
    }
    for (unsigned n = 0; n < bodies.size(); ++n) {
      b.SetInsertPoint(bodies[n]);
      // The old LOAD remains the pre-write value. The copied guard's LOAD
      // executes after this write and must not replace that historical input.
      auto *update = b.CreateAdd(oldLoad, b.getInt64(n + 1));
      b.CreateStore(update, function->getArg(5))->setAlignment(llvm::Align(1));
      bodyBranches.push_back(config.outer ? b.CreateCondBr(function->getArg(7), head, outerExit)
                                          : b.CreateBr(head));
      state->addIncoming(config.oldStateExpression ? oldNext : static_cast<llvm::Value *>(b.getInt64(n == 0 ? 16 : n == 1 ? 23 : n + 1)), bodies[n]);
      carry->addIncoming(historical, bodies[n]);
    }
    b.SetInsertPoint(targetSwitch);
    auto *defaultValue = b.CreateXor(joinedFlags, tableLoad, "default.value");
    auto result = [&](llvm::BasicBlock *where, llvm::BasicBlock *from, llvm::Value *value) {
      b.SetInsertPoint(where);
      auto *out = b.CreatePHI(i64, 1, "result.liveout"); out->addIncoming(value, from);
      auto *outside = b.CreatePHI(i64, 1, "external.liveout"); outside->addIncoming(function->getArg(4), from);
      b.CreateRet(b.CreateXor(out, outside));
    };
    result(guardFailure, head, flags); result(failure, join, joinedFlags);
    result(defaultExit, table, defaultValue);
    b.SetInsertPoint(outerExit); b.CreateRet(function->getArg(4));
  }
};

void verify(const llvm::Module &module) {
  std::string error; llvm::raw_string_ostream stream(error);
  const bool invalid = llvm::verifyModule(module, &stream);
  INFO(error); REQUIRE_FALSE(invalid);
}

std::string ir(const llvm::Module &module) {
  std::string text; llvm::raw_string_ostream stream(text); module.print(stream, nullptr); return text;
}

std::vector<llvm::Instruction *> marked(llvm::Function &function, const char *name) {
  std::vector<llvm::Instruction *> result;
  for (auto &block : function) for (auto &op : block)
    if (op.getMetadata(name)) result.push_back(&op);
  return result;
}

llvm::BasicBlock *copiedHead(Fixture &fixture, llvm::BasicBlock *origin) {
  auto *block = origin->getTerminator()->getSuccessor(0);
  for (unsigned step = 0; step != 4 && block != fixture.head; ++step) {
    for (auto &op : *block)
      if (op.getMetadata("old-state.freeze")) return block;
    auto *branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
    if (!branch || !branch->isUnconditional()) return nullptr;
    block = branch->getSuccessor(0);
  }
  return nullptr;
}

unsigned specializedBodies(Fixture &fixture) {
  unsigned count = 0;
  for (auto *body : fixture.bodies) count += copiedHead(fixture, body) != nullptr;
  return count;
}

void foldConstants(llvm::Module &module) {
  for (auto &function : module) for (auto &block : function)
    for (auto iterator = block.begin(); iterator != block.end();) {
      auto &op = *iterator++;
      if (op.isTerminator() || llvm::isa<llvm::PHINode>(op)) continue;
      if (auto *constant = llvm::ConstantFoldInstruction(&op, module.getDataLayout())) {
        op.replaceAllUsesWith(constant); op.eraseFromParent();
      }
    }
}

std::set<std::uint64_t> constantIndexes(Fixture &fixture) {
  foldConstants(fixture.module);
  std::set<std::uint64_t> result;
  for (auto *op : marked(*fixture.function, "old-state.address")) {
    auto *gep = llvm::cast<llvm::GetElementPtrInst>(op);
    if (auto *index = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1)))
      result.insert(index->getZExtValue());
  }
  return result;
}

void expectOldInputsPreserved(Fixture &fixture) {
  verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.available); CHECK(stats.complete); CHECK(stats.changed);
  CHECK(stats.branchesRewritten >= 2);
  verify(fixture.module);
  CHECK(specializedBodies(fixture) == fixture.bodies.size());
  CHECK(fixture.head->getTerminator() == fixture.headBranch);
  CHECK(fixture.headBranch->isConditional());
  CHECK(fixture.join->getTerminator() == fixture.boundsBranch);
  CHECK(fixture.boundsBranch->isConditional());
  CHECK(fixture.table->getTerminator() == fixture.targetSwitch);
  CHECK(fixture.targetSwitch->getDefaultDest() == fixture.defaultExit);
  for (auto *item : fixture.initial)
    CHECK(item->getTerminator()->getSuccessor(0) == fixture.head);
  CHECK(fixture.unknown->getTerminator()->getSuccessor(0) == fixture.head);
  auto freezes = marked(*fixture.function, "old-state.freeze");
  auto loads = marked(*fixture.function, "old-state.load");
  CHECK(freezes.size() == 1 + fixture.bodies.size());
  CHECK(loads.size() == freezes.size());
  llvm::DominatorTree dom(*fixture.function);
  for (unsigned n = 0; n < fixture.bodies.size(); ++n) {
    auto *copy = copiedHead(fixture, fixture.bodies[n]);
    if (!copy) continue;
    llvm::FreezeInst *freeze = nullptr;
    llvm::LoadInst *load = nullptr;
    for (auto &op : *copy) {
      if (op.getMetadata("old-state.freeze")) freeze = llvm::cast<llvm::FreezeInst>(&op);
      if (op.getMetadata("old-state.load")) load = llvm::cast<llvm::LoadInst>(&op);
    }
    REQUIRE(freeze); REQUIRE(load);
    CHECK(freeze != fixture.oldFreeze); CHECK(load != fixture.oldLoad);
    CHECK(freeze->getOperand(0) != freeze);
    CHECK(freeze->getOperand(0) != load);
    // An old state, read or freeze is not the just-selected state literal.
    // The unmodified original table may reach either body for any old state.
    CHECK_FALSE(llvm::isa<llvm::ConstantInt>(freeze->getOperand(0)));
    if (auto *input = llvm::dyn_cast<llvm::Instruction>(freeze->getOperand(0)))
      CHECK(dom.dominates(input, freeze));
    CHECK_FALSE(llvm::isa<llvm::UndefValue, llvm::PoisonValue>(freeze->getOperand(0)));
    if (fixture.config.outer) {
      CHECK(fixture.bodies[n]->getTerminator() == fixture.bodyBranches[n]);
      CHECK(fixture.bodyBranches[n]->getCondition() == fixture.function->getArg(7));
      CHECK(fixture.bodyBranches[n]->getSuccessor(1) == fixture.outerExit);
    }
  }
  const auto indexes = constantIndexes(fixture);
  CHECK(indexes.contains(0x400 + 16 * 8)); CHECK(indexes.contains(0x400 + 23 * 8));
  verify(fixture.module);
}
} // namespace

TEST_CASE("Loop state contexts capture historical LOAD freeze calculation and PHI values",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  for (auto value : {OldValue::State, OldValue::Load, OldValue::Freeze,
                     OldValue::Calculation, OldValue::SelfCarry}) {
    CAPTURE(static_cast<int>(value)); Config config; config.oldValue = value;
    Fixture fixture(config); expectOldInputsPreserved(fixture);
  }
}

TEST_CASE("Loop state contexts batch original known entries without requiring repeated passes",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  Config config; config.knownEntries = true; config.oldValue = OldValue::External;
  Fixture fixture(config); verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.changed); CHECK(stats.complete); CHECK(stats.branchesRewritten >= 3);
  verify(fixture.module);
  for (auto *entry : fixture.initial) CHECK(copiedHead(fixture, entry) != nullptr);
  CHECK(fixture.unknown->getTerminator()->getSuccessor(0) == fixture.head);
  const auto indexes = constantIndexes(fixture);
  for (auto state : {16u, 23u, 50u}) CHECK(indexes.contains(0x400 + state * 8));
  verify(fixture.module);
}

TEST_CASE("Loop state contexts retain the predecessor outer condition and old writes",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  Config config; config.outer = true; Fixture fixture(config);
  expectOldInputsPreserved(fixture);
}

TEST_CASE("Loop state proof never substitutes a new state for an old expression",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  Config config; config.oldStateExpression = true; Fixture fixture(config);
  verify(fixture.module); const auto before = ir(fixture.module);
  for (unsigned n = 0; n != 4; ++n) {
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK_FALSE(stats.changed); CHECK(stats.branchesRewritten == 0);
    CHECK(ir(fixture.module) == before); verify(fixture.module);
  }
}

TEST_CASE("Loop state contexts keep profitless old values and effectful guards conservative",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  for (auto effect : {Effect::None, Effect::Store, Effect::Call, Effect::Volatile,
                      Effect::Atomic, Effect::Fence}) {
    CAPTURE(static_cast<int>(effect)); Config config; config.headEffect = effect;
    config.profitable = effect != Effect::None; Fixture fixture(config);
    verify(fixture.module); const auto before = ir(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK_FALSE(stats.changed); CHECK(stats.branchesRewritten == 0);
    CHECK(ir(fixture.module) == before); verify(fixture.module);
  }
}

TEST_CASE("Loop state context batches retain unknown and overbudget original paths",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  Config config; config.bodies = 20; Fixture fixture(config);
  verify(fixture.module); const auto before = fixture.function->size();
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.changed); CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0);
  verify(fixture.module);
  const auto count = specializedBodies(fixture);
  CHECK(count > 0); CHECK(count < config.bodies);
  CHECK(fixture.function->size() - before <= 64);
  CHECK(fixture.unknown->getTerminator()->getSuccessor(0) == fixture.head);
  for (auto *entry : fixture.initial) CHECK(entry->getTerminator()->getSuccessor(0) == fixture.head);
}

TEST_CASE("Loop state snapshots fit the six block boundary without reloading old values",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  Config config; config.bridges = 2; Fixture fixture(config);
  expectOldInputsPreserved(fixture);
}

TEST_CASE("Loop state contexts remain stable after original known edges are consumed",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  Fixture fixture; verify(fixture.module);
  const auto first = runDispatcherSpecialization(fixture.module);
  CHECK(first.changed); CHECK(specializedBodies(fixture) == fixture.bodies.size());
  verify(fixture.module); const auto after = ir(fixture.module);
  for (unsigned n = 0; n != 5; ++n) {
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK_FALSE(stats.changed); CHECK(stats.branchesRewritten == 0);
    CHECK(ir(fixture.module) == after); verify(fixture.module);
  }
}

TEST_CASE("Loop state snapshot planning obeys zero public budgets without partial blocks",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  for (bool conditionLimit : {false, true}) {
    Fixture fixture; verify(fixture.module); const auto before = ir(fixture.module);
    DispatcherSpecializationOptions options;
    if (conditionLimit) options.maxConditions = 0;
    else options.maxExpressionNodes = 0;
    const auto stats = runDispatcherSpecialization(fixture.module, options);
    CHECK_FALSE(stats.changed); CHECK_FALSE(stats.complete);
    CHECK(stats.budgetExhausted > 0); CHECK(ir(fixture.module) == before);
    verify(fixture.module);
  }
}

TEST_CASE("Loop state snapshots preserve poison sensitive liveout operations",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  Config config; config.oldValue = OldValue::Calculation; Fixture fixture(config);
  llvm::cast<llvm::BinaryOperator>(fixture.oldCalculation)->setHasNoUnsignedWrap(true);
  expectOldInputsPreserved(fixture);
  for (auto *op : marked(*fixture.function, "old-state.calculation"))
    CHECK(llvm::cast<llvm::BinaryOperator>(op)->hasNoUnsignedWrap());
}

TEST_CASE("Loop state proof keeps flagged and unknown frozen conditions explicit",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  for (unsigned kind = 0; kind != 5; ++kind) {
    CAPTURE(kind); Fixture fixture;
    Builder b(fixture.headBranch);
    llvm::Value *condition = nullptr;
    if (kind == 0) {
      auto *small = llvm::cast<llvm::TruncInst>(b.CreateTrunc(fixture.state, b.getInt8Ty()));
      small->setHasNoUnsignedWrap(true);
      condition = b.CreateICmpUGT(b.CreateFreeze(small), b.getInt8(50));
    } else if (kind == 1) {
      auto *extended = llvm::cast<llvm::ZExtInst>(b.CreateZExt(
          b.CreateTrunc(fixture.state, b.getInt8Ty()), b.getInt64Ty()));
      extended->setNonNeg(true);
      condition = b.CreateICmpUGT(b.CreateFreeze(extended), b.getInt64(50));
    } else if (kind == 2) {
      auto *comparison = llvm::cast<llvm::ICmpInst>(b.CreateICmpUGT(fixture.state, b.getInt64(50)));
      comparison->setSameSign(); condition = b.CreateFreeze(comparison);
    } else if (kind == 3) {
      auto *overlap = llvm::cast<llvm::BinaryOperator>(b.CreateOr(fixture.state, b.getInt64(1)));
      llvm::cast<llvm::PossiblyDisjointInst>(overlap)->setIsDisjoint(true);
      condition = b.CreateICmpUGT(b.CreateFreeze(overlap), b.getInt64(50));
    } else {
      condition = b.CreateICmpUGT(b.CreateFreeze(b.CreateShl(fixture.state, b.getInt64(64))), b.getInt64(50));
    }
    fixture.headBranch->setCondition(condition);
    verify(fixture.module); const auto before = ir(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK_FALSE(stats.changed); CHECK(stats.branchesRewritten == 0);
    CHECK(ir(fixture.module) == before); verify(fixture.module);
  }
}

TEST_CASE("Loop state known self target does not grow profitless copy chains",
          "[llvm][dispatch-specialize][loop-carried-state-contexts]") {
  llvm::LLVMContext context;
  llvm::Module module("known-self-state", context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(i64,
      {llvm::Type::getInt1Ty(context), i64}, false), llvm::GlobalValue::ExternalLinkage,
      "known_self_state", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *known = llvm::BasicBlock::Create(context, "known.hundred", function);
  auto *unknown = llvm::BasicBlock::Create(context, "unknown", function);
  auto *loop = llvm::BasicBlock::Create(context, "loop", function);
  auto *exit = llvm::BasicBlock::Create(context, "exit", function);
  Builder b(entry); b.CreateCondBr(function->getArg(0), known, unknown);
  b.SetInsertPoint(known); b.CreateBr(loop);
  b.SetInsertPoint(unknown); b.CreateBr(loop);
  b.SetInsertPoint(loop);
  auto *state = b.CreatePHI(i64, 3, "state");
  state->addIncoming(b.getInt64(100), known);
  state->addIncoming(function->getArg(1), unknown);
  state->addIncoming(state, loop);
  b.CreateCondBr(b.CreateICmpUGT(state, b.getInt64(50)), loop, exit);
  b.SetInsertPoint(exit); b.CreateRet(state);
  verify(module); const auto before = ir(module); const auto blocks = function->size();
  // This is analysis-only. The known input's original infinite loop is never
  // given a native termination expectation or executed by this regression.
  for (unsigned n = 0; n != 8; ++n) {
    const auto stats = runDispatcherSpecialization(module);
    CHECK_FALSE(stats.changed); CHECK(stats.branchesRewritten == 0);
    CHECK(function->size() == blocks); CHECK(ir(module) == before); verify(module);
  }
}
