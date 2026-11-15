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

namespace {
// This is a separate, atomic API. Absence of the new symbol on an old SDK is
// not evidence of incorrect execution by the old per-edge API.
void rejectBatch(Fixture &x,const std::vector<DispatchCorridorEdge> &edges,
                 const DispatchCorridorBatchOptions &options={},bool budget=false){
 auto before=x.text();auto *function=&x.f();auto *argument=x.f().getArg(0);
 std::vector<llvm::BasicBlock *> blocks;
 std::vector<llvm::Instruction *> instructions;
 for(auto &b:x.f()){blocks.push_back(&b);for(auto &i:b)instructions.push_back(&i);}
 auto *metadata=x.module->getNamedMetadata("sentinel");
 auto result=threadDispatchCorridorBatch(x.f(),edges,options);INFO(result.message);
 CHECK_FALSE(result.changed);CHECK_FALSE(result.complete);
 CHECK(result.budgetExhausted==budget);CHECK(result.rejected==edges.size());
 CHECK(result.rewrittenEdges==0);CHECK(result.clonedBlocks==0);
 CHECK(result.clonedInstructions==0);CHECK(result.snapshots==0);
 CHECK(result.repairPhis==0);CHECK(result.repairCells==0);
 CHECK(x.text()==before);CHECK(&x.f()==function);CHECK(x.f().getArg(0)==argument);
 CHECK(x.module->getNamedMetadata("sentinel")==metadata);
 unsigned nb=0,ni=0;
 for(auto &b:x.f()){REQUIRE(nb<blocks.size());CHECK(&b==blocks[nb++]);
  for(auto &i:b){REQUIRE(ni<instructions.size());CHECK(&i==instructions[ni++]);}}
 CHECK(nb==blocks.size());CHECK(ni==instructions.size());x.verify();
}
std::string batchLoop(unsigned count){
 std::string ir="define i32 @f(ptr %guest,ptr %size){\nentry: br label %head\nhead:\n %state=phi i32 [0,%entry]";
 for(unsigned n=0;n<count;++n)ir+=",["+std::to_string((n+1)%count)+",%seed"+std::to_string(n)+"]";
 ir+="\n %counter=phi i32 [0,%entry]";
 for(unsigned n=0;n<count;++n)ir+=",[%advanced,%seed"+std::to_string(n)+"]";
 ir+="\n %advanced=add i32 %counter,1\n store i32 %advanced,ptr %guest,align 1\n"
     " %limit=load i32,ptr %size,align 1\n %fits=icmp ule i32 %advanced,%limit\n"
     " br i1 %fits,label %dispatch,label %fault\ndispatch:\n"
     " switch i32 %state,label %error [";
 for(unsigned n=0;n<count;++n)ir+="i32 "+std::to_string(n)+",label %seed"+std::to_string(n)+" ";
 ir+="]\n";
 for(unsigned n=0;n<count;++n)ir+="seed"+std::to_string(n)+": br label %head\n";
 ir+="fault:\n %faultCounter=phi i32 [%advanced,%head]\n ret i32 %faultCounter\n"
     "error: ret i32 %advanced\n}\n";
 return ir;
}
std::vector<DispatchCorridorEdge> batchLoopEdges(Fixture &x,unsigned count){
 std::vector<DispatchCorridorEdge> edges;
 // Deliberately reverse provider order. The implementation must use frozen
 // original block/slot order and must never enqueue newly generated copies.
 for(unsigned n=count;n!=0;--n){auto name="seed"+std::to_string(n-1);
  edges.push_back(x.edge(name.c_str(),{"head","dispatch"}));}
 return edges;
}
void noCyclicSwitch(Fixture &x){
 for(auto it=llvm::scc_begin(&x.f());!it.isAtEnd();++it){
  const auto &component=*it;bool cyclic=component.size()>1;
  if(!cyclic)for(auto *next:llvm::successors(component.front()))cyclic|=next==component.front();
  if(cyclic)for(auto *block:component)
   CHECK_FALSE(llvm::isa<llvm::SwitchInst>(block->getTerminator()));
 }
}
std::string boundedBatchHead(const std::string &condition=" %fits=icmp ult i32 %state,4",
                             const std::string &effect=""){
 return R"IR(define i32 @f(ptr %guest,i32 %raw,i1 %choose){
entry: br i1 %choose,label %seed,label %other
seed: br label %head
other: br label %head
head:
 %state=phi i32 [2,%seed],[0,%other],[%state,%head]
)IR"+effect+"\n"+condition+R"IR(
 br i1 %fits,label %dispatch,label %head
dispatch:
 switch i32 %state,label %error [i32 0,label %zero i32 2,label %two]
zero: ret i32 0
two: ret i32 2
error: ret i32 -1
})IR";
}
}

