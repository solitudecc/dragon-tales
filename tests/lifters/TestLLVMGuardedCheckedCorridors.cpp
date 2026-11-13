#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace dragon::passes;
using Builder = llvm::IRBuilder<llvm::NoFolder>;
constexpr const char *Generated = "dragon.guarded-transitions.generated";

struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module = lifter.module();
  llvm::Function *function = nullptr;
  std::set<std::string> originalBlocks;
  std::string mode;

  explicit Fixture(std::string kind = "loop") : mode(std::move(kind)) {
    module.setDataLayout("e-p:64:64");
    auto &c = module.getContext();
    auto *ptr = llvm::PointerType::getUnqual(c);
    auto *i64 = llvm::Type::getInt64Ty(c);
    auto *i1 = llvm::Type::getInt1Ty(c);
    function = llvm::Function::Create(
        llvm::FunctionType::get(i64, {ptr, ptr, i64, i1, i1}, false),
        llvm::GlobalValue::ExternalLinkage, "view", module);
    auto make = [&](const char *name) {
      originalBlocks.insert(name);
      return llvm::BasicBlock::Create(c, name, function);
    };
    auto *entry = make("entry"), *seed = make("seed"), *other = make("other");
    auto *seedFallback = make("seed.fallback"), *head = make("head");
    auto *dispatch = make("dispatch"), *hit = make("hit");
    auto *miss = make("miss"), *bounds = make("bounds");
    Builder b(entry);
    auto load = [&](llvm::Value *address, const char *name) {
      auto *value = b.CreateLoad(i64, address, name);
      value->setAlignment(llvm::Align(1));
      return value;
    };
    auto store = [&](llvm::Value *value, llvm::Value *address) {
      auto *write = b.CreateStore(value, address);
      write->setAlignment(llvm::Align(1));
    };
    auto *guest = b.CreateLoad(ptr, function->getArg(1), "guest");
    guest->setAlignment(llvm::Align(1));
    auto external = module.getOrInsertFunction(
        "external_maywrite", llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr}, false));
    b.CreateCall(external, {function->getArg(0), guest});
    auto *size = load(b.CreateGEP(b.getInt8Ty(), function->getArg(1), b.getInt64(8)), "size");
    auto *iterationAddress = b.CreateGEP(b.getInt8Ty(), function->getArg(0), b.getInt64(8));
    auto *observationAddress = b.CreateGEP(b.getInt8Ty(), function->getArg(0), b.getInt64(16));
    auto *historyAddress = b.CreateGEP(b.getInt8Ty(), function->getArg(0), b.getInt64(24));
    // In the historical model the original head must execute before seed.
    // That makes its LOAD/freeze dominate the selected historical input.
    if (mode == "historical") b.CreateBr(other);
    else b.CreateCondBr(function->getArg(4), seed, other);

    b.SetInsertPoint(seed);
    auto *prefix = load(function->getArg(0), "prefix.count");
    store(b.CreateAdd(prefix, b.getInt64(1), "prefix.increment"), function->getArg(0));
    llvm::Value *chosen = b.CreateSelect(function->getArg(3), b.getInt64(0), b.getInt64(1), "choose.state");
    if (mode == "unknown") chosen = function->getArg(2);
    auto *old = load(observationAddress, "seed.history");
    // Reexecuting this chooser on a loop backedge would repeat its write and
    // observe a different seed. The new loop belongs at head, never at seed.
    store(b.getInt64(0), b.CreateGEP(b.getInt8Ty(), function->getArg(0), b.getInt64(32)));
    llvm::Value *seedTarget = nullptr;
    if (mode == "switch-seed") {
      seedTarget = load(b.CreateGEP(b.getInt8Ty(), function->getArg(0), b.getInt64(40)), "seed.actual.target");
      auto *sw = b.CreateSwitch(seedTarget, seedFallback);
      sw->addCase(b.getInt64(7), head);
      sw->addCase(b.getInt64(8), head);
    } else b.CreateBr(head);
    if (mode != "unmarked") seed->getTerminator()->setMetadata(Generated, llvm::MDNode::get(c, {}));
    b.SetInsertPoint(seedFallback);
    if (seedTarget) {
      auto *sw = b.CreateSwitch(seedTarget, other);
      sw->addCase(b.getInt64(7), head);
      sw->addCase(b.getInt64(8), head);
    } else b.CreateBr(other);
    seedFallback->getTerminator()->setMetadata(Generated, llvm::MDNode::get(c, {}));
    b.SetInsertPoint(other);
    b.CreateBr(head);
    other->getTerminator()->setMetadata(Generated, llvm::MDNode::get(c, {}));

    b.SetInsertPoint(head);
    auto *state = b.CreatePHI(i64, 7, "state");
    auto *history = b.CreatePHI(i64, 7, "history");
    state->addIncoming(chosen, seed);
    history->addIncoming(old, seed);
    if (seedTarget) {
      state->addIncoming(chosen, seed); history->addIncoming(old, seed);
      state->addIncoming(chosen, seedFallback); history->addIncoming(old, seedFallback);
      state->addIncoming(chosen, seedFallback); history->addIncoming(old, seedFallback);
    }
    state->addIncoming(function->getArg(2), other);
    history->addIncoming(function->getArg(2), other);
    auto *fresh = load(observationAddress, "fresh");
    auto *frozen = b.CreateFreeze(fresh, "fresh.freeze");
    if (mode == "historical")
      history->setIncomingValue(history->getBasicBlockIndex(seed), frozen);
    store(history, historyAddress);
    auto *count = load(iterationAddress, "iteration.count");
    store(b.CreateAdd(count, b.getInt64(1), "iteration.increment"), iterationAddress);
    auto *flagged = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(state, b.getInt64(0), "retained.flags"));
    flagged->setHasNoSignedWrap(); flagged->setHasNoUnsignedWrap();
    if (mode == "call-inside") b.CreateCall(external, {function->getArg(0), guest});
    if (mode == "fence") b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    auto *index = b.CreateAdd(b.getInt64(1024), b.CreateShl(flagged, b.getInt64(3)), "table.index");
    auto *range = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
        b.CreateICmpULE(index, b.CreateSub(size, b.getInt64(8))));
    b.CreateCondBr(range, dispatch, bounds);

    b.SetInsertPoint(dispatch);
    auto *address = b.CreateGEP(b.getInt8Ty(), guest, index, "table.address");
    if (mode == "table-before") store(function->getArg(2), address);
    auto *actual = load(address, "actual.target");
    if (mode == "volatile") actual->setVolatile(true);
    if (mode == "table-after") store(function->getArg(2), address);
    auto *next = b.CreateXor(state, b.getInt64(1), "next.state");
    auto *sw = b.CreateSwitch(actual, miss);
    sw->addCase(b.getInt64(100), head); sw->addCase(b.getInt64(101), head);
    sw->addCase(b.getInt64(200), hit); sw->addCase(b.getInt64(201), hit);
    sw->addCase(b.getInt64(300), miss);
    if (mode == "historical") sw->addCase(b.getInt64(400), seed);
    state->addIncoming(next, dispatch); state->addIncoming(next, dispatch);
    history->addIncoming(frozen, dispatch); history->addIncoming(frozen, dispatch);
    b.SetInsertPoint(hit);
    auto *result = b.CreatePHI(i64, 2, "selected");
    result->addIncoming(actual, dispatch); result->addIncoming(actual, dispatch);
    store(frozen, observationAddress);
    b.CreateRet(b.CreateXor(result, frozen));
    b.SetInsertPoint(miss); store(frozen, observationAddress); b.CreateRet(actual);
    b.SetInsertPoint(bounds); store(frozen, observationAddress); b.CreateRet(history);

    auto bytes = [](std::uint64_t value) {
      std::string out; constexpr char hex[] = "0123456789abcdef";
      for (unsigned n = 0; n < 8; ++n) { auto v = (value >> (n * 8)) & 255; out += hex[v >> 4]; out += hex[v & 15]; }
      return out;
    };
    unsigned first = mode == "straight" ? 200 : 100;
    unsigned second = mode == "dupe-loop" ? 101 : mode == "straight" ? 201 : 200;
    if (mode == "bad-target") first = second = 999;
    std::string hints = "{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0x400\",\"bytes\":\"" + bytes(first) +
        "\"},{\"offset\":\"0x408\",\"bytes\":\"" + bytes(second) + "\"}]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(
        llvm::MDNode::get(c, llvm::MDString::get(c, hints)));
    verify();
  }
  void verify() { REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs())); }
  std::string text() { std::string s; llvm::raw_string_ostream out(s); module.print(out, nullptr); return s; }
  llvm::BasicBlock *block(llvm::StringRef name) {
    for (auto &b : *function) if (b.getName() == name) return &b;
    return nullptr;
  }
  llvm::Instruction *instruction(llvm::StringRef name) {
    for (auto &b : *function) for (auto &i : b) if (i.getName() == name) return &i;
    return nullptr;
  }
  Result run(const char *name = "guarded-checked-corridors", Options options = {}) {
    options.try_emplace("max_rewrites", std::int64_t(1));
    PassPipeline p; REQUIRE(p.add(name, options));
    RunOptions opts; opts.maxIterations = 1; opts.maxPassRuns = 1; opts.verifyLLVM = true;
    auto report = p.run(lifter, opts); REQUIRE(report.ok); REQUIRE(report.runs.size() == 1);
    verify(); return report.runs.front().result;
  }
  llvm::SwitchInst *freshSwitch() {
    llvm::SwitchInst *found = nullptr;
    for (auto &b : *function) if (!originalBlocks.contains(b.getName().str()))
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator()))
        if (auto *read = llvm::dyn_cast<llvm::LoadInst>(sw->getCondition()); read && read->getParent() == &b) {
          REQUIRE(found == nullptr); found = sw;
        }
    REQUIRE(found); return found;
  }
  void checkFallback(llvm::SwitchInst &fast) {
    auto *fallback = llvm::dyn_cast<llvm::SwitchInst>(fast.getDefaultDest()->getTerminator());
    REQUIRE(fallback); CHECK(fallback->getCondition() == fast.getCondition());
    CHECK(fallback->getNumCases() == (mode == "historical" ? 6 : 5));
    CHECK(fallback->getDefaultDest() == block("miss"));
    std::map<std::uint64_t, llvm::BasicBlock *> expected{
      {100, block("head")}, {101, block("head")}, {200, block("hit")}, {201, block("hit")}, {300, block("miss")}};
    if (mode == "historical") expected.emplace(400, block("seed"));
    for (auto c : fallback->cases()) CHECK(c.getCaseSuccessor() == expected.at(c.getCaseValue()->getZExtValue()));
    // The bridge may use one fallback-local PHI but must not reread Guest memory.
    for (auto &i : *fallback->getParent()) if (llvm::isa<llvm::LoadInst>(i))
      CHECK(&i == fast.getCondition());
  }
  void checkEveryPhi() {
    for (auto &b : *function) {
      std::map<llvm::BasicBlock *, unsigned> actual;
      for (auto &pred : *function) for (auto *succ : llvm::successors(&pred)) if (succ == &b) ++actual[&pred];
      for (auto &phi : b.phis()) {
        std::map<llvm::BasicBlock *, unsigned> inputs;
        for (unsigned n = 0; n < phi.getNumIncomingValues(); ++n) ++inputs[phi.getIncomingBlock(n)];
        CHECK(inputs == actual);
      }
    }
  }
};
}

