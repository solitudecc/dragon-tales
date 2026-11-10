#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMDeflattening.hpp>
#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/IR/Verifier.h>

#include "../../scripts/validation/same_function_select_fixtures.hpp"

using namespace dragon::llvmlifter;
using namespace dragon::validation::select_fixture;

namespace {
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module;
  View view;
  explicit Fixture(const Config &config) : module(config.name, context), view(build(module, config)) {}
  Fixture() : module("loop-select", context), view(buildLoop(module)) {}
};

void verify(const llvm::Module &module) {
  REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
}

void expectStats(Fixture &fixture, bool changed, std::size_t edges,
                 const DeflatteningOptions &options = {}) {
  verify(fixture.module);
  const auto stats = runProvableDeflattening(fixture.module, options);
  CHECK(stats.available);
  CHECK(stats.changed == changed);
  CHECK(stats.dispatchersVisited == 1);
  CHECK(stats.edgesRewritten == edges);
  CHECK(stats.dispatchersRewritten == (changed ? 1 : 0));
  CHECK(stats.unresolvedDispatchers == (changed ? 0 : 1));
  CHECK(stats.crossCallDispatchersRewritten == 0);
  CHECK(stats.crossCallSummariesConsumed == 0);
  CHECK(stats.crossCallDirectStoresConsumed == 0);
  verify(fixture.module);
}

void expectBranch(const View &view, bool conditional) {
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(view.predecessor->getTerminator());
  REQUIRE(branch);
  CHECK(branch->isConditional() == conditional);
  CHECK(branch->getSuccessor(0) == view.trueTarget);
  if (conditional && branch->isConditional()) {
    CHECK(branch->getCondition() == view.condition);
    CHECK(branch->getSuccessor(1) == view.falseTarget);
  }
}
}

TEST_CASE("Conditional state edges preserve freeze and noundef conditions",
          "[llvm][dispatch-deflatten][conditional-state]") {
  for (const auto form : {Form::Phi, Form::Memory}) {
    for (const auto condition : {Condition::Freeze, Condition::NoUndef}) {
      Config config{"stable-select"};
      config.form = form;
      config.condition = condition;
      Fixture fixture(config);
      expectStats(fixture, true, 1);
      expectBranch(fixture.view, true);
      const auto repeat = runProvableDeflattening(fixture.module);
      CHECK_FALSE(repeat.changed);
      CHECK(repeat.edgesRewritten == 0);
      verify(fixture.module);
    }
  }
}

TEST_CASE("Conditional state edges retain default and same-target semantics",
          "[llvm][dispatch-deflatten][conditional-state]") {
  for (const auto form : {Form::Phi, Form::Memory}) {
    for (const bool sameTarget : {false, true}) {
      Config config{"default-or-same"};
      config.form = form;
      config.defaultTrue = !sameTarget;
      config.sameTarget = sameTarget;
      Fixture fixture(config);
      expectStats(fixture, true, 1);
      expectBranch(fixture.view, !sameTarget);
    }
  }
}

TEST_CASE("Conditional state edges retain guards PHIs and live-out values",
          "[llvm][dispatch-deflatten][conditional-state]") {
  for (const auto form : {Form::Phi, Form::Memory}) {
    for (unsigned shape = 0; shape != 3; ++shape) {
      Config config{"unsupported-shape"};
      config.form = form;
      config.outerGuard = shape == 0;
      config.targetPhi = shape == 1;
      config.liveOut = shape == 2;
      Fixture fixture(config);
      expectStats(fixture, false, 0);
      auto *branch = llvm::cast<llvm::BranchInst>(fixture.view.predecessor->getTerminator());
      CHECK(branch->getSuccessor(0) == fixture.view.dispatch);
      if (config.outerGuard) {
        REQUIRE(branch->isConditional());
        CHECK(branch->getSuccessor(1) == fixture.view.guardExit);
      }
    }
  }
}

