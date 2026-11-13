#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
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
constexpr const char *Checked = "dragon.guarded-checked-corridors.generated";

// Domains below come from the integer definition of LSR/ASR, independently
// of the controller. The source remains unknown and predictions are hints.
struct SliceFixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module = lifter.module();
  llvm::Function *function;
  std::set<std::string> originals;
  std::string mode;
  unsigned width, bits;
  bool arithmetic;
  SliceFixture(unsigned w = 64, unsigned kept = 1, bool sign = false,
               std::string kind = "ordinary", int amount = -1)
      : mode(std::move(kind)), width(w), bits(kept), arithmetic(sign) {
    module.setDataLayout("e-p:64:64");
    auto &c = module.getContext();
    auto *ptr = llvm::PointerType::getUnqual(c);
    auto *i64 = llvm::Type::getInt64Ty(c);
    auto *word = llvm::IntegerType::get(c, width);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {ptr, ptr, word, llvm::Type::getInt1Ty(c), llvm::Type::getInt1Ty(c)}, false),
        llvm::GlobalValue::ExternalLinkage, "view", module);
    auto block = [&](const char *name) {
      originals.insert(name); return llvm::BasicBlock::Create(c, name, function);
    };
    auto *entry = block("entry"), *seed = block("seed"), *other = block("other");
    auto *head = block("head"), *dispatch = block("dispatch");
    auto *hit = block("hit"), *miss = block("miss"), *bounds = block("bounds");
    Builder b(entry);
    auto read = [&](llvm::Type *ty, llvm::Value *address, const char *name) {
      auto *v = b.CreateLoad(ty, address, name); v->setAlignment(llvm::Align(1)); return v;
    };
    auto *guest = read(ptr, function->getArg(1), "guest");
    auto external = module.getOrInsertFunction("external_maywrite",
        llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr}, false));
    b.CreateCall(external, {function->getArg(0), guest});
    auto *size = read(i64, b.CreateGEP(b.getInt8Ty(), function->getArg(1), b.getInt64(8)), "size");
    b.CreateCondBr(function->getArg(4), seed, other);
    entry->getTerminator()->setMetadata(Generated, llvm::MDNode::get(c, {}));
    b.SetInsertPoint(seed);
    llvm::Value *raw = read(word, function->getArg(0), "source.raw");
    if (mode == "undef") raw = llvm::UndefValue::get(word);
    if (mode == "poison") raw = llvm::PoisonValue::get(word);
    if (mode == "poison-operand") raw = b.CreateAdd(raw, llvm::PoisonValue::get(word), "poison.operand");
    if (mode == "freeze-undef") raw = b.CreateFreeze(llvm::UndefValue::get(word), "defined.source");
    if (mode == "freeze-poison") raw = b.CreateFreeze(llvm::PoisonValue::get(word), "defined.source");
    if (mode == "constant") raw = llvm::ConstantInt::get(word, 3);
    if (mode == "constant-negative") raw = llvm::ConstantInt::get(word, llvm::APInt::getAllOnes(width));
    if (mode == "constant-signbit") raw = llvm::ConstantInt::get(word, llvm::APInt::getSignedMinValue(width));
    if (mode == "constant-signed-max") raw = llvm::ConstantInt::get(word, llvm::APInt::getSignedMaxValue(width));
    const unsigned count = amount < 0 ? width - bits : static_cast<unsigned>(amount);
    llvm::Value *shiftCount = llvm::ConstantInt::get(word, count);
    if (mode == "unknown-count") shiftCount = function->getArg(2);
    if (mode == "negative-count") shiftCount = llvm::ConstantInt::get(word, llvm::APInt::getAllOnes(width));
    if (mode == "large-count") {
      llvm::APInt countBits(width, 0); countBits.setBit(width == 128 ? 96 : width - 1);
      shiftCount = llvm::ConstantInt::get(word, countBits);
    }
    if (mode == "mixed-wide-count") shiftCount = b.CreateSelect(function->getArg(3),
        llvm::ConstantInt::get(word, width - 1), llvm::ConstantInt::get(word, 0), "mixed.count");
    if (mode == "mixed-invalid-count") shiftCount = b.CreateSelect(function->getArg(3),
        llvm::ConstantInt::get(word, width - 1), llvm::ConstantInt::get(word, width), "mixed.count");
    if (mode == "mixed-valid-count") shiftCount = b.CreateSelect(function->getArg(3),
        llvm::ConstantInt::get(word, width - 1), llvm::ConstantInt::get(word, width - 2), "mixed.count");
    llvm::Value *shift = arithmetic ? b.CreateAShr(raw, shiftCount, "source.shift")
                                   : b.CreateLShr(raw, shiftCount, "source.shift");
    if (mode == "exact" || mode == "freeze-after-exact")
      llvm::cast<llvm::BinaryOperator>(shift)->setIsExact();
    if (mode == "freeze-after-exact") shift = b.CreateFreeze(shift, "frozen.shift");
    llvm::Value *wide = width < 64 ? (arithmetic ? b.CreateSExt(shift, i64, "source.wide")
                                                               : b.CreateZExt(shift, i64, "source.wide"))
                                  : width > 64 ? b.CreateTrunc(shift, i64, "source.wide") : shift;
    llvm::Value *chosen = b.CreateAdd(wide, b.getInt64(8), "choose.state");
    if (mode == "legacy-finite") chosen = b.CreateSelect(function->getArg(3), b.getInt64(8), b.getInt64(9), "legacy.state");
    b.CreateBr(head);
    if (mode == "generated" || mode == "checked" || mode == "legacy-finite")
      seed->getTerminator()->setMetadata(Generated, llvm::MDNode::get(c, {}));
    if (mode == "checked") seed->getTerminator()->setMetadata(Checked, llvm::MDNode::get(c, {}));
    b.SetInsertPoint(other); b.CreateBr(head);
    other->getTerminator()->setMetadata(Generated, llvm::MDNode::get(c, {}));
    b.SetInsertPoint(head);
    auto *state = b.CreatePHI(i64, 2, "state");
    state->addIncoming(chosen, seed); state->addIncoming(b.getInt64(10000), other);
    auto *retained = llvm::cast<llvm::BinaryOperator>(b.CreateAdd(state, b.getInt64(0), "retained.flags"));
    retained->setHasNoSignedWrap(); retained->setHasNoUnsignedWrap();
    if (mode == "call-inside") b.CreateCall(external, {function->getArg(0), guest});
    auto *index = b.CreateAdd(b.getInt64(1024), b.CreateShl(retained, b.getInt64(3)), "table.index");
    auto *range = b.CreateAnd(b.CreateICmpUGT(size, b.getInt64(7)),
        b.CreateICmpULE(index, b.CreateSub(size, b.getInt64(8))));
    b.CreateCondBr(range, dispatch, bounds);
    b.SetInsertPoint(dispatch);
    auto *address = b.CreateGEP(b.getInt8Ty(), guest, index, "table.address");
    if (mode == "table-before") b.CreateStore(function->getArg(2), address)->setAlignment(llvm::Align(1));
    auto *actual = read(i64, address, "actual.target");
    if (mode == "table-after") b.CreateStore(function->getArg(2), address)->setAlignment(llvm::Align(1));
    auto *sw = b.CreateSwitch(actual, miss);
    for (unsigned n = 0; n < 8; ++n) sw->addCase(b.getInt64(100 + n), hit);
    sw->addCase(b.getInt64(999), miss);
    b.SetInsertPoint(hit);
    auto *out = b.CreatePHI(i64, 8, "result");
    for (unsigned n = 0; n < 8; ++n) out->addIncoming(actual, dispatch);
    b.CreateStore(out, function->getArg(0))->setAlignment(llvm::Align(1)); b.CreateRet(out);
    b.SetInsertPoint(miss); b.CreateRet(actual);
    b.SetInsertPoint(bounds); b.CreateRet(b.getInt64(-1));
    // Supply all eight independent candidate slots, including intentionally
    // wrong but existing target 999. No entry assertion makes them true.
    std::string manifest = "{\"view_symbol\":\"view\",\"memory\":[";
    for (unsigned n = 0; n < 8; ++n) {
      if (n) manifest += ',';
      const std::uint64_t value = arithmetic && n >= 4 ? n - 8ull : n;
      const auto stateValue = std::uint64_t(8) + value;
      std::string offset, bytes;
      llvm::raw_string_ostream stream(offset); stream << llvm::format_hex(1024 + stateValue * 8, 0);
      const char *hex = "0123456789abcdef";
      const auto target = mode == "wrong-hint" && n == 1 ? 999u : 100u + n;
      for (unsigned j = 0; j < 8; ++j) { auto v = (std::uint64_t(target) >> (8 * j)) & 255; bytes += hex[v >> 4]; bytes += hex[v & 15]; }
      manifest += "{\"offset\":\"" + offset + "\",\"bytes\":\"" + bytes + "\"}";
    }
    manifest += "]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(
        llvm::MDNode::get(c, llvm::MDString::get(c, manifest)));
    verify();
  }
  void verify() { REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs())); }
  std::string text() { std::string s; llvm::raw_string_ostream out(s); module.print(out, nullptr); return s; }
  llvm::Instruction *instruction(llvm::StringRef name) {
    for (auto &b : *function) for (auto &i : b) if (i.getName() == name) return &i;
    return nullptr;
  }
  Result run(const char *name = "guarded-bit-slices", Options options = {}) {
    options.try_emplace("max_rewrites", std::int64_t(1));
    PassPipeline p; REQUIRE(p.add(name, options));
    RunOptions opts; opts.maxIterations = 1; opts.maxPassRuns = 1; opts.verifyLLVM = true;
    auto report = p.run(lifter, opts); REQUIRE(report.ok); REQUIRE(report.runs.size() == 1);
    verify(); return report.runs.front().result;
  }
  void checkPhiEdges() {
    for (auto &b : *function) {
      std::map<llvm::BasicBlock *, unsigned> edges;
      for (auto &pred : *function) for (auto *succ : llvm::successors(&pred)) if (succ == &b) ++edges[&pred];
      for (auto &phi : b.phis()) {
        std::map<llvm::BasicBlock *, unsigned> incoming;
        for (unsigned n = 0; n < phi.getNumIncomingValues(); ++n) ++incoming[phi.getIncomingBlock(n)];
        CHECK(incoming == edges);
      }
    }
  }
  llvm::SwitchInst *check(const std::set<std::uint64_t> &expected) {
    llvm::SwitchInst *fast = nullptr;
    for (auto &b : *function) if (!originals.contains(b.getName().str()))
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator()))
        if (auto *read = llvm::dyn_cast<llvm::LoadInst>(sw->getCondition()); read && read->getParent() == &b) {
          REQUIRE(fast == nullptr); fast = sw;
        }
    REQUIRE(fast);
    std::set<std::uint64_t> values;
    for (auto item : fast->cases()) values.insert(item.getCaseValue()->getZExtValue());
    CHECK(values == expected);
    auto *fallback = llvm::dyn_cast<llvm::SwitchInst>(fast->getDefaultDest()->getTerminator());
    REQUIRE(fallback); CHECK(fallback->getCondition() == fast->getCondition());
    CHECK(fallback->getNumCases() == 9); CHECK(fallback->getDefaultDest()->getName() == "miss");
    for (auto item : fallback->cases())
      CHECK(item.getCaseSuccessor()->getName() == (item.getCaseValue()->getZExtValue() == 999 ? "miss" : "hit"));
    for (auto &i : *fallback->getParent()) CHECK_FALSE(llvm::isa<llvm::LoadInst>(i));
    CHECK(instruction("source.shift")); CHECK(instruction("source.raw"));
    auto *flags = llvm::dyn_cast_or_null<llvm::BinaryOperator>(instruction("retained.flags.transition"));
    REQUIRE(flags); CHECK(flags->hasNoSignedWrap()); CHECK(flags->hasNoUnsignedWrap());
    checkPhiEdges(); return fast;
  }
};
std::set<std::uint64_t> targets(unsigned kept, bool sign) {
  std::set<std::uint64_t> result;
  for (unsigned n = 0; n < (1u << kept); ++n) {
    const int value = sign && n >= (1u << (kept - 1)) ? int(n) - int(1u << kept) : int(n);
    const unsigned hintIndex = value < 0 ? unsigned(8 + value) : unsigned(value);
    result.insert(100 + hintIndex);
  }
  return result;
}
}

