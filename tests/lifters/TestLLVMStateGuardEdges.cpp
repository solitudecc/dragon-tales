#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <array>
#include <cstdint>
#include <string>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;

enum class Expression { Direct, Arithmetic, SelectedPoison };

struct Config {
  unsigned width = 64;
  bool outerGuard = false;
  bool loop = false;
  Expression expression = Expression::Direct;
};

// This intentionally models flags-like definitions with users beyond the
// dispatch block. Threading straight to a destination would lose those writes.
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"state-guard-edges", context};
  llvm::Function *function;
  llvm::BasicBlock *known;
  std::array<llvm::BasicBlock *, 3> unknown;
  llvm::BasicBlock *dispatch;
  llvm::BasicBlock *above;
  llvm::BasicBlock *below;
  llvm::BasicBlock *extra;
  llvm::BasicBlock *guardExit;
  llvm::PHINode *state;
  llvm::PHINode *abovePhi;
  llvm::PHINode *belowPhi;
  llvm::Value *condition;
  llvm::Value *mixed;
  llvm::LoadInst *sizeLoad;
  llvm::BranchInst *originalBranch;
  llvm::BranchInst *outerBranch;
  bool hasOuterGuard;

  explicit Fixture(const Config &config = {}) : hasOuterGuard(config.outerGuard) {
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *stateType = llvm::IntegerType::get(context, config.width);
    auto *pointer = llvm::PointerType::getUnqual(context);
    function = llvm::Function::Create(
        llvm::FunctionType::get(i64,
            {i64, stateType, stateType, stateType, pointer, pointer,
             llvm::Type::getInt1Ty(context), llvm::Type::getInt1Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage, "state_guard", module);
    for (unsigned index : {0u, 1u, 2u, 3u, 6u, 7u})
      function->getArg(index)->addAttr(llvm::Attribute::NoUndef);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    known = llvm::BasicBlock::Create(context, "known", function);
    for (unsigned index = 0; index < unknown.size(); ++index)
      unknown[index] = llvm::BasicBlock::Create(context, "unknown", function);
    extra = llvm::BasicBlock::Create(context, "extra", function);
    dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
    above = llvm::BasicBlock::Create(context, "above", function);
    below = llvm::BasicBlock::Create(context, "below", function);
    guardExit = llvm::BasicBlock::Create(context, "guard.exit", function);
    Builder b(entry);
    auto *routing = b.CreateSwitch(function->getArg(0), unknown[2], 4);
    routing->addCase(b.getInt64(0), known);
    routing->addCase(b.getInt64(1), unknown[0]);
    routing->addCase(b.getInt64(2), unknown[1]);
    routing->addCase(b.getInt64(3), extra);
    b.SetInsertPoint(known);
    outerBranch = config.outerGuard
        ? b.CreateCondBr(function->getArg(6), dispatch, guardExit)
        : b.CreateBr(dispatch);
    for (auto *block : unknown) {
      b.SetInsertPoint(block);
      b.CreateBr(dispatch);
    }
    b.SetInsertPoint(extra);
    b.CreateCondBr(function->getArg(6), above, below);
    b.SetInsertPoint(dispatch);
    state = b.CreatePHI(stateType, config.loop ? 5 : 4, "state");
    state->addIncoming(b.getIntN(config.width, 50), known);
    for (unsigned index = 0; index < unknown.size(); ++index)
      state->addIncoming(function->getArg(index + 1), unknown[index]);
    sizeLoad = b.CreateLoad(i64, function->getArg(4), "guest.size");
    sizeLoad->setAlignment(llvm::Align(1));
    sizeLoad->setMetadata("state.guard.size", llvm::MDNode::get(context, {}));
    auto *flags = b.CreateXor(b.CreateZExtOrTrunc(state, i64),
                            b.getInt64(0x8000000000000081ULL), "flags");
    mixed = b.CreateAdd(flags, sizeLoad, "flags.and.size");
    llvm::Value *tested = state;
    if (config.expression == Expression::Arithmetic) {
      tested = b.CreateXor(b.CreateAdd(state, b.getIntN(config.width, 7)),
                          b.getIntN(config.width, 3), "computed.state");
      condition = b.CreateICmpEQ(tested, b.getIntN(config.width, 58), "condition");
    } else if (config.expression == Expression::SelectedPoison) {
      auto *choose = b.CreateICmpEQ(state, b.getIntN(config.width, 50));
      tested = b.CreateSelect(choose, b.getIntN(config.width, 7),
                             llvm::PoisonValue::get(stateType), "chosen.arm");
      condition = b.CreateICmpEQ(tested, b.getIntN(config.width, 7), "condition");
    } else {
      condition = b.CreateICmpUGT(tested, b.getIntN(config.width, 50), "condition");
    }
    originalBranch = b.CreateCondBr(condition, above, below);
    b.SetInsertPoint(above);
    abovePhi = b.CreatePHI(i64, 2, "success.liveout");
    abovePhi->addIncoming(mixed, dispatch);
    abovePhi->addIncoming(b.getInt64(91), extra);
    b.CreateStore(abovePhi, function->getArg(5))->setAlignment(llvm::Align(1));
    b.CreateRet(abovePhi);
    b.SetInsertPoint(below);
    belowPhi = b.CreatePHI(i64, 2, "failure.constant");
    belowPhi->addIncoming(b.getInt64(17), dispatch);
    belowPhi->addIncoming(b.getInt64(92), extra);
    // The extra edge does not execute dispatch. An independent PHI makes the
    // region-defined operand legal while testing non-PHI uses after the merge.
    auto *belowMixed = b.CreatePHI(i64, 2, "failure.liveout");
    belowMixed->addIncoming(mixed, dispatch);
    belowMixed->addIncoming(b.getInt64(93), extra);
    auto *result = b.CreateXor(belowMixed, belowPhi, "failure.result");
    b.CreateStore(result, function->getArg(5))->setAlignment(llvm::Align(1));
    if (config.loop) {
      auto *done = llvm::BasicBlock::Create(context, "loop.done", function);
      b.CreateCondBr(function->getArg(7), dispatch, done);
      state->addIncoming(function->getArg(1), below);
      b.SetInsertPoint(done);
    }
    b.CreateRet(result);
    b.SetInsertPoint(guardExit);
    b.CreateRet(b.getInt64(0x321));
  }

  llvm::BasicBlock *expectedTarget(Expression expression) const {
    return expression == Expression::Direct ? below : above;
  }
};

std::string ir(const llvm::Module &module) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  module.print(stream, nullptr);
  return text;
}

