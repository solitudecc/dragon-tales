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
#include <string>
#include <unordered_set>
#include <vector>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;

enum class Barrier { None, Call, Store, Volatile, Atomic, Fence, Alloca };
enum class Reentry { None, Head, Dispatch, Both };
struct Config {
  unsigned width = 64;
  unsigned bridges = 1;
  bool outer = false;
  bool unknownBounds = true;
  bool outsideArgument = false;
  bool flaggedLiveOut = false;
  bool poisonState = false;
  Barrier barrier = Barrier::None;
  Reentry reentry = Reentry::None;
};

void mark(llvm::Instruction *instruction, const char *name) {
  instruction->setMetadata(name, llvm::MDNode::get(instruction->getContext(), {}));
}

// The shared bridge is deliberately reached by every state. A proof for the
// first guard alone does not specialize the second PHI or its table address.
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"state-context-corridors", context};
  llvm::Function *function;
  llvm::BasicBlock *entry, *known, *shortcut, *head, *dispatch;
  llvm::BasicBlock *headFailure, *failure, *table, *fallback, *outerFailure;
  std::array<llvm::BasicBlock *, 3> unknown;
  std::vector<llvm::BasicBlock *> bridges;
  llvm::PHINode *state, *carried, *secondState, *secondFlags, *secondSize, *secondOpaque;
  llvm::PHINode *failureOutside, *defaultOutside;
  llvm::LoadInst *outsideLoad, *tableLoad;
  llvm::FreezeInst *fresh;
  llvm::BranchInst *firstBranch, *secondBranch, *knownBranch;
  llvm::SwitchInst *targetSwitch;
  llvm::GetElementPtrInst *tableAddress;
  llvm::Value *outside;
  Config config;

  explicit Fixture(Config selected = {}) : config(selected) {
    module.setDataLayout("e-p:64:64");
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *i1 = llvm::Type::getInt1Ty(context);
    auto *stateType = llvm::IntegerType::get(context, config.width);
    auto *ptr = llvm::PointerType::getUnqual(context);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {i64, stateType, stateType, stateType, stateType, ptr, ptr, i64, i1}, false),
        llvm::GlobalValue::ExternalLinkage, "state_corridor", module);
    for (unsigned index : {0u, 1u, 2u, 3u, 4u, 8u})
      function->getArg(index)->addAttr(llvm::Attribute::NoUndef);
    auto block = [&](const char *name) {return llvm::BasicBlock::Create(context, name, function);};
    entry = block("entry"); known = block("known"); shortcut = block("shortcut");
    for (auto &item : unknown) item = block("unknown");
    head = block("first.guard");
    for (unsigned index = 0; index < config.bridges; ++index) bridges.push_back(block("shared.bridge"));
    dispatch = block("second.join"); headFailure = block("first.failure");
    failure = block("memory.failure"); table = block("table.read");
    fallback = block("target.default"); outerFailure = block("outer.failure");
    auto *hit0 = block("target.hit.zero");
    auto *hit1 = block("target.hit.one");
    Builder b(entry);
    outsideLoad = b.CreateLoad(i64, function->getArg(5), "entry.value");
    outsideLoad->setAlignment(llvm::Align(1));
    outside = config.outsideArgument ? static_cast<llvm::Value *>(function->getArg(7)) : outsideLoad;
    auto *route = b.CreateSwitch(function->getArg(0), shortcut, 4);
    route->addCase(b.getInt64(0), known);
    for (unsigned index = 0; index < unknown.size(); ++index)
      route->addCase(b.getInt64(index + 1), unknown[index]);
    b.SetInsertPoint(known);
    knownBranch = config.outer ? b.CreateCondBr(function->getArg(8), head, outerFailure) : b.CreateBr(head);
    for (auto *item : unknown) {b.SetInsertPoint(item); b.CreateBr(head);}
    b.SetInsertPoint(shortcut); b.CreateBr(dispatch);
    b.SetInsertPoint(head);
    state = b.CreatePHI(stateType, 5, "first.state");
    state->addIncoming(b.getIntN(config.width, 50), known);
    for (unsigned index = 0; index < unknown.size(); ++index)
      state->addIncoming(function->getArg(index + 1), unknown[index]);
    carried = b.CreatePHI(i64, 5, "old.execution.input");
    for (auto *item : {known, unknown[0], unknown[1], unknown[2]})
      carried->addIncoming(function->getArg(7), item);
    fresh = llvm::cast<llvm::FreezeInst>(b.CreateFreeze(carried, "fresh.per.execution"));
    mark(fresh, "corridor.freeze");
    auto *wide = b.CreateZExtOrTrunc(state, i64);
    auto *flags = llvm::cast<llvm::Instruction>(b.CreateXor(wide, fresh, "flags.liveout"));
    mark(flags, "corridor.flags");
    if (config.flaggedLiveOut) {
      auto *high = b.CreateLShr(wide, b.getInt64(63));
      auto *narrow = llvm::cast<llvm::TruncInst>(b.CreateTrunc(high, b.getInt8Ty()));
      narrow->setHasNoUnsignedWrap(true); narrow->setHasNoSignedWrap(true);
      mark(narrow, "corridor.flagged.liveout");
      flags = llvm::cast<llvm::Instruction>(b.CreateXor(flags, b.CreateZExt(narrow, i64)));
    }
    firstBranch = b.CreateCondBr(b.CreateICmpUGT(state, b.getIntN(config.width, 50)), headFailure, bridges.front());

    llvm::Value *nextState = state, *bridgeFlags = flags, *bridgeSize = nullptr;
    for (unsigned index = 0; index < bridges.size(); ++index) {
      b.SetInsertPoint(bridges[index]);
      if (index == 0) {
        auto *load = b.CreateLoad(i64, function->getArg(5), "bridge.observation");
        load->setAlignment(llvm::Align(1)); mark(load, "corridor.bridge.load");
        bridgeSize = load;
        if (config.barrier == Barrier::Call) {
          auto helper = module.getOrInsertFunction("unknown_effect", llvm::FunctionType::get(b.getVoidTy(), {ptr}, false));
          mark(b.CreateCall(helper, {function->getArg(6)}), "corridor.barrier");
        } else if (config.barrier == Barrier::Store) {
          mark(b.CreateStore(function->getArg(7), function->getArg(6)), "corridor.barrier");
        } else if (config.barrier == Barrier::Volatile) {
          load->setVolatile(true); mark(load, "corridor.barrier");
        } else if (config.barrier == Barrier::Atomic) {
          load->setAtomic(llvm::AtomicOrdering::Acquire); mark(load, "corridor.barrier");
        } else if (config.barrier == Barrier::Fence) {
          mark(b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent), "corridor.barrier");
        } else if (config.barrier == Barrier::Alloca) {
          mark(b.CreateAlloca(i64), "corridor.barrier");
        }
        auto *plus = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(nextState, b.getIntN(config.width, 7)));
        if (config.poisonState) plus->setHasNoUnsignedWrap(true);
        nextState = b.CreateXor(plus, b.getIntN(config.width, 3), "next.state");
        bridgeFlags = b.CreateXor(bridgeFlags, load, "bridge.flags");
      } else {
        nextState = b.CreateXor(nextState, b.getIntN(config.width, 0), "continued.state");
        bridgeFlags = b.CreateAdd(bridgeFlags, b.getInt64(index), "continued.flags");
      }
      b.CreateBr(index + 1 < bridges.size() ? bridges[index + 1] : dispatch);
    }
    b.SetInsertPoint(dispatch);
    secondState = b.CreatePHI(stateType, 3, "second.state");
    secondState->addIncoming(nextState, bridges.back());
    secondState->addIncoming(function->getArg(4), shortcut);
    secondFlags = b.CreatePHI(i64, 3, "second.flags");
    secondFlags->addIncoming(bridgeFlags, bridges.back()); secondFlags->addIncoming(function->getArg(7), shortcut);
    secondSize = b.CreatePHI(i64, 3, "second.size.input");
    secondSize->addIncoming(bridgeSize, bridges.back()); secondSize->addIncoming(outsideLoad, shortcut);
    secondOpaque = b.CreatePHI(i64, 3, "second.old.execution");
    secondOpaque->addIncoming(fresh, bridges.back()); secondOpaque->addIncoming(function->getArg(7), shortcut);
    auto *freshSize = b.CreateLoad(i64, function->getArg(5), "size.per.execution");
    freshSize->setAlignment(llvm::Align(1)); mark(freshSize, "corridor.second.load");
    auto *index = llvm::cast<llvm::Instruction>(b.CreateAdd(
        b.CreateShl(b.CreateZExtOrTrunc(secondState, i64), b.getInt64(3)), b.getInt64(0x1000), "table.index"));
    mark(index, "corridor.index");
    tableAddress = llvm::cast<llvm::GetElementPtrInst>(b.CreateGEP(b.getInt8Ty(), function->getArg(6), index, "table.address"));
    mark(tableAddress, "corridor.address");
    llvm::Value *condition = config.unknownBounds
        ? b.CreateAnd(b.CreateICmpUGT(freshSize, b.getInt64(7)),
                      b.CreateICmpULE(index, b.CreateAdd(freshSize, b.getInt64(-8))))
        : b.CreateICmpEQ(secondState, b.getIntN(config.width, 58));
    secondBranch = b.CreateCondBr(condition, table, failure);
    b.SetInsertPoint(table);
    tableLoad = b.CreateLoad(i64, tableAddress, "runtime.table.value");
    tableLoad->setAlignment(llvm::Align(1)); mark(tableLoad, "corridor.table.load");
    auto *reentryState = b.CreateZExtOrTrunc(tableLoad, stateType);
    targetSwitch = b.CreateSwitch(tableLoad, fallback, 2);
    targetSwitch->addCase(b.getInt64(0x1111), config.reentry == Reentry::Head || config.reentry == Reentry::Both ? head : hit0);
    targetSwitch->addCase(b.getInt64(0x2222), config.reentry == Reentry::Dispatch || config.reentry == Reentry::Both ? dispatch : hit1);
    if (config.reentry == Reentry::Head || config.reentry == Reentry::Both) {
      state->addIncoming(reentryState, table);
      carried->addIncoming(secondOpaque, table);
    }
    if (config.reentry == Reentry::Dispatch || config.reentry == Reentry::Both) {
      secondState->addIncoming(reentryState, table);
      secondFlags->addIncoming(secondFlags, table); secondSize->addIncoming(secondSize, table);
      secondOpaque->addIncoming(secondOpaque, table);
    }
    // No result from the unexecuted bridge is invented on the shortcut path.
    auto makeReturn = [&](llvm::BasicBlock *where, llvm::BasicBlock *from,
                          llvm::Value *value, llvm::Value *external, llvm::Value *addend,
                          const char *name) {
      b.SetInsertPoint(where);
      auto *phi = b.CreatePHI(i64, 2, name); phi->addIncoming(external, from);
      auto *v = b.CreatePHI(i64, 2, "result.liveout"); v->addIncoming(value, from);
      auto *a = b.CreatePHI(i64, 2, "address.liveout"); a->addIncoming(addend, from);
      b.CreateRet(b.CreateXor(b.CreateXor(v, phi), a));
      return phi;
    };
    failureOutside = makeReturn(failure, dispatch, secondFlags, outside, index, "failure.outside");
    defaultOutside = makeReturn(fallback, table, secondFlags, outside, index, "default.outside");
    if (config.reentry != Reentry::Head && config.reentry != Reentry::Both)
      makeReturn(hit0, table, secondFlags, outside, index, "hit.zero.outside");
    else {b.SetInsertPoint(hit0); b.CreateRet(b.getInt64(0));}
    if (config.reentry != Reentry::Dispatch && config.reentry != Reentry::Both)
      makeReturn(hit1, table, secondFlags, outside, index, "hit.one.outside");
    else {b.SetInsertPoint(hit1); b.CreateRet(b.getInt64(0));}
    makeReturn(headFailure, head, flags, outside, b.getInt64(17), "head.failure.outside");
    b.SetInsertPoint(outerFailure); b.CreateRet(b.getInt64(0x123));
  }
};

