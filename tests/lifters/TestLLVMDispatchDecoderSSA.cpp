#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/DispatchDecoderSSA.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdlib>
#include <memory>
#include <string>

using namespace dragon::llvmlifter::detail;
namespace {
// New, unregistered private API: the old product cannot compile this TU. The
// original expression is executable but unnormalized; no old wrong-result claim.
struct Fixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit Fixture(const std::string &body) {
    llvm::SMDiagnostic error;
    module = llvm::parseAssemblyString("declare i32 @unknown(i32)\n" + body, error, context);
    std::string message; llvm::raw_string_ostream stream(message); error.print("decoder-test", stream);
    INFO(message); REQUIRE(module); verify();
  }
  llvm::Function &function() { return *module->getFunction("f"); }
  llvm::SwitchInst &selected() {
    for (auto &b : function()) if (auto *s = llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator())) return *s;
    std::abort();
  }
  std::string text() {
    std::string out; llvm::raw_string_ostream stream(out); module->print(stream, nullptr); return out;
  }
  void verify() {
    std::string error; llvm::raw_string_ostream stream(error);
    const bool invalid = llvm::verifyModule(*module, &stream); INFO(error); REQUIRE_FALSE(invalid);
  }
  DispatchDecoderSSAStats run(const DispatchDecoderSSAOptions &options = {}) {
    return normalizeDispatchDecoder(function(), selected(), options);
  }
  DispatchDecoderSSAStats consume(const DispatchDecoderSSAOptions &options = {}) {
#ifdef DRAGON_DECODER_BEFORE_V1
    // Explicit before adapter: v1 has no all-use entry; exercise its ORIGINAL
    // switch-only API on the exact same module. A failed identity/precision
    // assertion is not an old wrong-execution claim.
    return normalizeDispatchDecoder(function(), selected(), options);
#else
    return normalizeDispatchDecoderUses(function(), selected(), options);
#endif
  }
  void reject(const DispatchDecoderSSAOptions &options = {}, bool budget = false) {
    auto before = text(); auto result = run(options); INFO(result.message);
    CHECK_FALSE(result.changed); CHECK(result.refused); CHECK(result.budgetExhausted == budget);
    CHECK(text() == before); verify();
  }
};
std::string diamond(const std::string &leaf, const std::string &value = "%encoded",
                    const std::string &late = "138502176") {
  return R"IR(define i32 @f(i32 %raw, i32 %other, i1 %c, ptr %guest) {
entry:
  br i1 %c, label %a, label %b
a:
)IR" + leaf + R"IR(
  store i32 )IR" + value + R"IR(, ptr %guest, align 1
  br label %join
b:
  br label %join
join:
  %encoded.phi = phi i32 [)IR" + value + R"IR(, %a], [)IR" + late + R"IR(, %b]
  %added = add i32 %encoded.phi, 1640531527
  %decoded = xor i32 %added, 1779033703
  switch i32 %decoded, label %error [i32 0, label %zero i32 2, label %two i32 3, label %three]
zero: ret i32 0
two: ret i32 2
three: ret i32 3
error: ret i32 %decoded
})IR";
}
const std::string finite = R"IR(  %atom = and i32 %raw, 1
  %encoded = add nuw nsw i32 %atom, 138502173)IR";
llvm::PHINode *decodedPhi(Fixture &f) {
  return llvm::dyn_cast<llvm::PHINode>(f.selected().getCondition());
}
} // namespace

TEST_CASE("Private decoder distributes finite bit leaves and preserves liveouts", "[dispatch-decoder-ssa]") {
  Fixture fixture(diamond(finite));
  auto *old = fixture.selected().getCondition();
  auto result = fixture.run(); INFO(result.message); REQUIRE(result.changed);
  CHECK_FALSE(result.refused); CHECK_FALSE(result.budgetExhausted); fixture.verify();
  auto *phi = decodedPhi(fixture); REQUIRE(phi); REQUIRE(phi->getNumIncomingValues() == 2);
  auto *select = llvm::dyn_cast<llvm::SelectInst>(phi->getIncomingValue(0)); REQUIRE(select);
  CHECK(llvm::cast<llvm::ConstantInt>(select->getTrueValue())->getZExtValue() == 2);
  CHECK(llvm::cast<llvm::ConstantInt>(select->getFalseValue())->getZExtValue() == 3);
  CHECK(llvm::cast<llvm::ConstantInt>(phi->getIncomingValue(1))->isZero());
  auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(select->getCondition()); REQUIRE(cmp);
  CHECK(cmp->getOperand(0)->getName() == "atom");
  CHECK_FALSE(old->use_empty()); // error return still uses original decoder.
  unsigned stores = 0;
  for (auto &b : fixture.function()) for (auto &i : b) if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&i)) {
    ++stores; CHECK(s->getValueOperand()->getName() == "encoded");
  }
  CHECK(stores == 1);
}

