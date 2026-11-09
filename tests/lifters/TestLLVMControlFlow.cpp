#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace dragon;
using namespace dragon::passes;
using dragon::llvmlifter::LLVMLifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
// A bounded state machine with an input-dependent trip count and observable
// writes. The default is defined even when the initial state is external.
llvm::Function *machine(LLVMLifter &l, bool externalState=false) {
  auto &c=l.module().getContext();auto *t=llvm::Type::getInt32Ty(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(t,
      {t,llvm::PointerType::getUnqual(c),t},false),llvm::GlobalValue::ExternalLinkage,"machine",l.module());
  auto *entry=llvm::BasicBlock::Create(c,"entry",f);
  auto *dispatch=llvm::BasicBlock::Create(c,"dispatch",f);
  auto *start=llvm::BasicBlock::Create(c,"start",f);
  auto *choose=llvm::BasicBlock::Create(c,"choose",f);
  auto *step=llvm::BasicBlock::Create(c,"step",f);
  auto *done=llvm::BasicBlock::Create(c,"done",f);
  auto *other=llvm::BasicBlock::Create(c,"other",f);
  Builder b{entry};auto constant=[&](unsigned v){return llvm::ConstantInt::get(t,v);};
  auto *limit=b.CreateAnd(f->getArg(0),constant(3));b.CreateBr(dispatch);
  b.SetInsertPoint(dispatch);
  auto *state=b.CreatePHI(t,4,"state");auto *value=b.CreatePHI(t,4,"value");
  auto *count=b.CreatePHI(t,4,"count");
  auto *sw=b.CreateSwitch(state,other,4);
  sw->addCase(constant(10),start);sw->addCase(constant(20),choose);
  sw->addCase(constant(30),step);sw->addCase(constant(40),done);
  b.SetInsertPoint(start);b.CreateStore(value,f->getArg(1))->setVolatile(true);b.CreateBr(dispatch);
  b.SetInsertPoint(choose);
  auto *next=b.CreateSelect(b.CreateICmpULT(count,limit),constant(30),constant(40));b.CreateBr(dispatch);
  b.SetInsertPoint(step);auto *updated=b.CreateAdd(value,constant(1));
  auto *increment=b.CreateAdd(count,constant(1));
  b.CreateStore(updated,f->getArg(1))->setVolatile(true);b.CreateBr(dispatch);
  b.SetInsertPoint(done);b.CreateRet(value);
  b.SetInsertPoint(other);b.CreateRet(constant(0xbad));
  state->addIncoming(externalState?static_cast<llvm::Value *>(f->getArg(2)):constant(10),entry);
  state->addIncoming(constant(20),start);state->addIncoming(next,choose);state->addIncoming(constant(20),step);
  value->addIncoming(f->getArg(0),entry);value->addIncoming(value,start);
  value->addIncoming(value,choose);value->addIncoming(updated,step);
  count->addIncoming(constant(0),entry);count->addIncoming(constant(0),start);
  count->addIncoming(count,choose);count->addIncoming(increment,step);
  return f;
}
unsigned switches(llvm::Function &f) {
  unsigned n=0;for(auto &b:f)n+=llvm::isa<llvm::SwitchInst>(b.getTerminator());return n;
}
void verify(LLVMLifter &l) {
  std::string error;llvm::raw_string_ostream out{error};
  auto bad=llvm::verifyModule(l.module(),&out);INFO(error);REQUIRE_FALSE(bad);
}
// Optional export for the independent native/Python state-machine oracle.
void snapshot(LLVMLifter &l,const char *name) {
  if(auto *directory=std::getenv("DRAGON_CONTROL_FLOW_FIXTURE_DIR")) {
    std::filesystem::create_directories(directory);
    std::ofstream out{std::filesystem::path(directory)/name};out<<l.ir();
    REQUIRE(out.good());
  }
}
}

