#include <catch2/catch_test_macros.hpp>
#include "../../lib/LLVMPathValueRanges.hpp"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <utility>
#include <vector>

using namespace dragon::llvmlifter::detail;
namespace {
struct TraversalFixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit TraversalFixture(unsigned padding=0,bool duplicate=false,bool bypass=false,
                            unsigned booleanDepth=0,unsigned booleanWidth=0) {
    std::string text=R"IR(
define void @f(i32 %input, i1 %bypass, ptr %out) {
entry:
  %stable = freeze i32 %input
  %other = freeze i32 %input
  store i32 %stable, ptr %out
  PADDING
  ENTRY_BRANCH
guard:
  %cmp = icmp ult i32 %stable, 8
  BOOLEAN
  %choice = select i1 CONDITION, i64 41, i64 39
  br label %bridge
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
dead:
  %unreachable = freeze i32 %input
  ret void
}
)IR";
    auto replace=[&](const std::string &from,const std::string &to) {
      auto at=text.find(from);REQUIRE(at!=std::string::npos);
      REQUIRE(text.find(from,at+from.size())==std::string::npos);text.replace(at,from.size(),to);
    };
    std::string pure;
    for(unsigned n=0;n<padding;++n)
      pure+="%unrelated."+std::to_string(n)+" = add i32 %input, "+std::to_string(n+1)+"\n  ";
    replace("PADDING",pure);
    replace("ENTRY_BRANCH",bypass ? "br i1 %bypass, label %body, label %guard" : "br label %guard");
    std::string boolean;
    for(unsigned n=0;n<booleanDepth;++n)
      boolean+="%boolean."+std::to_string(n)+" = xor i1 "+
          (n ? "%boolean."+std::to_string(n-1) : "%cmp")+", false\n  ";
    std::string condition=booleanDepth ? "%boolean."+std::to_string(booleanDepth-1) : "%cmp";
    if(booleanWidth) {
      std::vector<std::string> layer(booleanWidth,"%cmp");unsigned number=0;
      while(layer.size()>1) {
        std::vector<std::string> next;
        for(std::size_t n=0;n<layer.size();n+=2) {
          if(n+1==layer.size())next.push_back(layer[n]);
          else {
            const auto name="%wide.boolean."+std::to_string(number++);
            boolean+=name+" = and i1 "+layer[n]+", "+layer[n+1]+"\n  ";next.push_back(name);
          }
        }
        layer=std::move(next);
      }
      condition=layer.front();
    }
    replace("BOOLEAN",boolean);
    replace("CONDITION",condition);
    if(duplicate) replace("br i1 %continue, label %body, label %exit",
                          "br i1 %continue, label %body, label %body");
    llvm::SMDiagnostic diagnostic;module=llvm::parseAssemblyString(text,diagnostic,context);
    if(!module)diagnostic.print("path-traversal-test",llvm::errs());
    REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  llvm::Function &function(){return *module->getFunction("f");}
  llvm::Instruction *find(llvm::StringRef name) {
    for(auto &block:function())for(auto &i:block)if(i.getName()==name)return &i;
    return nullptr;
  }
  llvm::Instruction *end(llvm::StringRef name) {
    for(auto &block:function())if(block.getName()==name)return block.getTerminator();
    return nullptr;
  }
  llvm::StoreInst *beforeGuard() {
    for(auto &i:function().getEntryBlock())if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i))return store;
    return nullptr;
  }
  std::string ir() {
    std::string text;llvm::raw_string_ostream out(text);module->print(out,nullptr);return text;
  }
  std::vector<llvm::Instruction *> nodes() {
    std::vector<llvm::Instruction *> result;
    for(auto &block:function())for(auto &i:block)result.push_back(&i);
    return result;
  }
};
void sameRange(const std::optional<PathValueRange> &a,const std::optional<PathValueRange> &b) {
  REQUIRE(bool(a)==bool(b));
  if(a){CHECK(a->bits==b->bits);CHECK(a->low==b->low);CHECK(a->high==b->high);}
}
void rangeIs(const std::optional<PathValueRange> &range,std::uint64_t low,std::uint64_t high) {
  REQUIRE(range);CHECK(range->bits==32);CHECK(range->low==low);CHECK(range->high==high);
}
}

