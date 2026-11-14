#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace dragon::passes;
using Builder = llvm::IRBuilder<llvm::NoFolder>;
constexpr const char *Generated = "dragon.guarded-transitions.generated";
constexpr const char *Predictions = "dragon.guard-predictions";

// These are predictions, not an alias proof: the nearest finite STORE is
// allowed to be unrelated, and the actual target must still select the path.
struct ExpressionFixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module = lifter.module();
  llvm::Function *function;
  std::string mode;
  unsigned width;
  ExpressionFixture(unsigned w = 32, std::string kind = "normal",
                    const char *manifest = Predictions)
      : mode(std::move(kind)), width(w) {
    module.setDataLayout("e-p:64:64");
    auto &c = module.getContext();
    auto *ptr = llvm::PointerType::getUnqual(c);
    auto *word = llvm::IntegerType::get(c, width);
    auto *i64 = llvm::Type::getInt64Ty(c);
    const bool loop = mode == "loop" || mode == "dupe-loop" || mode == "duplicate-seed-loop";
    const bool duplicateSeed = mode == "duplicate-seed-loop";
    const unsigned backedges = mode == "dupe-loop" ? 2 : 1;
    const bool subset = mode == "partial-missing" || mode == "partial-noncase" ||
        mode == "missing-all" || mode == "noncase-all";
    const bool encoded = mode == "encoded" || subset;
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {ptr, ptr, word, llvm::Type::getInt1Ty(c), llvm::Type::getInt1Ty(c)}, false),
        llvm::GlobalValue::ExternalLinkage, "view", module);
    auto block = [&](const char *name) { return llvm::BasicBlock::Create(c, name, function); };
    auto *entry = block("entry"), *seed = block("seed"), *other = block("other");
    auto *head = block("head"), *bounds = block("bounds"), *dispatch = block("dispatch");
    auto *hit = block("hit"), *miss = block("miss");
    Builder b(entry);
    auto read = [&](llvm::Type *type, llvm::Value *address, const char *name) {
      auto *v = b.CreateLoad(type, address, name); v->setAlignment(llvm::Align(1)); return v;
    };
    auto *guest = read(ptr, function->getArg(1), "guest");
    auto *size = read(i64, b.CreateGEP(b.getInt8Ty(), function->getArg(1), b.getInt64(8)), "size");
    auto address = [&](std::uint64_t offset) {
      return b.CreateGEP(b.getInt8Ty(), guest, b.getInt64(offset));
    };
    auto store = [&](llvm::Value *value, llvm::Value *where) {
      auto *s = b.CreateStore(value, where); s->setAlignment(llvm::Align(1)); return s;
    };
    b.CreateCondBr(function->getArg(4), seed, other);
    b.SetInsertPoint(seed);
    llvm::Value *chosen = b.CreateSelect(function->getArg(3),
        llvm::ConstantInt::get(word, llvm::APInt::getAllOnes(width)),
        llvm::ConstantInt::get(word, 0), "next.state");
    if (encoded) {
      // enc(s) = (s xor 0x6a09e667) + 0x9e3779b9 modulo 2^32.
      const std::uint32_t a = std::uint32_t(0x6a09e667u + 0x9e3779b9u);
      const unsigned second = mode == "partial-missing" || mode == "missing-all" ? 6 : 1;
      const std::uint32_t d = std::uint32_t((second ^ 0x6a09e667u) + 0x9e3779b9u);
      chosen = b.CreateSelect(function->getArg(3), b.getInt32(a), b.getInt32(d), "encoded.next");
    }
    if (mode == "unknown-store") chosen = function->getArg(2);
    if (mode == "undef-store") chosen = llvm::UndefValue::get(word);
    if (mode == "poison-store") chosen = llvm::PoisonValue::get(word);
    if (mode == "wrong-width") chosen = b.getInt16(0);
    auto *where = mode == "wrong-base" ? function->getArg(0) : address(256);
    store(chosen, where);
    if (mode == "unrelated-store")
      store(b.CreateSelect(function->getArg(3), llvm::ConstantInt::get(word, 1),
          llvm::ConstantInt::get(word, 2)), address(280));
    if (duplicateSeed) b.CreateCondBr(function->getArg(3), head, head); else b.CreateBr(head);
    b.SetInsertPoint(other); b.CreateBr(head);
    b.SetInsertPoint(head);
    llvm::PHINode *history = nullptr;
    if (loop) {
      history = b.CreatePHI(word, 2 + duplicateSeed + backedges, "history");
      history->addIncoming(function->getArg(2), seed);
      if (duplicateSeed) history->addIncoming(function->getArg(2), seed);
      history->addIncoming(function->getArg(2), other);
    }
    llvm::Value *state = read(word, address(256), "state.raw");
    if (mode == "volatile-state") llvm::cast<llvm::LoadInst>(state)->setVolatile(true);
    if (mode == "atomic-state") llvm::cast<llvm::LoadInst>(state)->setAtomic(llvm::AtomicOrdering::Monotonic);
    if (mode == "undef-state") state = llvm::UndefValue::get(word);
    if (mode == "poison-state") state = llvm::PoisonValue::get(word);
    if (mode == "multi-load") state = b.CreateXor(state, read(word, address(264), "second.raw"), "mixed.state");
    llvm::Value *added = b.CreateAdd(state, llvm::ConstantInt::get(word,
        encoded ? 1640531527u : 1u), "state.add");
    llvm::Value *decoded = b.CreateXor(added, llvm::ConstantInt::get(word,
        encoded ? 1779033703u : 2u), "state.xor");
    if (mode == "bad-shift") decoded = b.CreateShl(decoded, llvm::ConstantInt::get(word, width), "bad.shift");
    if (mode == "poison-operand") decoded = b.CreateAdd(decoded, llvm::PoisonValue::get(word), "poison.operand");
    if (mode == "call") {
      auto external = module.getOrInsertFunction("external_maywrite",
          llvm::FunctionType::get(b.getVoidTy(), {ptr}, false));
      b.CreateCall(external, {guest});
    }
    auto *extended = width < 64 ? b.CreateZExt(decoded, i64, "state.extended") : decoded;
    auto *scaled = b.CreateShl(extended, b.getInt64(3), "state.scaled");
    llvm::Value *index = b.CreateAdd(scaled, b.getInt64(1024), "table.index");
    if (mode == "nested-poison-index-select" || mode == "nested-undef-index-select" ||
        mode == "freeze-poison-index-select") {
      llvm::Value *invalid = mode == "nested-undef-index-select"
          ? static_cast<llvm::Value *>(llvm::UndefValue::get(word)) : llvm::PoisonValue::get(word);
      llvm::Value *condition = b.CreateICmpEQ(invalid, function->getArg(2), "nested.condition");
      if (mode == "freeze-poison-index-select") condition = b.CreateFreeze(condition, "defined.condition");
      index = b.CreateSelect(condition, b.getInt64(1040), b.getInt64(1048), "nested.select");
    }
    if (mode == "poison-index-select" || mode == "undef-index-select") {
      llvm::Value *condition = mode == "poison-index-select"
          ? llvm::PoisonValue::get(b.getInt1Ty()) : llvm::UndefValue::get(b.getInt1Ty());
      index = b.CreateSelect(condition, b.getInt64(1040), b.getInt64(1048), "invalid.condition");
    }
    b.CreateBr(bounds);
    b.SetInsertPoint(bounds);
    auto *range = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
        b.CreateICmpULE(index, b.CreateSub(size, b.getInt64(8))));
    if (subset) range = b.CreateAnd(range, b.CreateICmpULE(decoded,
        llvm::ConstantInt::get(word, 1), "state.bound"), "bounded.range");
    b.CreateCondBr(range, dispatch, miss);
    b.SetInsertPoint(dispatch);
    auto *table = b.CreateGEP(b.getInt8Ty(), guest, index, "table.address");
    if (mode == "overwrite-table") store(b.CreateZExtOrTrunc(function->getArg(2), i64), table);
    auto *actual = read(i64, table, "actual.target");
    if (mode == "volatile-target") actual->setVolatile(true);
    if (mode == "after-load") store(b.CreateZExtOrTrunc(function->getArg(2), i64), table);
    auto *sw = b.CreateSwitch(actual, miss);
    for (unsigned value : {100u, 200u, 300u, 400u}) {
      const bool backedge = loop && (value == 100 || (mode == "dupe-loop" && value == 300));
      sw->addCase(b.getInt64(value), backedge ? head : hit);
      if (backedge) history->addIncoming(state, dispatch);
    }
    b.SetInsertPoint(hit);
    auto *result = b.CreatePHI(i64, loop ? 4 - backedges : 4, "selected");
    for (unsigned n = 0; n < (loop ? 4 - backedges : 4); ++n) result->addIncoming(actual, dispatch);
    b.CreateRet(result);
    b.SetInsertPoint(miss); b.CreateRet(b.getInt64(-1));

    if (manifest) {
      auto bytes = [](std::uint64_t value) {
        std::string out; constexpr char hex[] = "0123456789abcdef";
        for (unsigned n = 0; n < 8; ++n) { auto v = (value >> (8 * n)) & 255; out += hex[v >> 4]; out += hex[v & 15]; }
        return out;
      };
      std::map<std::uint64_t, std::uint64_t> values{{1024, 300}, {1032, 400}, {1040, 100}, {1048, 200}};
      if (encoded) values = {{1024, 100}, {1032, 200}};
      if (mode == "partial-noncase") values[1032] = 999;
      if (mode == "missing-all") values = {{1032, 200}};
      if (mode == "noncase-all") values = {{1024, 999}, {1032, 999}};
      if (mode == "dupe-loop") values[1048] = 300;
      if (mode == "wrong-hints") values = {{1040, 300}, {1048, 400}};
      if (mode == "unknown-target") values = {{1040, 999}, {1048, 999}};
      std::string json = "{\"view_symbol\":\"view\",\"memory\":[";
      bool first = true;
      for (auto [offset, value] : values) {
        if (!first) json += ","; first = false;
        std::string addressText; llvm::raw_string_ostream os(addressText); os << llvm::format_hex(offset, 0);
        json += "{\"offset\":\"" + addressText + "\",\"bytes\":\"" + bytes(value) + "\"}";
      }
      json += "]}";
      module.getOrInsertNamedMetadata(manifest)->addOperand(
          llvm::MDNode::get(c, llvm::MDString::get(c, json)));
    }
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
  Result run(const char *name = "guarded-store-expressions", Options options = {}) {
    options.try_emplace("max_rewrites", std::int64_t(1));
    PassPipeline pipeline; REQUIRE(pipeline.add(name, options));
    RunOptions run; run.maxIterations = 1; run.maxPassRuns = 1; run.verifyLLVM = true;
    auto report = pipeline.run(lifter, run);
    REQUIRE(report.ok); REQUIRE(report.runs.size() == 1); verify();
    return report.runs.front().result;
  }
  std::vector<llvm::SwitchInst *> fast() {
    std::vector<llvm::SwitchInst *> out;
    for (auto &b : *function) if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator()))
      if (sw->getMetadata(Generated))
        if (auto *read = llvm::dyn_cast<llvm::LoadInst>(sw->getCondition()); read && read->getParent() == &b)
          out.push_back(sw);
    return out;
  }
  void check(const std::set<std::uint64_t> &expected) {
    auto predicted = fast(); REQUIRE(predicted.size() == 1);
    auto *sw = predicted.front(); std::set<std::uint64_t> actual;
    for (auto item : sw->cases()) actual.insert(item.getCaseValue()->getZExtValue());
    CHECK(actual == expected);
    auto *fallback = llvm::dyn_cast<llvm::SwitchInst>(sw->getDefaultDest()->getTerminator());
    REQUIRE(fallback); CHECK(fallback->getCondition() == sw->getCondition());
    CHECK(fallback->getNumCases() == 4); CHECK(fallback->getDefaultDest() == block("miss"));
    for (auto item : fallback->cases()) CHECK(item.getCaseSuccessor() == block("hit"));
    // The fallback must use the actual earlier read, even if a later STORE
    // changes the table. Repeating that LOAD would change the observation.
    for (auto &i : *fallback->getParent()) CHECK_FALSE(llvm::isa<llvm::LoadInst>(i));
    auto *source = llvm::dyn_cast<llvm::SwitchInst>(block("dispatch")->getTerminator());
    REQUIRE(source); CHECK(source->getNumCases() == 4); CHECK_FALSE(source->getMetadata(Generated));
    auto *raw = llvm::dyn_cast_or_null<llvm::LoadInst>(instruction("state.raw.transition"));
    REQUIRE(raw); CHECK(raw->getType()->isIntegerTy(width)); CHECK(raw->isSimple());
    auto *add = llvm::dyn_cast_or_null<llvm::BinaryOperator>(instruction("state.add.transition"));
    auto *decode = llvm::dyn_cast_or_null<llvm::BinaryOperator>(instruction("state.xor.transition"));
    REQUIRE(add); REQUIRE(decode); CHECK(add->getOperand(0) == raw); CHECK(decode->getOperand(0) == add);
    std::map<llvm::BasicBlock *, unsigned> predecessors;
    for (auto &b : *function) for (auto *successor : llvm::successors(&b)) if (successor == block("hit")) ++predecessors[&b];
    auto *phi = llvm::dyn_cast<llvm::PHINode>(&block("hit")->front()); REQUIRE(phi);
    std::map<llvm::BasicBlock *, unsigned> inputs;
    for (unsigned n = 0; n < phi->getNumIncomingValues(); ++n) ++inputs[phi->getIncomingBlock(n)];
    CHECK(inputs == predecessors);
  }
};
}