TEST_CASE("Checked corridors close root cases with fresh loop PHIs and full fallback", "[llvm][guarded-checked-corridors]") {
  for (const char *mode : {"loop", "dupe-loop", "switch-seed"}) {
    INFO(mode); Fixture f(mode); auto r = f.run(); REQUIRE(r.changed);
    CHECK(r.counters.at("rewritten_edges") == 1); CHECK(r.counters.at("loop_contexts") == 1);
    const unsigned backedges = std::string(mode) == "dupe-loop" ? 2 : 1;
    CHECK(r.counters.at("loop_backedges") == backedges);
    auto *fast = f.freshSwitch(); f.checkFallback(*fast); auto *root = f.block("head.transition"); REQUIRE(root);
    auto *state = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("state.transition")); REQUIRE(state);
    auto *history = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("history.transition")); REQUIRE(history);
    CHECK(state->getParent() == root); CHECK(state->getNumIncomingValues() == 1 + backedges);
    CHECK(history->getNumIncomingValues() == 1 + backedges);
    unsigned fresh = 0, initial = 0;
    for (unsigned n = 0; n < state->getNumIncomingValues(); ++n) {
      if (state->getIncomingBlock(n) == fast->getParent()) {
        ++fresh; CHECK(state->getIncomingValue(n) == f.instruction("next.state.transition"));
        CHECK(history->getIncomingValueForBlock(fast->getParent()) == f.instruction("fresh.freeze.transition"));
      } else { ++initial; CHECK(state->getIncomingValue(n) == f.instruction("choose.state"));
        CHECK(history->getIncomingValue(n) == f.instruction("seed.history")); }
    }
    CHECK(fresh == backedges); CHECK(initial == 1);
    for (auto c : fast->cases()) if (c.getCaseValue()->getZExtValue() < 200) CHECK(c.getCaseSuccessor() == root);
    f.checkEveryPhi();
  }
}