TEST_CASE("Atomic dispatch batch covers more than sixty four original loop edges","[dispatch-corridor-batch]"){
 SECTION("empty batch is an exact no-op"){
  Fixture x(batchLoop(2));auto before=x.text();auto *function=&x.f();
  auto result=threadDispatchCorridorBatch(x.f(),{});INFO(result.message);
  CHECK_FALSE(result.changed);CHECK(result.complete);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.candidatesVisited==0);CHECK(result.rejected==0);CHECK(result.rewrittenEdges==0);
  CHECK(x.text()==before);CHECK(&x.f()==function);x.verify();
 }
 SECTION("seventy shared corridors repair every original definition together"){
  constexpr unsigned count=70;Fixture x(batchLoop(count));
  auto *function=&x.f();auto *argument=x.f().getArg(0);
  auto result=threadDispatchCorridorBatch(x.f(),batchLoopEdges(x,count));INFO(result.message);
  REQUIRE(result.changed);CHECK(result.complete);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.candidatesVisited==count);CHECK(result.rewrittenEdges==count);CHECK(result.rejected==0);
  CHECK(result.clonedBlocks>64);CHECK(result.snapshots>=count);
  CHECK(&x.f()==function);CHECK(x.f().getArg(0)==argument);x.verify();noCyclicSwitch(x);
  unsigned counters=0,stores=0,loads=0;
  for(auto &b:x.f())for(auto &i:b){
   if(i.getName().starts_with("advanced.dispatch.edge")){
    ++counters;auto *add=llvm::dyn_cast<llvm::BinaryOperator>(&i);REQUIRE(add);
    auto *old=llvm::dyn_cast<llvm::PHINode>(add->getOperand(0));REQUIRE(old);
    CHECK(old->getName().starts_with("dispatch.old"));CHECK(old->getNumIncomingValues()==1);
    CHECK(old->getIncomingValue(0)!=add);
   }
   if(b.getName().starts_with("head.dispatch.edge")){
    stores+=llvm::isa<llvm::StoreInst>(i);loads+=llvm::isa<llvm::LoadInst>(i);
   }
  }
  CHECK(counters==count);CHECK(stores==count);CHECK(loads==count);
 }
}

namespace {
std::string carriedBatchLoop(unsigned count) {
 std::string ir="define i32 @f(ptr %guest,ptr %size,i32 %raw){\nentry:\n";
 for(unsigned j=0;j<20;++j)
  ir+=" %initial"+std::to_string(j)+"=xor i32 %raw,"+std::to_string(0x9e37u*(j+1))+"\n";
 ir+=" br label %head\nhead:\n %state=phi i32 [0,%entry]";
 for(unsigned n=0;n<count;++n)ir+=",["+std::to_string((n+1)%count)+",%seed"+std::to_string(n)+"]";
 ir+="\n %counter=phi i32 [0,%entry]";
 for(unsigned n=0;n<count;++n)ir+=",[%advanced,%seed"+std::to_string(n)+"]";
 for(unsigned j=0;j<20;++j){
  const auto id=std::to_string(j);ir+="\n %carried"+id+"=phi i32 [%initial"+id+",%entry]";
  for(unsigned n=0;n<count;++n)ir+=",[%next"+std::to_string(n)+"_"+id+",%seed"+std::to_string(n)+"]";
 }
 ir+="\n %advanced=add i32 %counter,1\n %stamp=xor i32 %carried0,%carried1\n"
     " store i32 %stamp,ptr %guest,align 1\n %limit=load i32,ptr %size,align 1\n"
     " %fits=icmp ule i32 %advanced,%limit\n %bounded=icmp ule i32 %advanced,256\n"
     " %continue=and i1 %fits,%bounded\n br i1 %continue,label %dispatch,label %fault\n"
     "dispatch: switch i32 %state,label %error [";
 for(unsigned n=0;n<count;++n)ir+="i32 "+std::to_string(n)+",label %seed"+std::to_string(n)+" ";
 ir+="]\n";
 for(unsigned n=0;n<count;++n){
  const auto id=std::to_string(n);ir+="seed"+id+":\n";
  for(unsigned j=0;j<20;++j)
   ir+=" %next"+id+"_"+std::to_string(j)+"=add i32 %carried"+std::to_string(j)+","+std::to_string(n+j+1)+"\n";
  ir+=" br label %head\n";
 }
 ir+="fault:\n %result0=xor i32 %advanced,%carried0\n";
 for(unsigned j=1;j<20;++j)
  ir+=" %result"+std::to_string(j)+"=xor i32 %result"+std::to_string(j-1)+",%carried"+std::to_string(j)+"\n";
 ir+=" ret i32 %result19\nerror: ret i32 %advanced\n}\n";
 return ir;
}
std::string useCapBatchLoop(unsigned stores) {
 std::string ir=R"IR(define i32 @f(ptr %guest){
entry: br label %head
head:
 %state=phi i32 [2,%entry],[2,%again]
 %carried=phi i32 [11,%entry],[%next,%again]
 %next=add i32 %carried,1
 switch i32 %state,label %error [i32 2,label %again]
again:
)IR";
 for(unsigned n=0;n<stores;++n)ir+=" store i32 %carried,ptr %guest,align 1\n";
 return ir+" br label %head\nerror: ret i32 0\n}\n";
}
}

TEST_CASE("Atomic dispatch batch shares complete structural use scans within fixed work","[dispatch-batch-use-reuse]"){
 constexpr unsigned count=70;Fixture x(carriedBatchLoop(count));
 DispatchCorridorBatchOptions options;options.maxWork=180000;
 auto result=threadDispatchCorridorBatch(x.f(),batchLoopEdges(x,count),options);
 INFO(result.message);INFO(result.work);
 REQUIRE(result.changed);CHECK(result.complete);CHECK_FALSE(result.budgetExhausted);
 CHECK(result.rewrittenEdges==count);CHECK(result.work<=options.maxWork);
 x.verify();noCyclicSwitch(x);
}

