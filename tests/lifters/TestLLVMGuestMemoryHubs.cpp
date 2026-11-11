#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Effect { None, Call, AliasStore, PartialStore, AtomicStore, VolatileLoad, Fence };
struct Config {
  unsigned hubs = 1;
  unsigned guards = 1;
  unsigned padding = 0;
  Effect sourceEffect = Effect::None;
  Effect hubEffect = Effect::None;
  bool outer = false;
  bool reentry = false;
  bool directCycle = false;
  bool externalEH = false;
  bool oldHistory = false;
  bool subtract = false;
  bool poisonFlag = false;
  bool narrow = false;
  bool differentBase = false;
  bool partialSource = false;
};

void mark(llvm::Instruction *instruction, const char *name) {
  instruction->setMetadata(name, llvm::MDNode::get(instruction->getContext(), {}));
}

struct View {
  llvm::Function *function;
  llvm::BasicBlock *known, *join, *success, *failure, *done, *bypass, *reentry;
  std::vector<llvm::BasicBlock *> hubs, unknown;
  std::vector<llvm::PHINode *> frames;
  llvm::PHINode *joinedFrame, *joinedLive;
  llvm::LoadInst *load;
  llvm::StoreInst *store;
  llvm::BranchInst *sourceBranch;
  llvm::Value *base;
};

void addEffect(Builder &b, Effect effect, llvm::Value *base,
                 llvm::Value *slot, llvm::Value *alias) {
  if (effect == Effect::Call) {
    const auto callee = b.GetInsertBlock()->getModule()->getOrInsertFunction(
        "unknown_hub_writer", llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false));
    b.CreateCall(callee, {base});
  } else if (effect == Effect::AliasStore) {
    b.CreateStore(b.getInt64(7), alias)->setAlignment(llvm::Align(1));
  } else if (effect == Effect::PartialStore) {
    b.CreateStore(b.getInt8(7), slot)->setAlignment(llvm::Align(1));
  } else if (effect == Effect::AtomicStore) {
    auto *store = b.CreateStore(b.getInt64(7), slot);
    store->setAlignment(llvm::Align(8));
    store->setAtomic(llvm::AtomicOrdering::Release);
  } else if (effect == Effect::VolatileLoad) {
    auto *load = b.CreateLoad(b.getInt64Ty(), slot);
    load->setAlignment(llvm::Align(1)); load->setVolatile(true);
  } else if (effect == Effect::Fence) {
    b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
  }
}

