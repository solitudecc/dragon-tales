#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/raw_ostream.h>
#include <limits>

using namespace dragon;
using namespace dragon::llvmlifter;
namespace {
constexpr const char *Root="dragon_static_entry_x86_64_1000";
std::unique_ptr<LLVMLifter> source() {
  Configuration configuration;Graph graph{Architecture::X86_64,configuration};
  Disassembler dis{Architecture::X86_64,configuration};
  const std::vector<std::uint8_t> bytes{0x48,0x8b,0x04,0x25,0x18,0,0,0,0xc3};
  dis.disassemble(bytes,0x1000,graph);
  auto lifter=std::make_unique<LLVMLifter>(Architecture::X86_64);
  REQUIRE(lifter->setExecutionMode(ExecutionMode::StaticAnalysis));
  REQUIRE(lifter->setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
  REQUIRE(lifter->lift(Function(&graph,0x1000),graph));
  lifter->module().setDataLayout("e-p:64:64");
  REQUIRE_FALSE(llvm::verifyModule(lifter->module(),&llvm::errs()));return lifter;
}
GuardedEntryContext facts() {
  return {0x1000,"test_1","unit fixture",{{32,{0,1,0,0,0,0,0,0}}},{{24,{123,0,0,0,0,0,0,0}}}};
}
unsigned targetLoads(llvm::Function &function) {
  unsigned result=0;
  for(auto &block:function)for(auto &instruction:block)if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
    auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
    if(!load->getType()->isIntegerTy(64)||!gep||gep->getNumIndices()!=1||!gep->getSourceElementType()->isIntegerTy(8))continue;
    auto *index=llvm::dyn_cast<llvm::ConstantInt>(gep->idx_begin()->get());
    result+=index&&index->equalsInt(24);
  }return result;
}
}
TEST_CASE("Guarded entry owns its context and preserves the ordinary root", "[llvm][guarded-entry]") {
  auto original=source();const auto before=original->ir();auto context=facts();std::string error;
  auto view=original->createGuardedEntryView(context,error);INFO(error);REQUIRE(view);
  CHECK(error.empty());CHECK(original->ir()==before);CHECK(original->guardedEntryManifest()=="{}");
  CHECK(&original->module().getContext()!=&view->module().getContext());
  auto *generic=view->module().getFunction(Root);REQUIRE(generic);
  auto *guarded=view->module().getFunction(std::string(Root)+".guarded.test_1");REQUIRE(guarded);
  CHECK(&*view->module().begin()==guarded);
  auto parsed=llvm::json::parse(view->guardedEntryManifest());REQUIRE(parsed);auto *manifest=parsed->getAsObject();REQUIRE(manifest);
  CHECK(manifest->getString("root_symbol")==Root);CHECK(manifest->getString("context_id")=="test_1");
  REQUIRE(manifest->getString("source_sha256"));CHECK(manifest->getString("source_sha256")->size()==64);
  auto *entry=llvm::dyn_cast<llvm::SwitchInst>(guarded->getEntryBlock().getTerminator());REQUIRE(entry);
  CHECK(entry->getNumCases()==1);CHECK(entry->getDefaultDest()->getName()=="entry.unsupported");
  unsigned fallbackCalls=0;
  for(auto &block:*guarded)for(auto &i:block)if(auto *call=llvm::dyn_cast<llvm::CallBase>(&i))fallbackCalls+=call->getCalledFunction()==generic;
  CHECK(fallbackCalls==1);
  original.reset();view->runMem2Reg();view->runInstCombine();view->runGVN();
  CHECK(targetLoads(*guarded)==1); // guard LOAD remains; matching body LOAD folds
  REQUIRE_FALSE(llvm::verifyModule(view->module(),&llvm::errs()));
}
TEST_CASE("Guarded entry retains ordinary recursion and machine loop reentry", "[llvm][guarded-entry]") {
  auto original=source();auto *root=original->module().getFunction(Root);REQUIRE(root);
  auto *entry=llvm::cast<llvm::SwitchInst>(root->getEntryBlock().getTerminator());auto *body=entry->case_begin()->getCaseSuccessor();
  auto *loop=llvm::BasicBlock::Create(original->module().getContext(),"test.loop",root);llvm::IRBuilder<>(loop).CreateBr(body);
  auto *phi=llvm::PHINode::Create(llvm::Type::getInt64Ty(original->module().getContext()),2,"test.entry.phi",body->begin());
  phi->addIncoming(llvm::ConstantInt::get(phi->getType(),1),&root->getEntryBlock());phi->addIncoming(llvm::ConstantInt::get(phi->getType(),2),loop);
  llvm::IRBuilder<> b(body,body->getFirstInsertionPt());b.CreateCall(root,{root->getArg(0),root->getArg(1),root->getArg(2),root->getArg(3)});
  REQUIRE_FALSE(llvm::verifyModule(original->module(),&llvm::errs()));
  std::string error;auto view=original->createGuardedEntryView(facts(),error);INFO(error);REQUIRE(view);
  auto *guarded=view->module().getFunction(std::string(Root)+".guarded.test_1");REQUIRE(guarded);
  unsigned normalCalls=0,guardedCalls=0;bool repaired=false,loopBypasses=false;
  for(auto &block:*guarded){
    for(auto &instruction:block){
      if(auto *call=llvm::dyn_cast<llvm::CallBase>(&instruction)){normalCalls+=call->getCalledFunction()==view->module().getFunction(Root);guardedCalls+=call->getCalledFunction()==guarded;}
      if(auto *p=llvm::dyn_cast<llvm::PHINode>(&instruction);p&&p->getName()=="test.entry.phi")for(unsigned n=0;n<p->getNumIncomingValues();++n)repaired|=p->getIncomingBlock(n)->getName()=="guarded.accept";
    }
    if(block.getName()=="test.loop")loopBypasses=block.getTerminator()->getSuccessor(0)->getName()==body->getName();
  }
  CHECK(normalCalls==2);CHECK(guardedCalls==0);CHECK(repaired);CHECK(loopBypasses);
  REQUIRE_FALSE(llvm::verifyModule(view->module(),&llvm::errs()));
}
TEST_CASE("Guarded entry refuses invalid contexts without changing the source", "[llvm][guarded-entry]") {
  auto original=source();const auto before=original->ir();
  for(unsigned which=0;which<13;++which)DYNAMIC_SECTION("invalid "<<which){
    auto context=facts();
    switch(which){
      case 0:context.contextId="";break;case 1:context.contextId="bad id";break;
      case 2:context.provenance=std::string(1,char(0xff));break;
      case 3:context.provenance="";break;
      case 4:context.registers.clear();context.memory.clear();break;
      case 5:context.memory.push_back(context.memory.front());break;
      case 6:context.memory[0].bytes.clear();break;
      case 7:context.memory[0].offset=std::numeric_limits<std::uint64_t>::max();break;
      case 8:context.memory[0].bytes.resize(4097);break;
      case 9:context.registers[0].offset=488;break;
      case 10:context.registers={{487,{1}}};break;
      case 11:context.entry=0x1001;break;
      case 12:context.contextId=std::string(65,'a');break;
    }
    std::string error;auto view=original->createGuardedEntryView(context,error);CHECK_FALSE(view);CHECK_FALSE(error.empty());CHECK(original->ir()==before);
  }
}
TEST_CASE("Guarded entry rejects optimized or already contextualized sources", "[llvm][guarded-entry]") {
  auto original=source();std::string error;auto view=original->createGuardedEntryView(facts(),error);REQUIRE(view);
  auto another=facts();another.contextId="again";CHECK_FALSE(view->createGuardedEntryView(another,error));CHECK_FALSE(error.empty());
  original->runMem2Reg();const auto before=original->ir();CHECK_FALSE(original->createGuardedEntryView(facts(),error));CHECK_FALSE(error.empty());CHECK(original->ir()==before);
}