TEST_CASE("Checked corridors keep chooser once and preserve each loop access and flag", "[llvm][guarded-checked-corridors]") {
  for (const char *mode : {"loop", "table-before", "table-after"}) {
    INFO(mode); Fixture f(mode); auto *function = f.function; auto *arg = function->getArg(0);
    REQUIRE(f.run().changed); CHECK(f.module.getFunction("view") == function); CHECK(function->getArg(0) == arg);
    auto *fast = f.freshSwitch(); f.checkFallback(*fast);
    unsigned calls = 0, prefix = 0;
    for (auto &b : *function) for (auto &i : b) {
      if (llvm::isa<llvm::CallBase>(i)) { ++calls; CHECK(b.getName() == "entry"); }
      if (i.getName().starts_with("prefix.increment")) { ++prefix; CHECK(b.getName() == "seed"); }
      if (!f.originalBlocks.contains(b.getName().str())) for (auto *succ : llvm::successors(&b)) CHECK(succ != f.block("seed"));
    }
    CHECK(calls == 1); CHECK(prefix == 1);
    CHECK(f.instruction("iteration.increment.transition") != nullptr);
    CHECK(f.instruction("fresh.transition") != f.instruction("fresh"));
    auto *flags = llvm::dyn_cast_or_null<llvm::BinaryOperator>(f.instruction("retained.flags.transition")); REQUIRE(flags);
    CHECK(flags->hasNoSignedWrap()); CHECK(flags->hasNoUnsignedWrap());
    if (std::string(mode) != "loop") {
      auto *read = llvm::cast<llvm::LoadInst>(fast->getCondition()); bool readSeen = false, wrote = false;
      for (auto &i : *fast->getParent()) {
        readSeen |= &i == read;
        if (auto *write = llvm::dyn_cast<llvm::StoreInst>(&i); write && write->getPointerOperand() == read->getPointerOperand()) {
          CHECK(write->getValueOperand() == arg->getParent()->getArg(2));
          CHECK(readSeen == (std::string(mode) == "table-after")); wrote = true;
        }
      }
      CHECK(wrote);
    }
    f.checkEveryPhi();
  }
}