View build(llvm::Module &module, const Config &config) {
  module.setDataLayout("e-p:64:64");
  auto &context = module.getContext();
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, ptr, i64, llvm::Type::getInt32Ty(context), i64, i64, i64,
       llvm::Type::getInt1Ty(context), llvm::Type::getInt1Ty(context), ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "checked_hubs", module);
  auto block = [&](const char *name) {return llvm::BasicBlock::Create(context, name, function);};
  auto *entry = block("entry"), *known = block("known.store");
  std::vector<llvm::BasicBlock *> hubs, unknown;
  for (unsigned n = 0; n < config.hubs; ++n) hubs.push_back(block("hub"));
  for (unsigned n = 0; n <= config.hubs; ++n) unknown.push_back(block("unknown"));
  auto *join = block("checked.join"), *success = block("checked.success");
  auto *failure = block("failure"), *done = block("done");
  auto *bypass = config.outer ? block("source.bypass") : nullptr;
  auto *reentry = config.reentry ? block("outside.reentry") : nullptr;
  Builder b(entry);
  auto *base = b.CreateLoad(ptr, function->getArg(1), "guest.base");
  base->setAlignment(llvm::Align(1));
  auto *loadBase = config.differentBase
      ? b.CreateLoad(ptr, function->getArg(1), "other.guest.base") : base;
  auto *indexType = config.narrow ? b.getInt32Ty() : i64;
  auto *stack = config.narrow ? b.CreateTrunc(function->getArg(3), indexType) : function->getArg(3);
  auto *knownFrame = config.subtract
      ? b.CreateSub(stack, llvm::ConstantInt::get(indexType, 8), "known.frame")
      : b.CreateAdd(stack, llvm::ConstantInt::getSigned(indexType, -8), "known.frame");
  auto *storeIndex = config.subtract
      ? b.CreateSub(knownFrame, llvm::ConstantInt::get(indexType, 120), "store.index")
      : b.CreateAdd(knownFrame, llvm::ConstantInt::getSigned(indexType, -120), "store.index");
  auto *slot = b.CreateGEP(b.getInt8Ty(), base, storeIndex, "store.slot");
  auto *route = b.CreateSwitch(function->getArg(4), unknown.back(), config.hubs + 1);
  route->addCase(b.getInt32(0), known);
  for (unsigned n = 0; n < config.hubs; ++n) route->addCase(b.getInt32(n + 1), unknown[n]);
  if (config.oldHistory) {
    for (unsigned n = 0; n < route->getNumSuccessors(); ++n) route->setSuccessor(n, unknown.front());
  }
  b.SetInsertPoint(known);
  auto *store = b.CreateStore(config.partialSource ? b.getInt8(50) : b.getInt64(50), slot);
  store->setAlignment(llvm::Align(1)); mark(store, "hub.source.store");
  addEffect(b, config.sourceEffect, base, slot, function->getArg(10));
  auto *sourceBranch = config.outer
      ? b.CreateCondBr(function->getArg(8), hubs.front(), bypass)
      : b.CreateBr(hubs.front());
  for (unsigned n = 0; n < unknown.size(); ++n) {
    b.SetInsertPoint(unknown[n]);
    b.CreateBr(n < config.hubs ? hubs[n] : config.reentry ? hubs.front() : join);
  }
  std::vector<llvm::PHINode *> frames, lives;
  for (unsigned n = 0; n < hubs.size(); ++n) {
    b.SetInsertPoint(hubs[n]);
    auto *frame = b.CreatePHI(indexType, 3, "hub.frame");
    auto *live = b.CreatePHI(i64, 3, "hub.live");
    frame->addIncoming(n ? static_cast<llvm::Value *>(frames.back()) : knownFrame,
                       n ? hubs[n - 1] : known);
    live->addIncoming(n ? static_cast<llvm::Value *>(lives.back()) : function->getArg(7),
                      n ? hubs[n - 1] : known);
    auto *unknownFrame = config.narrow
        ? b.CreateTrunc(function->getArg(5), indexType) : function->getArg(5);
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(unknownFrame))
      if (instruction->getParent() == hubs[n]) {
        instruction->removeFromParent();
        instruction->insertBefore(unknown[n]->getTerminator()->getIterator());
      }
    frame->addIncoming(unknownFrame, unknown[n]);
    live->addIncoming(b.getInt64(0x100 + n), unknown[n]);
    if (config.reentry && n == 0) {
      auto *lastUnknownFrame = config.narrow
          ? b.CreateTrunc(function->getArg(6), indexType) : function->getArg(6);
      if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(lastUnknownFrame))
        if (instruction->getParent() == hubs[n]) {
          instruction->removeFromParent();
          instruction->insertBefore(unknown.back()->getTerminator()->getIterator());
        }
      frame->addIncoming(lastUnknownFrame, unknown.back());
      live->addIncoming(b.getInt64(0x180), unknown.back());
    }
    frames.push_back(frame); lives.push_back(live);
    for (unsigned padding = 0; padding < config.padding; ++padding)
      b.CreateXor(live, b.getInt64(padding + 1), "hub.padding");
    if (n + 1 == hubs.size()) addEffect(b, config.hubEffect, base, slot, function->getArg(10));
    b.CreateBr(n + 1 == hubs.size() ? join : hubs[n + 1]);
  }
  b.SetInsertPoint(join);
  auto *joinedFrame = b.CreatePHI(indexType, 2, "joined.frame");
  auto *joinedLive = b.CreatePHI(i64, 2, "joined.live");
  auto *iteration = b.CreatePHI(i64, 2, "iteration");
  joinedFrame->addIncoming(frames.back(), hubs.back());
  joinedLive->addIncoming(lives.back(), hubs.back());
  iteration->addIncoming(b.getInt64(0), hubs.back());
  if (config.reentry) {
    joinedFrame->addIncoming(joinedFrame, reentry);
    joinedLive->addIncoming(joinedLive, reentry);
    iteration->addIncoming(b.getInt64(1), reentry);
  } else {
    auto *directFrame = config.narrow ? b.CreateTrunc(function->getArg(6), indexType) : function->getArg(6);
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(directFrame))
      if (instruction->getParent() == join) {
        instruction->removeFromParent();
        instruction->insertBefore(unknown.back()->getTerminator()->getIterator());
      }
    joinedFrame->addIncoming(directFrame, unknown.back());
    joinedLive->addIncoming(b.getInt64(0x200), unknown.back());
    iteration->addIncoming(b.getInt64(0), unknown.back());
  }
  auto *index = config.subtract
      ? b.CreateSub(joinedFrame, llvm::ConstantInt::get(indexType, 120), "load.index")
      : b.CreateAdd(joinedFrame, llvm::ConstantInt::getSigned(indexType, -120), "load.index");
  if (config.poisonFlag) llvm::cast<llvm::BinaryOperator>(index)->setHasNoUnsignedWrap(true);
  auto *business = config.reentry
      ? b.CreateXor(lives.front(), b.CreateZExtOrTrunc(joinedFrame, i64), "cross.original.use")
      : b.CreateXor(joinedLive, b.CreateZExtOrTrunc(joinedFrame, i64), "business");
  auto *sizeAddress = b.CreateGEP(b.getInt8Ty(), function->getArg(1), b.getInt64(8));
  auto *size = b.CreateLoad(i64, sizeAddress, "guest.size"); size->setAlignment(llvm::Align(1));
  auto *wideIndex = config.narrow ? b.CreateSExt(index, i64) : index;
  auto *fits = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
      b.CreateICmpULE(wideIndex, b.CreateAdd(size, b.getInt64(-8))));
  for (unsigned n = 1; n < config.guards; ++n) {
    auto *next = block("more.checks");
    auto *guard = b.CreateCondBr(fits, next, failure); mark(guard, "hub.checked.guard");
    b.SetInsertPoint(next);
  }
  auto *guard = b.CreateCondBr(fits, success, failure); mark(guard, "hub.checked.guard");
  b.SetInsertPoint(success);
  auto *loadAddress = b.CreateGEP(b.getInt8Ty(), loadBase, index);
  auto *load = b.CreateLoad(i64, loadAddress, "actual.guest.load");
  load->setAlignment(llvm::Align(1)); mark(load, "hub.actual.load");
  if (config.oldHistory) {
    frames.front()->setIncomingValue(frames.front()->getBasicBlockIndex(known), frames.front());
    b.CreateCondBr(function->getArg(9), known, done);
  } else if (config.directCycle) {
    frames.front()->addIncoming(joinedFrame, success);
    lives.front()->addIncoming(joinedLive, success);
    b.CreateCondBr(function->getArg(9), hubs.front(), done);
  } else if (config.reentry) {
    b.CreateCondBr(b.CreateAnd(function->getArg(9), b.CreateICmpEQ(iteration, b.getInt64(0))), reentry, done);
  } else b.CreateBr(done);
  b.SetInsertPoint(done);
  auto *bonus = b.CreatePHI(i64, 1, "outside.phi"); bonus->addIncoming(function->getArg(7), success);
  auto *result = b.CreateAdd(load, b.CreateXor(business, bonus));
  if (config.externalEH) {
    auto personality = module.getOrInsertFunction("__gxx_personality_v0",
        llvm::FunctionType::get(b.getInt32Ty(), true));
    function->setPersonalityFn(llvm::cast<llvm::Constant>(personality.getCallee()));
    const auto callee = module.getOrInsertFunction("may_throw_after_hub",
        llvm::FunctionType::get(b.getVoidTy(), false));
    auto *normal = block("invoke.normal"), *landing = block("invoke.unwind");
    b.CreateInvoke(callee, normal, landing, {});
    b.SetInsertPoint(normal); b.CreateRet(result);
    b.SetInsertPoint(landing);
    auto *pad = b.CreateLandingPad(llvm::StructType::get(ptr, b.getInt32Ty()), 0);
    pad->setCleanup(true);
    // This is an external exceptional live-out use of an actual region
    // definition. The new hub domain deliberately does not repair it.
    b.CreateRet(business);
  } else b.CreateRet(result);
  b.SetInsertPoint(failure);
  auto *reason = b.CreatePHI(i64, config.guards, "failure.reason");
  for (auto *predecessor : llvm::predecessors(failure)) reason->addIncoming(b.getInt64(0x55), predecessor);
  b.CreateRet(b.CreateXor(business, reason));
  if (bypass) {b.SetInsertPoint(bypass); b.CreateRet(function->getArg(7));}
  if (reentry) {
    b.SetInsertPoint(reentry);
    // An unknown aliasing write makes the next original LOAD a real fresh
    // access. It must not inherit the copy's just-forwarded value.
    b.CreateStore(b.getInt64(7), function->getArg(10))->setAlignment(llvm::Align(1));
    b.CreateBr(join);
  }
  return {function, known, join, success, failure, done, bypass, reentry,
          hubs, unknown, frames, joinedFrame, joinedLive, load, store, sourceBranch, base};
}

struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"checked-memory-hubs", context};
  Config config;
  View view;
  explicit Fixture(Config selected = {}) : config(selected), view(build(module, config)) {}
};

void verify(llvm::Module &module) {REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));}
std::string text(llvm::Module &module) {
  std::string result; llvm::raw_string_ostream stream(result); module.print(stream, nullptr); return result;
}
unsigned marked(llvm::Function &function, const char *tag) {
  unsigned count = 0; for (auto &block : function) for (auto &op : block) count += op.getMetadata(tag) != nullptr; return count;
}

void expectForward(Fixture &fixture) {
  auto &v = fixture.view; verify(fixture.module);
  const auto originalBlocks = v.function->size();
  const auto stats = detail::runGuestMemorySSA(fixture.module, {65536, false});
  CHECK(stats.available); CHECK(stats.changed); CHECK(stats.complete); CHECK(stats.loadsForwarded == 1);
  verify(fixture.module);
  CHECK(v.known->getTerminator() == v.sourceBranch);
  auto *copy = v.sourceBranch->getSuccessor(0); REQUIRE(copy != v.hubs.front());
  CHECK(v.sourceBranch->isConditional() == fixture.config.outer);
  if (fixture.config.outer) CHECK(v.sourceBranch->getSuccessor(1) == v.bypass);
  CHECK(v.function->size() - originalBlocks == fixture.config.hubs + fixture.config.guards + 1);
  CHECK(marked(*v.function, "hub.actual.load") == 1);
  CHECK(marked(*v.function, "hub.source.store") == 1);
  CHECK(v.load->getParent() == v.success); CHECK(v.store->getParent() == v.known);
  CHECK(v.frames.front()->getBasicBlockIndex(v.known) == -1);
  for (unsigned n = 0; n < v.unknown.size(); ++n)
    CHECK(v.unknown[n]->getTerminator()->getSuccessor(0) ==
          (n < v.hubs.size() ? v.hubs[n] : fixture.config.reentry ? v.hubs.front() : v.join));
  for (unsigned n = 0; n < fixture.config.hubs; ++n) {
    CHECK(copy->phis().empty());
    auto *branch = llvm::dyn_cast<llvm::BranchInst>(copy->getTerminator()); REQUIRE(branch);
    REQUIRE(branch->isUnconditional()); copy = branch->getSuccessor(0);
  }
  for (unsigned n = 0; n < fixture.config.guards; ++n) {
    CHECK(copy->phis().empty());
    auto *branch = llvm::dyn_cast<llvm::BranchInst>(copy->getTerminator()); REQUIRE(branch);
    REQUIRE(branch->isConditional()); CHECK(branch->getSuccessor(1) == v.failure);
    copy = branch->getSuccessor(0);
  }
  CHECK(copy != v.success);
  for (auto &op : *copy) CHECK(op.getMetadata("hub.actual.load") == nullptr);
  CHECK(v.done->phis().begin()->getBasicBlockIndex(copy) >= 0);
  if (fixture.config.reentry) {
    CHECK(v.reentry->getTerminator()->getSuccessor(0) == v.join);
    CHECK(v.joinedFrame->getBasicBlockIndex(v.reentry) >= 0);
    llvm::DominatorTree dom(*v.function);
    for (auto &block : *v.function) for (auto &op : block)
      if (op.getName() == "cross.original.use")
        if (auto *value = llvm::dyn_cast<llvm::Instruction>(op.getOperand(0))) CHECK(dom.dominates(value, &op));
  }
  verify(fixture.module);
}