TEST_CASE("Atomic dispatch batch reapplies use caps with each historical snapshot footprint","[dispatch-batch-use-reuse]"){
 SECTION("a complete first plan fits the original external-use cap"){
  Fixture x(useCapBatchLoop(8191));
  auto result=threadDispatchCorridorBatch(x.f(),{x.edge("entry",{"head"})});
  INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);x.verify();
 }
 SECTION("a later plan's snapshot still exceeds the cap after sharing use scans"){
  Fixture x(useCapBatchLoop(8191));
  rejectBatch(x,{x.edge("entry",{"head"}),x.edge("again",{"head"})},{},true);
 }
 SECTION("a refused earlier batch cannot donate stale use scans to another invocation"){
  Fixture x(useCapBatchLoop(8190));DispatchCorridorBatchOptions tooSmall;tooSmall.maxRepairCells=1;
  rejectBatch(x,{x.edge("entry",{"head"})},tooSmall,true);
  auto *before=x.block("again")->getTerminator();
  for(unsigned n=0;n<3;++n)new llvm::StoreInst(x.instruction("carried"),x.f().getArg(0),before);
  x.verify();rejectBatch(x,{x.edge("entry",{"head"})},{},true);
 }
}

TEST_CASE("Atomic dispatch batch repairs repeated actual edge and exit slots","[dispatch-corridor-batch]"){
 Fixture x(R"IR(define i32 @f(ptr %guest,i32 %raw,i1 %choose){
entry: br i1 %choose,label %seed,label %other
seed: br i1 %choose,label %head,label %head
other: br label %head
head:
 %state=phi i32 [2,%seed],[2,%seed],[%raw,%other]
 switch i32 %state,label %fallback [i32 2,label %exit i32 4,label %exit i32 6,label %exit i32 0,label %zero]
exit:
 %result=phi i32 [%state,%head],[%state,%head],[%state,%head]
 ret i32 %result
zero: ret i32 0
fallback: ret i32 -1
})IR");
 auto a=x.edge("seed",{"head"},0),b=x.edge("seed",{"head"},1);
 auto result=threadDispatchCorridorBatch(x.f(),{b,a});INFO(result.message);
 REQUIRE(result.changed);CHECK(result.complete);CHECK(result.rewrittenEdges==2);x.verify();
 auto *branch=llvm::cast<llvm::BranchInst>(x.block("seed")->getTerminator());
 REQUIRE(branch->isConditional());CHECK(branch->getSuccessor(0)!=branch->getSuccessor(1));
 auto *phi=llvm::dyn_cast_or_null<llvm::PHINode>(x.instruction("result"));REQUIRE(phi);
 unsigned actual=0,original=0,copied=0;
 for(auto &block:x.f())for(auto *next:llvm::successors(&block))actual+=next==phi->getParent();
 CHECK(actual==5);CHECK(phi->getNumIncomingValues()==actual);
 for(unsigned n=0;n<phi->getNumIncomingValues();++n){
  if(phi->getIncomingBlock(n)->getName().starts_with("head.dispatch.edge")){
   ++copied;auto *value=llvm::dyn_cast<llvm::ConstantInt>(phi->getIncomingValue(n));
   REQUIRE(value);CHECK(value->equalsInt(2));
  }else ++original;
 }
 CHECK(original==3);CHECK(copied==2);
}

TEST_CASE("Atomic dispatch batch keeps historical errors and effects in their original order","[dispatch-corridor-batch]"){
 Fixture x(historical);auto result=threadDispatchCorridorBatch(x.f(),{
   x.edge("other",{"join","step","header"}),x.edge("seed",{"join","step","header"})});
 INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);CHECK(result.rewrittenEdges==2);
 x.verify();noCyclicSwitch(x);
 unsigned corridors=0,faultEdges=0;
 for(auto &b:x.f())if(b.getName().starts_with("join.dispatch.edge")){
  ++corridors;llvm::StoreInst *store=nullptr;llvm::LoadInst *load=nullptr;llvm::ICmpInst *compare=nullptr;
  for(auto &i:b){if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i))store=s;
   if(auto *l=llvm::dyn_cast<llvm::LoadInst>(&i))load=l;
   if(auto *c=llvm::dyn_cast<llvm::ICmpInst>(&i))compare=c;}
  REQUIRE(store);REQUIRE(load);REQUIRE(compare);
  CHECK(store->comesBefore(load));CHECK(load->comesBefore(compare));
  auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());REQUIRE(branch);
  CHECK(branch->isConditional());CHECK(branch->getCondition()==compare);
  auto *counter=llvm::dyn_cast<llvm::BinaryOperator>(compare->getOperand(0));REQUIRE(counter);
  auto *snapshot=llvm::dyn_cast<llvm::PHINode>(counter->getOperand(0));REQUIRE(snapshot);
  CHECK(snapshot->getName().starts_with("dispatch.old"));CHECK(snapshot->getIncomingValue(0)!=counter);
  auto *past=llvm::dyn_cast_or_null<llvm::PHINode>(x.instruction("past"));REQUIRE(past);
  for(unsigned n=0;n<past->getNumIncomingValues();++n)if(past->getIncomingBlock(n)==&b){
   ++faultEdges;CHECK(past->getIncomingValue(n)==snapshot);
  }
 }
 CHECK(corridors==2);CHECK(faultEdges==2);
 CHECK(llvm::cast<llvm::BranchInst>(x.block("seed")->getTerminator())->isUnconditional());
 CHECK(llvm::cast<llvm::BranchInst>(x.block("other")->getTerminator())->isUnconditional());
}