TEST_CASE("Conditional state edges reject unproved and undefined conditions",
          "[llvm][dispatch-deflatten][conditional-state]") {
  for (const auto form : {Form::Phi, Form::Memory}) {
    for (const auto condition : {Condition::Unknown, Condition::Undef, Condition::Poison}) {
      for (const bool sameTarget : {false, true}) {
        Config config{"bad-condition"};
        config.form = form;
        config.condition = condition;
        config.sameTarget = sameTarget;
        Fixture fixture(config);
        expectStats(fixture, false, 0);
      }
    }
    // Freezing the STATE after a poison select does not establish the select
    // condition or restrict the frozen state to either arm's constant.
    Config config{"frozen-poison-state"};
    config.form = form;
    config.condition = Condition::Poison;
    config.freezeState = true;
    Fixture fixture(config);
    expectStats(fixture, false, 0);
  }
}

TEST_CASE("Conditional state edges reject unknown arms and retain unused poison rules",
          "[llvm][dispatch-deflatten][conditional-state]") {
  for (const auto form : {Form::Phi, Form::Memory}) {
    for (const auto arm : {Arm::Undef, Arm::Poison, Arm::FreezePoison}) {
      Config config{"bad-arm"};
      config.form = form;
      config.trueArm = arm;
      Fixture fixture(config);
      expectStats(fixture, false, 0);
    }
    for (const auto condition : {Condition::False, Condition::True,
                                 Condition::ComputedFalse, Condition::ComputedTrue}) {
      const bool trueCondition = condition == Condition::True || condition == Condition::ComputedTrue;
      Config config{"unused-poison-arm"};
      config.form = form;
      config.condition = condition;
      config.trueArm = trueCondition ? Arm::Constant : Arm::Poison;
      config.falseArm = trueCondition ? Arm::Poison : Arm::Constant;
      Fixture fixture(config);
      expectStats(fixture, true, 1);
      auto *branch = llvm::cast<llvm::BranchInst>(fixture.view.predecessor->getTerminator());
      REQUIRE(branch->isUnconditional());
      CHECK(branch->getSuccessor(0) == (trueCondition ? fixture.view.trueTarget : fixture.view.falseTarget));
    }
  }
}

TEST_CASE("Conditional state proof retains an edge at its expression budget",
          "[llvm][dispatch-deflatten][conditional-state]") {
  for (const auto form : {Form::Phi, Form::Memory}) {
    Config config{"arm-budget"};
    config.form = form;
    config.deepTrueArm = true;
    Fixture fixture(config);
    DeflatteningOptions options;
    options.maxExpressionNodes = 1;
    expectStats(fixture, false, 0, options);
  }
  Config config{"scan-budget"};
  config.form = Form::Memory;
  config.gap = Gap::Compute;
  config.conditionalState = false;
  config.extraPureComputations = 2;
  Fixture fixture(config);
  DeflatteningOptions options;
  options.maxExpressionNodes = 1;
  expectStats(fixture, false, 0, options);
}

TEST_CASE("Conditional memory state preserves loop iteration effects",
          "[llvm][dispatch-deflatten][conditional-state]") {
  Fixture fixture;
  expectStats(fixture, true, 2);
  expectBranch(fixture.view, true);
}

TEST_CASE("Reaching state stores retain computations reads and CALL barriers",
          "[llvm][dispatch-deflatten][conditional-state]") {
  for (const auto gap : {Gap::Compute, Gap::Load, Gap::ReadOnlyCall,
                         Gap::CallThenStore, Gap::StoreThenCall}) {
    Config config{"store-gap"};
    config.form = Form::Memory;
    config.gap = gap;
    config.conditionalState = gap == Gap::CallThenStore || gap == Gap::StoreThenCall;
    Fixture fixture(config);
    const bool changed = gap != Gap::StoreThenCall;
    expectStats(fixture, changed, changed ? 1 : 0);
    REQUIRE(fixture.view.gapInstruction);
    CHECK(fixture.view.gapInstruction->getParent() == fixture.view.predecessor);
    if (changed) expectBranch(fixture.view, gap == Gap::CallThenStore);
    else CHECK(fixture.view.predecessor->getTerminator()->getSuccessor(0) == fixture.view.dispatch);
  }
}