void verify(const llvm::Module &module) {
  std::string error;
  llvm::raw_string_ostream stream(error);
  const bool invalid = llvm::verifyModule(module, &stream);
  INFO(error);
  REQUIRE_FALSE(invalid);
}

unsigned markedLoads(const llvm::Function &function) {
  unsigned count = 0;
  for (const auto &block : function)
    for (const auto &instruction : block)
      count += instruction.getMetadata("state.guard.size") != nullptr;
  return count;
}

llvm::BasicBlock *expectSpecialized(Fixture &fixture, llvm::BasicBlock *target) {
  verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.available);
  CHECK(stats.complete);
  CHECK(stats.changed);
  CHECK(stats.branchesRewritten == 1);
  verify(fixture.module);
  auto *copy = fixture.known->getTerminator()->getSuccessor(0);
  CHECK(copy != fixture.dispatch);
  CHECK(copy != target); // The block's effects must execute on the new path.
  CHECK(fixture.dispatch->getTerminator() == fixture.originalBranch);
  REQUIRE(fixture.originalBranch->isConditional());
  CHECK(fixture.originalBranch->getCondition() == fixture.condition);
  for (auto *unknown : fixture.unknown)
    CHECK(unknown->getTerminator()->getSuccessor(0) == fixture.dispatch);
  CHECK(markedLoads(*fixture.function) == 2);
  CHECK(fixture.sizeLoad->getParent() == fixture.dispatch);
  CHECK(fixture.abovePhi->getBasicBlockIndex(fixture.extra) >= 0);
  CHECK(fixture.belowPhi->getBasicBlockIndex(fixture.extra) >= 0);
  if (copy == fixture.dispatch || copy == target) return nullptr;
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(copy->getTerminator());
  REQUIRE(branch);
  CHECK(branch->isUnconditional());
  CHECK(branch->getSuccessor(0) == target);
  CHECK(fixture.state->getBasicBlockIndex(fixture.known) == -1);
  if (target == fixture.below) {
    REQUIRE(fixture.belowPhi->getBasicBlockIndex(copy) >= 0);
    CHECK(llvm::cast<llvm::ConstantInt>(fixture.belowPhi->getIncomingValueForBlock(copy))
              ->getZExtValue() == 17);
  } else {
    REQUIRE(fixture.abovePhi->getBasicBlockIndex(copy) >= 0);
    auto *incoming = llvm::dyn_cast<llvm::Instruction>(
        fixture.abovePhi->getIncomingValueForBlock(copy));
    REQUIRE(incoming);
    CHECK(incoming->getParent() == copy);
  }
  if (fixture.hasOuterGuard) {
    CHECK(fixture.known->getTerminator() == fixture.outerBranch);
    CHECK(fixture.outerBranch->getCondition() == fixture.function->getArg(6));
    CHECK(fixture.outerBranch->getSuccessor(1) == fixture.guardExit);
  }
  return copy;
}

