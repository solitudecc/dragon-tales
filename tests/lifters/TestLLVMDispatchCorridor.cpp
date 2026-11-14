#include <catch2/catch_test_macros.hpp>
#include <llvm/ADT/SCCIterator.h>
#include <dragon/lifters/llvm/DispatchCorridor.hpp>
#ifdef DRAGON_DISPATCH_CORRIDOR_TESTING
#include "../../lib/LLVMDispatchCorridor.hpp"
#endif
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <string>
using namespace dragon::llvmlifter::detail;
namespace {
// Unregistered private API: old product cannot link this TU. Verifier success
// is not an oracle for historical versus fresh values.
struct Fixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit Fixture(const std::string &ir) {
    llvm::SMDiagnostic error;
    module=llvm::parseAssemblyString("declare void @unknown()\ndeclare i32 @pers(...)\n"+ir,error,context);
    std::string msg; llvm::raw_string_ostream stream(msg); error.print("corridor",stream);
    INFO(msg); REQUIRE(module); verify();
  }
  llvm::Function &f(){return *module->getFunction("f");}
  llvm::BasicBlock *block(const char *name){for(auto &b:f())if(b.getName()==name)return &b;return nullptr;}
  llvm::Instruction *instruction(const char *name){for(auto &b:f())for(auto &i:b)if(i.getName()==name)return &i;return nullptr;}
  std::string text(){std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;}
  void verify(){std::string e;llvm::raw_string_ostream out(e);auto bad=llvm::verifyModule(*module,&out);INFO(e);REQUIRE_FALSE(bad);}
  DispatchCorridorEdge edge(const char *source,std::initializer_list<const char *> path,unsigned slot=0){
    DispatchCorridorEdge e{block(source),slot,{}};REQUIRE(e.source);
    for(auto *name:path){auto *b=block(name);REQUIRE(b);e.path.push_back(b);}return e;
  }
  void reject(const DispatchCorridorEdge &edge,const DispatchCorridorOptions &options={},bool budget=false){
    auto before=text();auto *fn=&f();auto *arg=f().getArg(0);
    auto s=threadDispatchCorridor(f(),{edge},options);INFO(s.message);
    CHECK_FALSE(s.changed);CHECK_FALSE(s.complete);CHECK(s.budgetExhausted==budget);
    CHECK(text()==before);CHECK(&f()==fn);CHECK(f().getArg(0)==arg);verify();
  }
};
const std::string historical=R"IR(
define i32 @f(ptr %guest,ptr %size,i1 %choose){
entry: br label %header
header:
  %state=phi i32 [0,%entry],[%next,%step]
  %old=phi i32 [10,%entry],[%new,%step]
  switch i32 %state,label %done [i32 0,label %body i32 1,label %body]
body: br i1 %choose,label %seed,label %other
seed: br label %join
other: br label %join
join:
  %next=phi i32 [1,%seed],[0,%other]
  %carried=phi i32 [%old,%seed],[5,%other]
  %new=add i32 %old,1
  store i32 %carried,ptr %guest,align 1
  %limit=load i32,ptr %size,align 1
  %fits=icmp ule i32 %new,%limit
  br i1 %fits,label %step,label %fault
step: br label %header
fault:
  %past=phi i32 [%old,%join]
  ret i32 %past
done: ret i32 %old
})IR";
std::string bits(const std::string &leaf="  %bit=and i32 %raw,1\n  %choice=xor i32 %bit,3",
                 const std::string &effect="",const std::string &other="0"){
 return R"IR(define i32 @f(ptr %guest,ptr %size,i32 %raw,i1 %choose){
entry: br i1 %choose,label %seed,label %other
seed:
)IR"+leaf+R"IR(
  br label %head
other: br label %head
head:
  %state=phi i32 [%choice,%seed],[)IR"+other+R"IR(,%other]
)IR"+effect+R"IR(
  store i32 %state,ptr %guest,align 1
  %limit=load i32,ptr %size,align 1
  %fits=icmp uge i32 %limit,4
  br i1 %fits,label %dispatch,label %fault
dispatch:
  switch i32 %state,label %error [i32 0,label %zero i32 2,label %two i32 3,label %three]
fault:
  %faultState=phi i32 [%state,%head]
  ret i32 %faultState
zero: ret i32 0
two:
  %twoState=phi i32 [%state,%dispatch]
  ret i32 %twoState
three:
  %threeState=phi i32 [%state,%dispatch]
  ret i32 %threeState
error: ret i32 -1
})IR";
}
}
TEST_CASE("Private ordered corridor keeps historical inputs distinct from fresh header","[dispatch-corridor]"){
 Fixture x(historical);auto *fn=&x.f();auto *arg=x.f().getArg(0);
 auto s=threadDispatchCorridor(x.f(),{x.edge("seed",{"join","step","header"})});INFO(s.message);
 REQUIRE(s.changed);CHECK(s.complete);x.verify();CHECK(&x.f()==fn);CHECK(x.f().getArg(0)==arg);
 CHECK(s.rewrittenEdges==1);CHECK(s.snapshots>=1);
 llvm::BinaryOperator *counter=nullptr;
 for(auto &b:x.f())for(auto &i:b)if(i.getName().starts_with("new.dispatch.edge"))counter=llvm::dyn_cast<llvm::BinaryOperator>(&i);
 REQUIRE(counter);auto *snapshot=llvm::dyn_cast<llvm::PHINode>(counter->getOperand(0));REQUIRE(snapshot);
 CHECK(snapshot->getName().starts_with("dispatch.old"));CHECK(snapshot->getNumIncomingValues()==1);
 CHECK(snapshot->getIncomingValue(0)!=counter);
 bool storeOld=false,faultOld=false;
 for(auto &b:x.f())for(auto &i:b){
  if(auto *st=llvm::dyn_cast<llvm::StoreInst>(&i);st&&b.getName().starts_with("join.dispatch.edge"))storeOld|=st->getValueOperand()==snapshot;
  if(auto *phi=llvm::dyn_cast<llvm::PHINode>(&i);phi&&i.getName()=="past")
   for(unsigned n=0;n!=phi->getNumIncomingValues();++n)if(phi->getIncomingBlock(n)->getName().starts_with("join.dispatch.edge"))faultOld|=phi->getIncomingValue(n)==snapshot;
 }
 CHECK(storeOld);CHECK(faultOld);
}
TEST_CASE("Private ordered corridor consumes the bit selector at the old switch point","[dispatch-corridor]"){
 Fixture x(bits());auto s=threadDispatchCorridor(x.f(),{x.edge("seed",{"head","dispatch"})});INFO(s.message);REQUIRE(s.changed);x.verify();
 CHECK(llvm::cast<llvm::BranchInst>(x.block("seed")->getTerminator())->isUnconditional());
 unsigned choices=0,reads=0,stores=0;
 for(auto &b:x.f())for(auto &i:b){
  if(i.getName().starts_with("dispatch.choose")){
   ++choices;CHECK(b.getName().starts_with("dispatch.dispatch.edge"));auto *cmp=llvm::dyn_cast<llvm::ICmpInst>(&i);REQUIRE(cmp);
   auto *observed=llvm::dyn_cast<llvm::PHINode>(cmp->getOperand(0));REQUIRE(observed);CHECK(observed->getName().starts_with("dispatch.old"));
  }
  if(b.getName().starts_with("head.dispatch.edge")){reads+=llvm::isa<llvm::LoadInst>(i);stores+=llvm::isa<llvm::StoreInst>(i);}
 }
 CHECK(choices==1);CHECK(reads==1);CHECK(stores==1);
}
TEST_CASE("Private ordered corridor counts replacement successor slots for exit PHIs","[dispatch-corridor]"){
 SECTION("constant return remains seven after LLVM cleanup"){
 Fixture x(R"IR(define i32 @f(ptr %guest,i1 %choose){
entry: br i1 %choose,label %seed,label %other
seed: br label %head
other: br label %head
head:
 %state=phi i32 [2,%seed],[0,%other]
 switch i32 %state,label %exit [i32 2,label %exit i32 3,label %exit]
exit:
 %result=phi i32 [7,%head],[7,%head],[7,%head]
 ret i32 %result
})IR");
 auto s=threadDispatchCorridor(x.f(),{x.edge("seed",{"head"})});INFO(s.message);REQUIRE(s.changed);x.verify();
 // LLVM folds this original all-seven/all-exit case. Keep its source as a
 // compatibility control; the nonconstant case below must retain the PHI.
 auto *ret=llvm::dyn_cast<llvm::ReturnInst>(x.block("exit")->getTerminator());REQUIRE(ret);
 auto *value=llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue());REQUIRE(value);CHECK(value->equalsInt(7));
 }
 SECTION("nonconstant nondefault edges retain exact values and multiplicity"){
 Fixture x(R"IR(define i32 @f(ptr %guest,i32 %raw,i1 %choose){
entry: br i1 %choose,label %seed,label %other
seed: br label %head
other: br label %head
head:
 %state=phi i32 [2,%seed],[%raw,%other]
 switch i32 %state,label %fallback [i32 2,label %exit i32 4,label %exit i32 6,label %exit i32 0,label %zero]
exit:
 %result=phi i32 [%state,%head],[%state,%head],[%state,%head]
 ret i32 %result
zero: ret i32 0
fallback: ret i32 -1
})IR");
 auto s=threadDispatchCorridor(x.f(),{x.edge("seed",{"head"})});INFO(s.message);REQUIRE(s.changed);x.verify();
 INFO(x.text());
 auto *phi=llvm::dyn_cast_or_null<llvm::PHINode>(x.instruction("result"));REQUIRE(phi);
 CHECK(phi->getNumIncomingValues()==4);
 unsigned actual=0,cloned=0,original=0;
 for(auto &block:x.f())for(auto *next:llvm::successors(&block))actual+=next==phi->getParent();
 CHECK(actual==4);CHECK(actual==phi->getNumIncomingValues());
 for(unsigned n=0;n!=phi->getNumIncomingValues();++n){
  auto *value=phi->getIncomingValue(n);
  if(phi->getIncomingBlock(n)->getName().starts_with("head.dispatch.edge")){
   ++cloned;auto *constant=llvm::dyn_cast<llvm::ConstantInt>(value);REQUIRE(constant);CHECK(constant->equalsInt(2));
  }else{
   ++original;
   if(auto *remaining=llvm::dyn_cast<llvm::PHINode>(value)){
    REQUIRE(remaining->getNumIncomingValues()==1);CHECK(remaining->getIncomingValue(0)==x.f().getArg(1));
   }else CHECK(value==x.f().getArg(1));
  }
 }
 CHECK(cloned==1);CHECK(original==3);
 }
}
TEST_CASE("Private ordered corridor keeps refused edges in finite original scheduling","[dispatch-corridor]"){
 Fixture x(bits("  %choice=add i32 2,0","","%raw"));
 auto seed=x.edge("seed",{"head","dispatch"}),other=x.edge("other",{"head","dispatch"});
 auto s=threadDispatchCorridor(x.f(),{other,seed});INFO(s.message);REQUIRE(s.changed);CHECK_FALSE(s.complete);x.verify();
 CHECK(s.candidatesVisited==2);CHECK(s.rewrittenEdges==1);CHECK(s.rejected==1);
 CHECK(llvm::cast<llvm::BranchInst>(x.block("other")->getTerminator())->getSuccessor(0)==x.block("head"));
}
TEST_CASE("Private ordered corridor rejects unsafe effects and unknown state exactly","[dispatch-corridor]"){
 for(const std::string &effect:{" call void @unknown()"," %v=load volatile i32,ptr %guest,align 1",
   " %v=load atomic i32,ptr %guest monotonic,align 4"," fence seq_cst"," %v=freeze i32 %raw"}){
  INFO(effect);Fixture x(bits(" %choice=add i32 2,0",effect));x.reject(x.edge("seed",{"head","dispatch"}));
 }
 for(const std::string &leaf:{" %choice=add i32 %raw,1"," %choice=add i32 poison,2"," %choice=add i32 undef,2",
   " %bit=and i32 %raw,1\n %choice=shl i32 %bit,32"," %bit=and i32 %raw,1\n %choice=add nsw i32 %bit,2147483647",
   " %bit=and i32 %raw,1\n %choice=and i32 %bit,0"}){
  INFO(leaf);Fixture x(bits(leaf));x.reject(x.edge("seed",{"head","dispatch"}));
 }
 SECTION("blockaddress"){Fixture x("@address=global ptr blockaddress(@f,%head)\n"+bits());x.reject(x.edge("seed",{"head","dispatch"}));}
 SECTION("EH identity"){auto ir=bits();auto at=ir.find("){");REQUIRE(at!=std::string::npos);ir.insert(at+1," personality ptr @pers ");Fixture x(ir);x.reject(x.edge("seed",{"head","dispatch"}));}
}
TEST_CASE("Private ordered corridor prepays shared copy and SSA repair limits","[dispatch-corridor]"){
 for(unsigned n=0;n!=4;++n){
  Fixture x(historical);DispatchCorridorOptions options;
  if(n==0)options.maxClonedBlocks=1;if(n==1)options.maxClonedInstructions=1;
  if(n==2)options.maxRepairCells=1;if(n==3)options.maxWork=1;
  x.reject(x.edge("seed",{"join","step","header"}),options,true);
 }
}