namespace {
enum class FrozenValueKind { TruncNUW, TruncNSW, ZExtNNeg, DisjointOr, PlainOr };

llvm::Value *frozenFlaggedValue(Builder &b, FrozenValueKind kind, bool boolean) {
  llvm::Value *value;
  if (kind == FrozenValueKind::DisjointOr || kind == FrozenValueKind::PlainOr) {
    value = b.CreateOr(b.getIntN(boolean ? 1 : 8, 1),
                       b.getIntN(boolean ? 1 : 8, 1), "overlapping.or");
    llvm::cast<llvm::PossiblyDisjointInst>(value)->setIsDisjoint(
        kind == FrozenValueKind::DisjointOr);
  } else if (kind == FrozenValueKind::ZExtNNeg) {
    // i1 true is signed -1, just as i8 128 is signed -128: both violate nneg.
    auto *source = b.getIntN(boolean ? 1 : 8, boolean ? 1 : 128);
    auto *wide = llvm::cast<llvm::ZExtInst>(b.CreateZExt(
        source, b.getIntNTy(boolean ? 2 : 16), "negative.zext"));
    wide->setNonNeg(true);
    value = boolean ? b.CreateTrunc(wide, b.getInt1Ty()) : wide;
  } else {
    // 2 cannot fit unsigned i1; +1 cannot fit signed i1. The wider selector
    // cases likewise violate nuw at 256->i8 and nsw at +128->i8.
    auto *source = b.getIntN(boolean ? 2 : 16,
        kind == FrozenValueKind::TruncNUW ? (boolean ? 2 : 256) : (boolean ? 1 : 128));
    auto *trunc = llvm::cast<llvm::TruncInst>(b.CreateTrunc(
        source, b.getIntNTy(boolean ? 1 : 8), "overflow.trunc"));
    if (kind == FrozenValueKind::TruncNUW) trunc->setHasNoUnsignedWrap(true);
    else trunc->setHasNoSignedWrap(true);
    value = trunc;
  }
  // A flagged poison result becomes defined and stable with arbitrary bits;
  // it is not the ordinary unflagged result. PlainOr is a defined control.
  return b.CreateFreeze(value, "frozen.flagged.cast");
}

View frozenValueFixture(llvm::Module &module, FrozenValueKind kind,
                        bool asCondition, bool memory = false) {
  auto &context = module.getContext();
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
      {llvm::PointerType::getUnqual(context)}, false);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, "flagged_cast", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *one = llvm::BasicBlock::Create(context, "one", function);
  auto *two = llvm::BasicBlock::Create(context, "two", function);
  auto *fallback = llvm::BasicBlock::Create(context, "default", function);
  Builder b(entry);
  auto *frozen = frozenFlaggedValue(b, kind, asCondition);
  auto *incoming = asCondition
      ? b.CreateSelect(frozen, b.getInt64(1), b.getInt64(2)) : frozen;
  if (memory) b.CreateStore(incoming, function->getArg(0));
  b.CreateBr(dispatch);
  b.SetInsertPoint(dispatch);
  llvm::Value *state;
  if (memory) state = b.CreateLoad(incoming->getType(), function->getArg(0));
  else {
    auto *phi = b.CreatePHI(incoming->getType(), 1, "state");
    phi->addIncoming(incoming, entry);
    state = phi;
  }
  auto *sw = b.CreateSwitch(state, fallback, asCondition ? 2 : 1);
  if (asCondition) {
    sw->addCase(b.getInt64(1), one);
    sw->addCase(b.getInt64(2), two);
  } else {
    sw->addCase(b.getIntN(state->getType()->getIntegerBitWidth(),
                         kind == FrozenValueKind::TruncNUW ? 0 :
                         (kind == FrozenValueKind::DisjointOr || kind == FrozenValueKind::PlainOr) ? 1 : 128), one);
  }
  for (auto *block : {one, two, fallback}) {
    b.SetInsertPoint(block);
    b.CreateRetVoid();
  }
  return {function, entry, dispatch, one, two, nullptr, frozen, nullptr};
}
}