TEST_CASE("STORE expression predictions evaluate narrow encoded state modulo its width", "[llvm][guarded-store-expressions]") {
  for (unsigned width : {8u, 16u, 32u, 64u}) {
    INFO(width); ExpressionFixture fixture(width); auto result = fixture.run(); REQUIRE(result.changed);
    CHECK(result.counters.at("rewritten_edges") == 1); fixture.check({100, 200});
  }
  ExpressionFixture encoded(32, "encoded"); REQUIRE(encoded.run().changed); encoded.check({100, 200});
}

TEST_CASE("STORE expression prediction manifest is independent and opt in", "[llvm][guarded-store-expressions]") {
  for (const char *metadata : {static_cast<const char *>(nullptr), "dragon.guarded-entry"}) {
    ExpressionFixture fixture(32, "normal", metadata); auto before = fixture.text(); auto result = fixture.run();
    CHECK_FALSE(result.changed); CHECK(result.status == Status::Unavailable); CHECK(fixture.text() == before);
  }
  for (const char *name : {"guarded-state-corridors", "guarded-store-corridors", "guarded-bit-slices"}) {
    ExpressionFixture fixture; auto before = fixture.text(); auto result = fixture.run(name);
    CHECK_FALSE(result.changed); CHECK(result.status == Status::Unavailable); CHECK(fixture.text() == before);
  }
  const auto defaults = PassPipeline::defaultLLVM();
  CHECK(std::find(defaults.begin(), defaults.end(), "guarded-store-expressions") == defaults.end());
}