void verify(const llvm::Module &module) {
  std::string error;
  llvm::raw_string_ostream stream(error);
  const bool invalid = llvm::verifyModule(module, &stream);
  INFO(error);
  REQUIRE_FALSE(invalid);
}

std::vector<llvm::Instruction *> marked(llvm::Function &function, const char *name) {
  std::vector<llvm::Instruction *> result;
  for (auto &block : function) for (auto &instruction : block)
    if (instruction.getMetadata(name)) result.push_back(&instruction);
  return result;
}

std::string ir(const llvm::Module &module) {
  std::string text; llvm::raw_string_ostream stream(text); module.print(stream, nullptr); return text;
}

llvm::GetElementPtrInst *copiedAddress(Fixture &fixture) {
  auto addresses = marked(*fixture.function, "corridor.address");
  CHECK(addresses.size() == 2);
  for (auto *instruction : addresses)
    if (instruction != fixture.tableAddress) return llvm::cast<llvm::GetElementPtrInst>(instruction);
  return nullptr;
}

// Use LLVM's constant folder, not the production expression prover. A copied
// address is useful only if normal constant folding can recover its real index.
void foldConstants(llvm::Module &module) {
  for (auto &function : module) for (auto &block : function) {
    for (auto iterator = block.begin(); iterator != block.end();) {
      auto &instruction = *iterator++;
      if (instruction.isTerminator() || llvm::isa<llvm::PHINode>(instruction)) continue;
      if (auto *constant = llvm::ConstantFoldInstruction(&instruction, module.getDataLayout())) {
        instruction.replaceAllUsesWith(constant); instruction.eraseFromParent();
      }
    }
  }
}

