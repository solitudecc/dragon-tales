#include <catch2/catch_test_macros.hpp>
#include "../../lib/LLVMPhiLowBits.hpp"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
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
struct Fixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit Fixture(const std::string &ir) {
    llvm::SMDiagnostic diagnostic;module=llvm::parseAssemblyString(ir,diagnostic,context);
    std::string error;llvm::raw_string_ostream out(error);diagnostic.print("phi-low-test",out);
    INFO(error);REQUIRE(module);verify();
  }
  llvm::Function &f(){return *module->getFunction("f");}
  llvm::Instruction *i(const char *name){for(auto &b:f())for(auto &v:b)if(v.getName()==name)return &v;return nullptr;}
  llvm::PHINode *phi(const char *name){return llvm::dyn_cast_or_null<llvm::PHINode>(i(name));}
  llvm::TruncInst *trunc(const char *name="low"){return llvm::dyn_cast_or_null<llvm::TruncInst>(i(name));}
  std::string text(){std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;}
  void verify(){std::string error;llvm::raw_string_ostream out(error);auto bad=llvm::verifyModule(*module,&out);INFO(error);REQUIRE_FALSE(bad);}
  void reject(const std::vector<llvm::TruncInst *> &queries,const PhiLowBitsOptions &options={},bool budget=false){
    auto before=text();auto *function=&f();auto *argument=f().getArg(0);auto *old=trunc();
    auto result=replacePhiLowTruncs(f(),queries,options);INFO(result.message);
    CHECK_FALSE(result.changed);CHECK_FALSE(result.complete);CHECK(result.budgetExhausted==budget);
    CHECK(result.provedLowPhis.empty());CHECK(result.rewrittenUses==0);CHECK(result.rewrittenTruncs==0);
    CHECK(text()==before);CHECK(&f()==function);CHECK(f().getArg(0)==argument);CHECK(trunc()==old);verify();
  }
};
std::string loop(const std::string &largeSeed="0",const std::string &smallSeed="0",
                 const std::string &entry="",const std::string &step="add i32 %low,1",
                 const std::string &extend="zext",const std::string &low="trunc i64 %large to i32") {
  return "define i32 @f(ptr %memory,i32 %arg,i1 %take){\nentry:\n"+entry+R"IR(
 br label %head
head:
 %large=phi i64 [)IR"+largeSeed+R"IR(,%entry],[%wide,%back]
 %small=phi i32 [)IR"+smallSeed+R"IR(,%entry],[%next,%back]
 %low=)IR"+low+"\n %next="+step+"\n %wide="+extend+R"IR( i32 %next to i64
 store i32 %next,ptr %memory,align 1
 %more=icmp ult i32 %next,4
 br i1 %more,label %back,label %done
back: br label %head
done: ret i32 %next
})IR";
}
struct EffectSnapshot {
  std::vector<llvm::Instruction *> nodes;
  std::vector<std::vector<llvm::Value *>> operands;
  explicit EffectSnapshot(llvm::Function &f){for(auto &b:f)for(auto &i:b)
    if(i.isTerminator()||i.mayReadOrWriteMemory()||i.mayHaveSideEffects()){
      nodes.push_back(&i);std::vector<llvm::Value *> values;for(auto &u:i.operands())values.push_back(u);operands.push_back(values);
    }}
  void check(llvm::Function &f){std::vector<llvm::Instruction *> after;for(auto &b:f)for(auto &i:b)
    if(i.isTerminator()||i.mayReadOrWriteMemory()||i.mayHaveSideEffects())after.push_back(&i);
    REQUIRE(after==nodes);for(unsigned n=0;n<nodes.size();++n){
      REQUIRE(nodes[n]->getNumOperands()==operands[n].size());
      for(unsigned k=0;k<operands[n].size();++k)CHECK(nodes[n]->getOperand(k)==operands[n][k]);
    }}
};
}

