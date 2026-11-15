#include <catch2/catch_test_macros.hpp>
#include "../../lib/LLVMPhiConstant.hpp"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>

using namespace dragon::llvmlifter::detail;
namespace {
// New private API: the old SDK has no symbol. An old link failure is API
// absence, not evidence of an old executed semantic error.
struct PhiFixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit PhiFixture(const std::string &text) {
    llvm::SMDiagnostic error;
    module = llvm::parseAssemblyString(text, error, context);
    if (!module) error.print("phi-constant-test", llvm::errs());
    REQUIRE(module);
    REQUIRE_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  }
  llvm::Function &function() { return *module->getFunction("f"); }
  llvm::PHINode *phi(const std::string &name) {
    for (auto &block : function()) for (auto &i : block)
      if (i.getName() == name) return llvm::dyn_cast<llvm::PHINode>(&i);
    return nullptr;
  }
  std::string text() const {
    std::string result;
    llvm::raw_string_ostream out(result);
    module->print(out, nullptr);
    return result;
  }
};
std::string ring(unsigned count) {
  std::string s = "define void @f() {\nentry: br label %b0\n";
  for (unsigned n = 0; n < count; ++n) {
    s += "b" + std::to_string(n) + ":\n  %p" + std::to_string(n) + " = phi i64 ";
    if (!n)
      s += "[ 7, %entry ], [ %p" + std::to_string(count-1) + ", %b" + std::to_string(count-1) + " ]\n";
    else
      s += "[ %p" + std::to_string(n-1) + ", %b" + std::to_string(n-1) + " ]\n";
    s += "  br label %b" + std::to_string((n+1) % count) + "\n";
  }
  return s + "}\n";
}
}

TEST_CASE("Phi constant graph proves anchored cycles beyond scalar recursion depth",
          "[llvm][phi-constant-graph]") {
  for (unsigned count : {2u, 70u}) DYNAMIC_SECTION("nodes " << count) {
    PhiFixture f(ring(count)); const auto before = f.text();
    PhiConstantGraph proof(f.function());
    const auto result = proof.get(f.phi("p0"), 65536);
    INFO(result.message); REQUIRE(result.value);
    CHECK(result.value->equalsInt(7)); CHECK(result.value->getBitWidth() == 64);
    CHECK(result.nodes == count); CHECK_FALSE(result.budgetExhausted);
    CHECK(result.work <= 65536); CHECK(f.text() == before);
    // Successful publication covers the whole validated graph, not just root.
    for (const auto &name : {std::string("p0"), "p" + std::to_string(count-1)}) {
      auto cached = proof.get(f.phi(name), 1);
      REQUIRE(cached.value); CHECK(cached.value == result.value);
      CHECK(cached.work == 1); CHECK(cached.nodes == 0);
      CHECK_FALSE(cached.budgetExhausted); CHECK(f.text() == before);
    }
  }
}

TEST_CASE("Phi constant graph rejects nonliteral leaves and never publishes a late partial proof",
          "[llvm][phi-constant-graph]") {
  struct Leaf { const char *operation; const char *value; };
  for (const auto leaf : {Leaf{"%v = freeze i64 7", "%v"},
                          Leaf{"%v = add i64 7, 0", "%v"},
                          Leaf{"%v = zext i32 7 to i64", "%v"},
                          Leaf{"%v = load i64, ptr %address", "%v"},
                          Leaf{"", "%arg"}, Leaf{"", "undef"}, Leaf{"", "poison"}})
    DYNAMIC_SECTION("leaf " << leaf.operation << " " << leaf.value) {
      PhiFixture f(std::string("define void @f(i64 %arg, ptr %address) {\nentry:\n  ") +
                   leaf.operation + "\n  br label %join\njoin:\n  %root = phi i64 [ " +
                   leaf.value + ", %entry ]\n  ret void\n}\n");
      auto before = f.text(); PhiConstantGraph proof(f.function());
      auto result = proof.get(f.phi("root"), 65536);
      CHECK_FALSE(result.value); CHECK_FALSE(result.budgetExhausted);
      CHECK(f.text() == before);
    }
  PhiFixture f(R"IR(
define void @f(i1 %choose) {
entry: br i1 %choose, label %left, label %right
left:
  %good = phi i64 [ 7, %entry ]
  br label %join
right:
  %bad = phi i64 [ 9, %entry ]
  br label %join
join:
  %root = phi i64 [ %good, %left ], [ %bad, %right ]
  ret void
}
)IR");
  auto before = f.text(); PhiConstantGraph proof(f.function());
  auto bad = proof.get(f.phi("root"), 65536);
  CHECK_FALSE(bad.value); CHECK_FALSE(bad.budgetExhausted);
  // The good arm was visited before the late disagreeing literal. It must
  // not have become a cache entry during the refused whole-root query.
  auto child = proof.get(f.phi("good"), 1);
  CHECK_FALSE(child.value); CHECK(child.budgetExhausted);
  auto good = proof.get(f.phi("good"), 65536);
  REQUIRE(good.value); CHECK(good.value->equalsInt(7));
  CHECK_FALSE(proof.get(f.phi("root"), 1).value); CHECK(f.text() == before);
}

