#include <catch2/catch_test_macros.hpp>
#include "../../lib/GuardedTransition.hpp"
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>
namespace {
using namespace dragon::llvmlifter::detail;
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();llvm::Function *function;
  Fixture(unsigned slots=1,int invalid=0,bool controller=false) {
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);
    auto *i64=llvm::Type::getInt64Ty(c),*i1=llvm::Type::getInt1Ty(c);
    function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,i1,i1},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *name){return llvm::BasicBlock::Create(c,name,function);};
    auto *entry=block("entry"),*seed=block("seed"),*head=block("head"),*write=block("write"),*dispatch=block("dispatch"),*hit=block("hit"),*exit=block("exit"),*bounds=block("bounds");
    llvm::IRBuilder<llvm::NoFolder> b(entry);auto *guest=b.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));b.CreateBr(seed);
    entry->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
    b.SetInsertPoint(seed);auto *old=b.CreateLoad(i64,function->getArg(0),"old.load");old->setAlignment(llvm::Align(1));auto *snapshot=b.CreateFreeze(old,"old.snapshot");
    if(slots==2)b.CreateCondBr(function->getArg(3),head,head);else b.CreateBr(head);
    b.SetInsertPoint(head);auto *state=b.CreatePHI(i64,slots,"state");
    llvm::Value *input=controller?static_cast<llvm::Value *>(b.getInt64(1)):snapshot;
    if(invalid==1)input=llvm::UndefValue::get(i64);if(invalid==2)input=llvm::PoisonValue::get(i64);
    for(unsigned n=0;n<slots;++n)state->addIncoming(input,seed);
    auto *fresh=b.CreateLoad(i64,function->getArg(0),"fresh");fresh->setAlignment(llvm::Align(1));b.CreateCondBr(function->getArg(4),write,bounds);
    b.SetInsertPoint(write);b.CreateStore(state,function->getArg(0))->setAlignment(llvm::Align(1));b.CreateBr(dispatch);
    b.SetInsertPoint(dispatch);llvm::Value *address=function->getArg(0);
    if(controller)address=b.CreateGEP(b.getInt8Ty(),guest,b.CreateAdd(b.getInt64(1024),b.CreateShl(state,b.getInt64(3))));
    auto *actual=b.CreateLoad(i64,address,"actual");actual->setAlignment(llvm::Align(1));auto *sw=b.CreateSwitch(actual,exit);sw->addCase(b.getInt64(7),hit);sw->addCase(b.getInt64(8),hit);
    b.SetInsertPoint(hit);auto *selected=b.CreatePHI(i64,2,"selected");selected->addIncoming(actual,dispatch);selected->addIncoming(actual,dispatch);
    // Re-enter the unchanged seed: the next invocation must execute its LOAD
    // and freeze again, rather than reuse the previous entry's snapshot.
    b.CreateCondBr(function->getArg(3),seed,exit);
    b.SetInsertPoint(exit);auto *result=b.CreatePHI(i64,2,"result");result->addIncoming(actual,dispatch);result->addIncoming(selected,hit);b.CreateRet(result);
    b.SetInsertPoint(bounds);b.CreateRet(fresh);
    new llvm::GlobalVariable(module,ptr,false,llvm::GlobalValue::ExternalLinkage,function,"saved.function");
    module.getOrInsertNamedMetadata("test.identity")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,"stable")));
    if(controller)hit->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
    if(controller)module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,"{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0x408\",\"bytes\":\"0700000000000000\"}]}")));
    verify();
  }
  void verify(){REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));}
  llvm::BasicBlock *block(const char *name){for(auto &b:*function)if(b.getName()==name)return &b;return nullptr;}
  llvm::Instruction *instruction(const char *name){for(auto &b:*function)for(auto &i:b)if(i.getName()==name)return &i;return nullptr;}
  std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
  GuardedTransitionResult run(unsigned slot=0,GuardedTransitionOptions options={}) {
    GuardedTransitionRequest q{block("seed"),slot,block("head"),{block("head"),block("write"),block("dispatch")},{0,0},llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),7)};
    auto r=cloneGuardedTransition(*function,q,options);verify();return r;
  }
  Result pipeline(Options options={}){PassPipeline p;REQUIRE(p.add("guarded-state-corridors",options));auto result=p.run(lifter);REQUIRE(result.runs.size()==1);verify();return result.runs[0].result;}
};
}
TEST_CASE("Guarded transition eliminates only external singleton PHI in trial","[llvm][guarded-single-incoming]") {
  Fixture f;auto *function=f.function,*saved=f.module.getFunction("view");auto *arg=function->getArg(0);auto *metadata=f.module.getNamedMetadata("test.identity");
  std::vector<std::string> names;for(auto &b:*function)names.push_back(b.getName().str());
  auto r=f.run();INFO(r.message);REQUIRE(r.changed);CHECK(r.complete);CHECK(function==saved);CHECK(function->getArg(0)==arg);CHECK(metadata==f.module.getNamedMetadata("test.identity"));CHECK(f.module.getNamedGlobal("saved.function")->getInitializer()==function);
  std::size_t n=0;for(auto &b:*function){if(n<names.size())CHECK(b.getName()==names[n]);++n;}
  CHECK(f.block("head")->phis().empty());CHECK(f.instruction("state")==nullptr);
  auto *snapshot=f.instruction("old.snapshot");REQUIRE(snapshot);auto *write=f.block("write.transition");REQUIRE(write);
  unsigned stores=0;for(auto &i:*write)if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)){++stores;CHECK(store->getValueOperand()==snapshot);}CHECK(stores==1);
  REQUIRE(f.instruction("fresh.transition"));CHECK(f.instruction("fresh.transition")!=f.instruction("fresh"));
  CHECK(f.block("head.transition")->getTerminator()->getSuccessor(1)==f.block("bounds"));CHECK(f.block("hit")->getTerminator()->getSuccessor(0)==f.block("seed"));
  auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(f.block("transition.fallback")->getTerminator());REQUIRE(fallback);CHECK(fallback->getNumCases()==2);auto *compare=llvm::cast<llvm::ICmpInst>(llvm::cast<llvm::BranchInst>(f.block("dispatch.transition")->getTerminator())->getCondition());CHECK(compare->getOperand(0)==fallback->getCondition());
}
TEST_CASE("Guarded transition consumes duplicate original slots through last singleton","[llvm][guarded-single-incoming]") {
  Fixture f(2);REQUIRE(f.run(0).changed);auto &phi=*f.block("head")->phis().begin();CHECK(phi.getNumIncomingValues()==1);auto r=f.run(1);INFO(r.message);REQUIRE(r.changed);CHECK(f.block("head")->phis().empty());auto stable=f.text();CHECK_FALSE(f.run(1).changed);CHECK(f.text()==stable);
}
TEST_CASE("Guarded singleton rejection and budget leave source byte identical","[llvm][guarded-single-incoming]") {
  for(int invalid:{1,2}){Fixture f(1,invalid);auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.rejected);CHECK(f.text()==before);}
  for(auto budget:{GuardedTransitionOptions{0,2048},GuardedTransitionOptions{32,0}}){Fixture f;auto before=f.text();auto r=f.run(0,budget);CHECK_FALSE(r.changed);CHECK(r.budgetExhausted);CHECK(f.text()==before);CHECK(f.instruction("state")!=nullptr);}
}
TEST_CASE("State corridor discovery admits proven singleton while retaining other paths","[llvm][guarded-single-incoming]") {
  Fixture f(1,0,true);auto *function=f.function;auto result=f.pipeline({{"max_rewrites",std::int64_t(1)}});INFO(result.message);REQUIRE(result.changed);CHECK(result.counters.at("rewritten_edges")==1);CHECK(function==f.module.getFunction("view"));CHECK(f.block("head")->phis().empty());auto stable=f.text();CHECK_FALSE(f.pipeline().changed);CHECK(f.text()==stable);
}
