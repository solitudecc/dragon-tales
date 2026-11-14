#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/GuestCellSSA.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dragon::llvmlifter::detail;
namespace {
// New low-level API tests. The old product has no such helper and cannot
// compile this TU: this is not a claim that old execution returned wrong values.
struct Fixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit Fixture(const std::string &body,
                   const std::string &layout = "e-p:64:64") {
    llvm::SMDiagnostic error;
    module = llvm::parseAssemblyString("target datalayout = \"" + layout +
        "\"\ndeclare void @unknown(ptr)\n" + body, error, context);
    std::string message;
    llvm::raw_string_ostream stream(message);
    error.print("guest-cell-test", stream);
    INFO(message);
    REQUIRE(module);
    verify();
  }
  llvm::Function &function() { return *module->getFunction("f"); }
  llvm::LoadInst &load() {
    for (auto &block : function()) for (auto &instruction : block)
      if (instruction.getName() == "selected")
        return *llvm::cast<llvm::LoadInst>(&instruction);
    FAIL("selected LOAD missing");
    std::abort();
  }
  GuestCellSSAStats run(const GuestCellSSAOptions &options = {}) {
    return forwardGuestCellLoad(function(), *function().getArg(0), load(), options);
  }
  std::string text() {
    std::string result;
    llvm::raw_string_ostream stream(result);
    module->print(stream, nullptr);
    return result;
  }
  void verify() {
    std::string error;
    llvm::raw_string_ostream stream(error);
    const bool invalid = llvm::verifyModule(*module, &stream);
    INFO(error);
    REQUIRE_FALSE(invalid);
  }
  llvm::PHINode *firstPhi(const char *name) {
    for (auto &block : function()) if (block.getName() == name)
      return llvm::dyn_cast<llvm::PHINode>(&block.front());
    return nullptr;
  }
  void reject(const GuestCellSSAOptions &options = {}, bool budget = false) {
    const auto before = text();
    const auto stats = run(options);
    INFO(stats.message);
    CHECK_FALSE(stats.changed);
    CHECK(stats.refused);
    CHECK(stats.budgetExhausted == budget);
    CHECK(text() == before);
    verify();
  }
};

const std::string diamond = R"IR(
define i32 @f(ptr %guest, i32 %x, i1 %c) {
entry:
  br i1 %c, label %a, label %b
a:
  store i32 %x, ptr %guest, align 1
  br label %join
b:
  store i32 7, ptr %guest, align 1
  br label %join
join:
  %selected = load i32, ptr %guest, align 1
  ret i32 %selected
}
)IR";

std::string sameBlock(const std::string &effect,
                      const std::string &value = "%x") {
  return "define i32 @f(ptr %guest, ptr %other, i32 %x) {\nentry:\n"
      "  %cell = getelementptr i8, ptr %guest, i64 16\n"
      "  store i32 " + value + ", ptr %cell, align 1\n" + effect +
      "  %selected = load i32, ptr %cell, align 1\n"
      "  ret i32 %selected\n}\n";
}
}

TEST_CASE("Guest cell SSA joins every nearest STORE value", "[llvm][guest-cell-ssa]") {
  Fixture fixture(diamond);
  auto *function = &fixture.function();
  auto *argument = function->getArg(0);
  const auto stats = fixture.run();
  INFO(stats.message);
  REQUIRE(stats.changed);
  CHECK_FALSE(stats.refused);
  CHECK(stats.phis == 1);
  CHECK(&fixture.function() == function);
  CHECK(fixture.function().getArg(0) == argument);
  auto *phi = fixture.firstPhi("join");
  REQUIRE(phi);
  CHECK(phi->getNumIncomingValues() == 2);
  CHECK(phi->getIncomingBlock(0)->getName() == "a");
  CHECK(phi->getIncomingBlock(1)->getName() == "b");
  CHECK(phi->getIncomingValue(0) == function->getArg(1));
  CHECK(llvm::cast<llvm::ConstantInt>(phi->getIncomingValue(1))->equalsInt(7));
  fixture.verify();
}