TEST_CASE("Atomic dispatch batch refuses the whole request after any invalid original candidate","[dispatch-corridor-batch]"){
 for(bool reversed:{false,true}){
  INFO(reversed);Fixture x(bits(" %choice=add i32 2,0","","%raw"));
  auto valid=x.edge("seed",{"head","dispatch"}),unknown=x.edge("other",{"head","dispatch"});
  rejectBatch(x,reversed?std::vector<DispatchCorridorEdge>{unknown,valid}:std::vector<DispatchCorridorEdge>{valid,unknown});
 }
 SECTION("duplicate original successor slot is not silently deduplicated"){
  Fixture x(batchLoop(2));auto a=x.edge("seed0",{"head","dispatch"});rejectBatch(x,{a,a});
 }
 SECTION("late malformed path leaves the earlier valid path unmodified"){
  Fixture x(batchLoop(2));auto a=x.edge("seed0",{"head","dispatch"});
  auto b=x.edge("seed1",{"head","dispatch"});b.successorSlot=1;rejectBatch(x,{a,b});
 }
 SECTION("candidate sources inside another path are rejected before copying"){
  Fixture x(R"IR(define i32 @f(ptr %guest){
entry: br label %seed
seed: br label %head
head: br label %middle
middle: br label %dispatch
dispatch: switch i32 2,label %error [i32 2,label %done]
done: ret i32 2
error: ret i32 -1
})IR");
  rejectBatch(x,{x.edge("seed",{"head","middle","dispatch"}),x.edge("head",{"middle","dispatch"})});
 }
}

TEST_CASE("Atomic dispatch batch does not move poison consumption ahead of errors","[dispatch-corridor-batch]"){
 for(const std::string &leaf:{
   " %choice=select i1 %choose,i32 2,i32 2",
   " %bit=and i32 %raw,1\n %choice=mul i32 %bit,0",
   " %choice=add i32 poison,2"," %choice=add i32 undef,2",
   " %choice=shl i32 2,32"," %choice=add nsw i32 2147483647,1"}){
  INFO(leaf);Fixture x(bits(leaf));rejectBatch(x,{x.edge("seed",{"head","dispatch"})});
 }
 for(const std::string &effect:{" call void @unknown()"," %v=load volatile i32,ptr %guest,align 1",
   " %v=load atomic i32,ptr %guest monotonic,align 4"," fence seq_cst"}){
  INFO(effect);Fixture x(bits(" %choice=add i32 2,0",effect));
  rejectBatch(x,{x.edge("seed",{"head","dispatch"}),x.edge("other",{"head","dispatch"})});
 }
}

TEST_CASE("Atomic dispatch batch consumes both state values only at the original switch","[dispatch-corridor-batch]"){
 Fixture x(bits());auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});
 INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);x.verify();
 CHECK(llvm::cast<llvm::BranchInst>(x.block("seed")->getTerminator())->isUnconditional());
 unsigned choices=0,corridors=0;
 for(auto &b:x.f()){
  if(b.getName().starts_with("head.dispatch.edge")){
   ++corridors;llvm::StoreInst *store=nullptr;llvm::LoadInst *load=nullptr;
   for(auto &i:b){if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i))store=s;
    if(auto *l=llvm::dyn_cast<llvm::LoadInst>(&i))load=l;}
   REQUIRE(store);REQUIRE(load);CHECK(store->comesBefore(load));
   auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());REQUIRE(branch);
   CHECK(branch->isConditional());CHECK(branch->getSuccessor(1)==x.block("fault"));
  }
  for(auto &i:b)if(i.getName().starts_with("dispatch.choose")){
   ++choices;CHECK(b.getName().starts_with("dispatch.dispatch.edge"));
   auto *compare=llvm::dyn_cast<llvm::ICmpInst>(&i);REQUIRE(compare);
   auto *observed=llvm::dyn_cast<llvm::PHINode>(compare->getOperand(0));REQUIRE(observed);
   CHECK(observed->getName().starts_with("dispatch.old"));
  }
 }
 CHECK(choices==1);CHECK(corridors==1);
}