TEST_CASE("Path branch traversal agrees with legacy facts and exact accounting delta",
          "[llvm][path-branch-conditions]") {
  TraversalFixture fixture(37);const auto before=fixture.ir();const auto nodes=fixture.nodes();
  auto witnesses=std::vector<llvm::Value *>{fixture.find("stable"),fixture.find("other")};
  PathValueRanges legacy(fixture.function(),witnesses,65536);
  PathValueRanges explicitWhole(fixture.function(),witnesses,65536,PathRangeTraversal::WholeFunction);
  PathValueRanges branch(fixture.function(),witnesses,65536,PathRangeTraversal::BranchConditions);
  REQUIRE_FALSE(legacy.budgetExhausted());REQUIRE_FALSE(explicitWhole.budgetExhausted());REQUIRE_FALSE(branch.budgetExhausted());
  CHECK(legacy.work()==explicitWhole.work());CHECK(legacy.work()==branch.work()+nodes.size());
  for(auto *at:std::vector<llvm::Instruction *>{fixture.find("index"),fixture.end("exit"),fixture.beforeGuard(),fixture.find("cmp")}) {
    auto old=legacy.get(fixture.find("stable"),at);
    sameRange(old,explicitWhole.get(fixture.find("stable"),at));sameRange(old,branch.get(fixture.find("stable"),at));
  }
  rangeIs(branch.get(fixture.find("stable"),fixture.find("index")),0,7);
  rangeIs(branch.get(fixture.find("stable"),fixture.end("exit")),8,UINT32_MAX);
  CHECK_FALSE(branch.get(fixture.find("stable"),fixture.beforeGuard()));
  CHECK_FALSE(branch.get(fixture.find("other"),fixture.find("otherindex")));
  CHECK(fixture.ir()==before);CHECK(fixture.nodes()==nodes);REQUIRE_FALSE(llvm::verifyModule(*fixture.module,&llvm::errs()));
}

TEST_CASE("Path branch traversal avoids irrelevant instruction enumeration under fixed work",
          "[llvm][path-branch-conditions]") {
  TraversalFixture fixture(8192);const auto before=fixture.ir();const auto nodes=fixture.nodes();
  REQUIRE(nodes.size()>8192);auto witness=std::vector<llvm::Value *>{fixture.find("stable")};
  PathValueRanges legacy(fixture.function(),witness,256);
  PathValueRanges explicitWhole(fixture.function(),witness,256,PathRangeTraversal::WholeFunction);
  REQUIRE(legacy.budgetExhausted());REQUIRE(explicitWhole.budgetExhausted());
  CHECK(legacy.work()==explicitWhole.work());CHECK(legacy.work()<=256);
  CHECK_FALSE(legacy.get(fixture.find("stable"),fixture.find("index")));
  PathValueRanges branch(fixture.function(),witness,256,PathRangeTraversal::BranchConditions);
  REQUIRE_FALSE(branch.budgetExhausted());rangeIs(branch.get(fixture.find("stable"),fixture.find("index")),0,7);
  rangeIs(branch.get(fixture.find("stable"),fixture.end("exit")),8,UINT32_MAX);
  CHECK(branch.work()<=256);CHECK_FALSE(branch.budgetExhausted());
  // New objects pay their own construction/decode. No cached parent facts are reused.
  PathValueRanges another(fixture.function(),witness,256,PathRangeTraversal::BranchConditions);
  rangeIs(another.get(fixture.find("stable"),fixture.find("index")),0,7);
  CHECK(another.work()>0);CHECK_FALSE(another.budgetExhausted());
  CHECK(fixture.ir()==before);CHECK(fixture.nodes()==nodes);REQUIRE_FALSE(llvm::verifyModule(*fixture.module,&llvm::errs()));
}

