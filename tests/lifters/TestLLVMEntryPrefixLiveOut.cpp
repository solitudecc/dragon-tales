#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
using namespace dragon;
using namespace dragon::llvmlifter;
using namespace dragon::passes;
namespace {
using Builder=llvm::IRBuilder<llvm::NoFolder>;
struct Config { unsigned values=1, uses=1, edges=1; bool loop=false, remote=false, join=false; };
llvm::BasicBlock *named(llvm::Function &f, llvm::StringRef name) {
  for(auto &b:f)if(b.getName()==name)return &b;return nullptr;
}
llvm::Function *make(LLVMLifter &lifter, Config config={}) {
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  auto &m=lifter.module();m.setDataLayout("e-p:64:64");auto &c=m.getContext();
  auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);auto *i1=llvm::Type::getInt1Ty(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,i64,i1},false),llvm::GlobalValue::ExternalLinkage,"liveout",m);
  auto block=[&](const char *s){return llvm::BasicBlock::Create(c,s,f);};
  auto *entry=block("entry"),*accepted=block("guarded.accept"),*fallback=block("fallback"),*head=block("head"),*out=block("out");
  auto *remote=config.remote||config.loop?block("remote"):nullptr;
  auto *middle=config.join?block("middle"):nullptr;
  Builder b(entry);auto *seed=b.CreateFreeze(f->getArg(1),"entry.snapshot");b.CreateCondBr(f->getArg(2),accepted,fallback);
  b.SetInsertPoint(accepted);b.CreateBr(head);b.SetInsertPoint(fallback);b.CreateBr(head);
  b.SetInsertPoint(head);llvm::PHINode *history=nullptr;
  if(config.loop){history=b.CreatePHI(i64,3,"history");history->addIncoming(seed,accepted);history->addIncoming(seed,fallback);}
  llvm::SmallVector<llvm::Value *,8> fresh;
  for(unsigned n=0;n<config.values;++n) {
    auto *load=b.CreateLoad(i64,f->getArg(0),"fresh.load");load->setAlignment(llvm::Align(1));
    fresh.push_back(b.CreateFreeze(load,"fresh.freeze"));
  }
  if(config.join)b.CreateCondBr(f->getArg(2),out,middle);
  else if(config.edges>1) {
    auto *jump=b.CreateSwitch(f->getArg(1),out);
    for(unsigned n=1;n<config.edges;++n)jump->addCase(b.getInt64(n),out);
  } else b.CreateBr(out);
  if(middle){b.SetInsertPoint(middle);b.CreateBr(out);}
  b.SetInsertPoint(out);
  auto callee=m.getOrInsertFunction("unknown.writer",llvm::FunctionType::get(b.getVoidTy(),{ptr},false));
  b.CreateCall(callee,{f->getArg(0)});
  llvm::Value *answer=history?static_cast<llvm::Value *>(history):seed;
  for(auto *value:fresh)for(unsigned n=0;n<config.uses;++n)answer=b.CreateAdd(answer,value,"consume");
  b.CreateStore(answer,f->getArg(0))->setAlignment(llvm::Align(1));
  if(config.loop) {
    auto *snapshot=b.CreateAdd(fresh.front(),b.getInt64(0),"loop.snapshot");history->addIncoming(snapshot,out);
    b.CreateCondBr(f->getArg(2),head,remote);
  } else if(config.remote)b.CreateBr(remote);
  else b.CreateRet(answer);
  if(remote){b.SetInsertPoint(remote);b.CreateRet(config.remote?fresh.front():answer);}
  REQUIRE_FALSE(llvm::verifyModule(m,&llvm::errs()));return f;
}
Report run(LLVMLifter &lifter,std::int64_t instructions=2048) {
  PassPipeline pipeline;REQUIRE(pipeline.add("entry-prefix",{{"max_blocks",std::int64_t{32}},{"max_instructions",instructions}}));
  auto result=pipeline.run(lifter);REQUIRE(result.ok);REQUIRE(result.runs.size()==1);
  REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));return result;
}
}
TEST_CASE("Entry prefix bridges direct call exit with fresh SSA values", "[llvm][entry-prefix][entry-prefix-liveout]") {
  for(bool loop:{false,true})DYNAMIC_SECTION("loop "<<loop) {
    LLVMLifter lifter{Architecture::X86_64};auto *f=make(lifter,{1,1,1,loop});
    auto report=run(lifter);CHECK(report.changed);CHECK(report.runs[0].result.counters.at("peeled_edges")==1);
    auto *head=named(*f,"head");auto *copy=named(*f,"head.entry.prefix");REQUIRE(copy);
    auto *out=named(*f,"out");CHECK(named(*f,"out.entry.prefix")==nullptr);
    unsigned bridges=0;
    for(auto &phi:out->phis()) {
      ++bridges;CHECK(phi.getNumIncomingValues()==2);
      auto *old=llvm::dyn_cast<llvm::Instruction>(phi.getIncomingValueForBlock(head));REQUIRE(old);CHECK(old->getParent()==head);
      auto *newValue=phi.getIncomingValueForBlock(copy);
      if(auto *definition=llvm::dyn_cast<llvm::Instruction>(newValue))
        CHECK((definition->getParent()==copy||definition->getParent()==&f->getEntryBlock()));
    }
    CHECK(bridges==(loop?2:1));
    unsigned calls=0,freezes=0;
    for(auto &block:*f)for(auto &instruction:block) {
      if(llvm::isa<llvm::CallBase>(instruction)){++calls;CHECK(instruction.getParent()==out);}
      if(llvm::isa<llvm::FreezeInst>(instruction))++freezes;
    }
    CHECK(calls==1);CHECK(freezes==3);
    if(loop) {
      auto &history=*head->phis().begin();auto *incoming=llvm::dyn_cast<llvm::Instruction>(history.getIncomingValueForBlock(out));
      REQUIRE(incoming);CHECK(incoming->getName()=="loop.snapshot");CHECK(incoming->getParent()==out);
    }
  }
}
TEST_CASE("Entry prefix bridge preserves duplicate successor multiplicity", "[llvm][entry-prefix][entry-prefix-liveout]") {
  LLVMLifter lifter{Architecture::X86_64};auto *f=make(lifter,{1,1,3});auto report=run(lifter);
  CHECK(report.changed);auto *out=named(*f,"out");REQUIRE(!out->phis().empty());
  auto &phi=*out->phis().begin();CHECK(phi.getNumIncomingValues()==6);
  unsigned originals=0,copies=0;
  for(unsigned i=0;i<phi.getNumIncomingValues();++i) {
    originals+=phi.getIncomingBlock(i)==named(*f,"head");copies+=phi.getIncomingBlock(i)==named(*f,"head.entry.prefix");
  }
  CHECK(originals==3);CHECK(copies==3);
}
TEST_CASE("Entry prefix bridge cost shares original instruction budget", "[llvm][entry-prefix][entry-prefix-liveout]") {
  for(unsigned limit:{3u,4u})DYNAMIC_SECTION("limit "<<limit) {
    LLVMLifter lifter{Architecture::X86_64};make(lifter);const auto before=lifter.ir();auto report=run(lifter,limit);
    if(limit==3){CHECK_FALSE(report.changed);CHECK(lifter.ir()==before);CHECK(report.runs[0].result.counters.at("budget_exhausted")==1);}
    else {CHECK(report.changed);CHECK(report.runs[0].result.counters.at("cloned_instructions")==4);}
  }
}
TEST_CASE("Entry prefix keeps nonlocal and multiple predecessor liveouts rejected", "[llvm][entry-prefix][entry-prefix-liveout]") {
  for(auto config:{Config{1,1,1,false,true,false},Config{1,1,1,false,false,true}}) {
    LLVMLifter lifter{Architecture::X86_64};make(lifter,config);const auto before=lifter.ir();auto report=run(lifter);
    CHECK_FALSE(report.changed);CHECK(report.runs[0].result.status==Status::Incomplete);CHECK(lifter.ir()==before);
  }
}
TEST_CASE("Entry prefix direct exit repair caps are explicit", "[llvm][entry-prefix][entry-prefix-liveout]") {
  for(auto config:{Config{64,1,1},Config{65,1,1},Config{1,512,1},Config{1,513,1},Config{1,1,256},Config{1,1,257}})
    DYNAMIC_SECTION("values "<<config.values<<" uses "<<config.uses<<" edges "<<config.edges) {
      LLVMLifter lifter{Architecture::X86_64};make(lifter,config);const auto before=lifter.ir();auto report=run(lifter);
      const bool exceeds=config.values>64||config.uses>512||config.edges>256;
      if(exceeds){CHECK_FALSE(report.changed);CHECK(lifter.ir()==before);CHECK(report.runs[0].result.counters.at("budget_exhausted")==1);}
      else CHECK(report.changed);
    }
}