TEST_CASE("Private ordered corridor covers every frozen loop edge without copying copies","[dispatch-corridor]"){
 Fixture x(historical);
 auto a=x.edge("seed",{"join","step","header"}),b=x.edge("other",{"join","step","header"});
 auto s=threadDispatchCorridor(x.f(),{b,a});INFO(s.message);REQUIRE(s.changed);CHECK(s.complete);x.verify();
 CHECK(s.candidatesVisited==2);CHECK(s.rewrittenEdges==2);
 unsigned switches=0;
 for(auto &block:x.f())if(llvm::isa<llvm::SwitchInst>(block.getTerminator())){
  ++switches;
  // The original switch may remain only as the first-entry path.
  CHECK(llvm::pred_size(&block)==1);
 }
 CHECK(switches<=1);
 for(auto it=llvm::scc_begin(&x.f());!it.isAtEnd();++it){
  const auto &component=*it;bool cyclic=component.size()>1;
  if(!cyclic)for(auto *next:llvm::successors(component.front()))cyclic|=next==component.front();
  if(cyclic)for(auto *block:component)CHECK_FALSE(llvm::isa<llvm::SwitchInst>(block->getTerminator()));
 }
}
TEST_CASE("Private ordered corridor refuses an unproved post-store state read","[dispatch-corridor]"){
 Fixture x(R"IR(define i32 @f(ptr %guest,ptr %unknown,i32 %raw,i1 %choose){
entry: br i1 %choose,label %seed,label %other
seed: br label %head
other: br label %head
head:
 %value=phi i32 [2,%seed],[0,%other]
 store i32 %value,ptr %guest,align 1
 store i32 %raw,ptr %unknown,align 1
 %state=load i32,ptr %guest,align 1
 switch i32 %state,label %error [i32 2,label %two]
two: ret i32 2
error: ret i32 -1
})IR");
 x.reject(x.edge("seed",{"head"}));
}