TEST_CASE("LLVM control flow threads a constant-state dispatcher with a real loop", "[llvm][control-flow]") {
  LLVMLifter l{Architecture::X86_64};auto *f=machine(l);verify(l);
  // The old sweep leaves the dispatcher in place, independent of Z3.
  PassPipeline old;for(const auto &name:PassPipeline::defaultLLVM())REQUIRE(old.add(name));
  REQUIRE(old.run(l).ok);REQUIRE(switches(*f)==1);
  snapshot(l,"known-before.ll");
  PassPipeline p;REQUIRE(p.add("control-flow"));auto r=p.run(l);
  REQUIRE(r.ok);CHECK(r.changed);CHECK(switches(*f)==0);verify(l);
  REQUIRE(r.runs.size()==1);
  CHECK(r.runs[0].result.status==Status::Success);
  CHECK(r.runs[0].result.counters.at("switches_before")==1);
  CHECK(r.runs[0].result.counters.at("switches_after")==0);
  snapshot(l,"known-after.ll");
}

TEST_CASE("LLVM control flow retains external initial states and side effects", "[llvm][control-flow]") {
  LLVMLifter l{Architecture::X86_64};auto *f=machine(l,true);verify(l);
  snapshot(l,"external-before.ll");
  PassPipeline p;REQUIRE(p.add("control-flow"));auto r=p.run(l);REQUIRE(r.ok);verify(l);
  // Input state can select the default or enter in the middle. No natural-entry
  // assumption may be inferred from the state-machine shape.
  CHECK_FALSE(f->getArg(2)->use_empty());
  unsigned writes=0;bool hasDefault=false;
  for(auto &b:*f)for(auto &i:b) {
    if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i))writes+=s->isVolatile();
    for(auto &op:i.operands())if(auto *v=llvm::dyn_cast<llvm::ConstantInt>(op.get()))hasDefault|=v->equalsInt(0xbad);
  }
  CHECK(writes>=2);CHECK(hasDefault);
  snapshot(l,"external-after.ll");
}

TEST_CASE("LLVM control flow preserves unknown call and volatile state reads", "[llvm][control-flow]") {
  LLVMLifter l{Architecture::X86_64};auto &c=l.module().getContext();auto *t=llvm::Type::getInt32Ty(c);
  auto *p=llvm::PointerType::getUnqual(c);
  auto *callee=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(c),{p},false),
      llvm::GlobalValue::ExternalLinkage,"unknown_effect",l.module());
  auto *f=llvm::Function::Create(llvm::FunctionType::get(t,{p},false),llvm::GlobalValue::ExternalLinkage,"call",l.module());
  auto *entry=llvm::BasicBlock::Create(c,"entry",f),*yes=llvm::BasicBlock::Create(c,"yes",f),*no=llvm::BasicBlock::Create(c,"no",f);
  Builder b{entry};b.CreateStore(llvm::ConstantInt::get(t,10),f->getArg(0));b.CreateCall(callee,{f->getArg(0)});
  auto *state=b.CreateLoad(t,f->getArg(0));state->setVolatile(true);
  b.CreateCondBr(b.CreateICmpEQ(state,llvm::ConstantInt::get(t,10)),yes,no);
  b.SetInsertPoint(yes);b.CreateRet(llvm::ConstantInt::get(t,1));
  b.SetInsertPoint(no);b.CreateRet(llvm::ConstantInt::get(t,2));
  PassPipeline pipeline;REQUIRE(pipeline.add("control-flow"));REQUIRE(pipeline.run(l).ok);verify(l);
  unsigned calls=0,reads=0;
  for(auto &bb:*f)for(auto &i:bb) {
    if(auto *call=llvm::dyn_cast<llvm::CallInst>(&i))calls+=call->getCalledFunction()==callee;
    if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&i))reads+=load->isVolatile();
  }
  CHECK(calls==1);CHECK(reads==1);
  CHECK_FALSE(f->getArg(0)->use_empty());
}

TEST_CASE("LLVM control flow is opt in and rejects unknown options", "[llvm][control-flow]") {
  PassPipeline p;CHECK_FALSE(p.add("control-flow",{{"assume_initialized",true}}));
  REQUIRE(p.add("control-flow"));
  for(const auto &name:PassPipeline::defaultLLVM())CHECK(name!="control-flow");
  LLVMLifter l{Architecture::X86_64};machine(l);auto r=p.run(l,{4,4,true});
  REQUIRE(r.ok);CHECK(r.converged);CHECK_FALSE(r.budgetExhausted);verify(l);
}