void expectUnchanged(Fixture &fixture, MemoryForwardOptions options = {65536, false}, bool budgeted = false) {
  verify(fixture.module); const auto before = text(fixture.module);
  const auto stats = detail::runGuestMemorySSA(fixture.module, options);
  CHECK(stats.available); CHECK_FALSE(stats.changed); CHECK(stats.loadsForwarded == 0);
  if (budgeted) {CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0);}
  CHECK(text(fixture.module) == before); verify(fixture.module);
}
} // namespace

TEST_CASE("Checked Guest hubs select every join PHI on the proved path",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  for (unsigned hubs : {1u, 2u, 4u}) DYNAMIC_SECTION("hubs " << hubs) {
    Config config; config.hubs = hubs; Fixture fixture(config); expectForward(fixture);
  }
}

TEST_CASE("Checked Guest hubs retain outer guards failures and external PHIs",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  Config config; config.hubs = 2; config.guards = 2; config.outer = true;
  Fixture fixture(config); expectForward(fixture);
}

TEST_CASE("Checked Guest hubs repair cross original block uses on reentry",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  Config config; config.reentry = true; Fixture fixture(config); expectForward(fixture);
}

TEST_CASE("Checked Guest hubs translate exact subtraction through multiple PHIs",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  Config config; config.hubs = 2; config.subtract = true; Fixture fixture(config); expectForward(fixture);
}