TEST_CASE("Path branch traversal preserves witness and exact point boundaries",
          "[llvm][path-branch-conditions]") {
  for(auto mode:{PathRangeTraversal::WholeFunction,PathRangeTraversal::BranchConditions}) {
    TraversalFixture fixture,foreign;const auto before=fixture.ir();const auto nodes=fixture.nodes();
    PathValueRanges missing(fixture.function(),{},65536,mode);
    CHECK_FALSE(missing.get(fixture.find("stable"),fixture.find("index")));CHECK_FALSE(missing.budgetExhausted());
    PathValueRanges other(fixture.function(),{fixture.find("other")},65536,mode);
    CHECK_FALSE(other.get(fixture.find("other"),fixture.find("otherindex")));
    CHECK_FALSE(other.get(fixture.find("stable"),fixture.find("index")));
    PathValueRanges invalid(fixture.function(),{foreign.find("stable")},65536,mode);
    CHECK_FALSE(invalid.get(fixture.find("stable"),fixture.find("index")));
    PathValueRanges unreachable(fixture.function(),{fixture.find("unreachable")},65536,mode);
    CHECK_FALSE(unreachable.get(fixture.find("stable"),fixture.find("index")));
    PathValueRanges actual(fixture.function(),{fixture.find("stable")},65536,mode);
    CHECK_FALSE(actual.get(fixture.find("stable"),foreign.find("index")));
    CHECK_FALSE(actual.get(fixture.find("stable"),fixture.beforeGuard()));
    rangeIs(actual.get(fixture.find("stable"),fixture.find("index")),0,7);
    CHECK(fixture.ir()==before);CHECK(fixture.nodes()==nodes);
  }
}

TEST_CASE("Path branch traversal refuses duplicate successor and bypass facts",
          "[llvm][path-branch-conditions]") {
  for(auto mode:{PathRangeTraversal::WholeFunction,PathRangeTraversal::BranchConditions})
    for(unsigned shape:{0u,1u}) {
      TraversalFixture fixture(0,shape==0,shape==1);const auto before=fixture.ir();const auto nodes=fixture.nodes();
      PathValueRanges ranges(fixture.function(),{fixture.find("stable")},65536,mode);
      CHECK_FALSE(ranges.get(fixture.find("stable"),fixture.find("index")));
      CHECK_FALSE(ranges.budgetExhausted());CHECK(fixture.ir()==before);CHECK(fixture.nodes()==nodes);
    }
}

TEST_CASE("Path branch traversal keeps current and historical phi observations separate",
          "[llvm][path-branch-conditions]") {
  const char *text=R"IR(
define void @f(i32 %input, i32 %oldinput, ptr %out) {
entry:
  %initial = freeze i32 %input
  %oldinitial = freeze i32 %oldinput
  br label %loop
loop:
  %current = phi i32 [ %initial, %entry ], [ %next, %back ]
  %old = phi i32 [ %oldinitial, %entry ], [ %current, %back ]
  %cmp = icmp ult i32 %current, 8
  br i1 %cmp, label %bridge, label %exit
bridge:
  %copy = phi i32 [ %current, %loop ]
  %oldcopy = phi i32 [ %old, %loop ]
  %use = zext i32 %copy to i64
  %olduse = zext i32 %oldcopy to i64
  store i64 %use, ptr %out
  br label %back
back:
  %next = add i32 %current, 1
  br label %loop
exit:
  ret void
}
)IR";
  llvm::LLVMContext context;llvm::SMDiagnostic diagnostic;auto module=llvm::parseAssemblyString(text,diagnostic,context);
  REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));auto &function=*module->getFunction("f");
  auto find=[&](llvm::StringRef name)->llvm::Instruction *{for(auto&b:function)for(auto&i:b)if(i.getName()==name)return &i;return nullptr;};
  std::string before;llvm::raw_string_ostream original(before);module->print(original,nullptr);original.flush();
  for(auto mode:{PathRangeTraversal::WholeFunction,PathRangeTraversal::BranchConditions}) {
    PathValueRanges ranges(function,{find("current"),find("old"),find("next")},65536,mode);
    rangeIs(ranges.get(find("current"),find("use")),0,7);
    rangeIs(ranges.get(find("copy"),find("use")),0,7);
    CHECK_FALSE(ranges.get(find("old"),find("olduse")));
    CHECK_FALSE(ranges.get(find("oldcopy"),find("olduse")));
    CHECK_FALSE(ranges.get(find("current"),find("cmp")));
    CHECK_FALSE(ranges.get(find("next"),find("next")->getParent()->getTerminator()));
    CHECK_FALSE(ranges.budgetExhausted());
  }
  std::string after;llvm::raw_string_ostream result(after);module->print(result,nullptr);result.flush();CHECK(after==before);
}