TEST_CASE("Frozen flagged casts do not prove unique dispatcher selectors",
          "[llvm][dispatch-deflatten][conditional-state][flagged-casts]") {
  for (const auto kind : {FrozenValueKind::TruncNUW, FrozenValueKind::TruncNSW, FrozenValueKind::ZExtNNeg}) {
    for (const bool specialize : {false, true}) {
      llvm::LLVMContext context;
      llvm::Module module("flagged-selector", context);
      const auto view = frozenValueFixture(module, kind, false);
      verify(module);
      if (specialize) {
        const auto stats = runDispatcherSpecialization(module);
        CHECK(stats.available);
        CHECK_FALSE(stats.changed);
        CHECK(stats.switchesRewritten == 0);
        CHECK(stats.unresolvedSwitches == 1);
      } else {
        const auto stats = runProvableDeflattening(module);
        CHECK(stats.available);
        CHECK_FALSE(stats.changed);
        CHECK(stats.edgesRewritten == 0);
        CHECK(stats.unresolvedDispatchers == 1);
      }
      CHECK(llvm::isa<llvm::SwitchInst>(view.dispatch->getTerminator()));
      CHECK(view.predecessor->getTerminator()->getSuccessor(0) == view.dispatch);
      verify(module);
    }
  }
}

TEST_CASE("Frozen flagged cast conditions preserve their actual stable choice",
          "[llvm][dispatch-deflatten][conditional-state][flagged-casts]") {
  for (const auto kind : {FrozenValueKind::TruncNUW, FrozenValueKind::TruncNSW, FrozenValueKind::ZExtNNeg}) {
    for (const bool memory : {false, true}) {
      llvm::LLVMContext context;
      llvm::Module module("flagged-condition", context);
      const auto view = frozenValueFixture(module, kind, true, memory);
      verify(module);
      const auto stats = runProvableDeflattening(module);
      CHECK(stats.available);
      CHECK(stats.changed);
      CHECK(stats.edgesRewritten == 1);
      CHECK(stats.unresolvedDispatchers == 0);
      // Reuse the freeze itself; never pick an arm using the unflagged cast's
      // bits. This is a structural contract, not a fixed-value native oracle.
      expectBranch(view, true);
      verify(module);
    }
  }
}

TEST_CASE("Frozen disjoint OR selectors retain poison while plain OR retains precision",
          "[llvm][dispatch-deflatten][conditional-state][disjoint-or]") {
  for (const bool disjoint : {false, true}) {
    for (const bool specialize : {false, true}) {
      llvm::LLVMContext context;
      llvm::Module module("frozen-or-selector", context);
      const auto view = frozenValueFixture(module,
          disjoint ? FrozenValueKind::DisjointOr : FrozenValueKind::PlainOr, false);
      verify(module);
      if (specialize) {
        const auto stats = runDispatcherSpecialization(module);
        CHECK(stats.available);
        CHECK(stats.changed == !disjoint);
        CHECK(stats.switchesRewritten == (disjoint ? 0 : 1));
        CHECK(stats.unresolvedSwitches == (disjoint ? 1 : 0));
        CHECK(llvm::isa<llvm::SwitchInst>(view.dispatch->getTerminator()) == disjoint);
        if (!disjoint) CHECK(view.dispatch->getTerminator()->getSuccessor(0) == view.trueTarget);
      } else {
        const auto stats = runProvableDeflattening(module);
        CHECK(stats.available);
        CHECK(stats.changed == !disjoint);
        CHECK(stats.edgesRewritten == (disjoint ? 0 : 1));
        CHECK(stats.unresolvedDispatchers == (disjoint ? 1 : 0));
        CHECK(view.predecessor->getTerminator()->getSuccessor(0) ==
              (disjoint ? view.dispatch : view.trueTarget));
        CHECK(llvm::isa<llvm::SwitchInst>(view.dispatch->getTerminator()));
      }
      verify(module);
    }
  }
}

TEST_CASE("Frozen disjoint OR conditions retain their actual choice",
          "[llvm][dispatch-deflatten][conditional-state][disjoint-or]") {
  for (const bool disjoint : {false, true}) {
    for (const bool memory : {false, true}) {
      llvm::LLVMContext context;
      llvm::Module module("frozen-or-condition", context);
      const auto view = frozenValueFixture(module,
          disjoint ? FrozenValueKind::DisjointOr : FrozenValueKind::PlainOr, true, memory);
      verify(module);
      const auto stats = runProvableDeflattening(module);
      CHECK(stats.available);
      CHECK(stats.changed);
      CHECK(stats.edgesRewritten == 1);
      CHECK(stats.unresolvedDispatchers == 0);
      // Plain (1|1) is proved true. Overlapping disjoint OR is poison; its
      // freeze is an arbitrary stable bool, so that actual choice must remain.
      expectBranch(view, disjoint);
      verify(module);
    }
  }
}