TEST_CASE("Checked Guest hubs preserve writes calls and synchronization barriers",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  for (auto effect : {Effect::Call, Effect::AliasStore, Effect::PartialStore,
                     Effect::AtomicStore, Effect::VolatileLoad, Effect::Fence})
    for (bool inHub : {false, true}) DYNAMIC_SECTION("effect " << static_cast<int>(effect) << " hub " << inHub) {
      Config config; if (inHub) config.hubEffect = effect; else config.sourceEffect = effect;
      Fixture fixture(config); expectUnchanged(fixture);
    }
}

TEST_CASE("Checked Guest hubs reject unknown layout bases widths and poison flags",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  for (unsigned kind = 0; kind < 4; ++kind) DYNAMIC_SECTION("kind " << kind) {
    Config config; config.narrow = kind == 0; config.differentBase = kind == 1;
    config.partialSource = kind == 2; config.poisonFlag = kind == 3;
    Fixture fixture(config); expectUnchanged(fixture);
  }
}

TEST_CASE("Checked Guest hubs retain historical region PHIs as unknown",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  Config config; config.oldHistory = true; Fixture fixture(config); expectUnchanged(fixture);
}

TEST_CASE("Checked Guest hubs retain paths beyond block and instruction budgets",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  for (bool instructionLimit : {false, true}) DYNAMIC_SECTION("instruction limit " << instructionLimit) {
    Config config; config.hubs = instructionLimit ? 1 : 5; config.padding = instructionLimit ? 300 : 0;
    Fixture fixture(config); expectUnchanged(fixture, {65536, false}, true);
  }
}