TEST_CASE("Guest cell SSA cuts earlier effects and crosses only disjoint stores",
          "[llvm][guest-cell-ssa]") {
  for (unsigned width : {8u, 16u, 32u, 64u, 128u}) {
    INFO(width);
    const std::string type = "i" + std::to_string(width);
    Fixture fixture("define " + type + " @f(ptr %guest, " + type + " %x) {\n"
        "entry:\n call void @unknown(ptr %guest)\n"
        " %p = getelementptr i8, ptr %guest, i64 1\n"
        " store " + type + " %x, ptr %p, align 1\n"
        " %other = getelementptr i8, ptr %guest, i64 32\n"
        " store i8 3, ptr %other, align 1\n"
        " %selected = load " + type + ", ptr %p, align 1\n"
        " ret " + type + " %selected\n}\n");
    const auto stats = fixture.run();
    REQUIRE(stats.changed);
    CHECK(stats.phis == 0);
    auto *ret = llvm::cast<llvm::ReturnInst>(fixture.function().back().getTerminator());
    CHECK(ret->getReturnValue() == fixture.function().getArg(1));
    fixture.verify();
  }
}

TEST_CASE("Guest cell SSA loop retains fresh arithmetic and duplicate edge slots",
          "[llvm][guest-cell-ssa]") {
  Fixture fixture(R"IR(
define i32 @f(ptr %guest, i32 %limit, i1 %c) {
entry:
  store i32 0, ptr %guest, align 1
  br label %head
head:
  %selected = load i32, ptr %guest, align 1
  %next = add i32 %selected, 1
  %more = icmp ult i32 %next, %limit
  br i1 %more, label %body, label %done
body:
  store i32 %next, ptr %guest, align 1
  br i1 %c, label %head, label %head
done:
  ret i32 %selected
}
)IR");
  const auto stats = fixture.run();
  REQUIRE(stats.changed);
  auto *phi = fixture.firstPhi("head");
  REQUIRE(phi);
  CHECK(phi->getNumIncomingValues() == 3);
  CHECK(phi->getIncomingBlock(0)->getName() == "entry");
  CHECK(phi->getIncomingBlock(1)->getName() == "body");
  CHECK(phi->getIncomingBlock(2)->getName() == "body");
  CHECK(phi->getIncomingValue(1) == phi->getIncomingValue(2));
  auto *next = llvm::cast<llvm::Instruction>(phi->getIncomingValue(1));
  CHECK(next->getName() == "next");
  CHECK(next->getOperand(0) == phi);
  fixture.verify();
}

TEST_CASE("Guest cell SSA supports a multi-block loop definition join",
          "[llvm][guest-cell-ssa]") {
  Fixture fixture(R"IR(
define i64 @f(ptr %guest, i1 %c, i1 %more) {
entry:
  store i64 1, ptr %guest, align 1
  br label %head
head:
  %selected = load i64, ptr %guest, align 1
  %next = add i64 %selected, 2
  br i1 %more, label %choose, label %done
choose:
  br i1 %c, label %a, label %b
a:
  store i64 %next, ptr %guest, align 1
  br label %bridge
b:
  store i64 9, ptr %guest, align 1
  br label %bridge
bridge:
  br label %head
done:
  ret i64 %selected
}
)IR");
  REQUIRE(fixture.run().changed);
  auto *head = fixture.firstPhi("head");
  auto *bridge = fixture.firstPhi("bridge");
  REQUIRE(head);
  REQUIRE(bridge);
  CHECK(head->getIncomingValueForBlock(bridge->getParent()) == bridge);
  CHECK(bridge->getNumIncomingValues() == 2);
  fixture.verify();
}