TEST_CASE("Original PHI projection closes a defined shared-step recurrence","[phi-low-bits]") {
  Fixture x(loop());auto *old=x.trunc();auto *small=x.phi("small");REQUIRE(old);REQUIRE(small);
  auto *function=&x.f();auto *arg=x.f().getArg(0);EffectSnapshot effects(x.f());
  auto result=replacePhiLowTruncs(x.f(),{old});INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);
  CHECK_FALSE(result.budgetExhausted);CHECK(result.rewrittenTruncs==1);CHECK(result.rewrittenUses==1);
  REQUIRE(result.provedLowPhis.size()==1);CHECK(result.provedLowPhis[0]==small);
  CHECK(x.i("next")->getOperand(0)==small);CHECK(x.trunc()==old);CHECK(old->use_empty());
  CHECK(&x.f()==function);CHECK(x.f().getArg(0)==arg);effects.check(x.f());x.verify();
  auto again=replacePhiLowTruncs(x.f(),{old});INFO(again.message);CHECK(again.complete);CHECK_FALSE(again.changed);
}

TEST_CASE("Original PHI projection keeps signed and high-bit width semantics","[phi-low-bits]") {
  for(bool sign:{false,true}){
    INFO(sign);Fixture x(sign?loop("-1","-1","","add i32 %low,1","sext"):loop("4294967296","0"));
    auto result=replacePhiLowTruncs(x.f(),{x.trunc()});INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);x.verify();
  }
  auto ir=loop();
  auto replace=[&](const std::string &a,const std::string &b){std::size_t at=0;while((at=ir.find(a,at))!=std::string::npos){ir.replace(at,a.size(),b);at+=b.size();}};
  replace("i64","i128");replace("i32","i64");
  Fixture x(ir);auto result=replacePhiLowTruncs(x.f(),{x.trunc()});INFO(result.message);REQUIRE(result.changed);x.verify();
}

TEST_CASE("Original PHI projection treats freeze identity as the defined boundary","[phi-low-bits]") {
  const std::string entry=" %initial=freeze i32 %arg\n %initialWide=zext i32 %initial to i64\n";
  Fixture shared(loop("%initialWide","%initial",entry));
  auto result=replacePhiLowTruncs(shared.f(),{shared.trunc()});INFO(result.message);REQUIRE(result.changed);shared.verify();
  Fixture distinct(loop("%initialWide","%otherInitial",entry+" %otherInitial=freeze i32 %arg\n"));
  distinct.reject({distinct.trunc()});
}

TEST_CASE("Original PHI projection refuses unsafe definitions rather than assuming noundef","[phi-low-bits]") {
  for(const std::string &entry:{" %initial=add i32 %arg,0\n"," %initial=load i32,ptr %memory,align 1\n"}){
    INFO(entry);Fixture x(loop("%initialWide","%initial",entry+" %initialWide=zext i32 %initial to i64\n"));x.reject({x.trunc()});
  }
  for(const std::string &value:{"undef","poison"}){
    INFO(value);Fixture x(loop("%initialWide",value," %initialWide=zext i32 "+value+" to i64\n"));x.reject({x.trunc()});
  }
  for(const std::string &step:{"add nsw i32 %low,1","add nuw i32 %low,1","shl i32 %low,0"}){
    INFO(step);Fixture x(loop("0","0","",step));x.reject({x.trunc()});
  }
  Fixture annotated(loop("0","0","","add i32 %low,1","zext","trunc nuw i64 %large to i32"));annotated.reject({annotated.trunc()});
}

TEST_CASE("Original PHI projection forwards only complete dominating single PHIs","[phi-low-bits]") {
  auto ir=loop();const std::string point=" %low=trunc i64 %large to i32";auto at=ir.find(point);REQUIRE(at!=std::string::npos);
  ir.replace(at,point.size()," br label %body\nbody:\n %forward=phi i64 [%large,%head]\n %low=trunc i64 %forward to i32");
  Fixture x(ir);auto result=replacePhiLowTruncs(x.f(),{x.trunc()});INFO(result.message);REQUIRE(result.changed);x.verify();
  Fixture unreachable(R"IR(define i32 @f(ptr %memory){entry: ret i32 0
dead:
 %large=phi i64 [%large,%dead]
 %small=phi i32 [%small,%dead]
 %low=trunc i64 %large to i32
 %used=add i32 %low,1
 br label %dead
})IR");unreachable.reject({unreachable.trunc()});
}

TEST_CASE("Original PHI projection covers duplicate actual predecessor slots","[phi-low-bits]") {
  auto ir=loop();auto at=ir.find(" br label %head");REQUIRE(at!=std::string::npos);
  ir.replace(at,std::string(" br label %head").size()," br label %seed\nseed: br i1 %take,label %head,label %head");
  at=ir.find("[0,%entry]");REQUIRE(at!=std::string::npos);ir.replace(at,10,"[0,%seed],[0,%seed]");
  at=ir.find("[0,%entry]");REQUIRE(at!=std::string::npos);ir.replace(at,10,"[0,%seed],[0,%seed]");
  Fixture x(ir);auto result=replacePhiLowTruncs(x.f(),{x.trunc()});INFO(result.message);REQUIRE(result.changed);x.verify();
}