TEST_CASE("Bit slices predict all high one to three bit LSR and ASR patterns", "[llvm][guarded-bit-slices]") {
  for (unsigned width : {32u, 64u, 128u}) for (unsigned bits : {1u, 2u, 3u}) for (bool sign : {false, true}) {
    INFO(width << " " << bits << " " << sign);
    SliceFixture f(width, bits, sign); auto *function = f.function; auto *arg = function->getArg(0);
    auto *metadata = f.module.getNamedMetadata("dragon.guarded-entry"); auto r = f.run(); REQUIRE(r.changed);
    CHECK(r.counters.at("rewritten_edges") == 1); CHECK(r.status == Status::Incomplete);
    f.check(targets(bits, sign)); CHECK(f.module.getFunction("view") == function);
    CHECK(function->getArg(0) == arg); CHECK(f.module.getNamedMetadata("dragon.guarded-entry") == metadata);
  }
}

TEST_CASE("Bit slice boundaries retain zero count constants and refuse invalid or broad counts", "[llvm][guarded-bit-slices]") {
  for (unsigned width : {32u, 64u, 128u}) for (bool sign : {false, true}) {
    INFO(width << " " << sign);
    SliceFixture zero(width, 1, sign, "constant", 0); REQUIRE(zero.run().changed); zero.check({103});
    SliceFixture last(width, 1, sign); REQUIRE(last.run().changed); last.check(targets(1, sign));
    SliceFixture mixed(width, 2, sign, "mixed-valid-count"); REQUIRE(mixed.run().changed); mixed.check(targets(2, sign));
    for (const auto &mode : {std::string("ordinary"), std::string("constant")})
      for (int amount : {int(width), int(width + 1)}) {
        SliceFixture bad(width, 1, sign, mode, amount); auto before = bad.text();
        CHECK_FALSE(bad.run().changed); CHECK(bad.text() == before);
      }
    for (const char *mode : {"mixed-invalid-count", "unknown-count"}) {
      SliceFixture bad(width, 1, sign, mode); auto before = bad.text();
      CHECK_FALSE(bad.run().changed); CHECK(bad.text() == before);
    }
    SliceFixture unshifted(width, 1, sign, "ordinary", 0); auto before = unshifted.text();
    CHECK_FALSE(unshifted.run().changed); CHECK(unshifted.text() == before);
    SliceFixture broad(width, 4, sign); before = broad.text(); CHECK_FALSE(broad.run().changed); CHECK(broad.text() == before);
  }
}