TEST_CASE("Checked corridors snapshot an old region execution before fresh loop reads", "[llvm][guarded-checked-corridors]") {
  Fixture f("historical"); auto r = f.run(); REQUIRE(r.changed);
  CHECK(r.counters.at("loop_contexts") == 1); CHECK(r.counters.at("loop_backedges") == 1);
  auto *fast = f.freshSwitch(); f.checkFallback(*fast);
  auto *snapshotBlock = f.block("transition.inputs"); REQUIRE(snapshotBlock);
  auto *snapshot = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("transition.old")); REQUIRE(snapshot);
  CHECK(snapshot->getParent() == snapshotBlock); REQUIRE(snapshot->getNumIncomingValues() == 1);
  CHECK(snapshot->getIncomingBlock(0) == f.block("seed"));
  auto *seedBranch = llvm::dyn_cast<llvm::BranchInst>(f.block("seed")->getTerminator()); REQUIRE(seedBranch);
  CHECK(seedBranch->getSuccessor(0) == snapshotBlock);
  CHECK(snapshotBlock->getTerminator()->getSuccessor(0) == f.block("head.transition"));
  auto *history = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("history.transition")); REQUIRE(history);
  CHECK(history->getIncomingValueForBlock(snapshotBlock) == snapshot);
  CHECK(history->getIncomingValueForBlock(fast->getParent()) == f.instruction("fresh.freeze.transition"));
  // The selected seed can be reached from either old code or the cloned
  // fallback. Its snapshot observes that previous execution, while the
  // terminal fast backedge observes this iteration's new LOAD/freeze.
  std::set<llvm::Value *> visited, leaves;
  std::vector<llvm::Value *> pending{snapshot->getIncomingValue(0)};
  while (!pending.empty()) {
    auto *value = pending.back(); pending.pop_back(); if (!visited.insert(value).second) continue;
    REQUIRE(visited.size() <= 128);
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value))
      for (auto &incoming : phi->incoming_values()) pending.push_back(incoming.get());
    else leaves.insert(value);
  }
  const std::set<llvm::Value *> expected{f.instruction("fresh.freeze"), f.instruction("fresh.freeze.transition")};
  CHECK(leaves == expected);
  CHECK(f.instruction("prefix.increment.transition") == nullptr);
  f.checkEveryPhi();
}

