#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>
#include <algorithm>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
using namespace dragon;
using namespace dragon::llvmlifter;
using namespace dragon::passes;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
llvm::BasicBlock *named(llvm::Function &function, llvm::StringRef name) {
  for (auto &block : function) if (block.getName() == name) return &block;
  return nullptr;
}
llvm::Function *fixture(LLVMLifter &lifter, bool externalUse = false) {
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  auto &module = lifter.module(); module.setDataLayout("e-p:64:64");
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *f = llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,i64,i64,i1},false),
      llvm::GlobalValue::ExternalLinkage,"prefix",module);
  f->getArg(0)->setName("memory");f->getArg(1)->setName("seed");
  f->getArg(2)->setName("limit");f->getArg(3)->setName("accepted");
  auto block = [&](const char *name){return llvm::BasicBlock::Create(context,name,f);};
  auto *entry=block("entry"),*accept=block("guarded.accept"),*fallback=block("fallback");
  auto *header=block("header"),*latch=block("latch"),*dispatch=block("dispatch");
  auto *leaf=block("terminal"),*outside=block("outside");
  Builder b(entry);b.CreateCondBr(f->getArg(3),accept,fallback);
  b.SetInsertPoint(accept);b.CreateBr(header);b.SetInsertPoint(fallback);b.CreateBr(header);
  b.SetInsertPoint(header);auto *value=b.CreatePHI(i64,3,"current");
  value->addIncoming(f->getArg(1),accept);value->addIncoming(f->getArg(1),fallback);
  auto *loaded=b.CreateLoad(i64,f->getArg(0),"fresh.load");loaded->setAlignment(llvm::Align(1));
  auto *fresh=b.CreateFreeze(loaded,"fresh.freeze");
  b.CreateCondBr(b.CreateICmpULT(value,f->getArg(2)),latch,dispatch);
  b.SetInsertPoint(latch);auto *next=b.CreateAdd(value,b.getInt64(1),"next");
  b.CreateStore(next,f->getArg(0))->setAlignment(llvm::Align(1));b.CreateBr(header);value->addIncoming(next,latch);
  b.SetInsertPoint(dispatch);auto *selector=b.CreateXor(fresh,value,"selector");
  auto *jump=b.CreateSwitch(selector,leaf);jump->addCase(b.getInt64(0),leaf);jump->addCase(b.getInt64(7),outside);
  b.SetInsertPoint(leaf);auto *selected=b.CreatePHI(i64,2,"exit.selector");
  selected->addIncoming(selector,dispatch);selected->addIncoming(selector,dispatch);
  b.CreateStore(selected,f->getArg(0))->setAlignment(llvm::Align(1));b.CreateRet(selected);
  b.SetInsertPoint(outside);
  auto *self=b.CreateCall(f,{f->getArg(0),f->getArg(1),f->getArg(2),b.getFalse()});
  b.CreateRet(externalUse?selector:self);
  REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return f;
}
Report run(LLVMLifter &lifter, std::int64_t blocks = 32, std::int64_t instructions = 2048) {
  PassPipeline pipeline;
  REQUIRE(pipeline.add("entry-prefix",{{"max_blocks",blocks},{"max_instructions",instructions}}));
  auto result=pipeline.run(lifter);REQUIRE(result.ok);REQUIRE(result.runs.size()==1);
  REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));return result;
}
}
TEST_CASE("Entry prefix commits without replacing module function or arguments", "[llvm][pipeline][entry-prefix]") {
  LLVMLifter lifter{Architecture::X86_64};auto *f=fixture(lifter);
  auto *module=&lifter.module();auto *argument=f->getArg(0);
  f->setVisibility(llvm::GlobalValue::HiddenVisibility);
  f->setSection(".text.entry_prefix");f->addFnAttr(llvm::Attribute::NoInline);
  auto *comdat=module->getOrInsertComdat("prefix.group");f->setComdat(comdat);
  const auto attributes=f->getAttributes();
  auto &context=module->getContext();
  auto *pointerGlobal=new llvm::GlobalVariable(*module,f->getType(),true,
      llvm::GlobalValue::InternalLinkage,f,"function.identity");
  auto *global=new llvm::GlobalVariable(*module,llvm::Type::getInt64Ty(context),false,
      llvm::GlobalValue::InternalLinkage,llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),0),"global.state");
  auto *functionMetadata=llvm::MDNode::get(context,{llvm::MDString::get(context,"preserve-function")});
  auto *instructionMetadata=llvm::MDNode::get(context,{llvm::MDString::get(context,"preserve-instruction")});
  f->setMetadata("entry-prefix.marker",functionMetadata);
  Builder globals(&f->getEntryBlock().front());
  auto *oldGlobal=globals.CreateLoad(globals.getInt64Ty(),global,"old.global");
  auto *globalWrite=globals.CreateStore(globals.CreateAdd(oldGlobal,globals.getInt64(1)),global);
  globalWrite->setMetadata("entry-prefix.marker",instructionMetadata);
  auto *caller=llvm::Function::Create(f->getFunctionType(),llvm::GlobalValue::ExternalLinkage,"caller",*module);
  Builder callBuilder(llvm::BasicBlock::Create(context,"entry",caller));
  auto *externalCall=callBuilder.CreateCall(f,{caller->getArg(0),caller->getArg(1),caller->getArg(2),caller->getArg(3)});
  callBuilder.CreateRet(externalCall);
  REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  auto report=run(lifter);const auto &counts=report.runs[0].result.counters;
  CHECK(report.changed);CHECK(counts.at("candidate_edges")==1);CHECK(counts.at("peeled_edges")==1);
  CHECK(counts.at("terminal_edges")==2);CHECK(counts.at("backedges")==1);
  CHECK(&lifter.module()==module);CHECK(module->getFunction("prefix")==f);CHECK(f->getArg(0)==argument);
  CHECK(module->size()==2);CHECK(f->getComdat()==comdat);
  CHECK(pointerGlobal->getInitializer()==f);CHECK(externalCall->getCalledFunction()==f);
  CHECK(module->getFunction("caller")==caller);
  CHECK(f->getMetadata("entry-prefix.marker")==functionMetadata);
  unsigned globalLoads=0,globalStores=0;
  for(auto &block:*f)for(auto &instruction:block) {
    if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&instruction))
      globalLoads+=load->getPointerOperand()==global;
    if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&instruction))if(store->getPointerOperand()==global) {
      ++globalStores;CHECK(store->getMetadata("entry-prefix.marker")==instructionMetadata);
    }
  }
  CHECK(globalLoads==1);CHECK(globalStores==1);
  for(auto &function:*module)CHECK_FALSE(function.getName().contains(".entry.prefix.trial"));
  for(auto &function:*module)CHECK_FALSE(function.getName().contains(".entry.prefix.rollback"));
  CHECK(f->getVisibility()==llvm::GlobalValue::HiddenVisibility);
  CHECK(f->getSection()==".text.entry_prefix");CHECK(f->getAttributes()==attributes);
  CHECK(f->getLinkage()==llvm::GlobalValue::ExternalLinkage);
  auto *accept=named(*f,"guarded.accept");auto *root=accept->getTerminator()->getSuccessor(0);
  REQUIRE(root!=nullptr);CHECK(root->getName()=="header.entry.prefix");CHECK(root->phis().empty());
  auto *copiedLatch=named(*f,"latch.entry.prefix");REQUIRE(copiedLatch);
  CHECK(copiedLatch->getTerminator()->getSuccessor(0)==named(*f,"header"));
  auto *headerPhi=llvm::dyn_cast<llvm::PHINode>(&named(*f,"header")->front());REQUIRE(headerPhi);
  CHECK(headerPhi->getBasicBlockIndex(accept)==-1);
  CHECK(headerPhi->getBasicBlockIndex(copiedLatch)>=0);
  auto *leaf=named(*f,"terminal.entry.prefix");REQUIRE(leaf);
  auto *phi=llvm::dyn_cast<llvm::PHINode>(&leaf->front());REQUIRE(phi);CHECK(phi->getNumIncomingValues()==2);
  for(unsigned i=0;i<2;++i) {
    CHECK(phi->getIncomingBlock(i)==named(*f,"dispatch.entry.prefix"));
    auto *input=llvm::dyn_cast<llvm::Instruction>(phi->getIncomingValue(i));REQUIRE(input);
    CHECK(input->getParent()==named(*f,"dispatch.entry.prefix"));
  }
  unsigned freezes=0,selfCalls=0;
  for(auto &block:*f)for(auto &instruction:block) {
    if(llvm::isa<llvm::FreezeInst>(instruction))++freezes;
    if(auto *call=llvm::dyn_cast<llvm::CallBase>(&instruction)){++selfCalls;CHECK(call->getCalledFunction()==f);}
  }
  CHECK(freezes==2);CHECK(selfCalls==1);
  const auto once=lifter.ir();auto again=run(lifter);
  CHECK_FALSE(again.changed);CHECK(again.runs[0].result.counters.at("skipped_single_entry")==1);
  CHECK(lifter.ir()==once);
}
TEST_CASE("Entry prefix direct call liveout gains while nonlocal rejection stays transactional", "[llvm][pipeline][entry-prefix]") {
  SECTION("previous direct-return refusal is now a precisely repaired exit") {
    // Preserve the original fixture: only its obsolete refusal expectation is
    // upgraded. The fresh selector must survive the original unknown call.
    LLVMLifter lifter{Architecture::X86_64};auto *f=fixture(lifter,true);
    auto report=run(lifter);CHECK(report.changed);
    CHECK(report.runs[0].result.counters.at("peeled_edges")==1);
    auto *outside=named(*f,"outside");REQUIRE(outside);REQUIRE(!outside->phis().empty());
    auto &phi=*outside->phis().begin();CHECK(phi.getNumIncomingValues()==2);
    CHECK(llvm::cast<llvm::ReturnInst>(outside->getTerminator())->getReturnValue()==&phi);
    for(unsigned i=0;i<2;++i) {
      auto *value=llvm::dyn_cast<llvm::Instruction>(phi.getIncomingValue(i));REQUIRE(value);
      CHECK(value->getParent()==phi.getIncomingBlock(i));
    }
    CHECK(named(*f,"outside.entry.prefix")==nullptr);
  }
  SECTION("ordinary use beyond the direct exit remains rejected") {
    LLVMLifter lifter{Architecture::X86_64};auto *f=fixture(lifter,true);auto *root=named(*f,"header");
    auto *outside=named(*f,"outside");auto *ret=llvm::cast<llvm::ReturnInst>(outside->getTerminator());
    auto *value=ret->getReturnValue();ret->eraseFromParent();
    auto *remote=llvm::BasicBlock::Create(lifter.module().getContext(),"remote",f);
    llvm::BranchInst::Create(remote,outside);Builder(remote).CreateRet(value);
    REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
    const auto before=lifter.ir();auto report=run(lifter);
    CHECK_FALSE(report.changed);CHECK(report.runs[0].result.counters.at("rejected")==1);
    CHECK(report.runs[0].result.status==Status::Incomplete);
    CHECK(lifter.ir()==before);CHECK(named(*f,"header")==root);CHECK(lifter.module().size()==1);
  }
}
TEST_CASE("Entry prefix budgets report incomplete without altering rejected IR", "[llvm][pipeline][entry-prefix]") {
  for(auto cap:{0,1}) DYNAMIC_SECTION("blocks "<<cap) {
    LLVMLifter lifter{Architecture::X86_64};fixture(lifter);const auto before=lifter.ir();auto report=run(lifter,cap);
    CHECK_FALSE(report.changed);CHECK(report.runs[0].result.status==Status::Incomplete);
    CHECK(report.runs[0].result.counters.at("budget_exhausted")==1);CHECK(lifter.ir()==before);
  }
}
TEST_CASE("Entry prefix rejects address taken function identity transactionally", "[llvm][pipeline][entry-prefix]") {
  LLVMLifter lifter{Architecture::X86_64};auto *f=fixture(lifter);
  auto *address=llvm::BlockAddress::get(f,named(*f,"outside"));
  new llvm::GlobalVariable(lifter.module(),address->getType(),true,llvm::GlobalValue::InternalLinkage,address,"escaped.block");
  REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  const auto before=lifter.ir();auto report=run(lifter);
  CHECK_FALSE(report.changed);CHECK(report.runs[0].result.counters.at("rejected")==1);CHECK(lifter.ir()==before);
}
TEST_CASE("Entry prefix validates options without changing the default pipeline", "[llvm][pipeline][entry-prefix]") {
  const auto defaults=PassPipeline::defaultLLVM();
  CHECK(std::find(defaults.begin(),defaults.end(),"entry-prefix")==defaults.end());
  for(const Parameter &bad:{Parameter{true},Parameter{std::string{"32"}},Parameter{std::int64_t{-1}},Parameter{std::int64_t{4097}}}) {
    PassPipeline p;CHECK_FALSE(p.add("entry-prefix",{{"max_blocks",bad}}));
  }
  PassPipeline p;CHECK_FALSE(p.add("entry-prefix",{{"max_instructions",std::int64_t{65537}}}));
}