TEST_CASE("Bit slice hints preserve wrong values table mutation order and actual operations", "[llvm][guarded-bit-slices]") {
  for (const char *mode : {"ordinary", "generated", "wrong-hint", "table-before", "table-after", "exact"}) {
    INFO(mode); SliceFixture f(64, 1, false, mode); auto *shift = f.instruction("source.shift");
    std::string oldShift; llvm::raw_string_ostream stream(oldShift); shift->print(stream);
    REQUIRE(f.run().changed);
    auto *fast = f.check(std::string(mode) == "wrong-hint" ? std::set<std::uint64_t>{100, 999} : targets(1, false));
    std::string actualShift; llvm::raw_string_ostream after(actualShift); f.instruction("source.shift")->print(after);
    CHECK(oldShift == actualShift);
    if (std::string(mode) == "table-before" || std::string(mode) == "table-after") {
      auto *read = llvm::cast<llvm::LoadInst>(fast->getCondition()); bool readSeen = false, wrote = false;
      for (auto &i : *fast->getParent()) {
        readSeen |= &i == read;
        if (auto *write = llvm::dyn_cast<llvm::StoreInst>(&i)) {
          CHECK(write->getPointerOperand() == read->getPointerOperand());
          CHECK(write->getValueOperand() == f.function->getArg(2));
          CHECK(readSeen == (std::string(mode) == "table-after")); wrote = true;
        }
      }
      CHECK(wrote);
    }
    unsigned calls = 0;
    for (auto &b : *f.function) for (auto &i : b) if (auto *call = llvm::dyn_cast<llvm::CallBase>(&i)) {
      ++calls; CHECK(b.getName() == "entry"); CHECK_FALSE(call->doesNotAccessMemory());
    }
    CHECK(calls == 1);
    for (auto &arg : f.function->args()) CHECK_FALSE(arg.hasAttribute(llvm::Attribute::NoAlias));
  }
}

