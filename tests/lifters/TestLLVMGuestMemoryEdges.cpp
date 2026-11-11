#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;

enum class Barrier {
  None, UnknownCall, AliasWrite, PartialWrite, Fence, AtomicWrite,
  AtomicLoad, AtomicStore, VolatileLoad, VolatileStore, InlineAsm,
};

enum class IndexSource {
  Frame, Undef, Poison, DerivedUndef, SharedFreeze, DistinctFreezes,
};

enum class SubFlags { None, LoadNUW, LoadNSW, StoreNUW, StoreNSW };

struct Config {
  Barrier tail = Barrier::None;
  Barrier corridor = Barrier::None;
  bool outerGuard = false;
  bool duplicateEdge = false;
  bool loop = false;
  bool frozenStore = false;
  bool narrowIndex = false;
  bool partialStore = false;
  bool differentBase = false;
  bool outsidePhi = false;
  bool duplicateExit = false;
  bool subAddress = false;
  bool priorIterationIndex = false;
  bool unknownUndef = false;
  bool unknownPoison = false;
  IndexSource indexSource = IndexSource::Frame;
  SubFlags subFlags = SubFlags::None;
  unsigned guards = 1;
  unsigned padding = 0;
};

struct View {
  llvm::Function *function = nullptr;
  llvm::BasicBlock *known = nullptr;
  std::array<llvm::BasicBlock *, 3> unknown{};
  llvm::BasicBlock *join = nullptr;
  llvm::BasicBlock *failure = nullptr;
  llvm::BasicBlock *loop = nullptr;
  llvm::PHINode *frame = nullptr;
  llvm::LoadInst *load = nullptr;
  llvm::StoreInst *store = nullptr;
  llvm::Value *data = nullptr;
  llvm::Value *storedValue = nullptr;
};

void barrier(Builder &b, Barrier kind, llvm::Value *data, llvm::Value *slot,
             llvm::Value *alias, llvm::Value *salt) {
  switch (kind) {
  case Barrier::None: break;
  case Barrier::UnknownCall: {
    auto *module = b.GetInsertBlock()->getModule();
    const auto callee = module->getOrInsertFunction("unknown_checked_writer",
        llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false));
    b.CreateCall(callee, {data});
    break;
  }
  case Barrier::AliasWrite:
    b.CreateStore(salt, alias)->setAlignment(llvm::Align(1));
    break;
  case Barrier::PartialWrite:
    b.CreateStore(b.getInt8(3), slot)->setAlignment(llvm::Align(1));
    break;
  case Barrier::Fence:
    b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  case Barrier::AtomicWrite:
    b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, slot, b.getInt64(1),
                     llvm::MaybeAlign(8),
                     llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  case Barrier::AtomicLoad: {
    auto *load = b.CreateLoad(b.getInt64Ty(), slot);
    load->setAlignment(llvm::Align(8));
    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  }
  case Barrier::AtomicStore: {
    auto *store = b.CreateStore(salt, slot);
    store->setAlignment(llvm::Align(8));
    store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  }
  case Barrier::VolatileLoad:
    b.CreateLoad(b.getInt64Ty(), slot)->setVolatile(true);
    break;
  case Barrier::VolatileStore:
    b.CreateStore(salt, slot)->setVolatile(true);
    break;
  case Barrier::InlineAsm: {
    auto *type = llvm::FunctionType::get(b.getVoidTy(), false);
    b.CreateCall(llvm::InlineAsm::get(type, "", "~{memory}", true));
    break;
  }
  }
}