TEST_CASE("Private decoder evaluates narrow encoder flags over the full bit domain", "[dispatch-decoder-ssa]") {
  Fixture fixture(diamond(R"IR(  %atom = and i8 1, 1
  %raw8 = trunc i32 %raw to i8
  %bit = and i8 %raw8, 1
  %flip = xor i8 %bit, 1
  %times = mul nuw nsw i8 %flip, 5
  %wide = zext nneg i8 %times to i32
  %encoded = add nuw nsw i32 %wide, 138502170)IR"));
  auto result = fixture.run(); INFO(result.message); REQUIRE(result.changed); fixture.verify();
  auto *select = llvm::cast<llvm::SelectInst>(decodedPhi(fixture)->getIncomingValue(0));
  CHECK(llvm::cast<llvm::ConstantInt>(select->getTrueValue())->getZExtValue() == 6);
  CHECK(llvm::cast<llvm::ConstantInt>(select->getFalseValue())->getZExtValue() == 1);
}

TEST_CASE("Private decoder handles complete select and duplicate predecessor slots", "[dispatch-decoder-ssa]") {
  SECTION("existing i1 and equal decoded values retain observation") {
    Fixture fixture(diamond("  %encoded = select i1 %c, i32 138502173, i32 138502173"));
    auto result = fixture.run(); REQUIRE(result.changed); fixture.verify();
    auto *select = llvm::dyn_cast<llvm::SelectInst>(decodedPhi(fixture)->getIncomingValue(0));
    REQUIRE(select); CHECK(select->getCondition() == fixture.function().getArg(2));
    CHECK(select->getTrueValue() == select->getFalseValue());
  }
  SECTION("duplicate slots") {
    Fixture fixture(R"IR(define i32 @f(i32 %raw, i32 %other, i1 %c, ptr %guest) {
entry: br i1 %c, label %join, label %join
join:
  %phi = phi i32 [138502176, %entry], [138502176, %entry]
  %a = add i32 %phi, 1640531527
  %d = xor i32 %a, 1779033703
  switch i32 %d, label %error [i32 0, label %zero]
zero: ret i32 0
error: ret i32 %d
})IR");
    REQUIRE(fixture.run().changed); fixture.verify();
    CHECK(decodedPhi(fixture)->getNumIncomingValues() == 2);
    CHECK(decodedPhi(fixture)->getIncomingBlock(0) == decodedPhi(fixture)->getIncomingBlock(1));
  }
  SECTION("direct existing i1 arithmetic observation") {
    Fixture fixture(diamond("  %v = zext i1 %c to i32\n  %encoded = add i32 %v, 138502173"));
    REQUIRE(fixture.run().changed); fixture.verify();
    auto *select = llvm::cast<llvm::SelectInst>(decodedPhi(fixture)->getIncomingValue(0));
    CHECK(select->getCondition() == fixture.function().getArg(2));
  }
}

TEST_CASE("Private decoder rejects late unsafe arms without publishing partial SSA", "[dispatch-decoder-ssa]") {
  for (const auto *bad : {"poison", "undef"}) {
    INFO(bad); Fixture fixture(diamond(finite, "%encoded", bad)); fixture.reject();
  }
  for (const std::string &leaf : {
      "  %encoded = add i32 poison, 138502173",
      "  %encoded = add i32 undef, 138502173",
      "  %atom = and i32 poison, 1\n  %encoded = add i32 %atom, 138502173",
      "  %atom = and i32 %raw, 1\n  %encoded = add i32 %atom, poison",
      "  %atom = and i32 %raw, 1\n  %second = and i32 %other, 1\n  %encoded = add i32 %atom, %second",
      "  %encoded = call i32 @unknown(i32 %raw)",
      "  %encoded = load volatile i32, ptr %guest, align 1",
      "  %encoded = load atomic i32, ptr %guest monotonic, align 4",
      "  %encoded = freeze i32 %raw",
      "  %atom = and i32 %raw, 1\n  %encoded = udiv i32 %atom, 3"}) {
    INFO(leaf); Fixture fixture(diamond(leaf)); fixture.reject();
  }
}

TEST_CASE("Private decoder rejects unsafe exhaustive flag and shift domains", "[dispatch-decoder-ssa]") {
  for (const std::string &leaf : {
      "  %atom = and i32 %raw, 1\n  %encoded = add nuw i32 %atom, -1",
      "  %atom = and i32 %raw, 1\n  %encoded = add nsw i32 %atom, 2147483647",
      "  %atom = and i32 %raw, 1\n  %encoded = shl i32 %atom, 32",
      "  %atom = and i32 %raw, 1\n  %encoded = shl nsw i32 %atom, 31",
      "  %atom = and i32 %raw, 1\n  %encoded = lshr exact i32 %atom, 1",
      "  %atom = and i32 %raw, 1\n  %encoded = or disjoint i32 %atom, 1"}) {
    INFO(leaf); Fixture fixture(diamond(leaf)); fixture.reject();
  }
  for (const std::string &leaf : {
      "  %bit = trunc i32 %raw to i8\n  %atom = and i8 %bit, 1\n  %negative = add i8 %atom, -1\n  %encoded = zext nneg i8 %negative to i32",
      "  %atom = and i32 %raw, 1\n  %large = add i32 %atom, 255\n  %small = trunc nuw i32 %large to i8\n  %encoded = zext i8 %small to i32",
      "  %atom = and i32 %raw, 1\n  %large = add i32 %atom, 127\n  %small = trunc nsw i32 %large to i8\n  %encoded = zext i8 %small to i32"}) {
    INFO(leaf); Fixture fixture(diamond(leaf)); fixture.reject();
  }
}