TEST_CASE("Bit slices respect freeze boundaries and reject visible poison calls and checked clones", "[llvm][guarded-bit-slices]") {
  for (const char *mode : {"freeze-undef", "freeze-poison"}) {
    SliceFixture f(64, 1, false, mode); REQUIRE(f.run().changed); f.check(targets(1, false));
    CHECK(f.instruction("defined.source"));
  }
  for (const char *mode : {"undef", "poison", "poison-operand", "call-inside", "checked"}) {
    INFO(mode); SliceFixture f(64, 1, false, mode); auto before = f.text();
    CHECK_FALSE(f.run().changed); CHECK(f.text() == before);
  }
}

TEST_CASE("Bit slice budgets reject atomically and legacy profiles retain source bytes", "[llvm][guarded-bit-slices]") {
  for (const char *key : {"max_candidates", "max_rewrites", "max_blocks", "max_instructions"}) {
    SliceFixture f; auto before = f.text(); auto *function = f.function; auto *argument = function->getArg(0);
    auto *metadata = f.module.getNamedMetadata("dragon.guarded-entry");
    auto r = f.run("guarded-bit-slices", {{key, std::int64_t(0)}});
    CHECK_FALSE(r.changed); CHECK(r.status == Status::Incomplete); CHECK(r.counters.at("budget_exhausted") > 0);
    CHECK(f.text() == before); CHECK(f.module.getFunction("view") == function);
    CHECK(function->getArg(0) == argument); CHECK(f.module.getNamedMetadata("dragon.guarded-entry") == metadata);
  }
  for (const char *name : {"guarded-state-corridors", "guarded-store-corridors", "guarded-partial-corridors", "guarded-partial-masks", "guarded-checked-corridors"}) {
    SliceFixture f(64, 1, false, "generated"); auto before = f.text(); CHECK_FALSE(f.run(name).changed); CHECK(f.text() == before);
  }
  SliceFixture legacy(64, 1, false, "legacy-finite"), next(64, 1, false, "legacy-finite");
  auto a = legacy.run("guarded-checked-corridors"), b = next.run();
  REQUIRE(a.changed); REQUIRE(b.changed); CHECK(legacy.text() == next.text());
  CHECK(a.counters == b.counters); CHECK(a.status == b.status);
  const auto defaults = PassPipeline::defaultLLVM();
  CHECK(std::find(defaults.begin(), defaults.end(), "guarded-bit-slices") == defaults.end());
}

