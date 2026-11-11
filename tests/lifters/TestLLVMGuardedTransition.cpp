#include <catch2/catch_test_macros.hpp>
#include "../../lib/GuardedTransition.hpp"
#include <llvm/IR/CFG.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <string>

using namespace dragon::llvmlifter::detail;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
std::string text(const llvm::Module &module) {
  std::string result; llvm::raw_string_ostream stream(result); module.print(stream, nullptr); return result;
}
void verify(const llvm::Module &module) {
  std::string error; llvm::raw_string_ostream stream(error);
  const bool invalid = llvm::verifyModule(module, &stream);
  INFO(error); REQUIRE_FALSE(invalid);
}
llvm::BasicBlock *block(llvm::Function &function, llvm::StringRef name) {
  for (auto &b : function) if (b.getName() == name) return &b;
  return nullptr;
}
llvm::Instruction *instruction(llvm::Function &function, llvm::StringRef name) {
  for (auto &b : function) for (auto &i : b) if (i.getName() == name) return &i;
  return nullptr;
}
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"transition-test", context};
  llvm::Function *function;
  GuardedTransitionRequest request;
  Fixture(bool reentry = false) {
    module.setDataLayout("e-p:64:64");
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *i64 = llvm::Type::getInt64Ty(context), *i1 = llvm::Type::getInt1Ty(context);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,i1,i1,i1},false),
        llvm::GlobalValue::ExternalLinkage,"transition",module);
    auto make = [&](const char *name) { return llvm::BasicBlock::Create(context,name,function); };
    auto *entry=make("entry"), *seed=make("seed"), *other=make("other"), *head=make("head");
    auto *write=make("write"), *dispatch=make("dispatch"), *hit=make("hit");
    auto *miss=make("miss"), *bounds=make("bounds");
    Builder b(entry); b.CreateCondBr(function->getArg(3),seed,other);
    b.SetInsertPoint(seed); b.CreateBr(head);
    b.SetInsertPoint(other); b.CreateBr(head);
    b.SetInsertPoint(head);
    auto *state=b.CreatePHI(i64,reentry?3:2,"state");
    state->addIncoming(function->getArg(2),seed); state->addIncoming(function->getArg(2),other);
    auto *fresh=b.CreateLoad(i64,function->getArg(0),"fresh");
    fresh->setAlignment(llvm::Align(1));
    auto *frozen=b.CreateFreeze(fresh,"fresh.freeze");
    b.CreateCondBr(function->getArg(4),write,bounds);
    b.SetInsertPoint(write);
    // The write intentionally aliases the subsequent target LOAD. A candidate
    // from the original table may be wrong after this actual executed store.
    b.CreateStore(state,function->getArg(0))->setAlignment(llvm::Align(1));
    if (reentry) {
      b.CreateCondBr(function->getArg(5),dispatch,head);
      state->addIncoming(state,write);
    } else b.CreateBr(dispatch);
    b.SetInsertPoint(dispatch);
    auto *target=b.CreateLoad(i64,function->getArg(0),"target");
    target->setAlignment(llvm::Align(1));
    auto *jump=b.CreateSwitch(target,miss,2);
    jump->addCase(b.getInt64(7),hit); jump->addCase(b.getInt64(8),hit);
    b.SetInsertPoint(hit);
    auto *selected=b.CreatePHI(i64,2,"selected");
    selected->addIncoming(target,dispatch); selected->addIncoming(target,dispatch);
    b.CreateStore(frozen,function->getArg(1))->setAlignment(llvm::Align(1));
    b.CreateRet(selected);
    b.SetInsertPoint(miss);
    b.CreateStore(frozen,function->getArg(1))->setAlignment(llvm::Align(1));
    b.CreateRet(target);
    b.SetInsertPoint(bounds); b.CreateRet(frozen);
    request={seed,0,head,{head,write,dispatch},{0,0},b.getInt64(7)};
    verify(module);
  }
};
void checkSharedFreshTarget(Fixture &fixture) {
  auto *fallback=block(*fixture.function,"transition.fallback");
  REQUIRE(fallback);
  auto *jump=llvm::dyn_cast<llvm::SwitchInst>(fallback->getTerminator());
  REQUIRE(jump);
  CHECK(jump->getNumCases()==2);
  CHECK(jump->getDefaultDest()==block(*fixture.function,"miss"));
  auto *fresh=llvm::dyn_cast<llvm::LoadInst>(jump->getCondition());
  REQUIRE(fresh);
  CHECK(fresh->getParent()==block(*fixture.function,"dispatch.transition"));
  auto *branch=llvm::dyn_cast<llvm::BranchInst>(fresh->getParent()->getTerminator());
  REQUIRE(branch);
  auto *comparison=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
  REQUIRE(comparison);
  CHECK(comparison->getPredicate()==llvm::CmpInst::ICMP_EQ);
  CHECK(comparison->getOperand(0)==fresh);
  CHECK(branch->getSuccessor(0)==block(*fixture.function,"hit"));
  CHECK(branch->getSuccessor(1)==fallback);
  CHECK(llvm::cast<llvm::ConstantInt>(comparison->getOperand(1))->getZExtValue()==7);
}
} // namespace