TEST_CASE("Original PHI projection does not reuse a late-failing coinductive pair","[phi-low-bits]") {
  Fixture x(R"IR(define i32 @f(ptr %memory,i1 %take){
entry: br i1 %take,label %seed,label %other
seed: br label %head
head:
 %large=phi i64 [0,%seed],[%wide,%back],[0,%other]
 %bad=phi i32 [0,%seed],[%bad,%back],[1,%other]
 %good=phi i32 [0,%seed],[%good,%back],[0,%other]
 %low=trunc i64 %large to i32
 %next=add i32 %low,1
 %wide=zext i32 %low to i64
 store i32 %next,ptr %memory,align 1
 br i1 %take,label %back,label %done
back: br label %head
other: br label %head
done: ret i32 %next
})IR");
  auto result=replacePhiLowTruncs(x.f(),{x.trunc()});INFO(result.message);REQUIRE(result.changed);
  REQUIRE(result.provedLowPhis.size()==1);CHECK(result.provedLowPhis[0]==x.phi("good"));
  CHECK(x.i("next")->getOperand(0)==x.phi("good"));CHECK(result.pairsVisited>=2);x.verify();
}

TEST_CASE("Original PHI projection is atomic across explicit candidates and caps","[phi-low-bits]") {
  for(unsigned which=0;which<3;++which){
    INFO(which);Fixture x(loop());PhiLowBitsOptions options;
    if(which==0)options.maxNodes=1;if(which==1)options.maxDepth=1;if(which==2)options.maxWork=1;
    x.reject({x.trunc()},options,true);
  }
  Fixture duplicate(loop());duplicate.reject({duplicate.trunc(),duplicate.trunc()});
  auto ir=loop();auto at=ir.find(" %next=add");REQUIRE(at!=std::string::npos);
  ir.insert(at," %bad=trunc i64 8589934592 to i16\n %badUse=zext i16 %bad to i32\n");
  Fixture x(ir);x.reject({x.trunc(),x.trunc("bad")});
}

TEST_CASE("Original PHI projection preserves direct effect cut operands","[phi-low-bits]") {
  auto ir=loop();const std::string store="store i32 %next,ptr %memory";auto at=ir.find(store);REQUIRE(at!=std::string::npos);
  ir.replace(at,store.size(),"store i32 %low,ptr %memory");Fixture x(ir);x.reject({x.trunc()});
}

namespace {
std::string relationForwardedLoop(unsigned count,bool duplicate=false,const std::string &entry="",
                          const std::string &seed="%initial") {
 std::string setup=" %initial=freeze i32 %arg\n"+entry+" %initialWide=zext i32 "+seed+" to i64\n";
 auto ir=loop("%initialWide",seed,setup);
 const std::string wide=" %wide=zext i32 %next to i64\n";
 auto at=ir.find(wide);REQUIRE(at!=std::string::npos);ir.erase(at,wide.size());
 at=ir.find("[%next,%back]");REQUIRE(at!=std::string::npos);
 ir.replace(at,std::string("[%next,%back]").size(),"[%forward"+std::to_string(count-1)+",%back]");
 at=ir.find("label %back,label %done");REQUIRE(at!=std::string::npos);
 ir.replace(at,std::string("label %back,label %done").size(),"label %chain0,label %done");
 std::string chain;
 for(unsigned n=0;n<count;++n){
  const auto id=std::to_string(n),previous=n?"chain"+std::to_string(n-1):"head";
  const auto value=n?"%forward"+std::to_string(n-1):"%next";
  chain+="chain"+id+":\n %forward"+id+"=phi i32 ["+value+",%"+previous+"]";
  if(duplicate&&n)chain+=",["+value+",%"+previous+"]";
  const auto next=n+1<count?"chain"+std::to_string(n+1):"back";
  chain+=duplicate&&n+1<count?"\n br i1 %take,label %"+next+",label %"+next+"\n":"\n br label %"+next+"\n";
 }
 chain+="back:\n %wide=zext i32 %forward"+std::to_string(count-1)+" to i64\n br label %head";
 at=ir.find("back: br label %head");REQUIRE(at!=std::string::npos);ir.replace(at,std::string("back: br label %head").size(),chain);
 return ir;
}
std::string forwardedLoop(unsigned count,bool duplicate=false,const std::string &entry="",
                          const std::string &seed="%initial") {
 auto ir=relationForwardedLoop(count,duplicate,entry,seed);
 auto replace=[&](const std::string &from,const std::string &to){
  auto at=ir.find(from);REQUIRE(at!=std::string::npos);ir.replace(at,from.size(),to);
 };
 // The relation sees the same non-PHI step on both arms. Definedness must
 // still traverse every forwarding PHI feeding that step, as in the real
 // failing definition query. Keep the earlier relation-stress shape below.
 replace(" %next=add i32 %low,1"," %observed=add i32 %low,1");
 replace("store i32 %next,ptr","store i32 %observed,ptr");
 replace("icmp ult i32 %next,4","icmp ult i32 %observed,4");
 replace("done: ret i32 %next","done: ret i32 %observed");
 replace("[%forward"+std::to_string(count-1)+",%back]","[%next,%back]");
 replace("[%next,%head]","[%low,%head]");
 replace("back:\n %wide=zext i32 %forward"+std::to_string(count-1)+" to i64",
         "back:\n %next=add i32 %forward"+std::to_string(count-1)+",1\n %wide=zext i32 %next to i64");
 return ir;
}
}