View build(llvm::Module &module, const Config &config = {}) {
  auto &context = module.getContext();
  module.setDataLayout("e-p:64:64");
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *type = llvm::FunctionType::get(i64,
      {ptr, ptr, ptr, i64, i32, i64, i64, i64, i64, i1, i1}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                         "checked_memory", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *known = llvm::BasicBlock::Create(context, "known", function);
  std::array<llvm::BasicBlock *, 3> unknown{};
  for (auto &block : unknown)
    block = llvm::BasicBlock::Create(context, "unknown", function);
  auto *join = llvm::BasicBlock::Create(context, "join", function);
  auto *success = llvm::BasicBlock::Create(context, "success", function);
  auto *failure = llvm::BasicBlock::Create(context, "failure", function);
  auto *done = llvm::BasicBlock::Create(context, "done", function);
  auto *bypass = config.outerGuard
      ? llvm::BasicBlock::Create(context, "bypass", function) : nullptr;
  auto *loop = config.loop
      ? llvm::BasicBlock::Create(context, "loop", function) : nullptr;

  Builder b(entry);
  auto *data = b.CreateLoad(ptr, function->getArg(1), "guest.base");
  data->setAlignment(llvm::Align(1));
  auto *otherData = config.differentBase
      ? b.CreateLoad(ptr, function->getArg(1), "other.guest.base") : data;
  auto *indexType = config.narrowIndex ? b.getInt32Ty() : b.getInt64Ty();
  auto *stack = config.narrowIndex
      ? b.CreateTrunc(function->getArg(3), indexType) : function->getArg(3);
  llvm::Value *storeIndex = nullptr;
  llvm::Value *knownFrame = nullptr;
  if (config.subAddress) {
    knownFrame = b.CreateSub(stack, llvm::ConstantInt::get(indexType, 8), "known.frame");
    storeIndex = b.CreateSub(knownFrame,
        llvm::ConstantInt::get(indexType, 120), "store.index");
  } else {
    storeIndex = b.CreateAdd(stack,
        llvm::ConstantInt::getSigned(indexType, -128), "store.index");
    knownFrame = b.CreateAdd(stack,
        llvm::ConstantInt::getSigned(indexType, -8), "known.frame");
  }
  if (config.subFlags == SubFlags::StoreNUW)
    llvm::cast<llvm::BinaryOperator>(storeIndex)->setHasNoUnsignedWrap(true);
  if (config.subFlags == SubFlags::StoreNSW)
    llvm::cast<llvm::BinaryOperator>(storeIndex)->setHasNoSignedWrap(true);
  llvm::Value *sharedLoadIndex = nullptr;
  switch (config.indexSource) {
  case IndexSource::Frame: break;
  case IndexSource::Undef:
    storeIndex = sharedLoadIndex = llvm::UndefValue::get(indexType);
    break;
  case IndexSource::Poison:
    storeIndex = sharedLoadIndex = llvm::PoisonValue::get(indexType);
    break;
  case IndexSource::DerivedUndef:
    storeIndex = sharedLoadIndex = b.CreateAdd(llvm::UndefValue::get(indexType),
        stack, "unfrozen.index");
    break;
  case IndexSource::SharedFreeze:
    storeIndex = sharedLoadIndex = b.CreateFreeze(
        llvm::PoisonValue::get(indexType), "stable.index");
    break;
  case IndexSource::DistinctFreezes:
    storeIndex = b.CreateFreeze(llvm::PoisonValue::get(indexType), "store.frozen.index");
    sharedLoadIndex = b.CreateFreeze(llvm::PoisonValue::get(indexType), "load.frozen.index");
    break;
  }
  auto *slot = b.CreateGEP(b.getInt8Ty(), data, storeIndex, "store.slot");
  auto *route = b.CreateSwitch(function->getArg(4), unknown[2], 3);
  route->addCase(b.getInt32(0), config.priorIterationIndex ? unknown[2] : known);
  route->addCase(b.getInt32(1), unknown[0]);
  route->addCase(b.getInt32(2), unknown[1]);

  b.SetInsertPoint(known);
  auto *value = config.frozenStore
      ? b.CreateFreeze(llvm::PoisonValue::get(i64), "stable.store")
      : static_cast<llvm::Value *>(b.getInt64(50));
  auto *store = b.CreateStore(config.partialStore ? b.getInt8(50) : value, slot);
  store->setAlignment(llvm::Align(1));
  barrier(b, config.tail, data, slot, function->getArg(0), function->getArg(8));
  if (config.duplicateEdge)
    b.CreateCondBr(function->getArg(9), join, join);
  else if (config.outerGuard)
    b.CreateCondBr(function->getArg(9), join, bypass);
  else
    b.CreateBr(join);
  for (auto *block : unknown) {
    b.SetInsertPoint(block);
    b.CreateBr(join);
  }

  b.SetInsertPoint(join);
  auto *frame = b.CreatePHI(indexType, 4 + config.duplicateEdge + config.loop,
                             "frame");
  auto *live = b.CreatePHI(i64, 4 + config.duplicateEdge + config.loop, "live");
  frame->addIncoming(knownFrame, known);
  live->addIncoming(function->getArg(8), known);
  if (config.duplicateEdge) {
    frame->addIncoming(knownFrame, known);
    live->addIncoming(function->getArg(8), known);
  }
  for (unsigned index = 0; index != unknown.size(); ++index) {
    auto *unknownFrame = static_cast<llvm::Value *>(function->getArg(5 + index));
    if (config.narrowIndex) {
      Builder edge(unknown[index]->getTerminator());
      unknownFrame = edge.CreateTrunc(unknownFrame, indexType);
    }
    if (index == 0 && config.unknownUndef)
      unknownFrame = llvm::UndefValue::get(indexType);
    if (index == 0 && config.unknownPoison)
      unknownFrame = llvm::PoisonValue::get(indexType);
    frame->addIncoming(unknownFrame, unknown[index]);
    live->addIncoming(b.getInt64(0x1111 + index), unknown[index]);
  }
  if (config.loop && !config.priorIterationIndex) {
    frame->addIncoming(frame, loop);
    live->addIncoming(live, loop);
  }
  llvm::Value *businessLive = live;
  for (unsigned operation = 0; operation != config.padding; ++operation)
    businessLive = b.CreateXor(businessLive, b.getInt64(operation + 1), "live.padding");
  auto *index = config.subAddress
      ? b.CreateSub(frame, llvm::ConstantInt::get(indexType, 120), "load.index")
      : b.CreateAdd(frame, llvm::ConstantInt::getSigned(indexType, -120), "load.index");
  if (config.subFlags == SubFlags::LoadNUW)
    llvm::cast<llvm::BinaryOperator>(index)->setHasNoUnsignedWrap(true);
  if (config.subFlags == SubFlags::LoadNSW)
    llvm::cast<llvm::BinaryOperator>(index)->setHasNoSignedWrap(true);
  if (sharedLoadIndex) index = sharedLoadIndex;
  if (config.priorIterationIndex)
    index = b.CreateFreeze(llvm::PoisonValue::get(indexType), "iteration.index");
  auto *sizeAddress = b.CreateGEP(b.getInt8Ty(), function->getArg(1), b.getInt64(8));
  auto *size = b.CreateLoad(i64, sizeAddress, "memory.size");
  size->setAlignment(llvm::Align(1));
  auto *wideIndex = config.narrowIndex ? b.CreateSExt(index, i64) : index;
  auto *fits = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
      b.CreateICmpULE(wideIndex, b.CreateAdd(size, b.getInt64(-8))), "bounds");
  barrier(b, config.corridor, data, slot,
          function->getArg(0), function->getArg(8));
  for (unsigned guard = 1; guard < config.guards; ++guard) {
    auto *next = llvm::BasicBlock::Create(context, "more.checks", function);
    auto *branch = b.CreateCondBr(fits, next, failure);
    branch->setMetadata("checked.edge.guard", llvm::MDNode::get(context, {}));
    b.SetInsertPoint(next);
  }
  auto *guard = b.CreateCondBr(fits, success, failure);
  guard->setMetadata("checked.edge.guard", llvm::MDNode::get(context, {}));

  b.SetInsertPoint(success);
  auto *loadAddress = b.CreateGEP(b.getInt8Ty(), otherData, index, "load.slot");
  if (config.priorIterationIndex) store->setOperand(1, loadAddress);
  auto *load = b.CreateLoad(i64, loadAddress, "checked.load");
  load->setAlignment(llvm::Align(1));
  if (config.duplicateExit)
    b.CreateCondBr(function->getArg(10), done, done);
  else if (config.loop)
    b.CreateCondBr(function->getArg(10), loop, done);
  else
    b.CreateBr(done);

  b.SetInsertPoint(done);
  llvm::Value *bonus = b.getInt64(0);
  if (config.outsidePhi) {
    auto *phi = b.CreatePHI(i64, config.duplicateExit ? 2 : 1, "outside.constant.phi");
    phi->addIncoming(b.getInt64(0x23), success);
    if (config.duplicateExit) phi->addIncoming(b.getInt64(0x23), success);
    bonus = phi;
  }
  auto *result = b.CreateAdd(b.CreateAdd(load, businessLive), bonus, "business.result");
  // Values defined in the region escape to a block shared with its clone.
  // Merely cloning the address and replacing the load cannot repair these.
  auto *registerSlot = b.CreateGEP(b.getInt8Ty(), function->getArg(0), b.getInt64(24));
  b.CreateStore(b.CreateXor(b.CreateZExtOrTrunc(frame, i64), result), registerSlot)
      ->setAlignment(llvm::Align(1));
  b.CreateRet(result);

  b.SetInsertPoint(failure);
  llvm::Value *failureTag = b.getInt64(5);
  if (config.outsidePhi) {
    auto *phi = b.CreatePHI(i64, config.guards, "failure.constant.phi");
    for (auto *pred : llvm::predecessors(failure))
      phi->addIncoming(b.getInt64(5), pred);
    failureTag = phi;
  }
  const std::array<llvm::Value *, 7> fields{
      failureTag, b.getInt64(0), b.CreateZExtOrTrunc(frame, i64),
      businessLive, b.getInt64(1), b.getInt64(0), b.getInt64(3)};
  for (unsigned field = 0; field != fields.size(); ++field) {
    auto *address = b.CreateGEP(b.getInt8Ty(), function->getArg(2), b.getInt64(8 * field));
    auto *write = b.CreateStore(fields[field], address);
    write->setAlignment(llvm::Align(1));
    write->setMetadata("checked.edge.failure", llvm::MDNode::get(context, {}));
  }
  b.CreateRet(b.CreateXor(businessLive, b.getInt64(0xff00)));
  if (bypass) {
    b.SetInsertPoint(bypass);
    b.CreateRet(b.CreateXor(function->getArg(8), b.getInt64(0xa5)));
  }
  if (loop) {
    b.SetInsertPoint(loop);
    b.CreateBr(config.priorIterationIndex ? known : join);
  }
  return {function, known, unknown, join, failure, loop, frame, load, store,
          data, value};
}

struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"checked-edges", context};
  View view;
  explicit Fixture(const Config &config = {}) : view(build(module, config)) {}
};

void verify(const llvm::Module &module) {
  REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
}

std::string text(const llvm::Module &module) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  module.print(stream, nullptr);
  return output;
}

bool contains(const llvm::Function &function, const llvm::Instruction *needle) {
  for (const auto &block : function)
    for (const auto &instruction : block)
      if (&instruction == needle) return true;
  return false;
}

std::size_t marked(const llvm::Function &function, const char *name) {
  std::size_t count = 0;
  for (const auto &block : function)
    for (const auto &instruction : block)
      count += instruction.getMetadata(name) != nullptr;
  return count;
}

std::size_t guestLoads(const View &view) {
  std::size_t count = 0;
  for (const auto &block : *view.function)
    for (const auto &instruction : block) {
      const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (!load || !load->getType()->isIntegerTy()) continue;
      const auto *address = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
      if (address && address->getPointerOperand() == view.data) ++count;
    }
  return count;
}

void expectForward(Fixture &fixture, bool outerGuard = false, bool loop = false) {
  verify(fixture.module);
  const auto before = fixture.view.function->size();
  const auto stats = detail::runGuestMemorySSA(fixture.module, {65536, false});
  CHECK(stats.available);
  CHECK(stats.complete);
  CHECK(stats.changed);
  CHECK(stats.loadsForwarded == 1);
  CHECK(fixture.view.function->size() > before);
  auto *term = llvm::dyn_cast<llvm::BranchInst>(fixture.view.known->getTerminator());
  REQUIRE(term);
  CHECK(term->isConditional() == outerGuard);
  CHECK(term->getSuccessor(0) != fixture.view.join);
  if (outerGuard) CHECK(term->getSuccessor(1)->getName() == "bypass");
  for (auto *block : fixture.view.unknown)
    CHECK(block->getTerminator()->getSuccessor(0) == fixture.view.join);
  CHECK(contains(*fixture.view.function, fixture.view.load));
  CHECK(contains(*fixture.view.function, fixture.view.store));
  CHECK(guestLoads(fixture.view) == 1);
  CHECK(marked(*fixture.view.function, "checked.edge.guard") >= 2);
  CHECK(marked(*fixture.view.function, "checked.edge.failure") >= 7);
  CHECK(fixture.view.frame->getNumIncomingValues() == (loop ? 4 : 3));
  if (loop) CHECK(fixture.view.loop->getTerminator()->getSuccessor(0) == fixture.view.join);
  verify(fixture.module);
}

