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

namespace {
struct OrSubFixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit OrSubFixture(unsigned bits=32,std::string constant="7",bool reverse=false,
      std::string comparison="sge",std::string threshold="0",std::string subFlags="",
      std::string orFlags="",bool independent=false,bool swappedSub=false,
      bool duplicate=false,bool bypass=false,bool swappedCompare=false) {
    const auto type="i"+std::to_string(bits);
    std::string text="define void @f("+type+" %input, i1 %bypass, ptr %out) {\nentry:\n";
    text+=" %stable=freeze "+type+" %input\n %other=freeze "+type+" %input\n store "+type+" %stable,ptr %out\n";
    text+=bypass?" br i1 %bypass,label %body,label %guard\n":" br label %guard\n";
    text+="guard:\n %difference=sub "+subFlags+type+" "+(swappedSub?"%stable":constant)+", "+(swappedSub?constant:independent?"%other":"%stable")+"\n";
    text+=" %combined=or "+orFlags+type+" "+(reverse?"%difference, %stable":"%stable, %difference")+"\n";
    text+=" %condition=icmp "+comparison+" "+type+" "+(swappedCompare?threshold+", %combined":"%combined, "+threshold)+"\n";
    text+=duplicate?" br i1 %condition,label %body,label %body\n":" br i1 %condition,label %body,label %exit\n";
    text+="body:\n %index="+(bits==64?"add i64 %stable,0":"zext "+type+" %stable to i64")+"\n ret void\nexit:\n ret void\n}\n";
    llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);
    if(!module)error.print("or-sub-path-test",llvm::errs());
    REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  llvm::Instruction *find(llvm::StringRef name){for(auto &b:*module->getFunction("f"))for(auto &i:b)if(i.getName()==name)return &i;return nullptr;}
  llvm::Instruction *exit(){for(auto &b:*module->getFunction("f"))if(b.getName()=="exit")return b.getTerminator();return nullptr;}
  llvm::Instruction *before(){for(auto &i:module->getFunction("f")->getEntryBlock())if(llvm::isa<llvm::StoreInst>(i))return &i;return nullptr;}
  std::string ir(){std::string text;llvm::raw_string_ostream out(text);module->print(out,nullptr);return text;}
};
}

TEST_CASE("Signed OR of a stable value and constant minus that value gives unsigned path bounds",
          "[llvm][path-value-ranges][path-or-sub-range]") {
  for(unsigned bits:{1u,8u,32u,64u})for(bool reverse:{false,true})for(bool greater:{false,true}) {
    INFO(bits);INFO(reverse);INFO(greater);
    const std::string constant=bits==1?"0":"7";
    OrSubFixture f(bits,constant,reverse,greater?"sgt":"sge",greater?"-1":"0");const auto before=f.ir();
    PathValueRanges ranges(*f.module->getFunction("f"),{f.find("stable"),f.find("other")},65536);
    auto accepted=ranges.get(f.find("stable"),f.find("index"));REQUIRE(accepted);
    CHECK(accepted->bits==bits);CHECK(accepted->low==0);CHECK(accepted->high==(bits==1?0:7));
    auto rejected=ranges.get(f.find("stable"),f.exit());REQUIRE(rejected);
    CHECK(rejected->low==(bits==1?1:8));CHECK(rejected->high==(bits==64?UINT64_MAX:(std::uint64_t(1)<<bits)-1));
    CHECK_FALSE(ranges.get(f.find("stable"),f.before()));CHECK_FALSE(ranges.get(f.find("other"),f.find("index")));
    CHECK_FALSE(ranges.budgetExhausted());CHECK(ranges.work()<=65536);CHECK(f.ir()==before);
  }
  for(const auto &constant:{std::string("0"),std::string("2147483647")}) {
    OrSubFixture f(32,constant);PathValueRanges ranges(*f.module->getFunction("f"),{f.find("stable")},65536);
    const auto accepted=ranges.get(f.find("stable"),f.find("index"));REQUIRE(accepted);
    CHECK(accepted->high==std::stoull(constant));const auto rejected=ranges.get(f.find("stable"),f.exit());REQUIRE(rejected);
    CHECK(rejected->low==std::stoull(constant)+1);CHECK(rejected->high==UINT32_MAX);
  }
  OrSubFixture swapped(32,"7",false,"sle","0","","",false,false,false,false,true);
  PathValueRanges ranges(*swapped.module->getFunction("f"),{swapped.find("stable")},65536);
  const auto range=ranges.get(swapped.find("stable"),swapped.find("index"));REQUIRE(range);CHECK(range->high==7);
}

TEST_CASE("Signed OR subtraction range decoding retains exact SSA and poison boundaries",
          "[llvm][path-value-ranges][path-or-sub-range]") {
  for(unsigned mode=0;mode<11;++mode) {
    INFO(mode);
    OrSubFixture f(32,mode==0?"-1":mode==1?"2147483648":"7",false,
      mode==7?"sgt":"sge",mode==7?"0":"0",mode==2?"nsw ":mode==3?"nuw ":"",
      mode==4?"disjoint ":"",mode==5,mode==6,mode==8,mode==9);
    const auto before=f.ir();std::vector<llvm::Value *> witnesses;
    if(mode!=10)witnesses.push_back(f.find("stable"));witnesses.push_back(f.find("other"));
    PathValueRanges ranges(*f.module->getFunction("f"),witnesses,65536);
    CHECK_FALSE(ranges.get(f.find("stable"),f.find("index")));
    CHECK_FALSE(ranges.get(f.find("other"),f.find("index")));CHECK_FALSE(ranges.budgetExhausted());CHECK(f.ir()==before);
  }
  OrSubFixture f;const auto before=f.ir();
  PathValueRanges tiny(*f.module->getFunction("f"),{f.find("stable")},1);
  CHECK(tiny.budgetExhausted());CHECK(tiny.work()<=1);CHECK_FALSE(tiny.get(f.find("stable"),f.find("index")));
  PathValueRanges limited(*f.module->getFunction("f"),{f.find("stable")},65536);
  const auto spent=limited.work();CHECK_FALSE(limited.get(f.find("stable"),f.find("index"),1));
  CHECK(limited.budgetExhausted());CHECK(limited.work()-spent<=1);CHECK(f.ir()==before);
}