TEST_CASE("Guarded transitions preserve writes fresh targets and repeated exits",
          "[llvm][guarded-transition]") {
  Fixture fixture;
  auto result=cloneGuardedTransition(*fixture.function,fixture.request);
  INFO(result.message); REQUIRE(result.changed); CHECK(result.complete);
  verify(fixture.module);
  CHECK(result.clonedBlocks==4);
  CHECK(result.originalExitEdges==5); // bounds + three fallback slots + true
  checkSharedFreshTarget(fixture);
  auto *write=block(*fixture.function,"write.transition"); REQUIRE(write);
  unsigned stores=0;
  for(auto &i:*write) if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)) {
    ++stores; CHECK(store->getPointerOperand()==fixture.function->getArg(0));
    CHECK(store->getValueOperand()==fixture.function->getArg(2));
  }
  CHECK(stores==1);
  auto *phi=llvm::dyn_cast<llvm::PHINode>(instruction(*fixture.function,"selected"));
  REQUIRE(phi); CHECK(phi->getNumIncomingValues()==5);
  unsigned original=0, fallback=0, fast=0;
  for(unsigned i=0;i<phi->getNumIncomingValues();++i) {
    auto name=phi->getIncomingBlock(i)->getName();
    original+=name=="dispatch"; fallback+=name=="transition.fallback";
    fast+=name=="dispatch.transition";
  }
  CHECK(original==2); CHECK(fallback==2); CHECK(fast==1);
}

TEST_CASE("Guarded transitions keep side reentry on original history",
          "[llvm][guarded-transition]") {
  Fixture fixture(true);
  const auto result=cloneGuardedTransition(*fixture.function,fixture.request);
  INFO(result.message); REQUIRE(result.changed); verify(fixture.module);
  auto *copy=block(*fixture.function,"write.transition"); REQUIRE(copy);
  auto *branch=llvm::cast<llvm::BranchInst>(copy->getTerminator());
  CHECK(branch->getSuccessor(1)==block(*fixture.function,"head"));
  CHECK(branch->getSuccessor(0)==block(*fixture.function,"dispatch.transition"));
  auto *phi=llvm::cast<llvm::PHINode>(instruction(*fixture.function,"state"));
  CHECK(phi->getBasicBlockIndex(copy)>=0);
  CHECK(phi->getIncomingValueForBlock(copy)==fixture.function->getArg(2));
  CHECK(llvm::isa<llvm::FreezeInst>(instruction(*fixture.function,"fresh.freeze")));
  CHECK(llvm::isa<llvm::FreezeInst>(instruction(*fixture.function,"fresh.freeze.transition")));
  checkSharedFreshTarget(fixture);
}