TEST_CASE("Phi constant graph requires an anchor for every dependency component",
          "[llvm][phi-constant-graph]") {
  PhiFixture f(R"IR(
define void @f(i1 %choose) {
entry: ret void
seed: br label %rootblock
rootblock:
  %root = phi i64 [ 7, %seed ], [ %island, %islandblock ]
  ret void
islandblock:
  %island = phi i64 [ %island, %islandblock ]
  br i1 %choose, label %islandblock, label %rootblock
}
)IR");
  const auto before = f.text(); PhiConstantGraph proof(f.function());
  auto result = proof.get(f.phi("root"), 65536);
  CHECK_FALSE(result.value); CHECK_FALSE(result.budgetExhausted);
  // Root itself has a constant arm, but its pure unanchored sub-SCC does not.
  CHECK(result.message.find("no constant anchor") != std::string::npos);
  auto untouched = proof.get(f.phi("island"), 1);
  CHECK_FALSE(untouched.value); CHECK(untouched.budgetExhausted);
  auto island = proof.get(f.phi("island"), 65536);
  CHECK_FALSE(island.value); CHECK_FALSE(island.budgetExhausted);
  CHECK(f.text() == before);
}

TEST_CASE("Phi constant graph covers duplicate and all actual predecessor slots",
          "[llvm][phi-constant-graph]") {
  PhiFixture f(R"IR(
define void @f(i1 %first, i1 %again) {
entry: br i1 %first, label %loop, label %loop
loop:
  %root = phi i64 [ 7, %entry ], [ 7, %entry ], [ %root, %loop ]
  br i1 %again, label %loop, label %exit
exit: ret void
}
)IR");
  auto before = f.text(); PhiConstantGraph proof(f.function());
  auto result = proof.get(f.phi("root"), 65536);
  REQUIRE(result.value); CHECK(result.value->equalsInt(7)); CHECK(result.nodes == 1);
  CHECK(f.text() == before);
  // Deliberately malformed coverage, never executed: rebuild analysis after
  // mutation, then require conservative refusal without repairing the IR.
  f.phi("root")->removeIncomingValue(0u, false);
  before = f.text(); PhiConstantGraph incomplete(f.function());
  auto missing = incomplete.get(f.phi("root"), 65536);
  CHECK_FALSE(missing.value); CHECK_FALSE(missing.budgetExhausted);
  CHECK(f.text() == before);
  PhiFixture other(ring(2));
  CHECK_FALSE(incomplete.get(other.phi("p0"), 65536).value);
  PhiFixture wide("define void @f() { entry: br label %join\njoin: %p = phi i128 [ 7, %entry ]\nret void\n}\n");
  PhiConstantGraph narrowOnly(wide.function());
  CHECK_FALSE(narrowOnly.get(wide.phi("p"), 65536).value);
}

TEST_CASE("Phi constant graph keeps hard work and node limits without partial cache facts",
          "[llvm][phi-constant-graph]") {
  PhiFixture f(ring(70)); const auto before = f.text();
  PhiConstantGraph proof(f.function());
  for (std::size_t limit : {0u, 1u, 16u, 64u}) {
    auto result = proof.get(f.phi("p0"), limit);
    CHECK_FALSE(result.value); CHECK(result.budgetExhausted);
    CHECK(result.work <= limit); CHECK(f.text() == before);
  }
  auto complete = proof.get(f.phi("p0"), 1000000);
  REQUIRE(complete.value); CHECK(complete.work <= 65536);
  CHECK_FALSE(complete.budgetExhausted); CHECK(f.text() == before);
  PhiFixture exact(ring(512)); PhiConstantGraph exactProof(exact.function());
  auto atLimit = exactProof.get(exact.phi("p0"), 1000000);
  REQUIRE(atLimit.value); CHECK(atLimit.nodes == 512); CHECK(atLimit.work <= 65536);
  PhiFixture large(ring(513)); const auto largeBefore = large.text();
  PhiConstantGraph bounded(large.function());
  auto overflow = bounded.get(large.phi("p0"), 1000000);
  CHECK_FALSE(overflow.value); CHECK(overflow.budgetExhausted);
  CHECK(overflow.nodes == 512); CHECK(overflow.work <= 65536);
  auto partial = bounded.get(large.phi("p512"), 1);
  CHECK_FALSE(partial.value); CHECK(partial.budgetExhausted);
  CHECK(large.text() == largeBefore);
}