TEST_CASE("Private decoder rejects cyclic encoded state graphs transactionally", "[dispatch-decoder-ssa]") {
  Fixture fixture(R"IR(define i32 @f(i32 %raw, i32 %other, i1 %c, ptr %guest) {
entry: br label %loop
loop:
  %phi = phi i32 [138502176, %entry], [%phi, %loop]
  %a = add i32 %phi, 1640531527
  %d = xor i32 %a, 1779033703
  switch i32 %d, label %loop [i32 0, label %zero]
zero: ret i32 0
})IR");
  fixture.reject();
}

TEST_CASE("Private decoder observes each bounded publication cap", "[dispatch-decoder-ssa]") {
  for (unsigned cap = 0; cap != 4; ++cap) {
    Fixture fixture(diamond(finite)); DispatchDecoderSSAOptions options;
    if (cap == 0) options.maxNodes = 2;
    if (cap == 1) options.maxWork = 2;
    if (cap == 2) options.maxInstructions = 1;
    if (cap == 3) { options.maxPhis = 129; fixture.reject(options); continue; }
    fixture.reject(options, true);
  }
}

TEST_CASE("Private decoder explicitly consumes original bounds and liveout uses", "[dispatch-decoder-uses]") {
  auto body = diamond(finite);
  const auto at = body.find("  switch i32 %decoded"); REQUIRE(at != std::string::npos);
  body.insert(at, "  %bound = icmp ult i32 %decoded, 6\n  store i32 %decoded, ptr %guest, align 1\n");
  Fixture fixture(body);
  auto *old = fixture.selected().getCondition();
  const auto oldUsers = old->getNumUses(); CHECK(oldUsers == 4);
  auto stats = fixture.consume(); INFO(stats.message); REQUIRE(stats.changed); fixture.verify();
  auto *replacement = fixture.selected().getCondition(); REQUIRE(replacement != old);
  unsigned consumers = 0, retainedDecoders = 0;
  for (auto &block : fixture.function()) for (auto &i : block) {
    if (i.getName() == "decoded") { ++retainedDecoders; CHECK(&i == old); }
    if (i.getName() == "bound") { ++consumers; CHECK(i.getOperand(0) == replacement); }
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&i); store && store->getValueOperand() == replacement) ++consumers;
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&i); ret && block.getName() == "error") {
      ++consumers; CHECK(ret->getReturnValue() == replacement);
    }
  }
  CHECK(consumers == 3); CHECK(retainedDecoders == 1); CHECK(old->use_empty());
}

TEST_CASE("Private decoder all-use mode preserves PHI edges and poison observations", "[dispatch-decoder-uses]") {
  SECTION("PHI liveout receives the same equivalent value on its actual edge") {
    auto body = diamond(finite);
    auto at = body.find("zero: ret i32 0"); REQUIRE(at != std::string::npos);
    body.replace(at, std::string("zero: ret i32 0").size(),
        "zero:\n  %liveout = phi i32 [%decoded, %join]\n  ret i32 %liveout");
    Fixture fixture(body); auto *old = fixture.selected().getCondition();
    REQUIRE(fixture.consume().changed); fixture.verify();
    for (auto &block : fixture.function()) for (auto &i : block) if (i.getName() == "liveout")
      CHECK(llvm::cast<llvm::PHINode>(i).getIncomingValue(0) == fixture.selected().getCondition());
    CHECK(old->use_empty());
  }
  SECTION("poison i1 observation is retained without executing poison") {
    Fixture fixture(diamond("  %observed = icmp eq i32 poison, 0\n  %z = zext i1 %observed to i32\n  %masked = and i32 %z, 0\n  %encoded = add i32 %masked, 138502173"));
    REQUIRE(fixture.consume().changed); fixture.verify();
    auto *select = llvm::dyn_cast<llvm::SelectInst>(decodedPhi(fixture)->getIncomingValue(0));
    REQUIRE(select); CHECK(select->getCondition()->getName() == "observed");
    CHECK(select->getTrueValue() == select->getFalseValue());
    CHECK(llvm::cast<llvm::ConstantInt>(select->getTrueValue())->getZExtValue() == 3);
  }
  SECTION("a late original-use work refusal leaves the module exact") {
    Fixture fixture(diamond(finite));
    // Establish the old mode's exact cost on an independent identical module.
    Fixture control(diamond(finite)); auto oldCost = control.run(); REQUIRE(oldCost.changed);
    DispatchDecoderSSAOptions options; options.maxWork = oldCost.work;
    const auto before = fixture.text(); auto stats = fixture.consume(options);
    CHECK_FALSE(stats.changed); CHECK(stats.budgetExhausted); CHECK(fixture.text() == before);
    fixture.verify();
  }
}