void expectUnchanged(Fixture &fixture, MemoryForwardOptions options = {65536, false},
                     bool exhausted = false) {
  verify(fixture.module);
  const auto before = text(fixture.module);
  const auto stats = detail::runGuestMemorySSA(fixture.module, options);
  CHECK(stats.available);
  CHECK_FALSE(stats.changed);
  CHECK(stats.loadsForwarded == 0);
  CHECK(stats.complete == !exhausted);
  if (exhausted) CHECK(stats.budgetExhausted >= 1);
  CHECK(text(fixture.module) == before);
  verify(fixture.module);
}
} // namespace

TEST_CASE("Checked Guest edges specialize only the proven predecessor",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  Fixture fixture;
  expectForward(fixture);
}

TEST_CASE("Checked Guest edges preserve an outer guard and shared liveouts",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  Config config;
  config.outerGuard = true;
  Fixture fixture(config);
  expectForward(fixture, true);
}

TEST_CASE("Checked Guest edges retain unknown loop entries without a global invariant",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  Config config;
  config.loop = true;
  Fixture fixture(config);
  expectForward(fixture, false, true);
}

TEST_CASE("Checked Guest edges repair preexisting external constant PHIs",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  Config config;
  config.outsidePhi = true;
  config.guards = 2;
  Fixture fixture(config);
  expectForward(fixture);
  for (auto *block : {fixture.view.failure})
    for (const auto &phi : block->phis())
      CHECK(phi.getNumIncomingValues() == llvm::pred_size(block));
}

