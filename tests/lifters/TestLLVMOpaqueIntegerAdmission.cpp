#include <catch2/catch_test_macros.hpp>
#include "../../lib/LLVMOpaqueIntegerAdmission.hpp"
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
// New private API: an old missing-symbol link is API absence, not an old
// executed semantic error. Malformed PHI controls below are never executed.
using Hypotheses = std::map<llvm::Value *, llvm::ConstantInt *>;
struct OpaqueFixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit OpaqueFixture(const std::string &text) {
    llvm::SMDiagnostic error;
    module = llvm::parseAssemblyString(text, error, context);
    if (!module) error.print("opaque-integer-test", llvm::errs());
    REQUIRE(module);
    REQUIRE_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  }
  llvm::Function &function() { return *module->getFunction("f"); }
  llvm::Instruction *find(llvm::StringRef name) {
    for (auto &block : function()) for (auto &i : block)
      if (i.getName() == name) return &i;
    return nullptr;
  }
  std::string text() const {
    std::string s; llvm::raw_string_ostream out(s); module->print(out, nullptr);
    return s;
  }
};
std::string changingRing(unsigned count) {
  std::string s = "define void @f(i32 %input) {\nentry: br label %b0\n";
  for (unsigned n = 0; n < count; ++n) {
    s += "b" + std::to_string(n) + ":\n  %p" + std::to_string(n) + " = phi i32 ";
    if (!n) s += "[ %input, %entry ], [ %step, %b" + std::to_string(count-1) + " ]\n";
    else s += "[ %p" + std::to_string(n-1) + ", %b" + std::to_string(n-1) + " ]\n";
    if (n+1 == count) s += "  %step = add nuw nsw i32 %p" + std::to_string(n) + ", 1\n";
    if (!n) s += "  %bounded = and i32 %p0, 7\n";
    s += "  br label %b" + std::to_string((n+1)%count) + "\n";
  }
  return s + "}\n";
}
std::string deepLeaf(const std::string &operation, std::string value) {
  std::string s = "declare i32 @unknown(i32) memory(none)\n"
                  "define void @f(i32 %input, i32 %count) {\nentry:\n  " + operation + "\n";
  for (unsigned n = 0; n < 40; ++n) {
    auto next = "%v" + std::to_string(n);
    s += "  " + next + " = add i32 " + value + ", %input\n";
    value = next;
  }
  return s + "  br label %join\njoin:\n  %root = phi i32 [ " + value +
         ", %entry ]\n  ret void\n}\n";
}
}

TEST_CASE("Opaque integer admission closes changing Phi graphs without a constant claim",
          "[llvm][opaque-integer-admission]") {
  for (unsigned count : {2u, 60u}) DYNAMIC_SECTION("Phi nodes " << count) {
    OpaqueFixture f(changingRing(count)); const auto before = f.text();
    const Hypotheses none; OpaqueIntegerAdmission proof(f.function(), none);
    auto result = proof.get(f.find("p0"), 128, 65536);
    INFO(result.message); REQUIRE(result.admitted);
    CHECK_FALSE(result.budgetExhausted); CHECK(result.uniqueNodes == count+3);
    CHECK(result.work <= 65536); CHECK(f.text() == before);
    // Mask is a separate consumer. Admission supplies no numeric/stable fact
    // for either the historical Phi or the flagged increment on the cycle.
    CHECK(f.find("bounded"));
    CHECK(llvm::cast<llvm::BinaryOperator>(f.find("step"))->hasNoUnsignedWrap());
  }
  OpaqueFixture island(R"IR(
define void @f() {
entry: ret void
island:
  %root = phi i64 [ %root, %island ]
  br label %island
}
)IR");
  const Hypotheses none; const auto before = island.text();
  OpaqueIntegerAdmission opaque(island.function(), none);
  auto full = opaque.get(island.find("root"), 128, 65536);
  REQUIRE(full.admitted); CHECK(full.uniqueNodes == 1); CHECK(island.text() == before);
}