TEST_CASE("Guarded entry rejects observable prologue locals and extra entry reads", "[llvm][guarded-entry]") {
  for(unsigned mode=0;mode<3;++mode)DYNAMIC_SECTION("noncanonical "<<mode) {
    auto original=source();auto *root=original->module().getFunction(Root);REQUIRE(root);
    auto &entry=root->getEntryBlock();auto *entrySwitch=llvm::cast<llvm::SwitchInst>(entry.getTerminator());
    llvm::AllocaInst *slot=nullptr;for(auto &instruction:entry)if((slot=llvm::dyn_cast<llvm::AllocaInst>(&instruction)))break;
    REQUIRE(slot);
    if(mode==0){
      llvm::IRBuilder<> b(entrySwitch->case_begin()->getCaseSuccessor(),entrySwitch->case_begin()->getCaseSuccessor()->getFirstInsertionPt());
      b.CreatePtrToInt(slot,b.getInt64Ty(),"observable.host.address");
    }else if(mode==1){
      llvm::IRBuilder<> b(entrySwitch->case_begin()->getCaseSuccessor(),entrySwitch->case_begin()->getCaseSuccessor()->getFirstInsertionPt());
      auto callee=original->module().getOrInsertFunction("capture.local",llvm::FunctionType::get(b.getVoidTy(),{slot->getType()},false));b.CreateCall(callee,{slot});
    }else{
      llvm::IRBuilder<> b(entrySwitch);b.CreateAlignedLoad(b.getInt64Ty(),root->getArg(3),llvm::Align(1),"extra.entry.read");
    }
    REQUIRE_FALSE(llvm::verifyModule(original->module(),&llvm::errs()));const auto before=original->ir();std::string error;
    CHECK_FALSE(original->createGuardedEntryView(facts(),error));CHECK_FALSE(error.empty());CHECK(original->ir()==before);
  }
}
TEST_CASE("Guarded entry does not inherit unverified optimization promises", "[llvm][guarded-entry]") {
  auto original=source();auto *root=original->module().getFunction(Root);REQUIRE(root);
  // Attribute transport control, not a native proof of caller-added promises.
  for(auto attribute:{llvm::Attribute::Speculatable,llvm::Attribute::NoRecurse,
      llvm::Attribute::NoSync,llvm::Attribute::WillReturn,llvm::Attribute::NoFree,
      llvm::Attribute::NoCallback,llvm::Attribute::NoUnwind,llvm::Attribute::MustProgress})root->addFnAttr(attribute);
  root->setMemoryEffects(llvm::MemoryEffects::readOnly());
  root->addParamAttr(0,llvm::Attribute::NoUndef);root->addFnAttr("target-cpu","x86-64");
  const auto before=original->ir();std::string error;auto view=original->createGuardedEntryView(facts(),error);INFO(error);REQUIRE(view);
  auto *guarded=view->module().getFunction(std::string(Root)+".guarded.test_1");REQUIRE(guarded);
  for(auto attribute:{llvm::Attribute::Memory,llvm::Attribute::Speculatable,llvm::Attribute::NoRecurse,
      llvm::Attribute::NoSync,llvm::Attribute::WillReturn,llvm::Attribute::NoFree,
      llvm::Attribute::NoCallback,llvm::Attribute::NoUnwind,llvm::Attribute::MustProgress})CHECK_FALSE(guarded->hasFnAttribute(attribute));
  CHECK(guarded->hasParamAttribute(0,llvm::Attribute::NoUndef));
  CHECK(guarded->getFnAttribute("target-cpu").getValueAsString()=="x86-64");
  CHECK(guarded->getCallingConv()==root->getCallingConv());CHECK(original->ir()==before);
  CHECK(view->module().getFunction(Root)->hasFnAttribute(llvm::Attribute::Speculatable));
  REQUIRE_FALSE(llvm::verifyModule(view->module(),&llvm::errs()));
}
TEST_CASE("Guarded entry respects convergence and duplication restrictions", "[llvm][guarded-entry]") {
  for(bool onCall:{false,true})for(auto restriction:{llvm::Attribute::Convergent,llvm::Attribute::NoDuplicate})
    DYNAMIC_SECTION("call "<<onCall<<" attribute "<<static_cast<unsigned>(restriction)) {
      auto original=source();auto *root=original->module().getFunction(Root);REQUIRE(root);
      if(onCall){
        auto *entry=llvm::cast<llvm::SwitchInst>(root->getEntryBlock().getTerminator());auto *body=entry->case_begin()->getCaseSuccessor();
        llvm::IRBuilder<> b(body,body->getFirstInsertionPt());
        auto callee=original->module().getOrInsertFunction("restricted.callee",llvm::FunctionType::get(b.getVoidTy(),{},false));
        llvm::cast<llvm::Function>(callee.getCallee())->addFnAttr(restriction);b.CreateCall(callee,{});
      }else root->addFnAttr(restriction);
      REQUIRE_FALSE(llvm::verifyModule(original->module(),&llvm::errs()));const auto before=original->ir();std::string error;
      CHECK_FALSE(original->createGuardedEntryView(facts(),error));CHECK_FALSE(error.empty());CHECK(original->ir()==before);
    }
}