TEST_CASE("STORE expression wrong hints preserve actual observations and complete fallback", "[llvm][guarded-store-expressions]") {
  for (const char *mode : {"wrong-hints", "unrelated-store", "overwrite-table", "after-load"}) {
    INFO(mode); ExpressionFixture fixture(32, mode); auto result = fixture.run(); REQUIRE(result.changed);
    fixture.check(std::string(mode) == "wrong-hints" || std::string(mode) == "unrelated-store"
        ? std::set<std::uint64_t>{300, 400} : std::set<std::uint64_t>{100, 200});
    if (std::string(mode) == "overwrite-table" || std::string(mode) == "after-load") {
      auto *sw = fixture.fast().front(); auto *load = llvm::cast<llvm::LoadInst>(sw->getCondition());
      bool seenLoad = false, foundStore = false;
      for (auto &instruction : *sw->getParent()) {
        if (&instruction == load) seenLoad = true;
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            store && store->getPointerOperand() == load->getPointerOperand()) {
          CHECK(seenLoad == (std::string(mode) == "after-load")); foundStore = true;
        }
      }
      CHECK(foundStore);
    }
  }
}

TEST_CASE("STORE expression unsafe or nonfinite dependencies leave source unchanged", "[llvm][guarded-store-expressions]") {
  for (const char *mode : {"unknown-store", "undef-store", "poison-store", "wrong-width", "wrong-base",
                          "undef-state", "poison-state", "poison-operand", "multi-load", "volatile-state",
                          "atomic-state", "volatile-target", "bad-shift", "call", "unknown-target"}) {
    INFO(mode); ExpressionFixture fixture(32, mode); auto before = fixture.text(); auto result = fixture.run();
    CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
  }
}

TEST_CASE("STORE expression options enforce existing shared and hard budgets", "[llvm][guarded-store-expressions]") {
  for (const char *key : {"max_candidates", "max_rewrites", "max_blocks", "max_instructions"}) {
    INFO(key); ExpressionFixture fixture; auto before = fixture.text();
    auto result = fixture.run("guarded-store-expressions", {{key, std::int64_t(0)}});
    CHECK_FALSE(result.changed); CHECK(result.status == Status::Incomplete);
    CHECK(result.counters.at("budget_exhausted") > 0); CHECK(fixture.text() == before);
    for (Parameter value : {Parameter(std::int64_t(-1)), Parameter(std::string("1")), Parameter(true)}) {
      PassPipeline invalid; CHECK_FALSE(invalid.add("guarded-store-expressions", {{key, value}}));
    }
    PassPipeline huge; CHECK_FALSE(huge.add("guarded-store-expressions",
        {{key, std::int64_t(std::string(key) == "max_instructions" ? 262145 : 4097)}}));
  }
  PassPipeline unknown; CHECK_FALSE(unknown.add("guarded-store-expressions", {{"unknown", std::int64_t(1)}}));
}

TEST_CASE("STORE expression continuation loops retain fresh reads and every incoming slot", "[llvm][guarded-store-expressions]") {
  for (const char *mode : {"loop", "dupe-loop", "duplicate-seed-loop"}) {
    INFO(mode); ExpressionFixture fixture(32, mode); auto result = fixture.run(); REQUIRE(result.changed);
    CHECK(result.counters.at("rewritten_edges") == 1); CHECK(result.counters.at("loop_contexts") == 1);
    const unsigned backedges = std::string(mode) == "dupe-loop" ? 2 : 1;
    CHECK(result.counters.at("loop_backedges") == backedges);
    auto fast = fixture.fast(); REQUIRE(fast.size() == 1);
    auto *root = fixture.block("head.transition"); REQUIRE(root);
    auto *history = llvm::dyn_cast_or_null<llvm::PHINode>(fixture.instruction("history.transition")); REQUIRE(history);
    auto *fresh = fixture.instruction("state.raw.transition"); REQUIRE(fresh);
    CHECK(history->getParent() == root); CHECK(fresh->getParent() == root);
    CHECK(history->getNumIncomingValues() == 1 + backedges);
    unsigned oldInputs = 0, freshInputs = 0;
    for (unsigned n = 0; n < history->getNumIncomingValues(); ++n) {
      if (history->getIncomingBlock(n) == fast.front()->getParent()) {
        ++freshInputs; CHECK(history->getIncomingValue(n) == fresh);
      } else { ++oldInputs; CHECK(history->getIncomingValue(n) == fixture.function->getArg(2)); }
    }
    CHECK(oldInputs == 1); CHECK(freshInputs == backedges);
    unsigned loopCases = 0;
    for (auto item : fast.front()->cases()) {
      if (item.getCaseValue()->equalsInt(100) || item.getCaseValue()->equalsInt(300)) {
        CHECK(item.getCaseSuccessor() == root); ++loopCases;
      }
    }
    CHECK(loopCases == backedges);
    auto *fallback = llvm::dyn_cast<llvm::SwitchInst>(fast.front()->getDefaultDest()->getTerminator()); REQUIRE(fallback);
    CHECK(fallback->getCondition() == fast.front()->getCondition()); CHECK(fallback->getNumCases() == 4);
    for (auto item : fallback->cases()) {
      const auto value = item.getCaseValue()->getZExtValue();
      const bool originalLoop = value == 100 || (std::string(mode) == "dupe-loop" && value == 300);
      CHECK(item.getCaseSuccessor() == fixture.block(originalLoop ? "head" : "hit"));
    }
    // Audit every PHI against successor slots, not merely distinct blocks.
    for (auto &block : *fixture.function) {
      std::map<llvm::BasicBlock *, unsigned> actual;
      for (auto &predecessor : *fixture.function)
        for (auto *successor : llvm::successors(&predecessor)) if (successor == &block) ++actual[&predecessor];
      for (auto &phi : block.phis()) {
        std::map<llvm::BasicBlock *, unsigned> inputs;
        for (unsigned n = 0; n < phi.getNumIncomingValues(); ++n) ++inputs[phi.getIncomingBlock(n)];
        CHECK(inputs == actual);
      }
    }
    if (std::string(mode) == "duplicate-seed-loop") {
      auto *seed = llvm::cast<llvm::BranchInst>(fixture.block("seed")->getTerminator());
      CHECK(seed->getSuccessor(0) != seed->getSuccessor(1));
      CHECK(seed->getSuccessor(0) == root); CHECK(seed->getSuccessor(1) == fixture.block("head"));
    }
  }
}