TEST_CASE("Path branch traversal preserves shared query and decode limits",
          "[llvm][path-branch-conditions]") {
  for(auto mode:{PathRangeTraversal::WholeFunction,PathRangeTraversal::BranchConditions}) {
    TraversalFixture fixture;auto witnesses=std::vector<llvm::Value *>{fixture.find("stable")};const auto before=fixture.ir();
    PathValueRanges tiny(fixture.function(),witnesses,1,mode);
    CHECK(tiny.budgetExhausted());CHECK(tiny.work()<=1);CHECK_FALSE(tiny.get(fixture.find("stable"),fixture.find("index")));
    PathValueRanges reduced(fixture.function(),witnesses,65536,mode);const auto spent=reduced.work();
    CHECK_FALSE(reduced.get(fixture.find("stable"),fixture.find("index"),1));
    CHECK(reduced.budgetExhausted());CHECK(reduced.work()-spent<=1);
    std::vector<llvm::Value *> tooMany(257,fixture.find("stable"));
    PathValueRanges witnessCap(fixture.function(),tooMany,65536,mode);
    CHECK(witnessCap.budgetExhausted());CHECK_FALSE(witnessCap.get(fixture.find("stable"),fixture.find("index")));
    TraversalFixture deep(0,false,false,34);
    PathValueRanges depth(deep.function(),{deep.find("stable")},65536,mode);
    CHECK(depth.budgetExhausted());CHECK_FALSE(depth.get(deep.find("stable"),deep.find("index")));
    TraversalFixture wide(0,false,false,0,256);
    PathValueRanges nodeCap(wide.function(),{wide.find("stable")},65536,mode);
    CHECK(nodeCap.budgetExhausted());CHECK_FALSE(nodeCap.get(wide.find("stable"),wide.find("index")));
    llvm::LLVMContext context;std::string text="define void @many(i32 %input) {\nentry:\n %stable = freeze i32 %input\n br label %b0\n";
    for(unsigned n=0;n<4096;++n)text+="b"+std::to_string(n)+":\n "+
        (n==4095 ? std::string("ret void\n") : "br label %b"+std::to_string(n+1)+"\n");
    text+="}\n";llvm::SMDiagnostic diagnostic;auto many=llvm::parseAssemblyString(text,diagnostic,context);
    REQUIRE(many);REQUIRE_FALSE(llvm::verifyModule(*many,&llvm::errs()));auto *function=many->getFunction("many");REQUIRE(function);
    PathValueRanges blockCap(*function,{&function->getEntryBlock().front()},65536,mode);
    CHECK(blockCap.budgetExhausted());CHECK_FALSE(blockCap.get(&function->getEntryBlock().front(),function->getEntryBlock().getTerminator()));
    CHECK(fixture.ir()==before);
  }
  TraversalFixture fixture;const auto before=fixture.ir();
  PathValueRanges invalid(fixture.function(),{fixture.find("stable")},65536,static_cast<PathRangeTraversal>(255));
  CHECK_FALSE(invalid.get(fixture.find("stable"),fixture.find("index")));CHECK(fixture.ir()==before);
}