TEST_CASE("Atomic dispatch batch versions mapped PHI chains across different historical inputs","[dispatch-corridor-batch]"){
 Fixture x(R"IR(define i32 @f(ptr %guest,ptr %size,i1 %choose){
entry: br label %header
header:
 %state=phi i32 [0,%entry],[%next,%step]
 %history=phi i32 [10,%entry],[%new,%step]
 %second=phi i32 [40,%entry],[%newSecond,%step]
 switch i32 %state,label %done [i32 0,label %body i32 1,label %body]
body: br i1 %choose,label %seedA,label %seedB
seedA: br label %join
seedB: br label %join
join:
 %next=phi i32 [1,%seedA],[0,%seedB]
 %mapped=phi i32 [%history,%seedA],[%second,%seedB]
 br label %step
step:
 %chain=phi i32 [%mapped,%join]
 %new=add i32 %chain,1
 %newSecond=add i32 %chain,5
 store i32 %chain,ptr %guest,align 1
 %limit=load i32,ptr %size,align 1
 %fits=icmp ule i32 %new,%limit
 br i1 %fits,label %header,label %fault
fault:
 %past=phi i32 [%history,%step]
 ret i32 %past
done: ret i32 %second
})IR");
 auto result=threadDispatchCorridorBatch(x.f(),{
  x.edge("seedB",{"join","step","header"}),x.edge("seedA",{"join","step","header"})});
 INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);CHECK(result.rewrittenEdges==2);x.verify();
 auto *past=llvm::dyn_cast_or_null<llvm::PHINode>(x.instruction("past"));REQUIRE(past);
 llvm::Value *counterInputs[2]={nullptr,nullptr};unsigned count=0,faults=0;
 for(auto &b:x.f())if(b.getName().starts_with("step.dispatch.edge")){
  llvm::BinaryOperator *counter=nullptr,*second=nullptr;llvm::StoreInst *store=nullptr;
  for(auto &i:b){if(i.getName().starts_with("new.dispatch.edge"))counter=llvm::dyn_cast<llvm::BinaryOperator>(&i);
   if(i.getName().starts_with("newSecond.dispatch.edge"))second=llvm::dyn_cast<llvm::BinaryOperator>(&i);
   if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i))store=s;}
  REQUIRE(counter);REQUIRE(second);REQUIRE(store);REQUIRE(count<2);
  auto *old=llvm::dyn_cast<llvm::PHINode>(counter->getOperand(0));REQUIRE(old);
  CHECK(old->getName().starts_with("dispatch.old"));CHECK(old->getNumIncomingValues()==1);
  CHECK(second->getOperand(0)==old);CHECK(store->getValueOperand()==old);counterInputs[count++]=old;
  for(unsigned n=0;n<past->getNumIncomingValues();++n)if(past->getIncomingBlock(n)==&b){
   ++faults;auto *history=llvm::dyn_cast<llvm::PHINode>(past->getIncomingValue(n));REQUIRE(history);
   CHECK(history->getName().starts_with("dispatch.old"));CHECK(history->getIncomingValue(0)!=counter);
  }
 }
 CHECK(count==2);CHECK(faults==2);CHECK(counterInputs[0]!=counterInputs[1]);
 noCyclicSwitch(x);
}

TEST_CASE("Atomic dispatch batch retains a two-value bound at its original control use","[dispatch-corridor-batch]"){
 for(bool falseContinues:{false,true}){
  INFO(falseContinues);auto ir=boundedBatchHead(falseContinues?" %fits=icmp uge i32 %state,4":" %fits=icmp ult i32 %state,4");
  const std::string seed="seed: br label %head";auto at=ir.find(seed);REQUIRE(at!=std::string::npos);
  ir.replace(at,seed.size(),"seed:\n %bit=and i32 %raw,1\n %choice=xor i32 %bit,3\n br label %head");
  at=ir.find("[2,%seed]");REQUIRE(at!=std::string::npos);ir.replace(at,9,"[%choice,%seed]");
  if(falseContinues){const std::string branch="br i1 %fits,label %dispatch,label %head";
   at=ir.find(branch);REQUIRE(at!=std::string::npos);ir.replace(at,branch.size(),"br i1 %fits,label %head,label %dispatch");}
  Fixture x(ir);auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});
  INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);x.verify();unsigned count=0;
  for(auto &b:x.f())if(b.getName().starts_with("head.dispatch.edge")){
   ++count;auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());REQUIRE(branch);
   REQUIRE(branch->isConditional());auto *compare=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());REQUIRE(compare);
   CHECK(compare->getParent()==&b);CHECK(compare->getPredicate()==(falseContinues?llvm::ICmpInst::ICMP_UGE:llvm::ICmpInst::ICMP_ULT));
   CHECK(branch->getSuccessor(0)==branch->getSuccessor(1));CHECK(branch->getSuccessor(0)!=&b);
  }
  CHECK(count==1);
 }
 SECTION("unused flagged arithmetic remains executed without donating a state proof"){
  Fixture x(boundedBatchHead(" %fits=icmp ult i32 %state,4"," %extra=add nsw i32 %raw,1"));
  auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});INFO(result.message);
  REQUIRE(result.changed);CHECK(result.complete);x.verify();
  auto *extra=llvm::dyn_cast_or_null<llvm::BinaryOperator>(x.instruction("extra.dispatch.edge"));REQUIRE(extra);
  CHECK(extra->hasNoSignedWrap());CHECK(extra->getOperand(0)==x.f().getArg(1));
 }
 for(const std::string &leaf:{" %bit=and i32 %raw,1\n %choice=add i32 %bit,3",
   " %bit=and i32 %raw,1\n %choice=add nsw i32 %bit,3",
   " %bit=and i32 %raw,1\n %choice=add nsw i32 %bit,2147483647"}){
  INFO(leaf);auto ir=boundedBatchHead();const std::string seed="seed: br label %head";
  auto at=ir.find(seed);REQUIRE(at!=std::string::npos);ir.replace(at,seed.size(),"seed:\n"+leaf+"\n br label %head");
  at=ir.find("[2,%seed]");REQUIRE(at!=std::string::npos);ir.replace(at,9,"[%choice,%seed]");
  Fixture x(ir);rejectBatch(x,{x.edge("seed",{"head","dispatch"})});
 }
}