TEST_CASE("STORE expression admission checks poison and undef finite select conditions", "[llvm][guarded-store-expressions]") {
  for (const char *mode : {"poison-index-select", "undef-index-select"}) {
    INFO(mode); ExpressionFixture fixture(32, mode); auto before = fixture.text();
    auto result = fixture.run(); CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
  }
}

TEST_CASE("STORE expression admission checks nested invalid conditions across freeze boundaries", "[llvm][guarded-store-expressions]") {
  for (const char *mode : {"nested-poison-index-select", "nested-undef-index-select"}) {
    INFO(mode); ExpressionFixture fixture(32, mode); auto before = fixture.text();
    auto result = fixture.run(); CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
  }
  ExpressionFixture defined(32, "freeze-poison-index-select");
  auto *freeze = llvm::dyn_cast_or_null<llvm::FreezeInst>(defined.instruction("defined.condition")); REQUIRE(freeze);
  REQUIRE(llvm::isa<llvm::ICmpInst>(freeze->getOperand(0)));
  auto result = defined.run(); REQUIRE(result.changed); auto fast = defined.fast(); REQUIRE(fast.size() == 1);
  std::set<std::uint64_t> cases;
  for (auto item : fast.front()->cases()) cases.insert(item.getCaseValue()->getZExtValue());
  CHECK((cases == std::set<std::uint64_t>{100, 200}));
  freeze = llvm::dyn_cast_or_null<llvm::FreezeInst>(defined.instruction("defined.condition")); REQUIRE(freeze);
  auto *comparison = llvm::dyn_cast<llvm::ICmpInst>(freeze->getOperand(0)); REQUIRE(comparison);
  CHECK(llvm::isa<llvm::PoisonValue>(comparison->getOperand(0)));
  auto *fallback = llvm::dyn_cast<llvm::SwitchInst>(fast.front()->getDefaultDest()->getTerminator()); REQUIRE(fallback);
  CHECK(fallback->getCondition() == fast.front()->getCondition()); CHECK(fallback->getNumCases() == 4);
}

TEST_CASE("STORE expression correlated retries keep invalid historical arms", "[llvm][guarded-store-expressions]") {
  for (const char *mode : {"poison", "undef", "freeze-poison"}) {
    INFO(mode); ExpressionFixture fixture;
    auto &context = fixture.module.getContext(); auto *function = fixture.function;
    auto make = [&](const char *name) { return llvm::BasicBlock::Create(context, name, function); };
    auto *route = make("route"), *left = make("old.left"), *right = make("old.right"), *join = make("old.join");
    llvm::cast<llvm::BranchInst>(fixture.block("entry")->getTerminator())->setSuccessor(0, route);
    Builder b(route); b.CreateCondBr(function->getArg(3), left, right);
    b.SetInsertPoint(left); b.CreateBr(join);
    b.SetInsertPoint(right);
    llvm::Value *invalid = std::string(mode) == "undef"
        ? static_cast<llvm::Value *>(llvm::UndefValue::get(b.getInt1Ty())) : llvm::PoisonValue::get(b.getInt1Ty());
    if (std::string(mode) == "freeze-poison") invalid = b.CreateFreeze(invalid, "defined.old");
    b.CreateBr(join); b.SetInsertPoint(join);
    auto *history = b.CreatePHI(b.getInt1Ty(), 2, "old.boolean");
    // Unknown first incoming causes the initial ordinary enumeration to stop
    // before visiting the invalid second arm. A correlated retry must retain
    // that failure, even while another Boolean assignment is still missing.
    history->addIncoming(function->getArg(3), left); history->addIncoming(invalid, right);
    b.CreateBr(fixture.block("seed"));
    auto *head = fixture.block("head");
    auto *selected = llvm::PHINode::Create(b.getInt1Ty(), 2, "selected.old", &head->front());
    selected->addIncoming(history, fixture.block("seed")); selected->addIncoming(b.getFalse(), fixture.block("other"));
    b.SetInsertPoint(head->getTerminator());
    auto *sum = b.CreateAdd(b.CreateZExt(selected, b.getInt64Ty()),
        b.CreateZExt(function->getArg(4), b.getInt64Ty()), "correlated.state");
    auto *index = b.CreateAdd(b.getInt64(1024), b.CreateShl(sum, b.getInt64(3)), "correlated.index");
    fixture.instruction("table.index")->replaceAllUsesWith(index);
    // Earlier blocks are explicitly closed seeds. Only the ordinary seed's
    // actual head incoming is considered, so an invalid old arm cannot be
    // bypassed by discovering an unrelated earlier context.
    for (auto *block : {fixture.block("entry"), fixture.block("other"), route, left, right, join}) {
      block->getTerminator()->setMetadata(Generated, llvm::MDNode::get(context, {}));
      block->getTerminator()->setMetadata("dragon.guarded-store-expressions.generated", llvm::MDNode::get(context, {}));
    }
    fixture.verify(); auto before = fixture.text(); auto result = fixture.run();
    if (std::string(mode) != "freeze-poison") {
      CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
    } else {
      REQUIRE(result.changed); auto fast = fixture.fast(); REQUIRE(fast.size() == 1);
      std::set<std::uint64_t> cases;
      for (auto item : fast.front()->cases()) cases.insert(item.getCaseValue()->getZExtValue());
      CHECK((cases == std::set<std::uint64_t>{100, 300, 400}));
      auto *retained = llvm::dyn_cast_or_null<llvm::FreezeInst>(fixture.instruction("defined.old")); REQUIRE(retained);
      CHECK(llvm::isa<llvm::PoisonValue>(retained->getOperand(0)));
      auto *fallback = llvm::dyn_cast<llvm::SwitchInst>(fast.front()->getDefaultDest()->getTerminator()); REQUIRE(fallback);
      CHECK(fallback->getCondition() == fast.front()->getCondition()); CHECK(fallback->getNumCases() == 4);
    }
  }
}