TEST_CASE("Opaque integer admission visits deep bad leaves and all Select operands",
          "[llvm][opaque-integer-admission]") {
  struct Leaf { const char *operation; const char *value; };
  for (const auto leaf : {Leaf{"", "undef"}, Leaf{"", "poison"},
      Leaf{"%bad = shl i32 %input, 32", "%bad"},
      Leaf{"%bad = lshr i32 %input, 33", "%bad"},
      Leaf{"%bad = ashr i32 %input, %count", "%bad"},
      Leaf{"%bad = call i32 @unknown(i32 %input)", "%bad"}})
    DYNAMIC_SECTION("deep leaf " << leaf.operation << leaf.value) {
      OpaqueFixture f(deepLeaf(leaf.operation, leaf.value)); const auto before = f.text();
      const Hypotheses none; OpaqueIntegerAdmission proof(f.function(), none);
      auto result = proof.get(f.find("root"), 128, 65536);
      CHECK_FALSE(result.admitted); CHECK_FALSE(result.budgetExhausted);
      CHECK(result.uniqueNodes > 32); CHECK(f.text() == before);
    }
  for (const std::string operands : {"poison, i32 %input, i32 7",
                                     "%choose, i32 poison, i32 7",
                                     "%choose, i32 7, i32 undef"}) {
    OpaqueFixture f("define void @f(i1 %choose, i32 %input) {\nentry:\n"
                    "  %root = select i1 " + operands + "\n  ret void\n}\n");
    const Hypotheses none; auto before = f.text(); OpaqueIntegerAdmission proof(f.function(), none);
    auto result = proof.get(f.find("root"), 128, 65536);
    CHECK_FALSE(result.admitted); CHECK_FALSE(result.budgetExhausted); CHECK(f.text() == before);
  }
}