void runAndCheckContext(Fixture &fixture, unsigned failureSuccessor = 1) {
  verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.available); CHECK(stats.complete); CHECK(stats.changed);
  CHECK(stats.branchesRewritten >= 1);
  verify(fixture.module);
  auto *copy = fixture.known->getTerminator()->getSuccessor(0);
  CHECK(copy != fixture.head);
  CHECK(fixture.head->getTerminator() == fixture.firstBranch);
  CHECK(fixture.firstBranch->isConditional());
  for (auto *unknown : fixture.unknown)
    CHECK(unknown->getTerminator()->getSuccessor(0) == fixture.head);
  CHECK(fixture.shortcut->getTerminator()->getSuccessor(0) == fixture.dispatch);
  CHECK(fixture.dispatch->getTerminator() == fixture.secondBranch);
  CHECK(fixture.secondBranch->isConditional());
  CHECK(fixture.tableLoad->getParent() == fixture.table);
  CHECK(fixture.table->getTerminator() == fixture.targetSwitch);
  CHECK(fixture.targetSwitch->getDefaultDest() == fixture.fallback);
  CHECK(marked(*fixture.function, "corridor.bridge.load").size() == 2);
  CHECK(marked(*fixture.function, "corridor.second.load").size() == 2);
  CHECK(marked(*fixture.function, "corridor.table.load").size() == 2);
  CHECK(marked(*fixture.function, "corridor.freeze").size() == 2);
  auto *address = copiedAddress(fixture);
  if (!address) return;
  auto *copyGuard = llvm::dyn_cast<llvm::BranchInst>(address->getParent()->getTerminator());
  REQUIRE(copyGuard);
  if (fixture.config.unknownBounds) {
    CHECK(copyGuard->isConditional());
    CHECK_FALSE(llvm::isa<llvm::ConstantInt>(copyGuard->getCondition()));
    CHECK(copyGuard->getSuccessor(0) != copyGuard->getSuccessor(1));
    CHECK(copyGuard->getSuccessor(failureSuccessor) == fixture.failure);
    CHECK(fixture.failureOutside->getIncomingValueForBlock(address->getParent()) == fixture.outside);
  }
  llvm::LoadInst *copyLoad = nullptr;
  for (auto *instruction : marked(*fixture.function, "corridor.table.load"))
    if (instruction != fixture.tableLoad) copyLoad = llvm::cast<llvm::LoadInst>(instruction);
  REQUIRE(copyLoad);
  CHECK(copyLoad->getPointerOperand() == address);
  auto *copySwitch = llvm::dyn_cast<llvm::SwitchInst>(copyLoad->getParent()->getTerminator());
  REQUIRE(copySwitch);
  CHECK(copySwitch->getCondition() == copyLoad);
  CHECK(copySwitch->getDefaultDest() == fixture.fallback);
  CHECK(copySwitch->getNumCases() == fixture.targetSwitch->getNumCases());
  CHECK(fixture.defaultOutside->getIncomingValueForBlock(copyLoad->getParent()) == fixture.outside);
  if (fixture.config.outer) {
    CHECK(fixture.known->getTerminator() == fixture.knownBranch);
    CHECK(fixture.knownBranch->getCondition() == fixture.function->getArg(8));
    CHECK(fixture.knownBranch->getSuccessor(1) == fixture.outerFailure);
  }
  // The copied GEP itself survives; only its scalar integer input is folded.
  foldConstants(fixture.module);
  auto *index = llvm::dyn_cast<llvm::ConstantInt>(address->getOperand(1));
  REQUIRE(index);
  CHECK(index->getZExtValue() == 0x1000 + 58 * 8);
  if (!fixture.config.unknownBounds) {
    auto *condition = llvm::dyn_cast<llvm::ConstantInt>(copyGuard->getCondition());
    REQUIRE(condition);
    CHECK(condition->isOne());
  }
  verify(fixture.module);
}
} // namespace