void expectUnchanged(Fixture &fixture,
                     const DispatcherSpecializationOptions &options = {},
                     bool budget = false) {
  verify(fixture.module);
  const auto before = ir(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module, options);
  CHECK(stats.available);
  CHECK_FALSE(stats.changed);
  CHECK(stats.branchesRewritten == 0);
  if (budget) {
    CHECK_FALSE(stats.complete);
    CHECK(stats.budgetExhausted > 0);
  }
  CHECK(ir(fixture.module) == before);
  verify(fixture.module);
}
} // namespace

TEST_CASE("State guard edges clone effects while preserving three unknown inputs",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  expectSpecialized(fixture, fixture.below);
}

TEST_CASE("State guard edges preserve the outer predecessor condition",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Config config;
  config.outerGuard = true;
  Fixture fixture(config);
  expectSpecialized(fixture, fixture.below);
}

TEST_CASE("State guard edges retain unknown loop reentry and outside liveout PHIs",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Config config;
  config.loop = true;
  Fixture fixture(config);
  expectSpecialized(fixture, fixture.below);
  CHECK(fixture.below->getTerminator()->getSuccessor(0) == fixture.dispatch);
  CHECK(fixture.state->getIncomingValueForBlock(fixture.below) == fixture.function->getArg(1));
}

TEST_CASE("State guard edges evaluate integer widths and selected arithmetic arms",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (unsigned width : {8u, 16u, 32u, 64u}) {
    for (auto expression : {Expression::Direct, Expression::Arithmetic,
                            Expression::SelectedPoison}) {
      CAPTURE(width, static_cast<unsigned>(expression));
      Config config;
      config.width = width;
      config.expression = expression;
      Fixture fixture(config);
      expectSpecialized(fixture, fixture.expectedTarget(expression));
    }
  }
}

TEST_CASE("State guard edges honor explicit condition and expression budgets",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (bool conditions : {false, true}) {
    Fixture fixture;
    DispatcherSpecializationOptions options;
    if (conditions) options.maxConditions = 0;
    else options.maxExpressionNodes = 0;
    expectUnchanged(fixture, options, true);
  }
}

TEST_CASE("State guard edges retain unknown undef poison and independent freezes",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (unsigned kind = 0; kind != 6; ++kind) {
    CAPTURE(kind);
    Fixture fixture;
    Builder b(fixture.originalBranch);
    auto *type = fixture.state->getType();
    llvm::Value *value;
    if (kind == 0) value = fixture.function->getArg(1);
    else if (kind == 1) value = llvm::UndefValue::get(type);
    else if (kind == 2) value = llvm::PoisonValue::get(type);
    else if (kind == 3)
      value = b.CreateFreeze(llvm::PoisonValue::get(type), "arbitrary.state");
    else if (kind == 4)
      value = b.CreateFreeze(llvm::UndefValue::get(type), "arbitrary.state");
    else {
      auto *first = b.CreateFreeze(llvm::PoisonValue::get(type), "first.choice");
      auto *second = b.CreateFreeze(llvm::PoisonValue::get(type), "second.choice");
      fixture.originalBranch->setCondition(b.CreateICmpEQ(first, second));
      expectUnchanged(fixture);
      continue;
    }
    // Place the unknown on this edge, rather than poisoning an unrelated arm.
    if (kind <= 2) {
      fixture.state->setIncomingValue(fixture.state->getBasicBlockIndex(fixture.known), value);
    } else {
      fixture.originalBranch->setCondition(b.CreateICmpUGT(value, b.getInt64(50)));
    }
    expectUnchanged(fixture);
  }
}