TEST_CASE("Checked Guest edges reuse the actual stable frozen store value",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  Config config;
  config.frozenStore = true;
  Fixture fixture(config);
  expectForward(fixture);
  CHECK(contains(*fixture.view.function,
                 llvm::cast<llvm::Instruction>(fixture.view.storedValue)));
}

TEST_CASE("Checked Guest edges refuse intervening writes calls and synchronization",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  for (const auto kind : {Barrier::UnknownCall, Barrier::AliasWrite,
       Barrier::PartialWrite, Barrier::Fence, Barrier::AtomicWrite,
       Barrier::AtomicLoad, Barrier::AtomicStore, Barrier::VolatileLoad,
       Barrier::VolatileStore, Barrier::InlineAsm}) {
    for (const bool atJoin : {false, true}) {
      CAPTURE(static_cast<int>(kind), atJoin);
      Config config;
      (atJoin ? config.corridor : config.tail) = kind;
      Fixture fixture(config);
      expectUnchanged(fixture);
    }
  }
}

TEST_CASE("Checked Guest edges require unique edge base width and supported index",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  for (unsigned mode = 0; mode != 5; ++mode) {
    CAPTURE(mode);
    Config config;
    config.duplicateEdge = mode == 0;
    config.differentBase = mode == 1;
    config.partialStore = mode == 2;
    config.narrowIndex = mode == 3;
    config.duplicateExit = mode == 4;
    config.outsidePhi = mode == 4;
    Fixture fixture(config);
    expectUnchanged(fixture);
  }
}

TEST_CASE("Checked Guest edges obey cross block and PHI option gates",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  for (unsigned mode = 0; mode != 3; ++mode) {
    CAPTURE(mode);
    MemoryForwardOptions options{65536, false};
    if (mode == 0) options.enableCrossBlock = false;
    if (mode == 1) options.enablePhi = false;
    if (mode == 2) options.maxRewrites = 0;
    Fixture fixture;
    expectUnchanged(fixture, options);
  }
}

TEST_CASE("Checked Guest edges preserve the module when the corridor budget is exhausted",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  for (const bool tooManyBlocks : {false, true}) {
    CAPTURE(tooManyBlocks);
    Config config;
    config.guards = tooManyBlocks ? 7 : 1;
    config.padding = tooManyBlocks ? 0 : 300;
    Fixture fixture(config);
    expectUnchanged(fixture, {65536, false}, true);
  }
}

TEST_CASE("Checked Guest edges share the rewrite budget across functions",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  llvm::LLVMContext context;
  llvm::Module module("checked-edge-budget", context);
  const auto first = build(module);
  const auto second = build(module);
  verify(module);
  const auto stats = detail::runGuestMemorySSA(module, {1, false});
  CHECK(stats.available);
  CHECK(stats.changed);
  CHECK_FALSE(stats.complete);
  CHECK(stats.budgetExhausted >= 1);
  CHECK(stats.loadsForwarded == 1);
  CHECK(first.known->getTerminator()->getSuccessor(0) != first.join);
  CHECK(second.known->getTerminator()->getSuccessor(0) == second.join);
  CHECK(contains(*first.function, first.load));
  CHECK(contains(*second.function, second.load));
  verify(module);
}