TEST_CASE("Opaque integer admission keeps fresh observations and legal shift hypotheses opaque",
          "[llvm][opaque-integer-admission]") {
  OpaqueFixture f(R"IR(
declare i32 @llvm.fshl.i32(i32, i32, i32)
declare i32 @llvm.fshr.i32(i32, i32, i32)
declare i32 @unknown(i32) memory(none)
define void @f(ptr %address, ptr %countaddress, i32 %input) {
entry:
  %old = load i32, ptr %address, align 1
  %fresh = load i32, ptr %address, align 1
  %count = load i32, ptr %countaddress, align 1
  %frozen = freeze i32 poison
  %another = freeze i32 poison
  %shift = shl nuw i32 %old, %count
  %left = call i32 @llvm.fshl.i32(i32 %fresh, i32 %frozen, i32 32)
  %right = call i32 @llvm.fshr.i32(i32 %left, i32 %input, i32 -1)
  %root = xor i32 %shift, %right
  %ordinary = call i32 @unknown(i32 %input)
  %volatile = load volatile i32, ptr %address, align 1
  %atomic = load atomic i32, ptr %address monotonic, align 4
  ret void
}
)IR");
  const auto before = f.text(); const Hypotheses none;
  OpaqueIntegerAdmission noCount(f.function(), none);
  CHECK_FALSE(noCount.get(f.find("root"), 128, 65536).admitted);
  for (unsigned count : {0u, 1u, 31u}) {
    Hypotheses h{{f.find("count"), llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.context), count)}};
    OpaqueIntegerAdmission proof(f.function(), h);
    auto result = proof.get(f.find("root"), 128, 65536);
    INFO(result.message); REQUIRE(result.admitted); CHECK_FALSE(result.budgetExhausted);
    CHECK(result.uniqueNodes >= 9); CHECK(f.text() == before);
  }
  for (unsigned count : {32u, 33u, 0xffffffffu}) {
    Hypotheses h{{f.find("count"), llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.context), count)}};
    OpaqueIntegerAdmission proof(f.function(), h);
    CHECK_FALSE(proof.get(f.find("root"), 128, 65536).admitted);
  }
  Hypotheses wrongType{{f.find("count"), llvm::ConstantInt::get(llvm::Type::getInt64Ty(f.context), 1)}};
  OpaqueIntegerAdmission wrong(f.function(), wrongType);
  CHECK_FALSE(wrong.get(f.find("root"), 128, 65536).admitted);
  Hypotheses wrongSSA{{f.find("fresh"), llvm::ConstantInt::get(llvm::Type::getInt32Ty(f.context), 1)}};
  OpaqueIntegerAdmission exactOnly(f.function(), wrongSSA);
  CHECK_FALSE(exactOnly.get(f.find("root"), 128, 65536).admitted);
  OpaqueIntegerAdmission observations(f.function(), none);
  for (const char *name : {"old", "fresh", "frozen", "another"}) {
    auto result = observations.get(f.find(name), 128, 65536);
    REQUIRE(result.admitted); CHECK(result.uniqueNodes == 1);
  }
  for (const char *name : {"ordinary", "volatile", "atomic"})
    CHECK_FALSE(observations.get(f.find(name), 128, 65536).admitted);
  CHECK(f.text() == before);
  // H cannot hide a direct poison/undef count: the full integer operand
  // graph is still checked after its singleton count was found.
  for (bool poison : {false, true}) {
    OpaqueFixture bad(deepLeaf(std::string("%bad = shl i32 %input, ") +
                                (poison ? "poison" : "undef"), "%bad"));
    auto *shift = llvm::cast<llvm::BinaryOperator>(bad.find("bad"));
    Hypotheses h{{shift->getOperand(1), llvm::ConstantInt::get(llvm::Type::getInt32Ty(bad.context), 1)}};
    OpaqueIntegerAdmission proof(bad.function(), h); const auto unchanged = bad.text();
    auto result = proof.get(bad.find("root"), 128, 65536);
    CHECK_FALSE(result.admitted); CHECK_FALSE(result.budgetExhausted); CHECK(bad.text() == unchanged);
  }
  for (const std::string annotation : {"convergent", "noduplicate", "[ \"observation\"() ]"}) {
    OpaqueFixture call("declare i32 @llvm.fshl.i32(i32, i32, i32)\n"
        "define void @f(i32 %input) {\nentry:\n"
        "  %root = call i32 @llvm.fshl.i32(i32 %input, i32 %input, i32 1) " +
        annotation + "\n  ret void\n}\n");
    OpaqueIntegerAdmission proof(call.function(), none); auto unchanged = call.text();
    CHECK_FALSE(proof.get(call.find("root"), 128, 65536).admitted);
    CHECK(call.text() == unchanged);
  }
}

TEST_CASE("Opaque integer admission covers every actual Phi slot and scalar type",
          "[llvm][opaque-integer-admission]") {
  const char *text = R"IR(
define void @f(i1 %choose, i1 %again, i32 %input) {
entry: br i1 %choose, label %loop, label %loop
loop:
  %root = phi i32 [ %input, %entry ], [ %input, %entry ], [ %next, %loop ]
  %next = add i32 %root, 1
  br i1 %again, label %loop, label %exit
exit: ret void
}
)IR";
  OpaqueFixture f(text); const Hypotheses none;
  auto before = f.text(); OpaqueIntegerAdmission complete(f.function(), none);
  REQUIRE(complete.get(f.find("root"), 128, 65536).admitted); CHECK(f.text() == before);
  OpaqueFixture foreign(changingRing(2));
  CHECK_FALSE(complete.get(foreign.find("p0"), 128, 65536).admitted);
  CHECK_FALSE(complete.get(nullptr, 128, 65536).admitted);
  auto *phi = llvm::cast<llvm::PHINode>(f.find("root"));
  phi->removeIncomingValue(0u, false); // Invalid coverage control; never executed.
  before = f.text(); OpaqueIntegerAdmission missing(f.function(), none);
  auto result = missing.get(phi, 128, 65536);
  CHECK_FALSE(result.admitted); CHECK_FALSE(result.budgetExhausted); CHECK(f.text() == before);
  OpaqueFixture typed(text);
  // Deliberately malformed type, bypassing PHINode's well-typed setter.
  typed.find("root")->setOperand(0, llvm::ConstantInt::get(llvm::Type::getInt64Ty(typed.context), 7));
  before = typed.text(); OpaqueIntegerAdmission wrongType(typed.function(), none);
  CHECK_FALSE(wrongType.get(typed.find("root"), 128, 65536).admitted); CHECK(typed.text() == before);
  OpaqueFixture wide("define void @f(i128 %input) { entry: %root = add i128 %input, 1\nret void\n}\n");
  OpaqueIntegerAdmission narrow(wide.function(), none);
  CHECK_FALSE(narrow.get(wide.find("root"), 128, 65536).admitted);
}

