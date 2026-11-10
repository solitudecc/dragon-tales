#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Tail {
  Adjacent, Pure, Load, ReadonlyCall, ReadnoneCall, ReassignAfterCall,
  UnknownCall, AliasStore, PartialStore, VolatileLoad, VolatileStore,
  AtomicLoad, AtomicStore, AtomicRMW, CompareExchange, Fence,
  ConvergentCall, SideEffectAsm,
};

struct Case {
  Tail tail;
  const char *name;
  bool recover;
};
constexpr Case positives[] = {
    {Tail::Adjacent, "adjacent", true},
    {Tail::Pure, "pure", true},
    {Tail::Load, "load", true},
    {Tail::ReadonlyCall, "readonly-call", true},
    {Tail::ReadnoneCall, "readnone-call", true},
    {Tail::ReassignAfterCall, "reassign-after-call", true},
};
constexpr Case barriers[] = {
    {Tail::UnknownCall, "unknown-call", false},
    {Tail::AliasStore, "alias-store", false},
    {Tail::PartialStore, "partial-store", false},
    {Tail::VolatileLoad, "volatile-load", false},
    {Tail::VolatileStore, "volatile-store", false},
    {Tail::AtomicLoad, "atomic-load", false},
    {Tail::AtomicStore, "atomic-store", false},
    {Tail::AtomicRMW, "atomic-rmw", false},
    {Tail::CompareExchange, "compare-exchange", false},
    {Tail::Fence, "fence", false},
    {Tail::ConvergentCall, "convergent-call", false},
    {Tail::SideEffectAsm, "sideeffect-asm", false},
};

struct Fixture {
  llvm::Function *machine;
  llvm::BasicBlock *dispatch;
  llvm::BasicBlock *first;
  llvm::BasicBlock *second;
  llvm::BasicBlock *done;
  std::vector<llvm::Instruction *> prefix;
};