TEST_CASE("Checked Guest edges share the private clone cap across functions",
          "[llvm][guest-memory-ssa][guest-memory-edges]") {
  llvm::LLVMContext context;
  llvm::Module module("checked-edge-clone-cap", context);
  std::vector<View> views;
  for (unsigned index = 0; index != 65; ++index) views.push_back(build(module));
  verify(module);
  const auto stats = detail::runGuestMemorySSA(module, {65536, false});
  CHECK(stats.available);
  CHECK(stats.changed);
  CHECK_FALSE(stats.complete);
  CHECK(stats.budgetExhausted >= 1);
  CHECK(stats.loadsForwarded == 64);
  for (unsigned index = 0; index != views.size(); ++index) {
    CAPTURE(index);
    CHECK((views[index].known->getTerminator()->getSuccessor(0) != views[index].join)
          == (index < 64));
    CHECK(contains(*views[index].function, views[index].load));
    for (auto *unknown : views[index].unknown)
      CHECK(unknown->getTerminator()->getSuccessor(0) == views[index].join);
  }
  verify(module);
}

TEST_CASE("Checked Guest edges translate subtraction based addresses",
          "[llvm][guest-memory-ssa][guest-memory-edges][guest-memory-sub-edges]") {
  for (unsigned mode = 0; mode != 7; ++mode) {
    CAPTURE(mode);
    Config config;
    config.subAddress = true;
    config.guards = mode == 1 ? 2 : 1;
    config.outsidePhi = mode == 1;
    config.outerGuard = mode == 2;
    config.frozenStore = mode == 3;
    config.loop = mode == 4;
    config.unknownUndef = mode == 5;
    config.unknownPoison = mode == 6;
    Fixture fixture(config);
    expectForward(fixture, config.outerGuard, config.loop);
  }
}

TEST_CASE("Checked Guest subtraction proofs retain integer poison flags",
          "[llvm][guest-memory-ssa][guest-memory-edges][guest-memory-sub-edges]") {
  for (const auto flags : {SubFlags::LoadNUW, SubFlags::LoadNSW,
                           SubFlags::StoreNUW, SubFlags::StoreNSW}) {
    CAPTURE(static_cast<int>(flags));
    Config config;
    config.subAddress = true;
    config.subFlags = flags;
    Fixture fixture(config);
    // These are not interchangeable raw modular Sub operations. The proof
    // language must not discard their flags to invent an existing SSA match.
    expectUnchanged(fixture);
  }
}

TEST_CASE("Checked Guest address proofs reject unstable undef and poison indices",
          "[llvm][guest-memory-ssa][guest-memory-edges][guest-memory-sub-edges]") {
  for (const auto source : {IndexSource::Undef, IndexSource::Poison,
                            IndexSource::DerivedUndef}) {
    CAPTURE(static_cast<int>(source));
    Config config;
    config.indexSource = source;
    Fixture fixture(config);
    // An undef's identical Value pointer does not make its bits stable over
    // distinct uses. This is structural only, with no guessed native value.
    expectUnchanged(fixture);
  }
}

TEST_CASE("Checked Guest address proofs reuse only the same stable freeze",
          "[llvm][guest-memory-ssa][guest-memory-edges][guest-memory-sub-edges]") {
  Config config;
  config.indexSource = IndexSource::SharedFreeze;
  Fixture stable(config);
  expectForward(stable);
  config.indexSource = IndexSource::DistinctFreezes;
  Fixture distinct(config);
  expectUnchanged(distinct);
}

TEST_CASE("Checked Guest edges do not reuse a prior loop iteration address",
          "[llvm][guest-memory-ssa][guest-memory-edges][guest-memory-sub-edges]") {
  Config config;
  config.loop = true;
  config.priorIterationIndex = true;
  Fixture fixture(config);
  // Entry reaches the join through an unknown path. A later successful load
  // may branch through known, STORE to this iteration's frozen address, then
  // reenter the join. The freeze executes again there: a lexically identical
  // index instruction is not a proof that the next iteration picks its bits.
  // No fixed bit pattern or native expected result is assumed for the freeze.
  expectUnchanged(fixture);
}