TEST_CASE("State context crosses shared bridges while preserving unknown bounds and table default",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Fixture fixture; runAndCheckContext(fixture);
}

TEST_CASE("State context preserves an outer condition and external Argument PHIs",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Config config; config.outer = true; config.outsideArgument = true;
  Fixture fixture(config); runAndCheckContext(fixture);
}

TEST_CASE("State context makes the second state comparison constant without assuming bounds",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Config config; config.unknownBounds = false;
  Fixture fixture(config); runAndCheckContext(fixture);
}

TEST_CASE("State context finds the table path with two safe successors and reversed bounds",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Fixture fixture;
  // The failure arm now has pure computations and an unconditional bridge,
  // so successor eligibility alone cannot identify the table-reading arm.
  fixture.failure->splitBasicBlock(fixture.failure->getTerminator(), "failure.return");
  Builder b(fixture.secondBranch);
  fixture.secondBranch->setCondition(b.CreateNot(fixture.secondBranch->getCondition(), "bounds.failure"));
  fixture.secondBranch->swapSuccessors();
  CHECK(fixture.secondBranch->getSuccessor(0) == fixture.failure);
  CHECK(fixture.secondBranch->getSuccessor(1) == fixture.table);
  runAndCheckContext(fixture, 0);
}

TEST_CASE("State context crosses multiple bridges with integer widths and flagged liveouts",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  for (unsigned width : {8u, 16u, 32u, 64u, 128u}) {
    Config config; config.width = width; config.bridges = 2; config.flaggedLiveOut = true;
    CAPTURE(width); Fixture fixture(config); runAndCheckContext(fixture);
  }
}