TEST_CASE("Atomic dispatch batch proves integer flags over the entire one-bit domain","[dispatch-corridor-batch]"){
 for(const std::string &leaf:{
   " %bit=and i32 %raw,1\n %choice=add nsw i32 %bit,2",
   " %bit=and i32 %raw,1\n %choice=add nuw i32 %bit,2",
   " %bit=and i32 %raw,1\n %shift=shl nuw nsw i32 %bit,1\n %choice=add nuw nsw i32 %shift,2",
   " %bit=and i32 %raw,1\n %wide=shl nuw i32 %bit,1\n %small=lshr exact i32 %wide,1\n %choice=add i32 %small,2",
   " %bit=and i32 %raw,1\n %wide=shl nsw i32 %bit,1\n %small=ashr exact i32 %wide,1\n %choice=add i32 %small,2",
   " %bit=and i32 %raw,1\n %choice=or disjoint i32 %bit,2",
   " %bit=and i32 %raw,1\n %narrow=trunc nuw nsw i32 %bit to i8\n %wide=zext nneg i8 %narrow to i32\n %choice=add i32 %wide,2",
   " %choice=select i1 %choose,i32 2,i32 3"}){
  INFO(leaf);Fixture x(bits(leaf));auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});
  INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);CHECK(result.rewrittenEdges==1);x.verify();
  CHECK(llvm::cast<llvm::BranchInst>(x.block("seed")->getTerminator())->isUnconditional());
  unsigned choices=0;
  for(auto &b:x.f())for(auto &i:b)if(i.getName().starts_with("dispatch.choose")){
   ++choices;CHECK(b.getName().starts_with("dispatch.dispatch.edge"));
  }
  CHECK(choices==1);
 }
 for(const std::string &leaf:{
   " %bit=and i32 %raw,1\n %choice=add nsw i32 %bit,2147483647",
   " %bit=and i32 %raw,1\n %choice=add nuw i32 %bit,4294967295",
   " %bit=and i32 %raw,1\n %choice=sub nuw i32 0,%bit",
   " %bit=and i32 %raw,1\n %larger=add i32 %bit,1\n %choice=mul nuw i32 %larger,2147483648",
   " %bit=and i32 %raw,1\n %choice=or disjoint i32 %bit,1",
   " %bit=and i32 %raw,1\n %value=add i32 %bit,2\n %choice=lshr exact i32 %value,1",
   " %bit=and i32 %raw,1\n %value=add i32 %bit,2\n %choice=ashr exact i32 %value,1",
   " %bit=and i32 %raw,1\n %narrow=trunc i32 %bit to i1\n %choice=zext nneg i1 %narrow to i32",
   " %bit=and i32 %raw,1\n %value=add i32 %bit,2\n %small=trunc nuw i32 %value to i1\n %choice=zext i1 %small to i32",
   " %bit=and i32 %raw,1\n %small=trunc nsw i32 %bit to i1\n %choice=zext i1 %small to i32",
   " %bit=and i32 %raw,1\n %count=add i32 %bit,31\n %choice=shl i32 1,%count",
   " %bit=and i32 %raw,1\n %otherBit=zext i1 %choose to i32\n %choice=add i32 %bit,%otherBit"}){
  INFO(leaf);Fixture x(bits(leaf));rejectBatch(x,{x.edge("seed",{"head","dispatch"})});
 }
}

TEST_CASE("Atomic dispatch batch observes arbitrary bit positions without resolving their numeric input","[dispatch-corridor-batch][dispatch-bit-observation]"){
 for(const std::string &leaf:{
   " %bit=and i32 %raw,16\n %choice=add nuw nsw i32 %bit,60",
   " %bit=and i32 16,%raw\n %choice=add nuw nsw i32 %bit,60",
   " %bit=and i32 %raw,16\n %choice=add i32 %bit,2",
   " %bit=and i32 %raw,2147483648\n %choice=or disjoint i32 %bit,2",
   " %sign=lshr i32 %raw,31\n %choice=add nuw nsw i32 %sign,2",
   " %shift=lshr i32 %raw,27\n %bit=and i32 %shift,16\n %choice=add nuw nsw i32 %bit,60"}){
  INFO(leaf);Fixture x(bits(leaf));auto before=x.text();auto *fn=&x.f();
  auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});
  INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.rewrittenEdges==1);CHECK(&x.f()==fn);x.verify();
  unsigned choices=0;
  for(auto &b:x.f())for(auto &i:b){
   CHECK_FALSE(llvm::isa<llvm::FreezeInst>(i));
   if(i.getName().starts_with("dispatch.choose")){
    ++choices;CHECK(b.getName().starts_with("dispatch.dispatch.edge"));
    auto *compare=llvm::dyn_cast<llvm::ICmpInst>(&i);REQUIRE(compare);
    CHECK(compare->getPredicate()==llvm::ICmpInst::ICMP_EQ);
    auto *constant=llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(1));REQUIRE(constant);
    const bool sixty=leaf.find(",60")!=std::string::npos;
    CHECK(constant->equalsInt(sixty?60:2));
    auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());REQUIRE(branch);
    CHECK(branch->isConditional());
    CHECK(branch->getSuccessor(0)==x.block(sixty?"error":"two"));
    CHECK(branch->getSuccessor(1)==x.block(leaf.find("%sign=lshr")!=std::string::npos?"three":"error"));
   }
  }
  CHECK(choices==1);
  CHECK_FALSE(x.f().getArg(2)->hasAttribute(llvm::Attribute::NoUndef));
 }
}