namespace {
struct SliceLoopFixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module = lifter.module();
  llvm::Function *function = nullptr;
  std::set<std::string> originalBlocks;
  std::string mode;

  explicit SliceLoopFixture(std::string kind = "loop") : mode(std::move(kind)) {
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
    auto *initialRaw = load(observationAddress, "seed.raw");
    llvm::Value *chosen = b.CreateLShr(initialRaw, b.getInt64(63), "choose.state");
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
    auto *next = b.CreateLShr(frozen, b.getInt64(63), "next.state");
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
  Result run(const char *name = "guarded-bit-slices", Options options = {}) {
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

TEST_CASE("Bit slice loop contexts use each fresh read and all duplicate backedge PHIs", "[llvm][guarded-bit-slices]") {
  for (const char *mode : {"loop", "dupe-loop", "switch-seed"}) {
    INFO(mode); SliceLoopFixture f(mode); auto r = f.run(); REQUIRE(r.changed);
    CHECK(r.counters.at("rewritten_edges") == 1); CHECK(r.counters.at("loop_contexts") == 1);
    const unsigned backedges = std::string(mode) == "dupe-loop" ? 2 : 1;
    CHECK(r.counters.at("loop_backedges") == backedges);
    auto *fast = f.freshSwitch(); f.checkFallback(*fast); auto *root = f.block("head.transition"); REQUIRE(root);
    auto *state = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("state.transition")); REQUIRE(state);
    auto *history = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("history.transition")); REQUIRE(history);
    CHECK(state->getParent() == root); CHECK(state->getNumIncomingValues() == 1 + backedges);
    auto *freshShift = llvm::dyn_cast_or_null<llvm::BinaryOperator>(f.instruction("next.state.transition")); REQUIRE(freshShift);
    CHECK(freshShift->getOpcode() == llvm::Instruction::LShr);
    CHECK(freshShift->getOperand(0) == f.instruction("fresh.freeze.transition"));
    CHECK(freshShift->getOperand(0) != f.instruction("seed.raw"));
    unsigned fresh = 0, initial = 0;
    for (unsigned n = 0; n < state->getNumIncomingValues(); ++n) {
      if (state->getIncomingBlock(n) == fast->getParent()) {
        ++fresh; CHECK(state->getIncomingValue(n) == freshShift);
        CHECK(history->getIncomingValueForBlock(fast->getParent()) == f.instruction("fresh.freeze.transition"));
      } else { ++initial; CHECK(state->getIncomingValue(n) == f.instruction("choose.state")); }
    }
    CHECK(fresh == backedges); CHECK(initial == 1);
    for (auto item : fast->cases()) if (item.getCaseValue()->getZExtValue() < 200) CHECK(item.getCaseSuccessor() == root);
    CHECK(f.instruction("prefix.increment.transition") == nullptr);
    CHECK(f.instruction("iteration.increment.transition") != nullptr);
    f.checkEveryPhi();
  }
}

TEST_CASE("Bit slice histories keep previous observations separate from this iteration", "[llvm][guarded-bit-slices]") {
  SliceLoopFixture f("historical"); auto r = f.run(); REQUIRE(r.changed);
  CHECK(r.counters.at("loop_contexts") == 1); CHECK(r.counters.at("loop_backedges") == 1);
  auto *fast = f.freshSwitch(); f.checkFallback(*fast);
  auto *snapshotBlock = f.block("transition.inputs"); REQUIRE(snapshotBlock);
  auto *snapshot = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("transition.old")); REQUIRE(snapshot);
  CHECK(snapshot->getParent() == snapshotBlock); REQUIRE(snapshot->getNumIncomingValues() == 1);
  CHECK(snapshot->getIncomingBlock(0) == f.block("seed"));
  auto *history = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("history.transition")); REQUIRE(history);
  CHECK(history->getIncomingValueForBlock(snapshotBlock) == snapshot);
  CHECK(history->getIncomingValueForBlock(fast->getParent()) == f.instruction("fresh.freeze.transition"));
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
  auto *next = llvm::dyn_cast_or_null<llvm::BinaryOperator>(f.instruction("next.state.transition")); REQUIRE(next);
  CHECK(next->getOperand(0) == f.instruction("fresh.freeze.transition"));
  CHECK(next->getOperand(0) != snapshot);
  f.checkEveryPhi();
}