TEST_CASE("Checked corridors preserve straight continuations and legacy admission", "[llvm][guarded-checked-corridors]") {
  Fixture straight("straight"); auto r = straight.run(); REQUIRE(r.changed);
  CHECK(r.counters.at("loop_contexts") == 0); CHECK(r.counters.at("loop_backedges") == 0);
  auto *fast = straight.freshSwitch(); straight.checkFallback(*fast);
  CHECK(fast->getNumCases() == 2);
  for (auto c : fast->cases()) CHECK(c.getCaseSuccessor() == straight.block("hit"));
  CHECK(straight.instruction("state.transition") == nullptr);
  for (const char *name : {"guarded-state-corridors", "guarded-store-corridors", "guarded-partial-corridors", "guarded-partial-masks"}) {
    INFO(name); Fixture old; auto before = old.text(); CHECK_FALSE(old.run(name).changed); CHECK(old.text() == before);
  }
  Fixture unmarked("unmarked"); auto before = unmarked.text(); CHECK_FALSE(unmarked.run().changed); CHECK(unmarked.text() == before);
  const auto defaults = PassPipeline::defaultLLVM();
  CHECK(std::find(defaults.begin(), defaults.end(), "guarded-checked-corridors") == defaults.end());
}

TEST_CASE("Checked corridors refuse unsafe or unknown paths without partial edits", "[llvm][guarded-checked-corridors]") {
  for (const char *mode : {"call-inside", "fence", "volatile", "unknown", "bad-target"}) {
    INFO(mode); Fixture f(mode); auto before = f.text(); auto *function = f.function; auto *arg = function->getArg(0);
    auto r = f.run(); CHECK_FALSE(r.changed); CHECK(r.counters.at("rewritten_edges") == 0);
    CHECK(r.counters.at("loop_contexts") == 0); CHECK(r.counters.at("loop_backedges") == 0);
    CHECK(f.text() == before); CHECK(f.module.getFunction("view") == function); CHECK(function->getArg(0) == arg);
  }
}

TEST_CASE("Checked corridor budget refusal preserves identities and all source edges", "[llvm][guarded-checked-corridors]") {
  std::vector<Options> limits{{{"max_candidates", std::int64_t(0)}}, {{"max_rewrites", std::int64_t(0)}},
      {{"max_blocks", std::int64_t(0)}}, {{"max_instructions", std::int64_t(0)}},
      {{"max_blocks", std::int64_t(2)}}, {{"max_instructions", std::int64_t(1)}}};
  for (const auto &options : limits) {
    Fixture f; auto before = f.text(); auto *function = f.function; auto *arg = function->getArg(0);
    auto *metadata = f.module.getNamedMetadata("dragon.guarded-entry"); auto r = f.run("guarded-checked-corridors", options);
    CHECK_FALSE(r.changed); CHECK(r.status == Status::Incomplete); CHECK(r.counters.at("budget_exhausted") > 0);
    CHECK(r.counters.at("rewritten_edges") == 0); CHECK(r.counters.at("loop_contexts") == 0); CHECK(r.counters.at("loop_backedges") == 0);
    CHECK(f.text() == before); CHECK(f.module.getFunction("view") == function); CHECK(function->getArg(0) == arg);
    CHECK(f.module.getNamedMetadata("dragon.guarded-entry") == metadata); f.checkEveryPhi();
  }
}