TEST_CASE("Atomic dispatch bit observations retain ambiguity and flag refusals","[dispatch-corridor-batch][dispatch-bit-observation]"){
 for(const std::string &leaf:{
   " %bit=and i32 %raw,48\n %choice=add i32 %bit,2",
   " %bit=and i32 poison,16\n %choice=add i32 %bit,2",
   " %bit=and i32 undef,16\n %choice=add i32 %bit,2",
   " %bit=lshr exact i32 %raw,31\n %choice=add i32 %bit,2",
   " %bit=and i32 %raw,16\n %choice=add nsw i32 %bit,2147483640",
   " %bit=and i32 %raw,16\n %otherBit=zext i1 %choose to i32\n %choice=add i32 %bit,%otherBit"}){
  INFO(leaf);Fixture x(bits(leaf));rejectBatch(x,{x.edge("seed",{"head","dispatch"})});
 }
 SECTION("legacy entry retains its original admission"){
  Fixture x(bits(" %bit=and i32 %raw,16\n %choice=add i32 %bit,2"));
  x.reject(x.edge("seed",{"head","dispatch"}));
 }
}

TEST_CASE("Atomic dispatch batch checks only actual finite-domain switch destinations","[dispatch-corridor-batch]"){
 SECTION("an unselected padding case may point back into the original corridor"){
  auto ir=bits();const std::string last="i32 3,label %three]";auto at=ir.find(last);REQUIRE(at!=std::string::npos);
  ir.replace(at,last.size(),"i32 3,label %three i32 999,label %head]");
  const std::string incoming="[0,%other]";at=ir.find(incoming);REQUIRE(at!=std::string::npos);
  ir.replace(at,incoming.size(),"[0,%other],[%state,%dispatch]");Fixture x(ir);
  auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});INFO(result.message);
  REQUIRE(result.changed);CHECK(result.complete);CHECK(result.rewrittenEdges==1);x.verify();
  unsigned copies=0;
  for(auto &b:x.f())if(b.getName().starts_with("dispatch.dispatch.edge")){
   ++copies;auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());REQUIRE(branch);
   REQUIRE(branch->isConditional());auto *compare=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());REQUIRE(compare);
   CHECK(compare->getPredicate()==llvm::ICmpInst::ICMP_EQ);
   auto *constant=llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(1));REQUIRE(constant);
   REQUIRE((constant->equalsInt(2)||constant->equalsInt(3)));
   CHECK(branch->getSuccessor(0)==x.block(constant->equalsInt(2)?"two":"three"));
   CHECK(branch->getSuccessor(1)==x.block(constant->equalsInt(2)?"three":"two"));
  }
  CHECK(copies==1);
 }
 SECTION("one selected value returning into the path refuses the whole batch"){
  auto ir=bits();const std::string selected="i32 2,label %two";auto at=ir.find(selected);REQUIRE(at!=std::string::npos);
  // Keep two's original exit PHI well formed after redirecting selected case2.
  ir.replace(at,selected.size(),"i32 2,label %head i32 777,label %two");
  const std::string incoming="[0,%other]";at=ir.find(incoming);REQUIRE(at!=std::string::npos);
  ir.replace(at,incoming.size(),"[0,%other],[%state,%dispatch]");Fixture x(ir);
  rejectBatch(x,{x.edge("seed",{"head","dispatch"})});
 }
}

TEST_CASE("Atomic dispatch batch does not invent definedness for repeated atom uses","[dispatch-corridor-batch]"){
 // This is a structural control, never a native poison/undef execution oracle.
 // The independent entry selector ensures the observed atom is not already
 // consumed by entry's branch. No noundef/freeze promise may be introduced.
 for(bool selfBound:{false,true}){
  INFO(selfBound);
  const std::string leaf=" %low=zext i1 %choose to i32\n %high=select i1 %choose,i32 8,i32 0\n"
                         " %both=or disjoint i32 %low,%high\n %choice=add i32 %both,2";
  auto ir=selfBound?boundedBatchHead(" %fits=icmp ult i32 %state,16"):bits(leaf);
  if(selfBound){const std::string seed="seed: br label %head";auto at=ir.find(seed);REQUIRE(at!=std::string::npos);
   ir.replace(at,seed.size(),"seed:\n"+leaf+"\n br label %head");at=ir.find("[2,%seed]");REQUIRE(at!=std::string::npos);
   ir.replace(at,9,"[%choice,%seed]");}
  const std::string argument="i1 %choose)";auto at=ir.find(argument);REQUIRE(at!=std::string::npos);
  ir.replace(at,argument.size(),"i1 %choose,i1 %take)");
  const std::string entry="entry: br i1 %choose";at=ir.find(entry);REQUIRE(at!=std::string::npos);
  ir.replace(at,entry.size(),"entry: br i1 %take");Fixture x(ir);
  const unsigned atomIndex=selfBound?2:3;
  auto *atom=x.f().getArg(atomIndex);CHECK_FALSE(atom->hasAttribute(llvm::Attribute::NoUndef));
  auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});INFO(result.message);
  REQUIRE(result.changed);CHECK(result.complete);x.verify();CHECK(x.f().getArg(atomIndex)==atom);
  CHECK_FALSE(atom->hasAttribute(llvm::Attribute::NoUndef));
  CHECK(llvm::cast<llvm::BranchInst>(x.block("seed")->getTerminator())->isUnconditional());
  unsigned choices=0,bounds=0;
  for(auto &b:x.f()){
   for(auto &i:b){CHECK_FALSE(llvm::isa<llvm::FreezeInst>(i));
    if(i.getName().starts_with("dispatch.choose")){++choices;CHECK(b.getName().starts_with("dispatch.dispatch.edge"));}}
   if(selfBound&&b.getName().starts_with("head.dispatch.edge")){
    ++bounds;auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());REQUIRE(branch);
    REQUIRE(branch->isConditional());CHECK(branch->getSuccessor(0)==branch->getSuccessor(1));
    auto *condition=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());REQUIRE(condition);CHECK(condition->getParent()==&b);
   }
  }
  CHECK(choices==1);CHECK(bounds==unsigned(selfBound));
 }
}

