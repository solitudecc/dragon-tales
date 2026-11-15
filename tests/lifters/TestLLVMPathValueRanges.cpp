#include <catch2/catch_test_macros.hpp>
#include "../../lib/LLVMPathValueRanges.hpp"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace dragon::llvmlifter::detail;
namespace {
struct PathFixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit PathFixture(std::string condition, bool duplicate=false, bool bypass=false) {
    std::string text=R"IR(
define void @f(i32 %input, i1 %bypass, ptr %out) {
entry:
  %stable = freeze i32 %input
  %other = freeze i32 %input
  store i32 %stable, ptr %out
  ENTRY_BRANCH
guard:
  %cmp = icmp ult i32 %stable, 8
  CONDITION
bridge:
  %history = phi i64 [ %choice, %guard ]
  %continue = icmp ne i64 %history, 39
  br i1 %continue, label %body, label %exit
body:
  %index = zext i32 %stable to i64
  %otherindex = zext i32 %other to i64
  ret void
exit:
  ret void
}
)IR";
    auto replace=[&](const std::string &a,const std::string &b) {auto p=text.find(a);REQUIRE(p!=std::string::npos);text.replace(p,a.size(),b);};
    replace("ENTRY_BRANCH",bypass ? "br i1 %bypass, label %body, label %guard" : "br label %guard");
    replace("CONDITION",condition);
    if (duplicate) replace("br i1 %continue, label %body, label %exit", "br i1 %continue, label %body, label %body");
    llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);
    if (!module) error.print("path-ranges-test",llvm::errs());
    REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  llvm::Instruction *find(llvm::StringRef name) {
    for (auto &block:*module->getFunction("f")) for(auto &i:block) if(i.getName()==name)return &i;
    return nullptr;
  }
  llvm::Instruction *exit() {for(auto &b:*module->getFunction("f"))if(b.getName()=="exit")return b.getTerminator();return nullptr;}
  llvm::StoreInst *beforeGuard() {for(auto &i:module->getFunction("f")->getEntryBlock())if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i))return s;return nullptr;}
  std::string ir() {std::string result;llvm::raw_string_ostream out(result);module->print(out,nullptr);return result;}
};
const char *selectCondition="%choice = select i1 %cmp, i64 41, i64 39\n  br label %bridge";
}

TEST_CASE("Path range facts refer to actual dominated stable observations", "[llvm][path-value-ranges]") {
  PathFixture f(selectCondition);auto before=f.ir();
  PathValueRanges ranges(*f.module->getFunction("f"),{f.find("stable"),f.find("other")},65536);
  auto body=ranges.get(f.find("stable"),f.find("index"));
  REQUIRE(body);CHECK(body->bits==32);CHECK(body->low==0);CHECK(body->high==7);
  auto outside=ranges.get(f.find("stable"),f.exit());
  REQUIRE(outside);CHECK(outside->low==8);CHECK(outside->high==UINT32_MAX);
  // A later guard cannot constrain the value of a preceding STORE; another
  // Freeze at the same source location is a separate stable observation.
  CHECK_FALSE(ranges.get(f.find("stable"),f.beforeGuard()));
  CHECK_FALSE(ranges.get(f.find("other"),f.find("otherindex")));
  CHECK_FALSE(ranges.get(f.find("stable"),f.find("cmp")));
  CHECK_FALSE(ranges.budgetExhausted());CHECK(ranges.work()<=65536);CHECK(f.ir()==before);
}

TEST_CASE("Path ranges never derive a branch arm from duplicate or bypass edges", "[llvm][path-value-ranges]") {
  for (auto mode:{0,1}) {
    PathFixture f(selectCondition,mode==0,mode==1);auto before=f.ir();
    PathValueRanges ranges(*f.module->getFunction("f"),{f.find("stable")},65536);
    CHECK_FALSE(ranges.get(f.find("stable"),f.find("index")));
    CHECK(f.ir()==before);CHECK_FALSE(ranges.budgetExhausted());
  }
}

TEST_CASE("Path range boolean decoding preserves select polarity and incomplete facts", "[llvm][path-value-ranges]") {
  for (const std::string condition:{
      "%choice = select i1 %cmp, i64 39, i64 41\n  br label %bridge",
      "%flag = zext i1 %cmp to i64\n  %flagtest = icmp eq i64 %flag, 1\n  %choice = select i1 %flagtest, i64 41, i64 39\n  br label %bridge",
      "%combined = and i1 %cmp, %bypass\n  %choice = select i1 %combined, i64 41, i64 39\n  br label %bridge",
      "%combined = or i1 %cmp, %bypass\n  %choice = select i1 %combined, i64 41, i64 39\n  br label %bridge"}) {
    PathFixture f(condition);PathValueRanges ranges(*f.module->getFunction("f"),{f.find("stable")},65536);
    auto body=ranges.get(f.find("stable"),f.find("index"));
    if(condition.find("select i1 %cmp, i64 39")!=std::string::npos) {
      REQUIRE(body);CHECK(body->low==8);CHECK(body->high==UINT32_MAX);
    } else if(condition.find("%combined = or")!=std::string::npos) {
      CHECK_FALSE(body);auto outside=ranges.get(f.find("stable"),f.exit());REQUIRE(outside);CHECK(outside->low==8);
    } else { REQUIRE(body);CHECK(body->low==0);CHECK(body->high==7); }
  }
  PathFixture f(selectCondition);auto before=f.ir();
  PathValueRanges missing(*f.module->getFunction("f"),{},65536);
  CHECK_FALSE(missing.get(f.find("stable"),f.find("index")));
  PathValueRanges tiny(*f.module->getFunction("f"),{f.find("stable")},1);
  CHECK(tiny.budgetExhausted());CHECK(tiny.work()<=1);
  CHECK_FALSE(tiny.get(f.find("stable"),f.find("index")));CHECK(f.ir()==before);
  PathValueRanges reduced(*f.module->getFunction("f"),{f.find("stable")},65536);
  const auto spent=reduced.work();
  CHECK_FALSE(reduced.get(f.find("stable"),f.find("index"),1));
  CHECK(reduced.budgetExhausted());CHECK(reduced.work()-spent<=1);
}