TEST_CASE("Checked Guest hubs honor cross block PHI and rewrite gates",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  for (unsigned kind = 0; kind < 3; ++kind) {
    MemoryForwardOptions options{65536, false};
    if (kind == 0) options.enableCrossBlock = false;
    if (kind == 1) options.enablePhi = false;
    if (kind == 2) options.maxRewrites = 0;
    Fixture fixture; expectUnchanged(fixture, options);
  }
}

TEST_CASE("Checked Guest hubs do not turn original backedges into copy loops",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  Config config; config.directCycle = true; Fixture fixture(config); expectUnchanged(fixture);
}

TEST_CASE("Checked Guest hubs share the existing rewrite budget across functions",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  llvm::LLVMContext context; llvm::Module module("hub-rewrite-budget", context);
  const auto first = build(module, {}), second = build(module, {}); verify(module);
  const auto stats = detail::runGuestMemorySSA(module, {1, false});
  CHECK(stats.changed); CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0);
  CHECK(stats.loadsForwarded == 1);
  CHECK(first.known->getTerminator()->getSuccessor(0) != first.hubs.front());
  CHECK(second.known->getTerminator()->getSuccessor(0) == second.hubs.front()); verify(module);
}

TEST_CASE("Checked Guest hubs share the existing context cap across functions",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  llvm::LLVMContext context; llvm::Module module("hub-context-budget", context);
  std::vector<View> views;
  for (unsigned n = 0; n != 65; ++n) views.push_back(build(module, {}));
  verify(module); const auto stats = detail::runGuestMemorySSA(module, {65536, false});
  CHECK(stats.changed); CHECK_FALSE(stats.complete); CHECK(stats.budgetExhausted > 0);
  CHECK(stats.loadsForwarded == 64);
  for (unsigned n = 0; n < views.size(); ++n) {
    CAPTURE(n);
    CHECK((views[n].known->getTerminator()->getSuccessor(0) != views[n].hubs.front()) == (n < 64));
    CHECK(marked(*views[n].function, "hub.actual.load") == 1);
    for (unsigned k = 0; k < views[n].unknown.size(); ++k)
      CHECK(views[n].unknown[k]->getTerminator()->getSuccessor(0) ==
            (k < views[n].hubs.size() ? views[n].hubs[k] : views[n].join));
  }
  verify(module);
}

TEST_CASE("Checked Guest hubs keep new exceptional liveout paths outside the supported domain",
          "[llvm][guest-memory-ssa][guest-memory-hubs]") {
  SECTION("Additional hub leaves the original exceptional path unchanged") {
    Config config; config.externalEH = true; Fixture fixture(config); expectUnchanged(fixture);
  }
  SECTION("Historical direct edge contract still permits its existing SSA repair") {
    Config config; config.externalEH = true; Fixture fixture(config); auto &v = fixture.view;
    // Retain the same exceptional user but make the source a direct original
    // predecessor. This is the old path, not a new hub-domain authorization.
    auto *index = llvm::cast<llvm::GetElementPtrInst>(v.store->getPointerOperand())->getOperand(1);
    auto *knownFrame = llvm::cast<llvm::BinaryOperator>(index)->getOperand(0);
    v.sourceBranch->setSuccessor(0, v.join);
    for (auto &phi : v.hubs.front()->phis()) phi.removeIncomingValue(v.known, false);
    v.joinedFrame->addIncoming(knownFrame, v.known);
    v.joinedLive->addIncoming(v.function->getArg(7), v.known);
    for (auto &phi : v.join->phis()) if (phi.getName() == "iteration")
      phi.addIncoming(llvm::ConstantInt::get(phi.getType(), 0), v.known);
    verify(fixture.module);
    const auto stats = detail::runGuestMemorySSA(fixture.module, {65536, false});
    CHECK(stats.changed); CHECK(stats.loadsForwarded == 1);
    CHECK(v.sourceBranch->getSuccessor(0) != v.join);
    CHECK(v.load->getParent() == v.success); verify(fixture.module);
  }
}