TEST_CASE("State guard edges reject poisoned flagged integer proofs through freeze",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (unsigned kind = 0; kind != 8; ++kind) {
    CAPTURE(kind);
    Fixture fixture;
    Builder b(fixture.originalBranch);
    llvm::Value *value;
    if (kind == 0 || kind == 1) {
      auto *trunc = llvm::cast<llvm::TruncInst>(b.CreateTrunc(fixture.state, b.getInt1Ty()));
      if (kind == 0) trunc->setHasNoUnsignedWrap(true);
      else trunc->setHasNoSignedWrap(true);
      value = trunc;
    } else if (kind == 2) {
      fixture.state->setIncomingValue(fixture.state->getBasicBlockIndex(fixture.known),
          b.getInt64(0x8000000000000000ULL));
      auto *extension = llvm::cast<llvm::ZExtInst>(
          b.CreateZExt(fixture.state, b.getInt128Ty()));
      extension->setNonNeg(true);
      value = b.CreateICmpNE(extension, llvm::ConstantInt::get(extension->getType(), 0));
    } else if (kind == 3) {
      auto *comparison = llvm::cast<llvm::ICmpInst>(
          b.CreateICmpULT(fixture.state, b.getInt64(0x8000000000000000ULL)));
      comparison->setSameSign();
      value = comparison;
    } else if (kind == 4) {
      auto *overlap = llvm::cast<llvm::PossiblyDisjointInst>(
          b.CreateOr(fixture.state, b.getInt64(50)));
      overlap->setIsDisjoint(true);
      value = b.CreateICmpEQ(overlap, b.getInt64(50));
    } else if (kind == 5 || kind == 6) {
      auto *sum = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(
          fixture.state, b.getInt64(kind == 5 ? UINT64_MAX : INT64_MAX)));
      if (kind == 5) sum->setHasNoUnsignedWrap(true);
      else sum->setHasNoSignedWrap(true);
      value = b.CreateICmpEQ(sum, b.getInt64(49));
    } else {
      auto *shift = b.CreateShl(fixture.state, b.getInt64(64));
      value = b.CreateICmpEQ(shift, b.getInt64(0));
    }
    fixture.originalBranch->setCondition(b.CreateFreeze(value, "stable.poison.choice"));
    expectUnchanged(fixture);
  }
}

TEST_CASE("State guard edges clone one frozen liveout choice per execution",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  Builder b(fixture.originalBranch);
  auto *choice = llvm::cast<llvm::Instruction>(
      b.CreateFreeze(llvm::PoisonValue::get(b.getInt64Ty()), "liveout.choice"));
  choice->setMetadata("state.guard.freeze", llvm::MDNode::get(fixture.context, {}));
  auto *sum = b.CreateAdd(fixture.mixed, choice, "frozen.liveout");
  auto *value = b.CreateXor(sum, choice, "reuse.same.choice");
  fixture.abovePhi->setIncomingValue(fixture.abovePhi->getBasicBlockIndex(fixture.dispatch), value);
  auto *belowMixed = llvm::cast<llvm::PHINode>(fixture.belowPhi->getNextNode());
  belowMixed->setIncomingValue(belowMixed->getBasicBlockIndex(fixture.dispatch), value);
  auto *copy = expectSpecialized(fixture, fixture.below);
  if (!copy) return;
  llvm::Instruction *copiedChoice = nullptr;
  unsigned total = 0;
  for (auto &block : *fixture.function)
    for (auto &instruction : block)
      if (instruction.getMetadata("state.guard.freeze")) {
        ++total;
        if (&block == copy) copiedChoice = &instruction;
      }
  CHECK(total == 2);
  REQUIRE(copiedChoice);
  CHECK(copiedChoice != choice);
  CHECK(copiedChoice->getNumUses() == 2);
  for (const auto *user : copiedChoice->users())
    CHECK(llvm::cast<llvm::Instruction>(user)->getParent() == copy);
}

TEST_CASE("State guard edges refuse memory effects and call boundaries without edits",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (unsigned kind = 0; kind != 8; ++kind) {
    CAPTURE(kind);
    Fixture fixture;
    Builder b(fixture.originalBranch);
    if (kind == 0) {
      auto *callee = llvm::Function::Create(llvm::FunctionType::get(b.getVoidTy(), false),
          llvm::GlobalValue::ExternalLinkage, "unknown.call", fixture.module);
      b.CreateCall(callee);
    } else if (kind == 1) {
      b.CreateStore(b.getInt64(12), fixture.function->getArg(5));
    } else if (kind == 2) {
      fixture.sizeLoad->setVolatile(true);
    } else if (kind == 3) {
      fixture.sizeLoad->setAlignment(llvm::Align(8));
      fixture.sizeLoad->setAtomic(llvm::AtomicOrdering::Monotonic);
    } else if (kind == 4) {
      b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    } else if (kind == 5) {
      b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, fixture.function->getArg(5),
          b.getInt64(1), llvm::Align(8), llvm::AtomicOrdering::Monotonic);
    } else if (kind == 6) {
      b.CreateAlloca(b.getInt64Ty());
    } else {
      b.CreateStore(b.getInt64(12), fixture.function->getArg(5), true);
    }
    expectUnchanged(fixture);
  }
}