TEST_CASE("State context terminal switch reentry returns to fresh original executions",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  for (auto reentry : {Reentry::Head, Reentry::Dispatch, Reentry::Both}) {
    CAPTURE(static_cast<int>(reentry));
    Config config; config.reentry = reentry; Fixture fixture(config);
    verify(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK(stats.changed); CHECK(stats.complete); verify(fixture.module);
    llvm::LoadInst *copyLoad = nullptr; llvm::FreezeInst *copyFreeze = nullptr;
    for (auto *instruction : marked(*fixture.function, "corridor.table.load"))
      if (instruction != fixture.tableLoad) copyLoad = llvm::cast<llvm::LoadInst>(instruction);
    for (auto *instruction : marked(*fixture.function, "corridor.freeze"))
      if (instruction != fixture.fresh) copyFreeze = llvm::cast<llvm::FreezeInst>(instruction);
    REQUIRE(copyLoad); REQUIRE(copyFreeze); CHECK(copyFreeze != fixture.fresh);
    auto *switchInst = llvm::cast<llvm::SwitchInst>(copyLoad->getParent()->getTerminator());
    if (reentry == Reentry::Head || reentry == Reentry::Both) {
      CHECK(switchInst->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt64Ty(fixture.context), 0x1111))->getCaseSuccessor() == fixture.head);
      CHECK(fixture.state->getIncomingValueForBlock(copyLoad->getParent()) == copyLoad);
      CHECK(fixture.carried->getIncomingValueForBlock(copyLoad->getParent()) == copyFreeze);
    }
    if (reentry == Reentry::Dispatch || reentry == Reentry::Both) {
      CHECK(switchInst->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt64Ty(fixture.context), 0x2222))->getCaseSuccessor() == fixture.dispatch);
      CHECK(fixture.secondState->getIncomingValueForBlock(copyLoad->getParent()) == copyLoad);
    }
    CHECK(fixture.head->getTerminator() == fixture.firstBranch);
    CHECK(fixture.dispatch->getTerminator() == fixture.secondBranch);
  }
}