TEST_CASE("Opaque integer admission charges complete cached footprints and publishes no partial graph",
          "[llvm][opaque-integer-admission]") {
  OpaqueFixture f(changingRing(60)); const Hypotheses none; const auto before = f.text();
  OpaqueIntegerAdmission proof(f.function(), none);
  for (std::size_t work : {0u, 1u, 16u, 64u}) {
    auto result = proof.get(f.find("p0"), 128, work);
    CHECK_FALSE(result.admitted); CHECK(result.budgetExhausted); CHECK(result.work <= work);
  }
  auto zero = proof.get(f.find("p0"), 0, 65536);
  CHECK_FALSE(zero.admitted); CHECK(zero.budgetExhausted); CHECK(zero.uniqueNodes == 0);
  auto full = proof.get(f.find("p0"), 128, 65536);
  REQUIRE(full.admitted); REQUIRE(full.uniqueNodes == 63);
  auto tooSmall = proof.get(f.find("p0"), full.uniqueNodes-1, 1);
  CHECK_FALSE(tooSmall.admitted); CHECK(tooSmall.budgetExhausted);
  CHECK(tooSmall.uniqueNodes == full.uniqueNodes); CHECK(tooSmall.work == 1);
  auto cached = proof.get(f.find("p0"), full.uniqueNodes, 1);
  REQUIRE(cached.admitted); CHECK_FALSE(cached.budgetExhausted);
  CHECK(cached.uniqueNodes == full.uniqueNodes); CHECK(cached.work == 1);
  // Root-only cache: visiting p0 never caches every subnode for zero cost.
  auto child = proof.get(f.find("p59"), 128, 1);
  CHECK_FALSE(child.admitted); CHECK(child.budgetExhausted);
  CHECK(f.text() == before);
  OpaqueFixture exact(changingRing(125)); OpaqueIntegerAdmission atCap(exact.function(), none);
  auto fits = atCap.get(exact.find("p0"), 1000, 65536);
  REQUIRE(fits.admitted); CHECK(fits.uniqueNodes == 128);
  OpaqueFixture excess(changingRing(126)); OpaqueIntegerAdmission overflow(excess.function(), none);
  auto over = overflow.get(excess.find("p0"), 1000, 65536);
  CHECK_FALSE(over.admitted); CHECK(over.budgetExhausted); CHECK(over.uniqueNodes == 128);
  auto partial = overflow.get(excess.find("p125"), 128, 1);
  CHECK_FALSE(partial.admitted); CHECK(partial.budgetExhausted);
  OpaqueFixture late(deepLeaf("", "poison")); const auto lateBefore = late.text();
  OpaqueIntegerAdmission noPartial(late.function(), none);
  auto bad = noPartial.get(late.find("root"), 128, 65536);
  CHECK_FALSE(bad.admitted); CHECK_FALSE(bad.budgetExhausted);
  auto notCached = noPartial.get(late.find("v39"), 128, 1);
  CHECK_FALSE(notCached.admitted); CHECK(notCached.budgetExhausted);
  CHECK(late.text() == lateBefore);
}