TEST_CASE("State guard edges refuse duplicate predecessor edges without edits",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  fixture.outerBranch->eraseFromParent();
  Builder b(fixture.known);
  fixture.outerBranch = b.CreateCondBr(fixture.function->getArg(6),
                                     fixture.dispatch, fixture.dispatch);
  fixture.state->addIncoming(b.getInt64(50), fixture.known);
  expectUnchanged(fixture);
}

TEST_CASE("State guard edges refuse invoke predecessors and exception edges",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  auto *personality = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(fixture.context), true),
      llvm::GlobalValue::ExternalLinkage, "personality", fixture.module);
  fixture.function->setPersonalityFn(personality);
  auto *callee = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(fixture.context), false),
      llvm::GlobalValue::ExternalLinkage, "may.throw", fixture.module);
  auto *unwind = llvm::BasicBlock::Create(fixture.context, "unwind", fixture.function);
  fixture.outerBranch->eraseFromParent();
  Builder b(fixture.known);
  b.CreateInvoke(callee, fixture.dispatch, unwind);
  b.SetInsertPoint(unwind);
  auto *exception = b.CreateLandingPad(llvm::StructType::get(
      llvm::PointerType::getUnqual(fixture.context), b.getInt32Ty()), 0);
  exception->setCleanup(true);
  b.CreateResume(exception);
  expectUnchanged(fixture);
}

TEST_CASE("State guard edges perform at most one clone of each original branch per run",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  fixture.state->setIncomingValue(fixture.state->getBasicBlockIndex(fixture.unknown[0]),
      llvm::ConstantInt::get(fixture.state->getType(), 51));
  expectSpecialized(fixture, fixture.below);
  CHECK(fixture.unknown[0]->getTerminator()->getSuccessor(0) == fixture.dispatch);
}

TEST_CASE("State guard edges evaluate signed boundaries and arithmetic shifts exactly",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (unsigned width : {1u, 8u, 16u, 32u, 64u, 128u}) {
    for (const auto &number : {llvm::APInt::getSignedMinValue(width),
                              llvm::APInt::getSignedMaxValue(width),
                              llvm::APInt::getAllOnes(width), llvm::APInt(width, 0),
                              llvm::APInt(width, 1)}) {
      for (bool shift : {false, true}) {
        CAPTURE(width, number.getLimitedValue(), shift);
        Config config;
        config.width = width;
        Fixture fixture(config);
        fixture.state->setIncomingValue(fixture.state->getBasicBlockIndex(fixture.known),
            llvm::ConstantInt::get(fixture.context, number));
        Builder b(fixture.originalBranch);
        fixture.condition = shift
            ? b.CreateICmpEQ(b.CreateAShr(fixture.state, b.getIntN(width, width - 1)),
                            llvm::ConstantInt::get(fixture.context, llvm::APInt::getAllOnes(width)))
            : b.CreateICmpSLT(fixture.state, b.getIntN(width, 0));
        fixture.originalBranch->setCondition(fixture.condition);
        expectSpecialized(fixture, number.isNegative() ? fixture.above : fixture.below);
      }
    }
  }
}

TEST_CASE("State guard edges preserve flagged load liveouts outside the proof slice",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (bool signedWrap : {false, true}) {
    Fixture fixture;
    Builder b(fixture.originalBranch);
    auto *flagged = llvm::cast<llvm::TruncInst>(b.CreateTrunc(fixture.sizeLoad, b.getInt8Ty()));
    flagged->setHasNoSignedWrap(signedWrap);
    flagged->setHasNoUnsignedWrap(!signedWrap);
    flagged->setMetadata("state.guard.flagged", llvm::MDNode::get(fixture.context, {}));
    auto *value = b.CreateZExt(b.CreateFreeze(flagged), b.getInt64Ty());
    fixture.abovePhi->setIncomingValue(fixture.abovePhi->getBasicBlockIndex(fixture.dispatch), value);
    auto *belowMixed = llvm::cast<llvm::PHINode>(fixture.belowPhi->getNextNode());
    belowMixed->setIncomingValue(belowMixed->getBasicBlockIndex(fixture.dispatch), value);
    auto *copy = expectSpecialized(fixture, fixture.below);
    if (!copy) continue;
    llvm::TruncInst *copied = nullptr;
    for (auto &instruction : *copy)
      if (instruction.getMetadata("state.guard.flagged"))
        copied = llvm::dyn_cast<llvm::TruncInst>(&instruction);
    REQUIRE(copied);
    CHECK(copied->hasNoSignedWrap() == signedWrap);
    CHECK(copied->hasNoUnsignedWrap() == !signedWrap);
    auto *load = llvm::dyn_cast<llvm::LoadInst>(copied->getOperand(0));
    REQUIRE(load);
    CHECK(load->getParent() == copy);
  }
}