TEST_CASE("STORE expression mapped target subsets retain actual bounds and full fallback", "[llvm][guarded-store-expressions]") {
  for (const char *mode : {"partial-missing", "partial-noncase"}) {
    INFO(mode); ExpressionFixture fixture(32, mode); auto result = fixture.run(); REQUIRE(result.changed);
    CHECK(result.status == Status::Incomplete); CHECK(result.counters.at("rewritten_edges") == 1);
    CHECK(result.counters.at("partial_rewritten_edges") == 1); fixture.check({100});
    auto *sourceRead = llvm::dyn_cast_or_null<llvm::LoadInst>(fixture.instruction("state.raw")); REQUIRE(sourceRead);
    CHECK(sourceRead->getType()->isIntegerTy(32));
    for (const char *name : {"bounds", "bounds.transition"}) {
      auto *block = fixture.block(name); REQUIRE(block);
      auto *branch = llvm::dyn_cast<llvm::BranchInst>(block->getTerminator()); REQUIRE(branch);
      CHECK(branch->isConditional()); CHECK(branch->getSuccessor(1) == fixture.block("miss"));
    }
    auto *checked = llvm::dyn_cast_or_null<llvm::ICmpInst>(fixture.instruction("state.bound.transition")); REQUIRE(checked);
    CHECK(checked->getPredicate() == llvm::CmpInst::ICMP_ULE);
    CHECK(checked->getOperand(0) == fixture.instruction("state.xor.transition"));
    auto *limit = llvm::dyn_cast<llvm::ConstantInt>(checked->getOperand(1)); REQUIRE(limit); CHECK(limit->equalsInt(1));
    auto *miss = llvm::dyn_cast<llvm::ReturnInst>(fixture.block("miss")->getTerminator()); REQUIRE(miss);
    auto *error = llvm::dyn_cast<llvm::ConstantInt>(miss->getReturnValue()); REQUIRE(error); CHECK(error->getValue().isAllOnes());
  }
  for (const char *mode : {"missing-all", "noncase-all"}) {
    INFO(mode); ExpressionFixture fixture(32, mode); auto before = fixture.text(); auto result = fixture.run();
    CHECK_FALSE(result.changed); CHECK(result.counters.at("candidate_edges") == 0);
    CHECK(result.counters.at("partial_rewritten_edges") == 0); CHECK(fixture.text() == before);
  }
  // The strict historical profiles still reject a complete finite address set
  // if any address lacks a table hint. This relaxation belongs to the new
  // explicit prediction profile; it does not change old mapping admission.
  for (const char *pass : {"guarded-state-corridors", "guarded-store-corridors"}) {
    INFO(pass); ExpressionFixture fixture(32, "partial-missing", "dragon.guarded-entry");
    Builder b(fixture.block("head")->getTerminator());
    auto *finite = b.CreateSelect(fixture.function->getArg(3), b.getInt64(1024), b.getInt64(1072), "finite.index");
    fixture.instruction("table.index")->replaceAllUsesWith(finite); fixture.verify();
    auto before = fixture.text(); auto result = fixture.run(pass);
    CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
  }
}


namespace {
// Extend only this test input. Existing ten cases and their fixture modes are
// unchanged. The caller has already run before the fresh controller corridor.
void addFunnelObservation(ExpressionFixture &fixture, unsigned width, bool right,
                          std::uint64_t count, const std::string &mode = "seed") {
  auto *chosen = llvm::dyn_cast_or_null<llvm::SelectInst>(fixture.instruction("encoded.next")); REQUIRE(chosen);
  const bool earlier = mode == "earlier" || mode == "external-earlier";
  const bool inPath = mode == "path";
  llvm::Instruction *insert = earlier ? fixture.block("entry")->getTerminator()
      : inPath ? fixture.block("head")->getTerminator() : chosen;
  Builder b(insert); auto *word = llvm::IntegerType::get(fixture.module.getContext(), width);
  auto *source = b.CreateZExtOrTrunc(fixture.function->getArg(2), word, "observation.source");
  const auto typedCount = llvm::APInt(64, count).zextOrTrunc(width);
  llvm::Value *args[] = {source, b.CreateXor(source, llvm::ConstantInt::get(word,
      llvm::APInt::getAllOnes(width)), "observation.other"), llvm::ConstantInt::get(word, typedCount)};
  for (unsigned n = 0; n < 3; ++n) {
    if (mode == "poison-" + std::to_string(n)) args[n] = llvm::PoisonValue::get(word);
    if (mode == "undef-" + std::to_string(n)) args[n] = llvm::UndefValue::get(word);
  }
  const auto id = right ? llvm::Intrinsic::fshr : llvm::Intrinsic::fshl;
  auto *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(&fixture.module, id, {word});
  llvm::FunctionCallee callee = intrinsic;
  if (mode == "external" || mode == "external-earlier")
    callee = fixture.module.getOrInsertFunction("external_funnel",
        llvm::FunctionType::get(word, {word, word, word}, false));
  std::vector<llvm::OperandBundleDef> bundles;
  if (mode == "bundle") bundles.emplace_back("test.observation", std::vector<llvm::Value *>{source});
  auto *observed = b.CreateCall(callee, args, bundles, "observation.funnel");
  if (mode == "convergent") observed->setConvergent();
  if (!inPath) {
    // For a defined integer x, x*(x+1) has even parity. This pass must retain
    // the actual arithmetic/condition; observing the funnel result is not a
    // license to replace the condition or either state STORE alternative.
    auto *next = b.CreateAdd(observed, llvm::ConstantInt::get(word, 1), "observation.next");
    auto *product = b.CreateMul(observed, next, "observation.product");
    auto *bit = b.CreateAnd(product, llvm::ConstantInt::get(word, 1), "observation.parity");
    auto *condition = b.CreateICmpEQ(bit, llvm::ConstantInt::get(word, 0), "observation.condition");
    chosen->setCondition(condition);
  }
  fixture.verify();
}
void checkFunnelObservation(ExpressionFixture &fixture, unsigned width, bool right,
                            std::uint64_t count, const char *location) {
  unsigned calls = 0;
  for (auto &block : *fixture.function) for (auto &instruction : block)
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
      ++calls; CHECK(call->getParent() == fixture.block(location));
      CHECK_FALSE(block.getTerminator()->getMetadata(Generated));
      auto *callee = call->getCalledFunction(); REQUIRE(callee);
      CHECK(callee->getIntrinsicID() == (right ? llvm::Intrinsic::fshr : llvm::Intrinsic::fshl));
      CHECK(call->getType()->isIntegerTy(width));
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2)); REQUIRE(amount);
      CHECK(amount->getValue() == llvm::APInt(64, count).zextOrTrunc(width));
    }
  CHECK(calls == 1);
  auto *condition = llvm::dyn_cast_or_null<llvm::ICmpInst>(fixture.instruction("observation.condition")); REQUIRE(condition);
  auto *choice = llvm::dyn_cast_or_null<llvm::SelectInst>(fixture.instruction("encoded.next")); REQUIRE(choice);
  CHECK(choice->getCondition() == condition); CHECK_FALSE(llvm::isa<llvm::ConstantInt>(choice->getCondition()));
  auto *product = llvm::dyn_cast_or_null<llvm::BinaryOperator>(fixture.instruction("observation.product")); REQUIRE(product);
  CHECK(product->getOpcode() == llvm::Instruction::Mul);
  CHECK(product->getOperand(0) == fixture.instruction("observation.funnel"));
  fixture.check({100});
  auto *bounds = fixture.block("bounds.transition"); REQUIRE(bounds);
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(bounds->getTerminator()); REQUIRE(branch);
  CHECK(branch->isConditional()); CHECK(branch->getSuccessor(1) == fixture.block("miss"));
  auto *limit = llvm::dyn_cast_or_null<llvm::ICmpInst>(fixture.instruction("state.bound.transition")); REQUIRE(limit);
  CHECK(limit->getOperand(0) == fixture.instruction("state.xor.transition"));
}
}