TEST_CASE("State context stops before effects while retaining the proved first guard gain",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  for (auto barrier : {Barrier::Call, Barrier::Store, Barrier::Volatile,
                       Barrier::Atomic, Barrier::Fence, Barrier::Alloca}) {
    CAPTURE(static_cast<int>(barrier));
    Config config; config.barrier = barrier; Fixture fixture(config);
    verify(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK(stats.changed); CHECK(stats.branchesRewritten == 1); verify(fixture.module);
    auto *copy = fixture.known->getTerminator()->getSuccessor(0);
    CHECK(copy != fixture.head);
    CHECK(copy->getTerminator()->getSuccessor(0) == fixture.bridges.front());
    CHECK(marked(*fixture.function, "corridor.barrier").size() == 1);
    CHECK(marked(*fixture.function, "corridor.address").size() == 1);
    CHECK(marked(*fixture.function, "corridor.table.load").size() == 1);
  }
}

TEST_CASE("State context obeys public proof budgets before mutation",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  for (bool conditions : {false, true}) {
    Fixture fixture; verify(fixture.module); const auto before = ir(fixture.module);
    DispatcherSpecializationOptions options;
    if (conditions) options.maxConditions = 0; else options.maxExpressionNodes = 0;
    const auto stats = runDispatcherSpecialization(fixture.module, options);
    CHECK_FALSE(stats.changed); CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0);
    CHECK(ir(fixture.module) == before); verify(fixture.module);
  }
}

TEST_CASE("State context stops at block and operation limits with the original first gain intact",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  for (bool operationLimit : {false, true}) {
    CAPTURE(operationLimit);
    Config config; config.bridges = operationLimit ? 1 : 6;
    Fixture fixture(config);
    if (operationLimit) {
      Builder b(fixture.bridges.front()->getTerminator());
      llvm::Value *value = fixture.function->getArg(7);
      for (unsigned index = 0; index < 260; ++index)
        value = b.CreateAdd(value, b.getInt64(index), "budget.operation");
    }
    verify(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK(stats.changed); CHECK(stats.branchesRewritten == 1);
    CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0);
    verify(fixture.module);
    auto *copy = fixture.known->getTerminator()->getSuccessor(0);
    CHECK(copy != fixture.head);
    CHECK(copy->getTerminator()->getSuccessor(0) == fixture.bridges.front());
    CHECK(marked(*fixture.function, "corridor.address").size() == 1);
    CHECK(marked(*fixture.function, "corridor.table.load").size() == 1);
  }
}

TEST_CASE("State context copies do not expand again on repeated invocations",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Fixture fixture;
  runAndCheckContext(fixture);
  const auto after = ir(fixture.module);
  const auto blocks = fixture.function->size();
  for (unsigned iteration = 0; iteration < 4; ++iteration) {
    CAPTURE(iteration);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK(stats.available); CHECK(stats.complete); CHECK_FALSE(stats.changed);
    CHECK(stats.branchesRewritten == 0);
    CHECK(fixture.function->size() == blocks);
    CHECK(ir(fixture.module) == after);
    verify(fixture.module);
  }
}