TEST_CASE("State guard edges separate previous execution PHIs from current edge proofs",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (bool indirect : {false, true}) {
    for (bool conditionUsesCarried : {false, true}) {
      CAPTURE(indirect, conditionUsesCarried);
      llvm::LLVMContext context;
      llvm::Module module("old-region-phi", context);
      auto *i64 = llvm::Type::getInt64Ty(context);
      auto *function = llvm::Function::Create(llvm::FunctionType::get(i64, {i64}, false),
          llvm::GlobalValue::ExternalLinkage, "old_execution", module);
      auto *entry = llvm::BasicBlock::Create(context, "entry", function);
      auto *region = llvm::BasicBlock::Create(context, "region", function);
      auto *back = llvm::BasicBlock::Create(context, "back", function);
      auto *done = llvm::BasicBlock::Create(context, "done", function);
      Builder b(entry);
      b.CreateBr(region);
      b.SetInsertPoint(region);
      auto *state = b.CreatePHI(i64, 2, "state");
      auto *carried = b.CreatePHI(i64, 2, "carried");
      state->addIncoming(function->getArg(0), entry);
      state->addIncoming(b.getInt64(50), back);
      carried->addIncoming(function->getArg(0), entry);
      auto *previous = b.CreateAdd(state, b.getInt64(1), "previous.execution");
      auto *condition = conditionUsesCarried
          ? b.CreateICmpEQ(carried, b.getInt64(51))
          : b.CreateICmpUGT(state, b.getInt64(50));
      b.CreateCondBr(condition, done, back);
      b.SetInsertPoint(back);
      auto *incoming = indirect ? b.CreateAdd(previous, b.getInt64(1), "outside.old.execution")
                                : previous;
      carried->addIncoming(incoming, back);
      b.CreateBr(region);
      b.SetInsertPoint(done);
      b.CreateRet(carried);
      verify(module);
      const auto before = ir(module);
      const auto stats = runDispatcherSpecialization(module);
      CHECK(stats.available);
      const bool maySpecialize = indirect && !conditionUsesCarried;
      CHECK(stats.changed == maySpecialize);
      CHECK(stats.branchesRewritten == (maySpecialize ? 1 : 0));
      if (!maySpecialize) {
        CHECK(ir(module) == before);
      } else {
        // This is an available SSA value from the previous back block. It may
        // be used unchanged; its operands must never be reinterpreted using
        // the state PHI substitution for the next execution of region.
        auto *copy = back->getTerminator()->getSuccessor(0);
        CHECK(copy != region);
        if (copy != region) {
          auto *branch = llvm::cast<llvm::BranchInst>(copy->getTerminator());
          CHECK(branch->isUnconditional());
          CHECK(branch->getSuccessor(0) == back);
          CHECK(llvm::cast<llvm::Instruction>(incoming)->getParent() == back);
        }
      }
      verify(module);
    }
  }
}

TEST_CASE("State guard edges report bounded block expansion without partial edits",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  Builder b(fixture.originalBranch);
  for (unsigned index = 0; index != 257; ++index)
    b.CreateAdd(fixture.sizeLoad, b.getInt64(index), "retained.operation");
  expectUnchanged(fixture, {}, true);
}

TEST_CASE("State guard edges reject address taken blocks and vector SSA without edits",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  for (bool vector : {false, true}) {
    Fixture fixture;
    if (vector) {
      Builder b(fixture.originalBranch);
      auto *type = llvm::FixedVectorType::get(b.getInt64Ty(), 2);
      b.CreateInsertElement(llvm::PoisonValue::get(type), fixture.state, b.getInt64(0));
    } else {
      new llvm::GlobalVariable(fixture.module,
          llvm::PointerType::getUnqual(fixture.context), false,
          llvm::GlobalValue::ExternalLinkage, llvm::BlockAddress::get(fixture.dispatch),
          "address.taken");
    }
    expectUnchanged(fixture);
  }
}