TEST_CASE("STORE expression seed funnels remain single execution observations", "[llvm][guarded-store-expressions][guarded-intrinsic-seeds]") {
  for (unsigned width : {32u, 64u}) for (bool right : {false, true}) {
    for (std::uint64_t count : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(width - 1),
                               std::uint64_t(width), std::uint64_t(width + 1), ~std::uint64_t(0)}) {
      INFO(width << " " << right << " " << count);
      ExpressionFixture fixture(32, "partial-missing"); addFunnelObservation(fixture, width, right, count);
      auto result = fixture.run(); REQUIRE(result.changed); CHECK(result.counters.at("rewritten_edges") == 1);
      CHECK(result.counters.at("partial_rewritten_edges") == 1); checkFunnelObservation(fixture, width, right, count, "seed");
    }
    ExpressionFixture earlier(32, "partial-missing"); addFunnelObservation(earlier, width, right, width, "earlier");
    auto result = earlier.run(); REQUIRE(result.changed); checkFunnelObservation(earlier, width, right, width, "entry");
  }
}

TEST_CASE("STORE expression funnel admission preserves call and invalid operand barriers", "[llvm][guarded-store-expressions][guarded-intrinsic-seeds]") {
  for (const char *mode : {"path", "external", "external-earlier", "poison-0", "poison-1", "poison-2",
                          "undef-0", "undef-1", "undef-2", "convergent", "bundle"}) {
    for (unsigned width : {32u, 64u}) for (bool right : {false, true}) {
      INFO(mode << " " << width << " " << right);
      ExpressionFixture fixture(32, "partial-missing"); addFunnelObservation(fixture, width, right, width, mode);
      auto before = fixture.text(); auto result = fixture.run(); CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
    }
  }
  // Even a genuine funnel observation does not change admission in the old
  // profiles, which continue to read only their original metadata namespace.
  for (const char *pass : {"guarded-state-corridors", "guarded-store-corridors"}) {
    ExpressionFixture fixture(32, "partial-missing", "dragon.guarded-entry");
    addFunnelObservation(fixture, 32, false, 32); auto before = fixture.text(); auto result = fixture.run(pass);
    CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
  }
}