TEST_CASE("Guest cell SSA rejects possible aliases and observable barriers exactly",
          "[llvm][guest-cell-ssa]") {
  const std::vector<std::string> effects{
      " call void @unknown(ptr %guest)\n",
      " store i32 2, ptr %other, align 1\n",
      " %partial = getelementptr i8, ptr %guest, i64 17\n"
      " store i8 2, ptr %partial, align 1\n",
      " store volatile i32 2, ptr %cell, align 1\n",
      " store atomic i32 2, ptr %cell seq_cst, align 4\n",
      " %v = load volatile i32, ptr %other, align 1\n",
      " %a = load atomic i32, ptr %other seq_cst, align 4\n",
      " %r = atomicrmw add ptr %cell, i32 1 seq_cst\n",
      " fence seq_cst\n"};
  for (const auto &effect : effects) {
    INFO(effect);
    Fixture fixture(sameBlock(effect));
    fixture.reject();
  }
  for (const auto &value : {"undef", "poison"}) {
    Fixture fixture(sameBlock("", value));
    fixture.reject();
  }
}

TEST_CASE("Guest cell SSA refuses a STORE-free reverse cycle",
          "[llvm][guest-cell-ssa]") {
  Fixture fixture(R"IR(
define i32 @f(ptr %guest, i1 %c) {
entry:
  store i32 1, ptr %guest, align 1
  br label %head
head:
  %selected = load i32, ptr %guest, align 1
  br i1 %c, label %body, label %done
body:
  br label %head
done:
  ret i32 %selected
}
)IR");
  fixture.reject();
}

TEST_CASE("Guest cell SSA covers unreachable predecessors instead of dropping them",
          "[llvm][guest-cell-ssa]") {
  for (bool defined : {false, true}) {
    Fixture fixture("define i32 @f(ptr %guest) {\nentry:\n"
        " store i32 1, ptr %guest, align 1\n br label %join\n"
        "orphan:\n" + std::string(defined ?
        " store i32 8, ptr %guest, align 1\n" : "") +
        " br label %join\njoin:\n"
        " %selected = load i32, ptr %guest, align 1\n ret i32 %selected\n}\n");
    if (!defined) { fixture.reject(); continue; }
    REQUIRE(fixture.run().changed);
    auto *phi = fixture.firstPhi("join");
    REQUIRE(phi);
    CHECK(phi->getNumIncomingValues() == 2);
    CHECK(phi->getIncomingBlock(1)->getName() == "orphan");
    fixture.verify();
  }
  Fixture entry("define i32 @f(ptr %guest) {\nentry:\n"
      " %selected = load i32, ptr %guest, align 1\n ret i32 %selected\n}\n");
  entry.reject();
}

TEST_CASE("Guest cell SSA late predecessor refusal never creates partial PHIs",
          "[llvm][guest-cell-ssa]") {
  std::string ir = diamond;
  const auto position = ir.find("  store i32 7, ptr %guest, align 1\n");
  REQUIRE(position != std::string::npos);
  ir.erase(position, std::string("  store i32 7, ptr %guest, align 1\n").size());
  Fixture fixture(ir);
  fixture.reject();
}

TEST_CASE("Guest cell SSA prepays all PHIs and commit work before mutation",
          "[llvm][guest-cell-ssa]") {
  Fixture accepted(diamond);
  const auto required = accepted.run().work;
  REQUIRE(required > 1);
  for (unsigned mode = 0; mode < 5; ++mode) {
    Fixture fixture(diamond);
    GuestCellSSAOptions options;
    if (mode == 0) options.maxWork = 0;
    if (mode == 1) options.maxNodes = 1;
    if (mode == 2) options.maxBlocks = 1;
    if (mode == 3) options.maxPhis = 0;
    if (mode == 4) options.maxWork = required - 1;
    fixture.reject(options, true);
  }
}