TEST_CASE("State guard edges repair ordinary liveout users after a new incoming edge",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  fixture.extra->getTerminator()->eraseFromParent();
  Builder b(fixture.extra);
  b.CreateBr(fixture.guardExit);
  for (auto *block : {fixture.above, fixture.below})
    for (auto &phi : block->phis())
      phi.removeIncomingValue(fixture.extra, false);
  b.SetInsertPoint(fixture.below->getTerminator());
  auto *direct = llvm::cast<llvm::Instruction>(b.CreateAdd(
      fixture.mixed, fixture.belowPhi, "ordinary.liveout.user"));
  llvm::cast<llvm::ReturnInst>(fixture.below->getTerminator())->setOperand(0, direct);
  verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.available);
  CHECK(stats.changed);
  CHECK(stats.branchesRewritten == 1);
  verify(fixture.module);
  auto *copy = fixture.known->getTerminator()->getSuccessor(0);
  CHECK(copy != fixture.dispatch);
  if (copy == fixture.dispatch) return;
  CHECK(copy->getTerminator()->getSuccessor(0) == fixture.below);
  auto *repaired = llvm::dyn_cast<llvm::PHINode>(direct->getOperand(0));
  REQUIRE(repaired);
  CHECK(repaired->getIncomingValueForBlock(fixture.dispatch) == fixture.mixed);
  auto *newValue = llvm::dyn_cast<llvm::Instruction>(repaired->getIncomingValueForBlock(copy));
  REQUIRE(newValue);
  CHECK(newValue->getParent() == copy);
  for (auto *unknown : fixture.unknown)
    CHECK(unknown->getTerminator()->getSuccessor(0) == fixture.dispatch);
}

TEST_CASE("State guard edges reject borrowed metadata values before cloning",
          "[llvm][dispatch-specialize][state-guard-edges]") {
  Fixture fixture;
  auto *metadata = llvm::ValueAsMetadata::get(fixture.mixed);
  REQUIRE(metadata);
  REQUIRE(fixture.mixed->isUsedByMetadata());
  expectUnchanged(fixture);
}

TEST_CASE("State guard edges preserve external SSA in successor PHIs before further guards",
          "[llvm][dispatch-specialize][state-guard-edges][state-guard-external-phi]") {
  Fixture fixture;
  Builder entry(fixture.function->getEntryBlock().getTerminator());
  auto *outside = entry.CreateLoad(entry.getInt64Ty(), fixture.function->getArg(4),
                                   "outside.region.value");
  outside->setAlignment(llvm::Align(1));
  fixture.belowPhi->setIncomingValue(fixture.belowPhi->getBasicBlockIndex(fixture.dispatch), outside);
  // Keep this successor eligible for the following original-candidate scan.
  // A missing external mapping must not leave a null PHI for that scan.
  for (auto iterator = fixture.below->begin(); iterator != fixture.below->end();) {
    auto &instruction = *iterator++;
    if (llvm::isa<llvm::StoreInst>(instruction)) instruction.eraseFromParent();
  }
  auto *oldReturn = llvm::cast<llvm::ReturnInst>(fixture.below->getTerminator());
  auto *result = oldReturn->getReturnValue();
  oldReturn->eraseFromParent();
  auto *success = llvm::BasicBlock::Create(fixture.context, "external.check.success", fixture.function);
  auto *failure = llvm::BasicBlock::Create(fixture.context, "external.check.failure", fixture.function);
  Builder b(fixture.below);
  auto *guard = b.CreateICmpUGT(outside, b.getInt64(0x1000));
  b.CreateCondBr(guard, success, failure);
  b.SetInsertPoint(success);
  b.CreateRet(result);
  b.SetInsertPoint(failure);
  b.CreateRet(b.CreateXor(result, b.getInt64(0xbad)));
  verify(fixture.module);
  const auto stats = runDispatcherSpecialization(fixture.module);
  CHECK(stats.available);
  CHECK(stats.changed);
  CHECK(stats.branchesRewritten == 1);
  verify(fixture.module);
  auto *copy = fixture.known->getTerminator()->getSuccessor(0);
  CHECK(copy != fixture.dispatch);
  if (copy == fixture.dispatch) return;
  CHECK(fixture.belowPhi->getIncomingValueForBlock(copy) == outside);
  CHECK(fixture.belowPhi->getIncomingValueForBlock(fixture.dispatch) == outside);
  CHECK(fixture.below->getTerminator()->getSuccessor(0) == success);
  CHECK(fixture.below->getTerminator()->getSuccessor(1) == failure);
  CHECK(llvm::cast<llvm::BranchInst>(fixture.below->getTerminator())->getCondition() == guard);
}