namespace {
std::string conditionalRing(bool freshRead = false, bool literalEntry = true) {
  std::string s = "define void @f(ptr %address, i1 %choose) {\nentry:\n"
                  "  %first = load i64, ptr %address, align 1\n"
                  "  br label %a\na:\n  %root = phi i64 [ ";
  s += literalEntry ? "32768" : "%first";
  s += ", %entry ], [ %right, %b ]\n  br label %b\nb:\n"
       "  %right = phi i64 [ %root, %a ], [ ";
  s += freshRead ? "%fresh" : "%first";
  s += ", %extra ]\n  br i1 %choose, label %a, label %extra\nextra:\n";
  if (freshRead) s += "  %fresh = load i64, ptr %address, align 1\n";
  return s + "  br label %b\n}\n";
}
llvm::Instruction *conditionalInstruction(PhiFixture &f, const std::string &name) {
  for (auto &block : f.function()) for (auto &instruction : block)
    if (instruction.getName() == name) return &instruction;
  return nullptr;
}
using LoadHypotheses = std::map<llvm::Value *, llvm::ConstantInt *>;
}

TEST_CASE("Conditional Phi constant graph uses only an explicit exact LOAD anchor",
          "[llvm][phi-constant-graph][phi-constant-load-hypotheses]") {
  PhiFixture f(conditionalRing()); const auto before = f.text();
  auto *load = llvm::dyn_cast_or_null<llvm::LoadInst>(conditionalInstruction(f, "first"));
  REQUIRE(load);
  auto *constant = llvm::ConstantInt::get(llvm::Type::getInt64Ty(f.context), 32768);
  PhiConstantGraph strict(f.function());
  CHECK_FALSE(strict.get(f.phi("root"), 65536).value);
  LoadHypotheses hypotheses{{load, constant}};
  PhiConstantGraph conditional(f.function(), hypotheses);
  auto result = conditional.get(f.phi("root"), 65536);
  INFO(result.message); REQUIRE(result.value);
  CHECK(result.value == constant); CHECK(result.nodes == 2);
  CHECK_FALSE(result.budgetExhausted); CHECK(result.work <= 65536);
  CHECK(result.message.find("conditional") != std::string::npos);
  CHECK(f.text() == before);
  auto cached = conditional.get(f.phi("right"), 1);
  REQUIRE(cached.value); CHECK(cached.value == constant); CHECK(cached.work == 1);
  CHECK(cached.message.find("conditional") != std::string::npos);

  PhiFixture fresh(conditionalRing(true)); const auto freshBefore = fresh.text();
  auto *first = llvm::cast<llvm::LoadInst>(conditionalInstruction(fresh, "first"));
  auto *later = llvm::cast<llvm::LoadInst>(conditionalInstruction(fresh, "fresh"));
  REQUIRE(first != later); REQUIRE(first->getPointerOperand() == later->getPointerOperand());
  auto *sameValue = llvm::ConstantInt::get(llvm::Type::getInt64Ty(fresh.context), 32768);
  LoadHypotheses oldOnly{{first, sameValue}};
  PhiConstantGraph noAddressEquivalence(fresh.function(), oldOnly);
  auto rejected = noAddressEquivalence.get(fresh.phi("root"), 65536);
  CHECK_FALSE(rejected.value); CHECK_FALSE(rejected.budgetExhausted);
  CHECK(fresh.text() == freshBefore);
  // Only another separately supplied hypothesis for that exact fresh SSA can
  // serve as an anchor. This is still conditional, never a memory certificate.
  LoadHypotheses both{{first, sameValue}, {later, sameValue}};
  PhiConstantGraph explicitlyBoth(fresh.function(), both);
  REQUIRE(explicitlyBoth.get(fresh.phi("root"), 65536).value);
  CHECK(fresh.text() == freshBefore);
}