// A single function has three genuine CFF states and a conditional early exit.
// The state is Guest+0x100, never RIP or an entry/continuation selector. Tail
// calculations/loads feed the first block's branch, making their preservation
// observable without adding another store after the state definition.
Fixture fixture(LLVMLifter &lifter, Tail tail) {
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(i32, {ptr, ptr, i32, ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "machine", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "cff.dispatch", function);
  auto *first = llvm::BasicBlock::Create(context, "cff.first", function);
  auto *second = llvm::BasicBlock::Create(context, "cff.second", function);
  auto *done = llvm::BasicBlock::Create(context, "cff.done", function);
  auto *fallback = llvm::BasicBlock::Create(context, "cff.default", function);
  auto *bypass = llvm::BasicBlock::Create(context, "cff.bypass", function);
  Builder builder(entry);
  auto c = [&](unsigned value) { return llvm::ConstantInt::get(i32, value); };
  auto *data = builder.CreateLoad(ptr, function->getArg(1), "guest.data");
  auto address = [&](unsigned offset, const char *name) {
    return builder.CreateGEP(builder.getInt8Ty(), data,
                             builder.getInt64(offset), name);
  };
  auto *stateSlot = address(0x100, "guest.state");
  auto *readSlot = address(0x300, "guest.read");
  auto *atomicSlot = address(0x304, "guest.atomic");
  auto *output = address(0x200, "guest.output");
  auto store = [&](llvm::Value *value, llvm::Value *pointer) {
    auto *instruction = builder.CreateStore(value, pointer);
    instruction->setAlignment(llvm::Align(1));
    return instruction;
  };
  auto opaque = [&] {
    auto functionType = llvm::FunctionType::get(
        builder.getVoidTy(), {ptr, ptr, ptr, i32}, false);
    auto callee = lifter.module().getOrInsertFunction("opaque_writer", functionType);
    builder.CreateCall(callee, {function->getArg(0), function->getArg(1), stateSlot,
                                function->getArg(2)});
  };
  auto condition = [&](llvm::Value *value) {
    auto *mixed = builder.CreateXor(value, c(0xa5a5a5a5), "tail.mix");
    auto *restored = builder.CreateXor(mixed, c(0xa5a5a5a5), "tail.restore");
    return builder.CreateICmpEQ(builder.CreateAnd(restored, c(4)), c(0), "tail.test");
  };
  store(c(10), stateSlot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreateLoad(i32, stateSlot, "cff.state");
  state->setAlignment(llvm::Align(1));
  auto *selector = builder.CreateSwitch(state, fallback, 3);
  selector->addCase(c(10), first);
  selector->addCase(c(20), second);
  selector->addCase(c(30), done);
  builder.SetInsertPoint(first);
  llvm::Value *branchCondition = nullptr;
  if (tail == Tail::Adjacent) branchCondition = condition(function->getArg(2));
  if (tail == Tail::ReassignAfterCall) opaque();
  store(c(20), stateSlot);
  llvm::Value *branchValue = function->getArg(2);
  switch (tail) {
  case Tail::Load:
  case Tail::VolatileLoad: {
    auto *load = builder.CreateLoad(i32, readSlot, "tail.load");
    load->setAlignment(llvm::Align(1));
    load->setVolatile(tail == Tail::VolatileLoad);
    branchValue = load;
    break;
  }
  case Tail::ReadonlyCall:
  case Tail::ReadnoneCall:
  case Tail::ConvergentCall: {
    const bool readnone = tail == Tail::ReadnoneCall;
    auto callee = lifter.module().getOrInsertFunction(
        readnone ? "pure_helper" : "readonly_helper",
        llvm::FunctionType::get(i32, {ptr, i32}, false));
    auto *declaration = llvm::cast<llvm::Function>(callee.getCallee());
    if (readnone) declaration->setDoesNotAccessMemory();
    else declaration->setOnlyReadsMemory();
    if (tail == Tail::ConvergentCall)
      declaration->addFnAttr(llvm::Attribute::Convergent);
    branchValue = builder.CreateCall(callee, {readSlot, function->getArg(2)},
                                     "tail.call");
    break;
  }
  case Tail::UnknownCall:
    opaque();
    break;
  case Tail::AliasStore:
    store(builder.CreateSelect(
              builder.CreateICmpNE(builder.CreateAnd(function->getArg(2), c(2)), c(0)),
              c(999), c(30)), function->getArg(3));
    break;
  case Tail::PartialStore: {
    auto *byte = builder.CreateTrunc(
        builder.CreateAnd(function->getArg(2), c(1)), builder.getInt8Ty());
    auto *part = builder.CreateGEP(builder.getInt8Ty(), stateSlot,
                                   builder.getInt64(1));
    store(byte, part);
    break;
  }
  case Tail::VolatileStore:
    store(builder.CreateXor(function->getArg(2), c(0x2468ace0)), atomicSlot)
        ->setVolatile(true);
    break;
  case Tail::AtomicLoad: {
    auto *load = builder.CreateLoad(i32, atomicSlot, "tail.atomic");
    load->setAlignment(llvm::Align(4));
    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    branchValue = load;
    break;
  }
  case Tail::AtomicStore: {
    auto *write = store(builder.CreateXor(function->getArg(2), c(0x2468ace0)),
                         atomicSlot);
    write->setAlignment(llvm::Align(4));
    write->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  }
  case Tail::AtomicRMW:
    builder.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, atomicSlot,
                            builder.CreateXor(function->getArg(2), c(0x2468ace0)),
                            llvm::Align(4), llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  case Tail::CompareExchange:
    builder.CreateAtomicCmpXchg(
        atomicSlot, function->getArg(2),
        builder.CreateXor(function->getArg(2), c(0x2468ace0)), llvm::Align(4),
        llvm::AtomicOrdering::SequentiallyConsistent,
        llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  case Tail::Fence:
    builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    break;
  case Tail::SideEffectAsm: {
    auto *type = llvm::FunctionType::get(builder.getVoidTy(), false);
    auto *assembly = llvm::InlineAsm::get(type, "", "~{memory}", true);
    builder.CreateCall(assembly);
    break;
  }
  default:
    break;
  }
  if (!branchCondition) branchCondition = condition(branchValue);
  builder.CreateCondBr(branchCondition, dispatch, bypass);
  builder.SetInsertPoint(second);
  store(builder.CreateXor(function->getArg(2), c(0x13579bdf)), output);
  store(c(30), stateSlot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(done);
  builder.CreateRet(builder.CreateAdd(function->getArg(2), c(7)));
  builder.SetInsertPoint(fallback);
  builder.CreateRet(builder.CreateXor(function->getArg(2), c(0xbad)));
  builder.SetInsertPoint(bypass);
  builder.CreateRet(builder.CreateXor(function->getArg(2), c(0x51de)));
  std::vector<llvm::Instruction *> prefix;
  for (auto &instruction : *first)
    if (&instruction != first->getTerminator()) prefix.push_back(&instruction);
  return {function, dispatch, first, second, done, std::move(prefix)};
}

void verify(LLVMLifter &lifter) {
  std::string error;
  llvm::raw_string_ostream output(error);
  const bool invalid = llvm::verifyModule(lifter.module(), &output);
  INFO(error);
  REQUIRE_FALSE(invalid);
}

void snapshot(LLVMLifter &lifter, const std::string &name) {
  if (const auto *directory = std::getenv("DRAGON_REACHING_STORE_FIXTURE_DIR")) {
    std::filesystem::create_directories(directory);
    std::ofstream output{std::filesystem::path(directory) / name};
    output << lifter.ir();
    REQUIRE(output.good());
  }
}

void runCase(const Case &test) {
  CAPTURE(test.name);
  LLVMLifter lifter{Architecture::X86_64};
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  auto data = fixture(lifter, test.tail);
  verify(lifter);
  snapshot(lifter, std::string(test.name) + "-before.ll");
  // Do not run GVN or GuestMemorySSA first: this regression must exercise the
  // deflattening reaching-store proof rather than a prior load replacement.
  const auto stats = lifter.runProvableDeflattening();
  CHECK(stats.changed);
  CHECK(stats.edgesRewritten == (test.recover ? 3 : 2));
  CHECK(stats.unresolvedDispatchers == (test.recover ? 0 : 1));
  CHECK(stats.crossCallDispatchersRewritten == 0);
  CHECK(stats.crossCallSummariesConsumed == 0);
  CHECK(data.machine->getEntryBlock().getTerminator()->getSuccessor(0) == data.first);
  CHECK(data.second->getTerminator()->getSuccessor(0) == data.done);
  CHECK(data.first->getTerminator()->getSuccessor(0) ==
        (test.recover ? data.second : data.dispatch));
  CHECK(data.first->getTerminator()->getSuccessor(1)->getName() == "cff.bypass");
  // Preserve every prefix instruction in its original order. In particular,
  // proving the state must not delete/move the live tail load or CALL.
  std::vector<llvm::Instruction *> actual;
  for (auto &instruction : *data.first)
    if (&instruction != data.first->getTerminator()) actual.push_back(&instruction);
  CHECK(actual == data.prefix);
  verify(lifter);
  lifter.runControlFlowSimplification();
  verify(lifter);
  snapshot(lifter, std::string(test.name) + "-after.ll");
}
} // namespace

TEST_CASE("Same-function CFF reaches state stores across safe live tails",
          "[llvm][reaching-store]") {
  for (const auto &test : positives) runCase(test);
}

TEST_CASE("Same-function CFF keeps reaching-store hazard edges unresolved",
          "[llvm][reaching-store]") {
  for (const auto &test : barriers) runCase(test);
}

TEST_CASE("Reaching-store native oracle exports explicit faulty controls",
          "[llvm][reaching-store]") {
  for (auto tail : {Tail::AliasStore, Tail::Pure}) {
    LLVMLifter lifter{Architecture::X86_64};
    auto data = fixture(lifter, tail);
    auto *branch = llvm::cast<llvm::BranchInst>(data.first->getTerminator());
    if (tail == Tail::AliasStore) {
      // Deliberately discard a possible state clobber. Never use this module
      // as successful coverage: the native oracle must expose its wrong path.
      branch->setSuccessor(0, data.second);
    } else {
      // Deliberately discard the live condition/early-exit behavior.
      branch->setCondition(llvm::ConstantInt::getTrue(lifter.module().getContext()));
    }
    verify(lifter);
    lifter.runControlFlowSimplification();
    verify(lifter);
    snapshot(lifter, tail == Tail::AliasStore ? "alias-store-unsafe.ll" : "pure-unsafe.ll");
  }
}