TEST_CASE("State context does not reinterpret an old future-region value as a fresh execution",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Fixture fixture;
  auto *seed = llvm::BasicBlock::Create(fixture.context, "initial.unknown", fixture.function, fixture.head);
  Builder b(seed); b.CreateBr(fixture.head);
  llvm::cast<llvm::SwitchInst>(fixture.entry->getTerminator())->setSuccessor(1, seed);
  fixture.state->addIncoming(fixture.function->getArg(1), seed);
  fixture.carried->addIncoming(fixture.function->getArg(7), seed);
  fixture.targetSwitch->addCase(b.getInt64(0x3333), fixture.known);
  fixture.carried->setIncomingValue(fixture.carried->getBasicBlockIndex(fixture.known), fixture.tableLoad);
  verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.changed); CHECK(stats.complete); CHECK(stats.branchesRewritten == 1);
  verify(fixture.module);
  auto *inputs = fixture.known->getTerminator()->getSuccessor(0);
  CHECK(inputs != fixture.head);
  // A new S execution is now permitted only after the prior execution's
  // LOAD has been captured on the known predecessor edge. Preserve this
  // original fixture: an entry snapshot strengthens the historical-value
  // contract rather than fixing it by refusing all future-region copies.
  CHECK(marked(*fixture.function, "corridor.table.load").size() == 2);
  llvm::LoadInst *copyLoad = nullptr;
  for (auto *instruction : marked(*fixture.function, "corridor.table.load"))
    if (instruction != fixture.tableLoad) copyLoad = llvm::cast<llvm::LoadInst>(instruction);
  REQUIRE(copyLoad);
  llvm::FreezeInst *copyFreeze = nullptr;
  for (auto *instruction : marked(*fixture.function, "corridor.freeze"))
    if (instruction != fixture.fresh) copyFreeze = llvm::cast<llvm::FreezeInst>(instruction);
  REQUIRE(copyFreeze);
  auto *snapshot = llvm::dyn_cast<llvm::PHINode>(copyFreeze->getOperand(0)); REQUIRE(snapshot);
  CHECK(snapshot->getParent() == inputs); CHECK(snapshot->getNumIncomingValues() == 1);
  CHECK(snapshot->getIncomingBlock(0) == fixture.known);
  CHECK(copyFreeze->getOperand(0) != copyLoad);
  llvm::DominatorTree dom(*fixture.function);
  auto *incoming = llvm::dyn_cast<llvm::Instruction>(snapshot->getIncomingValue(0)); REQUIRE(incoming);
  CHECK(dom.dominates(incoming, fixture.known->getTerminator()));
  CHECK(dom.dominates(copyFreeze, copyLoad));
  // A repaired PHI may merge LOADs from previous original/copied S executions;
  // it must not invent a constant, undef or a fresh unguarded LOAD at B.
  std::vector<llvm::Value *> pending{incoming};
  std::unordered_set<llvm::Value *> visited;
  std::unordered_set<llvm::Value *> historicalLoads;
  while (!pending.empty()) {
    auto *value = pending.back(); pending.pop_back();
    if (!visited.insert(value).second) continue;
    if (value == fixture.tableLoad || value == copyLoad) {historicalLoads.insert(value); continue;}
    auto *phi = llvm::dyn_cast<llvm::PHINode>(value); REQUIRE(phi);
    for (auto &arm : phi->incoming_values()) pending.push_back(arm.get());
  }
  CHECK(historicalLoads.contains(fixture.tableLoad)); CHECK(historicalLoads.contains(copyLoad));
  CHECK(fixture.head->getTerminator() == fixture.firstBranch);
  CHECK(fixture.dispatch->getTerminator() == fixture.secondBranch);
  CHECK(fixture.secondBranch->isConditional());
  CHECK(fixture.tableLoad->getParent() == fixture.table);
  CHECK(fixture.table->getTerminator() == fixture.targetSwitch);
  CHECK(fixture.targetSwitch->getDefaultDest() == fixture.fallback);
  auto *copySwitch = llvm::dyn_cast<llvm::SwitchInst>(copyLoad->getParent()->getTerminator()); REQUIRE(copySwitch);
  CHECK(copySwitch->getCondition() == copyLoad); CHECK(copySwitch->getDefaultDest() == fixture.fallback);
  CHECK(copySwitch->getNumCases() == fixture.targetSwitch->getNumCases());
  for (auto item : fixture.targetSwitch->cases()) {
    auto found = copySwitch->findCaseValue(item.getCaseValue()); REQUIRE(found != copySwitch->case_default());
    CHECK(found->getCaseSuccessor() == item.getCaseSuccessor());
  }
  for (auto *unknown : fixture.unknown) CHECK(unknown->getTerminator()->getSuccessor(0) == fixture.head);
  CHECK(seed->getTerminator()->getSuccessor(0) == fixture.head);
  CHECK(fixture.shortcut->getTerminator()->getSuccessor(0) == fixture.dispatch);
  CHECK(fixture.defaultOutside->getIncomingValueForBlock(copyLoad->getParent()) == fixture.outside);
  verify(fixture.module);
}