TEST_CASE("Private ordered corridor refuses historical numeric state proof without future folding","[dispatch-corridor][dispatch-corridor-history-proof]"){
 for(bool beforeSeed:{false,true}){
  INFO(beforeSeed);
  const std::string seed=beforeSeed?"seed:\n %before=add i32 %history,1\n br label %join\n":"seed: br label %join\n";
  const std::string next=beforeSeed?" %next=add i32 %before,0\n":" %next=add i32 %history,1\n";
  Fixture x(R"IR(define i32 @f(i1 %choose){
entry: br label %header
header:
 %state=phi i32 [0,%entry],[%next,%step]
 %history=phi i32 [10,%entry],[20,%step]
 switch i32 %state,label %error [i32 0,label %seed i32 11,label %eleven i32 21,label %twentyone]
)IR"+seed+"join:\n"+next+R"IR( br label %step
step: br label %header
eleven: ret i32 11
twentyone: ret i32 21
error: ret i32 -1
})IR");
  // Old history is 10 on this selected edge; a future-header expansion would
  // incorrectly certify 21 rather than the actual 11. New scope refuses the
  // historical numeric proof entirely. The source itself remains executable.
  x.reject(x.edge("seed",{"join","step","header"}));
 }
}
TEST_CASE("Private ordered corridor still admits a full bit domain over historical data","[dispatch-corridor][dispatch-corridor-history-proof]"){
 Fixture x(R"IR(define i32 @f(i1 %choose){
entry: br label %header
header:
 %state=phi i32 [0,%entry],[%next,%step]
 %history=phi i32 [10,%entry],[20,%step]
 switch i32 %state,label %error [i32 0,label %seed i32 11,label %eleven i32 12,label %twelve]
seed: br label %join
join:
 %atom=and i32 %history,1
 %next=add i32 %atom,11
 br label %step
step: br label %header
eleven: ret i32 11
twelve: ret i32 12
error: ret i32 -1
})IR");
 auto result=threadDispatchCorridor(x.f(),{x.edge("seed",{"join","step","header"})});
 INFO(result.message);REQUIRE(result.changed);x.verify();
 auto *atom=llvm::dyn_cast<llvm::BinaryOperator>(x.instruction("atom.dispatch.edge"));REQUIRE(atom);
 auto *snapshot=llvm::dyn_cast<llvm::PHINode>(atom->getOperand(0));REQUIRE(snapshot);
 CHECK(snapshot->getName().starts_with("dispatch.old"));
 for(auto &b:x.f())for(auto &i:b)if(i.getName().starts_with("dispatch.choose"))
  CHECK(b.getName().starts_with("header.dispatch.edge"));
}
TEST_CASE("Private ordered corridor preserves fastcc and recursive musttail identity","[dispatch-corridor][dispatch-corridor-transaction]"){
 Fixture x(R"IR(define fastcc i32 @f(i32 noundef %arg,i1 %choose) nounwind{
entry: br i1 %choose,label %seed,label %other
seed: br label %head
other: br label %head
head:
 %state=phi i32 [0,%seed],[1,%other]
 switch i32 %state,label %tailone [i32 0,label %tailzero]
tailzero:
 %a=musttail call fastcc i32 @f(i32 noundef %arg,i1 false)
 ret i32 %a
tailone:
 %b=musttail call fastcc i32 @f(i32 noundef %arg,i1 true)
 ret i32 %b
})IR");
 auto *function=&x.f();auto *arg=x.f().getArg(0);
 auto result=threadDispatchCorridor(x.f(),{x.edge("seed",{"head"})});INFO(result.message);
 REQUIRE(result.changed);x.verify();CHECK(&x.f()==function);CHECK(x.f().getArg(0)==arg);
 CHECK(x.f().getCallingConv()==llvm::CallingConv::Fast);CHECK(x.f().hasFnAttribute(llvm::Attribute::NoUnwind));
 unsigned recursive=0;
 for(auto &b:x.f())for(auto &i:b)if(auto *call=llvm::dyn_cast<llvm::CallInst>(&i)){
  ++recursive;CHECK(call->isMustTailCall());CHECK(call->getCalledFunction()==function);
  CHECK(call->getCallingConv()==llvm::CallingConv::Fast);
 }
 CHECK(recursive==2);
}
TEST_CASE("Private ordered corridor rejects unsupported unnamed and foreign block identities exactly","[dispatch-corridor][dispatch-corridor-transaction]"){
 SECTION("unnamed function"){
  Fixture x(bits(" %choice=add i32 2,0"));auto edge=x.edge("seed",{"head","dispatch"});
  auto *function=&x.f();auto *argument=function->getArg(0);function->setName("");
  auto before=x.text();auto result=threadDispatchCorridor(*function,{edge});INFO(result.message);
  CHECK_FALSE(result.changed);CHECK_FALSE(result.complete);CHECK(x.text()==before);
  CHECK_FALSE(function->hasName());CHECK(function->getArg(0)==argument);x.verify();
 }
 SECTION("foreign blockaddress inside ConstantExpr"){
  auto ir=bits(" %choice=add i32 2,0");auto at=ir.find("entry: br");REQUIRE(at!=std::string::npos);
  ir.replace(at,std::string("entry: br").size(),
   "entry:\n %foreign=add i64 ptrtoint (ptr blockaddress(@otherfn,%target) to i64),0\n br");
  Fixture x("define void @otherfn(){entry: br label %target\ntarget: ret void}\n"+ir);
  x.reject(x.edge("seed",{"head","dispatch"}));
 }
 SECTION("recursive call inside copied path"){
  Fixture x(bits(" %choice=add i32 2,0"," %recursive=call i32 @f(ptr %guest,ptr %size,i32 %raw,i1 %choose)"));
  x.reject(x.edge("seed",{"head","dispatch"}));
 }
}
#ifdef DRAGON_DISPATCH_CORRIDOR_TESTING
TEST_CASE("Private ordered corridor restores named metadata on controlled commit failure","[dispatch-corridor][dispatch-corridor-transaction]"){
 for(unsigned stage:{1u,2u}){
  INFO(stage);
  Fixture x(bits(" %choice=add i32 2,0")+"\n!sentinel = !{!0}\n!0 = !{i32 7}\n");
  auto *function=&x.f();auto *argument=function->getArg(0);
  auto *head=x.block("head");auto *state=x.instruction("state");
  auto *sentinel=x.module->getNamedMetadata("sentinel");
  REQUIRE(sentinel);REQUIRE_FALSE(x.module->getNamedMetadata("llvm.dbg.cu"));
  auto edge=x.edge("seed",{"head","dispatch"});auto before=x.text();
  auto result=threadDispatchCorridorCommitFailureForTest(x.f(),{edge},stage);INFO(result.message);
  CHECK_FALSE(result.changed);CHECK_FALSE(result.complete);CHECK(x.text()==before);
  CHECK(&x.f()==function);CHECK(x.f().getArg(0)==argument);CHECK(x.block("head")==head);
  CHECK(x.instruction("state")==state);CHECK(x.module->getNamedMetadata("sentinel")==sentinel);
  CHECK_FALSE(x.module->getNamedMetadata("llvm.dbg.cu"));x.verify();
 }
}
#endif

TEST_CASE("Private ordered corridor rejects foreign blockaddress wrapped in metadata","[dispatch-corridor][dispatch-corridor-transaction]"){
 auto ir=bits(" %choice=add i32 2,0");auto at=ir.find("entry: br");REQUIRE(at!=std::string::npos);
 ir.replace(at,std::string("entry: br").size(),"entry:\n %marker=add i32 1,0, !capture !0\n br");
 Fixture x("define void @otherfn(){entry: br label %target\ntarget: ret void}\n"+ir+
   "\n!0 = !{ptr blockaddress(@otherfn,%target)}\n");
 x.reject(x.edge("seed",{"head","dispatch"}));
}