namespace {
void addHistoricalShape(ExpressionFixture &fixture, const std::string &mode,
                        bool reverse = false, unsigned chain = 0,
                        unsigned widthLeaves = 0, bool extraCast = false) {
  auto &context = fixture.module.getContext(); auto *function = fixture.function;
  const bool mutual = mode == "mutual" || mode == "mutual-dupe";
  const bool duplicate = mode == "mutual-dupe";
  const bool cache = mode.starts_with("cache-");
  auto block = [&](const char *name) { return llvm::BasicBlock::Create(context, name, function); };
  auto *route = block("history.route"), *left = block("history.left"), *right = block("history.right");
  auto *loop = block("history.loop"), *back = block("history.back");
  llvm::cast<llvm::BranchInst>(fixture.block("entry")->getTerminator())->setSuccessor(0, route);
  llvm::cast<llvm::BranchInst>(fixture.block("other")->getTerminator())->setSuccessor(0, route);
  Builder b(route); b.CreateCondBr(function->getArg(3), left, right);
  b.SetInsertPoint(left); b.CreateBr(loop);
  b.SetInsertPoint(right); llvm::Value *initial = function->getArg(2);
  if (mode == "poison-arm") initial = llvm::PoisonValue::get(b.getInt32Ty());
  if (mode == "undef-arm") initial = llvm::UndefValue::get(b.getInt32Ty());
  if (mode == "freeze-arm") initial = b.CreateFreeze(llvm::PoisonValue::get(b.getInt32Ty()), "history.frozen");
  if (mode == "call-arm") {
    auto callee = fixture.module.getOrInsertFunction("history_external", llvm::FunctionType::get(b.getInt32Ty(), {b.getInt32Ty()}, false));
    initial = b.CreateCall(callee, {function->getArg(2)}, "history.external");
  }
  if (mode == "volatile-arm" || mode == "atomic-arm") {
    auto *load = b.CreateLoad(b.getInt32Ty(), function->getArg(0), "history.observation"); load->setAlignment(llvm::Align(1));
    if (mode == "volatile-arm") load->setVolatile(true); else load->setAtomic(llvm::AtomicOrdering::Monotonic);
    initial = load;
  }
  if (mode == "nondom-funnel") {
    auto *callee = llvm::Intrinsic::getOrInsertDeclaration(&fixture.module, llvm::Intrinsic::fshl, {b.getInt32Ty()});
    initial = b.CreateCall(callee, {function->getArg(2), function->getArg(2), b.getInt32(7)}, "history.funnel");
  }
  if (mode == "unsupported-arm") {
    auto *vector = llvm::Constant::getNullValue(llvm::FixedVectorType::get(b.getInt32Ty(), 2));
    initial = b.CreateExtractElement(vector, function->getArg(2), "history.unsupported");
  }
  b.CreateBr(loop); b.SetInsertPoint(loop);
  auto *p = b.CreatePHI(b.getInt32Ty(), duplicate ? 4 : 3, "history.p");
  auto *q = mutual ? b.CreatePHI(b.getInt32Ty(), duplicate ? 4 : 3, "history.q") : nullptr;
  auto *iteration = b.CreatePHI(b.getInt32Ty(), duplicate ? 4 : 3, "history.iteration");
  llvm::Value *increment = b.getInt32(1);
  if (mode == "poison-operand") increment = llvm::PoisonValue::get(b.getInt32Ty());
  if (mode == "undef-operand") increment = llvm::UndefValue::get(b.getInt32Ty());
  auto *next = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(q ? static_cast<llvm::Value *>(q) : p, increment, "history.next"));
  next->setHasNoSignedWrap(); next->setHasNoUnsignedWrap();
  auto *qnext = q ? b.CreateXor(p, b.getInt32(0x12345678), "history.qnext") : nullptr;
  auto *count = b.CreateAdd(iteration, b.getInt32(1), "history.count");
  b.CreateCondBr(b.CreateICmpULT(iteration, b.getInt32(2)), back, fixture.block("seed"));
  b.SetInsertPoint(back);
  if (duplicate) b.CreateCondBr(function->getArg(3), loop, loop); else b.CreateBr(loop);
  auto incoming = [&](llvm::PHINode *phi, llvm::Value *carried, llvm::Value *rightValue) {
    auto addBack = [&]() { phi->addIncoming(carried, back); if (duplicate) phi->addIncoming(carried, back); };
    if (!reverse) addBack();
    phi->addIncoming(function->getArg(2), left); phi->addIncoming(rightValue, right);
    if (reverse) addBack();
  };
  incoming(p, next, initial); if (q) incoming(q, qnext, function->getArg(2));
  for (auto *predecessor : {left, right}) iteration->addIncoming(b.getInt32(0), predecessor);
  iteration->addIncoming(count, back); if (duplicate) iteration->addIncoming(count, back);
  auto *choice = llvm::dyn_cast_or_null<llvm::SelectInst>(fixture.instruction("encoded.next")); REQUIRE(choice);
  b.SetInsertPoint(choice); llvm::Value *observed = p;
  if (q) observed = b.CreateXor(p, q, "history.pair");
  for (unsigned n = 0; n < chain; ++n) observed = b.CreateAdd(observed, b.getInt32(0), "history.chain");
  if (widthLeaves) {
    std::vector<llvm::Value *> leaves{b.CreateZExt(observed, b.getInt64Ty(), "history.wide")};
    for (unsigned n = 0; n < widthLeaves; ++n) leaves.push_back(b.getInt64(1000 + n));
    while (leaves.size() > 1) {
      std::vector<llvm::Value *> nextLevel;
      for (std::size_t n = 0; n < leaves.size(); n += 2)
        nextLevel.push_back(n + 1 < leaves.size() ? b.CreateAdd(leaves[n], leaves[n + 1], "history.tree") : leaves[n]);
      leaves = std::move(nextLevel);
    }
    observed = leaves.front();
    if (extraCast) observed = b.CreateZExt(observed, llvm::IntegerType::get(context, 128), "history.extra");
  }
  auto *condition = b.CreateICmpULT(observed, llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(observed->getType()), 0x70000000), "history.condition");
  choice->setCondition(condition);
  if (cache) {
    auto *head = fixture.block("head"); b.SetInsertPoint(&head->front());
    llvm::Value *bit;
    if (mode == "cache-acyclic") {
      auto *fresh = b.CreateLoad(b.getInt32Ty(), function->getArg(0), "history.fresh"); fresh->setAlignment(llvm::Align(1));
      bit = b.CreateICmpEQ(fresh, b.getInt32(7), "history.second");
    } else bit = b.CreateICmpEQ(p, function->getArg(2), "history.second");
    auto *carried = b.CreateZExt(bit, b.getInt32Ty(), "history.carried");
    auto *sw = llvm::cast<llvm::SwitchInst>(fixture.block("dispatch")->getTerminator());
    sw->setSuccessor(1, loop); // The fixture adds case100 first.
    p->addIncoming(carried, fixture.block("dispatch")); iteration->addIncoming(b.getInt32(0), fixture.block("dispatch"));
    auto *hitPhi = llvm::cast<llvm::PHINode>(&fixture.block("hit")->front());
    hitPhi->removeIncomingValue(hitPhi->getBasicBlockIndex(fixture.block("dispatch")), false);
    auto *seedStore = static_cast<llvm::StoreInst *>(nullptr);
    for (auto &instruction : *fixture.block("seed")) if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) seedStore = store;
    REQUIRE(seedStore); seedStore->setOperand(0, function->getArg(2));
    llvm::Value *first = choice;
    if (mode == "cache-failed") {
      Builder seedBuilder(choice);
      auto *bad = seedBuilder.CreateAnd(condition, llvm::PoisonValue::get(b.getInt1Ty()), "history.bad");
      first = seedBuilder.CreateSelect(bad, choice->getTrueValue(), choice->getFalseValue(), "history.bad.choice");
    }
    auto *second = b.CreateSelect(mode == "cache-failed" ? condition : bit, b.getInt32(0), b.getInt32(0), "history.second.choice");
    auto *value = b.CreateAdd(first, second, "history.stored");
    auto *store = b.CreateStore(value, seedStore->getPointerOperand()); store->setAlignment(llvm::Align(1));
    // Close this already-consumed seed role without marking the block as a
    // generated corridor. It can still be copied from the earlier seed, so
    // a separate legal head-as-seed context cannot hide the cache-role check.
    head->getTerminator()->setMetadata("dragon.guarded-store-expressions.generated", llvm::MDNode::get(context, {}));
  }
  if (mode == "integer-cycle") {
    auto *head = fixture.block("head"); auto *selected = llvm::PHINode::Create(b.getInt32Ty(), 1, "history.integer", &head->front());
    selected->addIncoming(p, fixture.block("seed")); b.SetInsertPoint(head->getTerminator());
    auto *index = b.CreateAdd(b.getInt64(1024), b.CreateShl(b.CreateZExt(selected, b.getInt64Ty()), b.getInt64(3)), "history.integer.index");
    fixture.instruction("table.index")->replaceAllUsesWith(index);
  }
  for (auto *closed : {fixture.block("entry"), fixture.block("other"), route, left, right, loop, back}) {
    closed->getTerminator()->setMetadata(Generated, llvm::MDNode::get(context, {}));
    closed->getTerminator()->setMetadata("dragon.guarded-store-expressions.generated", llvm::MDNode::get(context, {}));
  }
  fixture.verify();
}
void checkHistoricalShape(ExpressionFixture &fixture, bool duplicate = false) {
  fixture.check({100});
  CHECK(fixture.block("history.loop.transition") == nullptr);
  auto *next = llvm::dyn_cast_or_null<llvm::BinaryOperator>(fixture.instruction("history.next")); REQUIRE(next);
  CHECK(next->hasNoSignedWrap()); CHECK(next->hasNoUnsignedWrap());
  auto *choice = llvm::dyn_cast_or_null<llvm::SelectInst>(fixture.instruction("encoded.next")); REQUIRE(choice);
  CHECK(choice->getCondition() == fixture.instruction("history.condition"));
  auto *p = llvm::dyn_cast_or_null<llvm::PHINode>(fixture.instruction("history.p")); REQUIRE(p);
  CHECK(p->getNumIncomingValues() == (duplicate ? 4 : 3));
  std::map<llvm::BasicBlock *, unsigned> predecessors;
  for (auto &block : *fixture.function) for (auto *successor : llvm::successors(&block))
    if (successor == fixture.block("history.loop")) ++predecessors[&block];
  for (auto &phi : fixture.block("history.loop")->phis()) {
    std::map<llvm::BasicBlock *, unsigned> actual;
    for (unsigned n = 0; n < phi.getNumIncomingValues(); ++n) ++actual[phi.getIncomingBlock(n)];
    CHECK(actual == predecessors);
  }
  auto *branch = llvm::cast<llvm::BranchInst>(fixture.block("bounds.transition")->getTerminator());
  CHECK(branch->getSuccessor(1) == fixture.block("miss"));
}
}