TEST_CASE("Atomic dispatch batch folds only a proved same-time pure head bound","[dispatch-corridor-batch]"){
 SECTION("constant selected state cannot take the original self edge"){
  Fixture x(boundedBatchHead());auto result=threadDispatchCorridorBatch(x.f(),{x.edge("seed",{"head","dispatch"})});
  INFO(result.message);REQUIRE(result.changed);CHECK(result.complete);CHECK(result.rewrittenEdges==1);x.verify();
  unsigned copies=0;
  for(auto &b:x.f())if(b.getName().starts_with("head.dispatch.edge")){
   ++copies;auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());REQUIRE(branch);
   CHECK(branch->isUnconditional());CHECK(branch->getSuccessor(0)!=&b);
  }
  CHECK(copies==1);
 }
 for(const std::string &condition:{" %fits=icmp ult i32 %state,2"," %fits=icmp ult i32 %state,%raw",
   " %fits=icmp slt i32 %state,4"}){
  INFO(condition);Fixture x(boundedBatchHead(condition));rejectBatch(x,{x.edge("seed",{"head","dispatch"})});
 }
 SECTION("a LOAD in the self-edge head cannot be folded away"){
  Fixture x(boundedBatchHead(" %fits=icmp ult i32 %state,4"," %observed=load i32,ptr %guest,align 1"));
  rejectBatch(x,{x.edge("seed",{"head","dispatch"})});
 }
}

TEST_CASE("Atomic dispatch batch prepays aggregate growth and repair before mutation","[dispatch-corridor-batch]"){
 for(unsigned n=0;n<6;++n){
  INFO(n);Fixture x(batchLoop(3));DispatchCorridorBatchOptions options;
  if(n==0)options.maxCandidates=2;if(n==1)options.maxRewrites=2;
  if(n==2)options.maxClonedBlocks=2;if(n==3)options.maxClonedInstructions=1;
  if(n==4)options.maxRepairCells=1;if(n==5)options.maxWork=1;
  rejectBatch(x,batchLoopEdges(x,3),options,true);
 }
 for(bool instructions:{false,true}){
  INFO(instructions);Fixture x(batchLoop(70));DispatchCorridorBatchOptions options;
  if(instructions)options.maxClonedInstructions=64;else options.maxClonedBlocks=64;
  rejectBatch(x,batchLoopEdges(x,70),options,true);
 }
}

#ifdef DRAGON_DISPATCH_CORRIDOR_TESTING
TEST_CASE("Atomic dispatch batch restores the entire module after controlled commit failure","[dispatch-corridor-batch][dispatch-corridor-transaction]"){
 for(unsigned stage:{1u,2u}){
  INFO(stage);Fixture x(batchLoop(3)+"\n!sentinel = !{!0}\n!0 = !{i32 19}\n");
  auto before=x.text();auto *function=&x.f();auto *argument=x.f().getArg(0);
  auto *head=x.block("head");auto *state=x.instruction("state");auto *counter=x.instruction("advanced");
  auto *sentinel=x.module->getNamedMetadata("sentinel");REQUIRE(sentinel);
  REQUIRE_FALSE(x.module->getNamedMetadata("llvm.dbg.cu"));
  auto result=threadDispatchCorridorBatchCommitFailureForTest(x.f(),batchLoopEdges(x,3),stage);INFO(result.message);
  CHECK_FALSE(result.changed);CHECK_FALSE(result.complete);CHECK(result.rewrittenEdges==0);
  CHECK(result.clonedBlocks==0);CHECK(result.clonedInstructions==0);CHECK(result.snapshots==0);
  CHECK(result.repairPhis==0);CHECK(result.repairCells==0);CHECK(x.text()==before);
  CHECK(&x.f()==function);CHECK(x.f().getArg(0)==argument);CHECK(x.block("head")==head);
  CHECK(x.instruction("state")==state);CHECK(x.instruction("advanced")==counter);
  CHECK(x.module->getNamedMetadata("sentinel")==sentinel);
  CHECK_FALSE(x.module->getNamedMetadata("llvm.dbg.cu"));x.verify();
 }
}
#endif