TEST_CASE("State context does not fold a poison-sensitive first-guard proof through freeze",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Fixture fixture;
  Builder b(fixture.firstBranch);
  auto *bad = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(fixture.state, b.getInt64(~std::uint64_t{0})));
  bad->setHasNoUnsignedWrap(true);
  auto *condition = b.CreateICmpUGT(b.CreateFreeze(bad), b.getInt64(0));
  fixture.firstBranch->setCondition(condition);
  verify(fixture.module);
  const auto before = ir(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK_FALSE(stats.changed); CHECK(stats.complete);
  CHECK(stats.branchesRewritten == 0); CHECK(ir(fixture.module) == before);
  verify(fixture.module);
}

TEST_CASE("State context rejects repeated bridge successors without removing a valid seed gain",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  Fixture fixture;
  fixture.bridges.front()->getTerminator()->eraseFromParent();
  Builder b(fixture.bridges.front());
  b.CreateCondBr(fixture.function->getArg(8), fixture.dispatch, fixture.dispatch);
  for (auto &phi : fixture.dispatch->phis())
    phi.addIncoming(phi.getIncomingValueForBlock(fixture.bridges.front()), fixture.bridges.front());
  verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.changed); CHECK(stats.branchesRewritten == 1);
  verify(fixture.module);
  auto *copy = fixture.known->getTerminator()->getSuccessor(0);
  CHECK(copy != fixture.head);
  CHECK(copy->getTerminator()->getSuccessor(0) == fixture.bridges.front());
  CHECK(marked(*fixture.function, "corridor.address").size() == 1);
}

TEST_CASE("State context rejects exception and address-taken bridge regions",
          "[llvm][dispatch-specialize][state-context-corridors]") {
  for (bool exception : {false, true}) {
    CAPTURE(exception);
    Fixture fixture;
    if (exception) {
      auto *ptr = llvm::PointerType::getUnqual(fixture.context);
      auto personality = fixture.module.getOrInsertFunction("personality",
          llvm::FunctionType::get(llvm::Type::getInt32Ty(fixture.context), true));
      fixture.function->setPersonalityFn(llvm::cast<llvm::Constant>(personality.getCallee()));
      auto callee = fixture.module.getOrInsertFunction("may_throw",
          llvm::FunctionType::get(llvm::Type::getVoidTy(fixture.context), false));
      auto *unwind = llvm::BasicBlock::Create(fixture.context, "unwind", fixture.function);
      fixture.bridges.front()->getTerminator()->eraseFromParent();
      Builder b(fixture.bridges.front()); b.CreateInvoke(callee, fixture.dispatch, unwind);
      b.SetInsertPoint(unwind);
      auto *pad = b.CreateLandingPad(llvm::StructType::get(ptr, b.getInt32Ty()), 0);
      pad->setCleanup(true); b.CreateResume(pad);
    } else {
      auto *address = llvm::BlockAddress::get(fixture.bridges.front());
      new llvm::GlobalVariable(fixture.module, address->getType(), true,
          llvm::GlobalValue::ExternalLinkage, address, "observable.bridge.address");
    }
    verify(fixture.module);
    const auto stats = runDispatcherSpecialization(fixture.module);
    CHECK(stats.changed); CHECK(stats.branchesRewritten == 1);
    verify(fixture.module);
    auto *copy = fixture.known->getTerminator()->getSuccessor(0);
    CHECK(copy != fixture.head);
    CHECK(copy->getTerminator()->getSuccessor(0) == fixture.bridges.front());
    CHECK(marked(*fixture.function, "corridor.address").size() == 1);
  }
}