TEST_CASE("Bit slice edge freezes invalid huge counts and mixed broad counts stay conservative", "[llvm][guarded-bit-slices][guarded-bit-slice-edges]") {
  for (unsigned width : {32u, 64u, 128u}) for (bool sign : {false, true}) {
    INFO(width << " " << sign);
    // An exact shift may be poison; freezing its result can select any word.
    // Moving the small result-bit domain through that freeze would be unsound.
    SliceFixture frozen(width, 1, sign, "freeze-after-exact");
    auto *exact = llvm::dyn_cast_or_null<llvm::BinaryOperator>(frozen.instruction("source.shift"));
    auto *freeze = llvm::dyn_cast_or_null<llvm::FreezeInst>(frozen.instruction("frozen.shift"));
    REQUIRE(exact); REQUIRE(exact->isExact()); REQUIRE(freeze);
    CHECK(freeze->getOperand(0) == exact);
    auto before = frozen.text(); CHECK_FALSE(frozen.run().changed); CHECK(frozen.text() == before);
    for (const char *mode : {"negative-count", "large-count"}) {
      SliceFixture bad(width, 1, sign, mode); before = bad.text();
      // The i128 large count is 1<<96: it must be rejected before narrowing
      // an APInt count to uint64_t, even in an assertions-enabled build.
      CHECK_FALSE(bad.run().changed); CHECK(bad.text() == before);
    }
    // The count-zero arm has a whole-word domain. Only the count-(W-1)
    // results become hints, and every other outcome keeps the full fallback.
    SliceFixture mixed(width, 1, sign, "mixed-wide-count");
    auto *count = mixed.instruction("mixed.count"); REQUIRE(count);
    auto *shift = llvm::dyn_cast_or_null<llvm::BinaryOperator>(mixed.instruction("source.shift"));
    REQUIRE(shift); CHECK(shift->getOperand(1) == count);
    REQUIRE(mixed.run().changed); mixed.check(targets(1, sign));
    auto *freshCount = mixed.instruction("mixed.count"); REQUIRE(freshCount);
    auto *retainedShift = llvm::dyn_cast_or_null<llvm::BinaryOperator>(mixed.instruction("source.shift"));
    REQUIRE(retainedShift); CHECK(retainedShift->getOperand(1) == freshCount);
  }
}