TEST_CASE("Guarded transitions preserve module function and argument identity",
          "[llvm][guarded-transition]") {
  Fixture fixture;
  auto *f=fixture.function; auto *arg=f->getArg(0); auto *module=&fixture.module;
  std::vector<std::string> originalBlockNames;
  for(auto &b:*f) originalBlockNames.push_back(b.getName().str());
  f->setVisibility(llvm::GlobalValue::HiddenVisibility);
  f->setSection(".text.transition"); f->addFnAttr(llvm::Attribute::NoInline);
  f->setComdat(module->getOrInsertComdat("transition.group"));
  auto attributes=f->getAttributes(); auto *comdat=f->getComdat();
  auto *global=new llvm::GlobalVariable(*module,f->getType(),true,
      llvm::GlobalValue::InternalLinkage,f,"saved.function");
  auto *metadata=module->getOrInsertNamedMetadata("transition.test");
  metadata->addOperand(llvm::MDNode::get(fixture.context,
      llvm::MDString::get(fixture.context,"original")));
  auto *caller=llvm::Function::Create(f->getFunctionType(),
      llvm::GlobalValue::ExternalLinkage,"caller",*module);
  Builder b(llvm::BasicBlock::Create(fixture.context,"entry",caller));
  std::vector<llvm::Value*> args;
  for(auto &a:caller->args()) args.push_back(&a);
  auto *call=b.CreateCall(f,args); b.CreateRet(call);
  verify(*module);
  const auto result=cloneGuardedTransition(*f,fixture.request);
  INFO(result.message); REQUIRE(result.changed); verify(*module);
  CHECK(f->getParent()==module); CHECK(f->getArg(0)==arg);
  CHECK(global->getInitializer()==f); CHECK(call->getCalledFunction()==f);
  CHECK(f->getAttributes()==attributes); CHECK(f->getComdat()==comdat);
  CHECK(f->getSection()==".text.transition");
  CHECK(f->getVisibility()==llvm::GlobalValue::HiddenVisibility);
  CHECK(module->getNamedMetadata("transition.test")==metadata);
  CHECK(metadata->getNumOperands()==1);
  CHECK(module->size()==2);
  auto current=f->begin();
  for(const auto &name:originalBlockNames) {
    REQUIRE(current!=f->end()); CHECK(current->getName()==name); ++current;
  }
  CHECK(current!=f->end());
}

TEST_CASE("Guarded transition rejections leave the module unchanged",
          "[llvm][guarded-transition]") {
  for(int kind=0;kind!=9;++kind) DYNAMIC_SECTION("kind "<<kind) {
    Fixture fixture;
    GuardedTransitionOptions options;
    if(kind==0) options.maxBlocks=3;
    if(kind==1) options.maxInstructions=1;
    if(kind==2) fixture.request.candidate=llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(fixture.context),99);
    if(kind==3) fixture.request.seedSuccessor=3;
    if(kind==4) fixture.request.forwardSuccessors[0]=1;
    if(kind==5) llvm::cast<llvm::LoadInst>(instruction(*fixture.function,"target"))->setVolatile(true);
    if(kind==6) {
      auto *term=block(*fixture.function,"write")->getTerminator();
      Builder b(term); auto callee=fixture.module.getOrInsertFunction("unknown",
          llvm::FunctionType::get(b.getVoidTy(),false)); b.CreateCall(callee);
    }
    if(kind==7) {
      auto *term=block(*fixture.function,"write")->getTerminator();
      Builder b(term); b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    }
    if(kind==8) {
      auto *copy=instruction(*fixture.function,"target");
      llvm::cast<llvm::LoadInst>(copy)->setAtomic(llvm::AtomicOrdering::Monotonic);
    }
    verify(fixture.module); auto before=text(fixture.module);
    const auto result=cloneGuardedTransition(*fixture.function,fixture.request,options);
    CHECK_FALSE(result.changed); CHECK_FALSE(result.complete); CHECK(result.rejected);
    CHECK(result.budgetExhausted==(kind==0||kind==1));
    CHECK(text(fixture.module)==before);
    verify(fixture.module);
  }
}