TEST_CASE("Conditional Phi anchors reject missing inconsistent or non LOAD hypotheses",
          "[llvm][phi-constant-graph][phi-constant-load-hypotheses]") {
  PhiFixture f(conditionalRing()); const auto before = f.text();
  auto *load = conditionalInstruction(f, "first");
  auto *i64 = llvm::Type::getInt64Ty(f.context);
  auto *constant = llvm::ConstantInt::get(i64, 32768);
  const LoadHypotheses cases[] = {
      {}, {{nullptr, constant}}, {{load, nullptr}},
      {{load, llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.context), 32768)}},
      {{load, llvm::ConstantInt::get(i64, 9)}},
      {{f.phi("root"), constant}}, {{f.phi("right"), constant}}};
  for (const auto &hypotheses : cases) {
    PhiConstantGraph proof(f.function(), hypotheses);
    auto result = proof.get(f.phi("root"), 65536);
    CHECK_FALSE(result.value); CHECK_FALSE(result.budgetExhausted);
    CHECK(f.text() == before);
  }
  for (const std::string operation : {"%v = freeze i64 %arg", "%v = add i64 %arg, 0", ""}) {
    const auto key = operation.empty() ? "%arg" : "%v";
    PhiFixture leaf("define void @f(i64 %arg) {\nentry:\n  " + operation +
                    "\n  br label %join\njoin:\n  %root = phi i64 [ " + key +
                    ", %entry ]\n  ret void\n}\n");
    auto original = leaf.text();
    llvm::Value *selected = operation.empty() ? static_cast<llvm::Value *>(leaf.function().getArg(0))
                                             : conditionalInstruction(leaf, "v");
    REQUIRE(selected);
    LoadHypotheses forbidden{{selected, llvm::ConstantInt::get(llvm::Type::getInt64Ty(leaf.context), 32768)}};
    PhiConstantGraph proof(leaf.function(), forbidden);
    CHECK_FALSE(proof.get(leaf.phi("root"), 65536).value);
    CHECK(leaf.text() == original);
  }
  for (const std::string instruction : {
           "%v = load volatile i64, ptr %address, align 1",
           "%v = load atomic i64, ptr %address unordered, align 8",
           "%v = load i64, ptr %address, align 1, !range !0"}) {
    const auto metadata = instruction.find("!range") != std::string::npos ? "!0 = !{i64 0, i64 65536}\n" : "";
    PhiFixture leaf("define void @f(ptr %address) {\nentry:\n  " + instruction +
                    "\n  br label %join\njoin:\n  %root = phi i64 [ %v, %entry ]\n  ret void\n}\n" + metadata);
    auto original = leaf.text(); auto *selected = conditionalInstruction(leaf, "v");
    REQUIRE(selected);
    LoadHypotheses forbidden{{selected, llvm::ConstantInt::get(llvm::Type::getInt64Ty(leaf.context), 32768)}};
    PhiConstantGraph proof(leaf.function(), forbidden);
    CHECK_FALSE(proof.get(leaf.phi("root"), 65536).value);
    CHECK(leaf.text() == original);
  }
}

TEST_CASE("Conditional Phi caches stay in their immutable hypothesis scope",
          "[llvm][phi-constant-graph][phi-constant-load-hypotheses]") {
  PhiFixture f(conditionalRing(false, false)); const auto before = f.text();
  auto *load = conditionalInstruction(f, "first");
  auto *seven = llvm::ConstantInt::get(llvm::Type::getInt64Ty(f.context), 7);
  auto *nine = llvm::ConstantInt::get(llvm::Type::getInt64Ty(f.context), 9);
  LoadHypotheses h7{{load, seven}}, h9{{load, nine}};
  PhiConstantGraph first(f.function(), h7);
  for (std::size_t limit : {0u, 1u, 4u}) {
    auto result = first.get(f.phi("root"), limit);
    CHECK_FALSE(result.value); CHECK(result.budgetExhausted); CHECK(result.work <= limit);
  }
  auto result7 = first.get(f.phi("root"), 65536);
  REQUIRE(result7.value); CHECK(result7.value == seven);
  PhiConstantGraph second(f.function(), h9);
  auto result9 = second.get(f.phi("root"), 65536);
  REQUIRE(result9.value); CHECK(result9.value == nine);
  CHECK(first.get(f.phi("root"), 1).value == seven);
  PhiConstantGraph strict(f.function());
  CHECK_FALSE(strict.get(f.phi("root"), 65536).value);
  CHECK(f.text() == before);
}