TEST_CASE("Bit slice edge constant negative and sign bit sources use architectural signedness", "[llvm][guarded-bit-slices][guarded-bit-slice-edges]") {
  for (unsigned width : {32u, 64u, 128u}) for (bool sign : {false, true}) {
    INFO(width << " " << sign);
    for (const char *mode : {"constant-negative", "constant-signbit", "constant-signed-max"}) {
      INFO(mode); SliceFixture f(width, 1, sign, mode);
      REQUIRE(f.run().changed);
      // At W-1, all-ones and the sign bit produce 1 with LSR and -1
      // with ASR; the largest signed positive word produces zero.
      const std::uint64_t target = std::string(mode) == "constant-signed-max" ? 100 : sign ? 107 : 101;
      f.check({target});
    }
    SliceFixture zero(width, 1, sign, "constant-negative", 0);
    if (sign) { REQUIRE(zero.run().changed); zero.check({107}); }
    else {
      // Logical count zero preserves all bits. Its computed address has no
      // supplied manifest row, so there is no admitted target prediction.
      auto before = zero.text(); CHECK_FALSE(zero.run().changed); CHECK(zero.text() == before);
    }
  }
}

TEST_CASE("Bit slice edge ordinary loop seeds preserve fresh values and refuse own clone markers", "[llvm][guarded-bit-slices][guarded-bit-slice-edges]") {
  SliceLoopFixture f("unmarked");
  REQUIRE_FALSE(f.block("seed")->getTerminator()->getMetadata(Generated));
  REQUIRE_FALSE(f.block("seed")->getTerminator()->getMetadata(Checked));
  auto r = f.run(); REQUIRE(r.changed);
  CHECK(r.counters.at("rewritten_edges") == 1); CHECK(r.counters.at("loop_contexts") == 1);
  CHECK(r.counters.at("loop_backedges") == 1);
  auto *fast = f.freshSwitch(); f.checkFallback(*fast);
  auto *root = f.block("head.transition"); REQUIRE(root);
  auto *state = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("state.transition")); REQUIRE(state);
  auto *history = llvm::dyn_cast_or_null<llvm::PHINode>(f.instruction("history.transition")); REQUIRE(history);
  REQUIRE(state->getNumIncomingValues() == 2); REQUIRE(history->getNumIncomingValues() == 2);
  CHECK(state->getIncomingValueForBlock(f.block("seed")) == f.instruction("choose.state"));
  CHECK(state->getIncomingValueForBlock(fast->getParent()) == f.instruction("next.state.transition"));
  CHECK(history->getIncomingValueForBlock(fast->getParent()) == f.instruction("fresh.freeze.transition"));
  CHECK(f.instruction("prefix.increment.transition") == nullptr);
  CHECK(f.instruction("iteration.increment.transition") != nullptr);
  for (auto c : fast->cases()) if (c.getCaseValue()->getZExtValue() < 200) CHECK(c.getCaseSuccessor() == root);
  for (auto &block : *f.function) if (!f.originalBlocks.contains(block.getName().str())) {
    CHECK(block.getTerminator()->getMetadata(Generated));
    CHECK(block.getTerminator()->getMetadata(Checked));
  }
  f.checkEveryPhi();
  // The same eligible loop becomes ineligible when its seed has both own
  // clone markers. The positive above proves the graph itself can rewrite.
  SliceLoopFixture marked("unmarked");
  auto &context = marked.module.getContext();
  marked.block("seed")->getTerminator()->setMetadata(Generated, llvm::MDNode::get(context, {}));
  marked.block("seed")->getTerminator()->setMetadata(Checked, llvm::MDNode::get(context, {}));
  auto before = marked.text(); CHECK_FALSE(marked.run().changed); CHECK(marked.text() == before);
}