TEST_CASE("Guarded transitions snapshot historical target without reusing its fresh load",
          "[llvm][guarded-transition]") {
  llvm::LLVMContext context; llvm::Module module("history",context);
  module.setDataLayout("e-p:64:64");
  auto *ptr=llvm::PointerType::getUnqual(context); auto *i64=llvm::Type::getInt64Ty(context);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64},false),
      llvm::GlobalValue::ExternalLinkage,"history",module);
  auto make=[&](const char *name){return llvm::BasicBlock::Create(context,name,f);};
  auto *entry=make("entry"),*head=make("head"),*dispatch=make("dispatch");
  auto *latch=make("latch"),*exit=make("exit");
  Builder b(entry);b.CreateBr(head);
  b.SetInsertPoint(head);auto *history=b.CreatePHI(i64,2,"history");
  history->addIncoming(b.getInt64(0),entry);
  b.CreateStore(history,f->getArg(1))->setAlignment(llvm::Align(1));
  b.CreateBr(dispatch);
  b.SetInsertPoint(dispatch);auto *fresh=b.CreateLoad(i64,f->getArg(0),"target");
  fresh->setAlignment(llvm::Align(1));auto *jump=b.CreateSwitch(fresh,exit,1);
  jump->addCase(b.getInt64(7),latch);
  b.SetInsertPoint(latch);
  b.CreateStore(f->getArg(2),f->getArg(0))->setAlignment(llvm::Align(1));
  b.CreateBr(head);history->addIncoming(fresh,latch);
  b.SetInsertPoint(exit);b.CreateRet(fresh);
  verify(module);
  GuardedTransitionRequest request{latch,0,head,{head,dispatch},{0},b.getInt64(7)};
  const auto result=cloneGuardedTransition(*f,request);
  INFO(result.message); REQUIRE(result.changed); verify(module);
  CHECK(result.snapshotPhis==1); CHECK(result.clonedBlocks==4);
  auto *snapshot=llvm::dyn_cast<llvm::PHINode>(instruction(*f,"transition.old"));
  REQUIRE(snapshot);
  auto *newLoad=instruction(*f,"target.transition");
  REQUIRE(llvm::isa<llvm::LoadInst>(newLoad));
  CHECK(snapshot->getIncomingBlock(0)==block(*f,"latch"));
  CHECK(snapshot->getIncomingValue(0)!=newLoad);
  auto *headCopy=block(*f,"head.transition"); REQUIRE(headCopy);
  auto *store=llvm::dyn_cast<llvm::StoreInst>(&headCopy->front());
  REQUIRE(store); CHECK(store->getValueOperand()==snapshot);
  CHECK(llvm::cast<llvm::SwitchInst>(block(*f,"transition.fallback")->getTerminator())->getCondition()==newLoad);
}

TEST_CASE("Guarded transition liveout use budget preserves the complete source",
          "[llvm][guarded-transition]") {
  llvm::LLVMContext context; llvm::Module module("repair-budget",context);
  auto *ptr=llvm::PointerType::getUnqual(context);
  auto *i64=llvm::Type::getInt64Ty(context); auto *i1=llvm::Type::getInt1Ty(context);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,i1},false),
      llvm::GlobalValue::ExternalLinkage,"repair_budget",module);
  auto make=[&](const char *name){return llvm::BasicBlock::Create(context,name,f);};
  auto *entry=make("entry"),*seed=make("seed"),*other=make("other");
  auto *dispatch=make("dispatch"),*exit=make("exit");
  Builder b(entry);b.CreateCondBr(f->getArg(1),seed,other);
  b.SetInsertPoint(seed);b.CreateBr(dispatch);
  b.SetInsertPoint(other);b.CreateBr(dispatch);
  b.SetInsertPoint(dispatch);
  auto *target=b.CreateLoad(i64,f->getArg(0),"target");
  target->setAlignment(llvm::Align(1));
  auto *jump=b.CreateSwitch(target,exit,511);
  for(unsigned n=0;n<511;++n) jump->addCase(b.getInt64(n),exit);
  b.SetInsertPoint(exit);
  std::vector<llvm::Value*> phis;
  for(unsigned n=0;n<17;++n) {
    auto *phi=b.CreatePHI(i64,512,"exit.value");
    for(unsigned edge=0;edge<512;++edge) phi->addIncoming(target,dispatch);
    phis.push_back(phi);
  }
  auto *sum=phis.front();
  for(unsigned n=1;n<phis.size();++n) sum=b.CreateAdd(sum,phis[n]);
  b.CreateRet(sum);
  verify(module);
  auto before=text(module);auto *arg=f->getArg(0);
  GuardedTransitionRequest request{seed,0,dispatch,{dispatch},{},b.getInt64(7)};
  GuardedTransitionOptions options; options.maxInstructions=65536;
  const auto result=cloneGuardedTransition(*f,request,options);
  INFO(result.message);
  CHECK_FALSE(result.changed); CHECK_FALSE(result.complete);
  CHECK(result.rejected); CHECK(result.budgetExhausted);
  CHECK(result.message=="live-out use budget exhausted");
  CHECK(result.clonedBlocks==0); CHECK(result.originalExitEdges==0);
  CHECK(f->getArg(0)==arg); CHECK(module.getFunction("repair_budget")==f);
  CHECK(module.size()==1); CHECK(text(module)==before); verify(module);
}