TEST_CASE("STORE expression closed Boolean shapes admit historical self and mutual PHIs", "[llvm][guarded-store-expressions][guarded-boolean-shape-cycles]") {
  for (const char *mode : {"self", "mutual", "mutual-dupe", "freeze-arm"}) for (bool reverse : {false, true}) {
    INFO(mode << " " << reverse); ExpressionFixture fixture(32, "partial-missing"); addHistoricalShape(fixture, mode, reverse);
    auto result = fixture.run(); REQUIRE(result.changed); CHECK(result.counters.at("partial_rewritten_edges") == 1);
    checkHistoricalShape(fixture, std::string(mode) == "mutual-dupe");
  }
}

TEST_CASE("STORE expression closed shapes reject late invalid arms without cache publication", "[llvm][guarded-store-expressions][guarded-boolean-shape-cycles]") {
  for (const char *mode : {"poison-arm", "undef-arm", "poison-operand", "undef-operand", "call-arm", "volatile-arm", "atomic-arm", "nondom-funnel", "unsupported-arm", "integer-cycle", "cache-cyclic", "cache-failed"})
    for (bool reverse : {false, true}) {
      INFO(mode << " " << reverse); ExpressionFixture fixture(32, "partial-missing"); addHistoricalShape(fixture, mode, reverse);
      auto before = fixture.text(); auto result = fixture.run(); CHECK_FALSE(result.changed); CHECK(fixture.text() == before);
    }
  ExpressionFixture acyclic(32, "partial-missing"); addHistoricalShape(acyclic, "cache-acyclic");
  auto result = acyclic.run(); REQUIRE(result.changed); CHECK(result.counters.at("partial_rewritten_edges") == 1);
  auto fast = acyclic.fast(); REQUIRE(fast.size() == 1); REQUIRE(fast.front()->getNumCases() == 1);
  CHECK(fast.front()->case_begin()->getCaseValue()->equalsInt(100));
  auto *fallback = llvm::dyn_cast<llvm::SwitchInst>(fast.front()->getDefaultDest()->getTerminator()); REQUIRE(fallback);
  CHECK(fallback->getCondition() == fast.front()->getCondition()); CHECK(fallback->getNumCases() == 4);
  CHECK(acyclic.instruction("history.second.transition") != nullptr);
  CHECK(acyclic.instruction("history.condition.transition") == nullptr);
}

TEST_CASE("STORE expression closed shapes preserve caller depth and shared node budgets", "[llvm][guarded-store-expressions][guarded-boolean-shape-cycles]") {
  // stored Select contributes one existing Enumerator node. The root starts
  // at depth1; 28 add-zero nodes leave the deepest new constant at depth32.
  ExpressionFixture depth32(32, "partial-missing"); addHistoricalShape(depth32, "self", false, 28);
  REQUIRE(depth32.run().changed); checkHistoricalShape(depth32);
  ExpressionFixture depth33(32, "partial-missing"); addHistoricalShape(depth33, "self", false, 29);
  auto before = depth33.text(); auto exhausted = depth33.run(); CHECK_FALSE(exhausted.changed); CHECK(depth33.text() == before);
  CHECK(exhausted.counters.at("budget_exhausted") > 0);
  // P/next/arg/one=4, zext=1, 59 leaves+59 adds=118, ICmp+bound=2:
  // 125 shape nodes + outer Select + its two finite arms = exactly128.
  ExpressionFixture nodes128(32, "partial-missing"); addHistoricalShape(nodes128, "self", false, 0, 59);
  REQUIRE(nodes128.run().changed); checkHistoricalShape(nodes128);
  ExpressionFixture nodes129(32, "partial-missing"); addHistoricalShape(nodes129, "self", false, 0, 59, true);
  before = nodes129.text(); exhausted = nodes129.run(); CHECK_FALSE(exhausted.changed); CHECK(nodes129.text() == before);
  CHECK(exhausted.counters.at("budget_exhausted") > 0);
}

TEST_CASE("STORE expression closed candidates keep the complete legacy prefix first", "[llvm][guarded-store-expressions][guarded-boolean-shape-cycles][guarded-boolean-shape-scheduling]") {
  for (Options limits : {Options{{"max_rewrites", std::int64_t(1)}},
                         Options{{"max_blocks", std::int64_t(4)}, {"max_rewrites", std::int64_t(32)}},
                         Options{{"max_candidates", std::int64_t(1)}, {"max_rewrites", std::int64_t(32)}}}) {
    ExpressionFixture fixture(32, "partial-missing"); addHistoricalShape(fixture, "self");
    auto &context = fixture.module.getContext();
    auto *late = llvm::BasicBlock::Create(context, "late.ordinary", fixture.function);
    auto *entry = llvm::cast<llvm::BranchInst>(fixture.block("entry")->getTerminator());
    entry->setSuccessor(1, late);
    Builder b(late);
    auto *state = b.CreateSelect(fixture.function->getArg(3), b.getInt32(138502176), b.getInt32(138502170), "late.state");
    // Same finite enc(0)/enc(6) pair as the early cyclic observation, but this
    // ordinary context was already admitted before the closed-graph extension.
    auto *pointer = b.CreateGEP(b.getInt8Ty(), fixture.instruction("guest"), b.getInt64(256));
    auto *store = b.CreateStore(state, pointer); store->setAlignment(llvm::Align(1));
    b.CreateBr(fixture.block("head")); fixture.verify();
    auto result = fixture.run("guarded-store-expressions", limits); REQUIRE(result.changed);
    CHECK(result.counters.at("rewritten_edges") == 1);
    auto *ordinary = llvm::cast<llvm::BranchInst>(fixture.block("late.ordinary")->getTerminator());
    auto *cyclic = llvm::cast<llvm::BranchInst>(fixture.block("seed")->getTerminator());
    CHECK(ordinary->getSuccessor(0) == fixture.block("head.transition"));
    CHECK(cyclic->getSuccessor(0) == fixture.block("head"));
    fixture.check({100});
    CHECK(fixture.block("history.loop.transition") == nullptr);
  }
}