TEST_CASE("Original PHI definition proof walks dominating forwarding chains iteratively","[phi-low-forward]") {
 for(unsigned count:{40u,80u}){
  INFO(count);Fixture x(forwardedLoop(count));auto *old=x.trunc();auto *small=x.phi("small");
  EffectSnapshot effects(x.f());auto *function=&x.f();PhiLowBitsOptions options;
  options.maxDepth=8;options.maxNodes=128;
  auto result=replacePhiLowTruncs(x.f(),{old},options);INFO(result.message);INFO(result.nodes);
  REQUIRE(result.changed);CHECK(result.complete);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.nodes>=count);CHECK(result.nodes<=options.maxNodes);
  CHECK(x.i("observed")->getOperand(0)==small);CHECK(x.trunc()==old);CHECK(&x.f()==function);
  effects.check(x.f());x.verify();
 }
}

TEST_CASE("Iterative PHI forwarding retains node and genuine recursive transfer caps","[phi-low-forward]") {
 SECTION("every forwarded original vertex still consumes the shared node cap"){
  Fixture x(forwardedLoop(40));PhiLowBitsOptions options;options.maxNodes=16;
  x.reject({x.trunc()},options,true);
 }
 SECTION("nonidentity arithmetic still consumes recursive depth"){
  std::string entry;std::string value="%initial";
  for(unsigned n=0;n<40;++n){auto next="%expression"+std::to_string(n);entry+=" "+next+"=add i32 "+value+",1\n";value=next;}
  Fixture x(forwardedLoop(40,false,entry,value));x.reject({x.trunc()},{},true);
 }
 SECTION("duplicate actual incoming slots are not single-edge identities"){
  Fixture x(forwardedLoop(40,true));x.reject({x.trunc()},{},true);
 }
 SECTION("a load or flagged leaf is not made defined by a short forwarding chain"){
  for(const std::string &step:{"load i32,ptr %memory,align 1","add nsw i32 %arg,1"}){
   auto ir=forwardedLoop(4);const std::string initial="freeze i32 %arg";auto at=ir.find(initial);REQUIRE(at!=std::string::npos);ir.replace(at,initial.size(),step);
   Fixture x(ir);x.reject({x.trunc()});
  }
 }
}

TEST_CASE("Original PHI relation proof walks checked representation identities iteratively","[phi-low-forward]") {
 for(unsigned count:{40u,80u}){
  INFO(count);Fixture x(relationForwardedLoop(count));auto *old=x.trunc();auto *small=x.phi("small");
  EffectSnapshot effects(x.f());PhiLowBitsOptions options;options.maxDepth=8;
  auto result=replacePhiLowTruncs(x.f(),{old},options);INFO(result.message);INFO(result.nodes);
  REQUIRE(result.changed);CHECK(result.complete);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.nodes>=count);CHECK(result.nodes<=options.maxNodes);
  CHECK(x.i("next")->getOperand(0)==small);CHECK(x.trunc()==old);
  effects.check(x.f());x.verify();
 }
}