TEST_CASE("Checked Guest memory edge copies preserve external successor PHI values",
          "[llvm][guest-memory-edges][state-guard-edges][guest-edge-external-phi]") {
  for (bool useArgument : {false, true}) {
    CAPTURE(useArgument);
    llvm::LLVMContext context;
    llvm::Module module("guest-edge-outside-phi", context);
    module.setDataLayout("e-p:64:64");
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {ptr, ptr, i64, i64, i64, llvm::Type::getInt1Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage, "guest_checked_external", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *known = llvm::BasicBlock::Create(context, "known", function);
    auto *unknown = llvm::BasicBlock::Create(context, "unknown", function);
    auto *join = llvm::BasicBlock::Create(context, "shared.check", function);
    auto *success = llvm::BasicBlock::Create(context, "success", function);
    auto *failure = llvm::BasicBlock::Create(context, "failure", function);
    auto *done = llvm::BasicBlock::Create(context, "done", function);
    Builder b(entry);
    auto *data = b.CreateLoad(ptr, function->getArg(1), "guest.base");
    data->setAlignment(llvm::Align(1));
    auto *sizeAddress = b.CreateGEP(b.getInt8Ty(), function->getArg(1), b.getInt64(8));
    auto *external = b.CreateLoad(i64, sizeAddress, "external.value");
    external->setAlignment(llvm::Align(1));
    llvm::Value *outside = useArgument ? static_cast<llvm::Value *>(function->getArg(4)) : external;
    auto *knownFrame = b.CreateAdd(function->getArg(2), b.getInt64(-8));
    auto *storedIndex = b.CreateAdd(function->getArg(2), b.getInt64(-128));
    auto *storedAddress = b.CreateGEP(b.getInt8Ty(), data, storedIndex);
    b.CreateCondBr(function->getArg(5), known, unknown);
    b.SetInsertPoint(known);
    auto *stored = b.CreateStore(b.getInt64(50), storedAddress);
    stored->setAlignment(llvm::Align(1));
    b.CreateBr(join);
    b.SetInsertPoint(unknown);
    b.CreateBr(join);
    b.SetInsertPoint(join);
    auto *frame = b.CreatePHI(i64, 2, "frame");
    frame->addIncoming(knownFrame, known);
    frame->addIncoming(function->getArg(3), unknown);
    auto *index = b.CreateAdd(frame, b.getInt64(-120), "load.index");
    auto *size = b.CreateLoad(i64, sizeAddress, "memory.size");
    size->setAlignment(llvm::Align(1));
    auto *fits = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
        b.CreateICmpULE(index, b.CreateAdd(size, b.getInt64(-8))));
    auto *originalGuard = b.CreateCondBr(fits, success, failure);
    b.SetInsertPoint(success);
    auto *address = b.CreateGEP(b.getInt8Ty(), data, index);
    auto *load = b.CreateLoad(i64, address, "checked.load");
    load->setAlignment(llvm::Align(1));
    b.CreateBr(done);
    b.SetInsertPoint(done);
    auto *donePhi = b.CreatePHI(i64, 1, "outside.success");
    donePhi->addIncoming(outside, success);
    b.CreateRet(b.CreateAdd(load, donePhi));
    b.SetInsertPoint(failure);
    auto *failurePhi = b.CreatePHI(i64, 1, "outside.failure");
    failurePhi->addIncoming(outside, join);
    b.CreateRet(b.CreateXor(failurePhi, b.getInt64(0xbad)));
    verify(module);
    const auto stats = detail::runGuestMemorySSA(module, {65536, false});
    CHECK(stats.available);
    CHECK(stats.changed);
    CHECK(stats.loadsForwarded == 1);
    std::string verificationError;
    llvm::raw_string_ostream verificationStream(verificationError);
    const bool invalid = llvm::verifyModule(module, &verificationStream);
    INFO(verificationError);
    CHECK_FALSE(invalid);
    // Report each old-code source variant without dereferencing invalid PHIs.
    if (invalid) continue;
    auto *copy = known->getTerminator()->getSuccessor(0);
    CHECK(copy != join);
    if (copy == join) continue;
    CHECK(unknown->getTerminator()->getSuccessor(0) == join);
    CHECK(join->getTerminator() == originalGuard);
    CHECK(load->getParent() == success);
    CHECK(failurePhi->getIncomingValueForBlock(copy) == outside);
    auto *copiedSuccess = copy->getTerminator()->getSuccessor(0);
    CHECK(donePhi->getIncomingValueForBlock(copiedSuccess) == outside);
    CHECK(donePhi->getIncomingValueForBlock(success) == outside);
    CHECK(failurePhi->getIncomingValueForBlock(join) == outside);
  }
}