TEST_CASE("Guest cell SSA uses constant modular offsets without alignment assumptions",
          "[llvm][guest-cell-ssa]") {
  Fixture exact(R"IR(
define i128 @f(ptr %guest, i128 %x) {
entry:
  %near = getelementptr i8, ptr %guest, i64 -1
  %storeptr = getelementptr i8, ptr %near, i64 2
  store i128 %x, ptr %storeptr, align 1
  %loadptr = getelementptr i8, ptr %guest, i64 1
  %selected = load i128, ptr %loadptr, align 1
  ret i128 %selected
}
)IR");
  REQUIRE(exact.run().changed);
  exact.verify();
  Fixture overlap(R"IR(
define i8 @f(ptr %guest) {
entry:
  store i8 1, ptr %guest, align 1
  %near = getelementptr i8, ptr %guest, i64 -1
  store i16 2, ptr %near, align 1
  %selected = load i8, ptr %guest, align 1
  ret i8 %selected
}
)IR");
  overlap.reject();
}

TEST_CASE("Guest cell SSA refuses nonconstant and narrow-index pointer layouts",
          "[llvm][guest-cell-ssa]") {
  Fixture dynamic(R"IR(
define i32 @f(ptr %guest, i64 %offset) {
entry:
  %p = getelementptr i8, ptr %guest, i64 %offset
  store i32 1, ptr %p, align 1
  %selected = load i32, ptr %p, align 1
  ret i32 %selected
}
)IR");
  dynamic.reject();
  Fixture narrow(diamond, "e-p:64:64:64:32");
  narrow.reject();
}

TEST_CASE("Guest cell SSA only forwards the explicitly selected original LOAD",
          "[llvm][guest-cell-ssa]") {
  Fixture fixture(R"IR(
define i32 @f(ptr %guest, i32 %x) {
entry:
  store i32 %x, ptr %guest, align 1
  %selected = load i32, ptr %guest, align 1
  %untouched = load i32, ptr %guest, align 1
  %sum = add i32 %selected, %untouched
  ret i32 %sum
}
)IR");
  REQUIRE(fixture.run().changed);
  unsigned count = 0;
  for (auto &block : fixture.function()) for (auto &instruction : block)
    if (llvm::isa<llvm::LoadInst>(instruction)) {
      ++count;
      CHECK(instruction.getName() == "untouched");
    }
  CHECK(count == 1);
  fixture.verify();
}

TEST_CASE("Guest cell SSA retains original stored PHIs without interpreting values",
          "[llvm][guest-cell-ssa]") {
  Fixture fixture(R"IR(
define i32 @f(ptr %guest, i32 %x, i1 %c) {
entry:
  br i1 %c, label %a, label %b
a:
  br label %join
b:
  br label %join
join:
  %chosen = phi i32 [ %x, %a ], [ 9, %b ]
  store i32 %chosen, ptr %guest, align 1
  %selected = load i32, ptr %guest, align 1
  ret i32 %selected
}
)IR");
  auto *original = fixture.firstPhi("join");
  REQUIRE(original);
  const auto stats = fixture.run();
  REQUIRE(stats.changed);
  CHECK(stats.phis == 0);
  CHECK(fixture.firstPhi("join") == original);
  auto *ret = llvm::cast<llvm::ReturnInst>(original->getParent()->getTerminator());
  CHECK(ret->getReturnValue() == original);
  fixture.verify();
}

TEST_CASE("Guest cell SSA retains an executed unknown call before the nearest STORE",
          "[llvm][guest-cell-ssa]") {
  Fixture fixture(R"IR(
declare i32 @make(ptr)
define i32 @f(ptr %guest) {
entry:
  %actual = call i32 @make(ptr %guest)
  store i32 %actual, ptr %guest, align 1
  %selected = load i32, ptr %guest, align 1
  ret i32 %selected
}
)IR");
  const auto stats = fixture.run();
  REQUIRE(stats.changed);
  CHECK(stats.phis == 0);
  unsigned calls = 0;
  for (auto &instruction : fixture.function().getEntryBlock())
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      ++calls;
      auto *ret = llvm::cast<llvm::ReturnInst>(call->getParent()->getTerminator());
      CHECK(ret->getReturnValue() == call);
    }
  CHECK(calls == 1);
  fixture.verify();
}
