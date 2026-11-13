#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
struct Fixture {
 dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};llvm::Module &module=lifter.module();llvm::Function *function;
 Fixture(const std::string &mode="normal") {
  module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);
  function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64},false),llvm::GlobalValue::ExternalLinkage,"view",module);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,function);};auto *entry=block("entry"),*seed=block("seed"),*head=block("head"),*dispatch=block("dispatch"),*high=block("high"),*exit=block("exit");
  llvm::IRBuilder<llvm::NoFolder> b(entry);auto *guest=b.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));auto *size=b.CreateLoad(i64,b.CreateGEP(b.getInt8Ty(),function->getArg(1),b.getInt64(8)),"size");size->setAlignment(llvm::Align(1));
  auto *read=b.CreateLoad(i64,function->getArg(0),"state.read");read->setAlignment(llvm::Align(1));llvm::Value *state=read;
  if(mode=="freeze")state=b.CreateFreeze(state,"state.freeze");if(mode=="flags"){auto *add=llvm::cast<llvm::BinaryOperator>(b.CreateAdd(state,b.getInt64(0),"state.flags"));add->setHasNoUnsignedWrap();add->setHasNoSignedWrap();state=add;}if(mode=="undef")state=llvm::UndefValue::get(i64);if(mode=="poison")state=llvm::PoisonValue::get(i64);if(mode=="wide")state=b.CreateZExt(state,llvm::Type::getInt128Ty(c));
  llvm::Value *cmp=nullptr;if(mode=="early-cmp")cmp=b.CreateICmpUGT(state,b.getInt64(50),"original.cmp");b.CreateBr(seed);
  b.SetInsertPoint(seed);auto helper=module.getOrInsertFunction("maywrite",llvm::FunctionType::get(b.getVoidTy(),{ptr,ptr},false));if(mode=="seed-call")b.CreateCall(helper,{function->getArg(0),guest});
  auto *effect=b.CreateStore(function->getArg(2),b.CreateGEP(b.getInt8Ty(),guest,b.getInt64(512)));effect->setAlignment(llvm::Align(1));
  if(!cmp)cmp=mode=="swapped"?b.CreateICmpULT(b.getInt64(50),state,"original.cmp"):b.CreateICmpUGT(state,llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(state->getType()),50),"original.cmp");
  b.CreateCondBr(cmp,mode=="duplicate"?head:high,head);
  b.SetInsertPoint(head);auto *selected=b.CreatePHI(i64,mode=="duplicate"?2:1,"selected");auto *incoming=mode=="bad-path"?static_cast<llvm::Value *>(llvm::UndefValue::get(i64)):static_cast<llvm::Value *>(read);selected->addIncoming(incoming,seed);if(mode=="duplicate")selected->addIncoming(incoming,seed);
  if(mode=="tail-call")b.CreateCall(helper,{function->getArg(0),guest});b.CreateStore(selected,b.CreateGEP(b.getInt8Ty(),guest,b.getInt64(256)))->setAlignment(llvm::Align(1));
  auto *offset=b.CreateAdd(b.getInt64(1024),b.CreateShl(selected,b.getInt64(3)),"index");auto *range=b.CreateAnd(b.CreateICmpUGT(size,b.getInt64(7)),b.CreateICmpULE(offset,b.CreateSub(size,b.getInt64(8))));b.CreateCondBr(range,dispatch,exit);
  b.SetInsertPoint(dispatch);auto *address=b.CreateGEP(b.getInt8Ty(),guest,offset);auto *actual=b.CreateLoad(i64,address,"actual.target");actual->setAlignment(llvm::Align(1));if(mode=="table-after")b.CreateStore(function->getArg(2),address)->setAlignment(llvm::Align(1));auto *sw=b.CreateSwitch(actual,exit);sw->addCase(b.getInt64(100),exit);sw->addCase(b.getInt64(200),exit);
  b.SetInsertPoint(high);b.CreateStore(b.getInt64(777),function->getArg(0))->setAlignment(llvm::Align(1));b.CreateRet(b.getInt64(777));
  b.SetInsertPoint(exit);auto *out=b.CreatePHI(i64,4);out->addIncoming(b.getInt64(666),head);for(unsigned n=0;n<3;++n)out->addIncoming(actual,dispatch);b.CreateRet(out);
  std::string stateHint=mode=="wrong-state"?"0x3":"0x2",targetHint=mode=="wrong-target"?"0x999":"0x64";
  auto row="{\"function\":\"view\",\"block\":\"seed\",\"state\":\""+stateHint+"\",\"target\":\""+targetHint+"\"}";
  auto json="{\"hints\":["+row+(mode=="duplicate-hint"?","+row:"")+"]}";module.getOrInsertNamedMetadata("dragon.guarded-state-values")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));
  if(mode=="function-metadata")function->setMetadata("custom",llvm::MDNode::get(c,llvm::MDString::get(c,"preserve")));
  if(mode=="blockaddress")new llvm::GlobalVariable(module,ptr,true,llvm::GlobalValue::ExternalLinkage,llvm::BlockAddress::get(function,head),"block_address");
  new llvm::GlobalVariable(module,ptr,true,llvm::GlobalValue::ExternalLinkage,function,"saved_function");verify();
 }
 llvm::BasicBlock *named(llvm::StringRef n){for(auto &b:*function)if(b.getName()==n)return &b;return nullptr;}
 void verify(){REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));}
 std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
 Result run(Options o={}){PassPipeline p;REQUIRE(p.add("guarded-state-values",o));auto r=p.run(lifter);REQUIRE(r.runs.size()==1);verify();return r.runs.front().result;}
 void check(){auto *seed=named("seed");auto *guard=llvm::cast<llvm::BranchInst>(seed->getTerminator());auto *defined=llvm::dyn_cast<llvm::FreezeInst>(guard->getCondition());REQUIRE(defined);auto *eq=llvm::dyn_cast<llvm::ICmpInst>(defined->getOperand(0));REQUIRE(eq);CHECK(eq->getPredicate()==llvm::ICmpInst::ICMP_EQ);auto *fallback=llvm::dyn_cast<llvm::BranchInst>(guard->getSuccessor(1)->getTerminator());REQUIRE(fallback);CHECK(fallback->isConditional());CHECK(llvm::cast<llvm::Instruction>(fallback->getCondition())->getName()=="original.cmp");CHECK(fallback->getSuccessor(1)==named("head"));
  unsigned loads=0,targetGuards=0,effects=0,calls=0;for(auto &b:*function)for(auto &i:b){if(i.getName()=="state.read")++loads;if(llvm::isa<llvm::CallBase>(i))++calls;if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i);store && store->getParent()==seed)++effects;
   if(auto *branch=llvm::dyn_cast<llvm::BranchInst>(&i);branch && branch->isConditional())if(auto *condition=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());condition && condition->getPredicate()==llvm::ICmpInst::ICMP_EQ)if(auto *load=llvm::dyn_cast<llvm::LoadInst>(condition->getOperand(0));load && load->getParent()==&b){++targetGuards;auto *sw=llvm::dyn_cast<llvm::SwitchInst>(branch->getSuccessor(1)->getTerminator());REQUIRE(sw);CHECK(sw->getCondition()==load);CHECK(sw->getNumCases()==2);}
  }CHECK(loads==1);CHECK(effects==1);CHECK(targetGuards==1);CHECK(calls<=1);
 }
};
}
TEST_CASE("State value guards retain original condition effects and target fallback","[llvm][guarded-state-values]") {
 for(const char *mode:{"normal","swapped","early-cmp","freeze","flags","seed-call","wrong-state","table-after"}){INFO(mode);Fixture f(mode);auto *fn=f.function;auto *arg=fn->getArg(0);std::vector<std::string> names;for(auto &b:*fn)names.push_back(b.getName().str());auto r=f.run();REQUIRE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("rewritten_edges")==1);CHECK(r.counters.at("budget_exhausted")==0);CHECK(f.module.getFunction("view")==fn);CHECK(fn->getArg(0)==arg);CHECK(f.module.getGlobalVariable("saved_function")->getInitializer()==fn);unsigned n=0;for(auto &b:*fn){if(n<names.size())CHECK(b.getName()==names[n]);++n;}f.check();auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);}
}
TEST_CASE("State value guards repair every duplicate original branch slot","[llvm][guarded-state-values]") {
 Fixture f("duplicate");REQUIRE(f.run().changed);auto *original=llvm::dyn_cast<llvm::PHINode>(&f.named("head")->front());REQUIRE(original);CHECK(original->getNumIncomingValues()==2);CHECK(original->getIncomingBlock(0)==original->getIncomingBlock(1));CHECK(original->getIncomingValue(0)==original->getIncomingValue(1));CHECK(original->getIncomingBlock(0)->getName().starts_with("state.value.fallback"));f.check();
}
TEST_CASE("State value guards reject unsupported queries and roll back inner failure","[llvm][guarded-state-values]") {
 for(const char *mode:{"undef","poison","wide","tail-call","bad-path","wrong-target","duplicate-hint","function-metadata","blockaddress"}){INFO(mode);Fixture f(mode);auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("rejected")>0);CHECK(f.text()==before);for(auto &fn:f.module)CHECK_FALSE(fn.getName().contains(".trial"));if(std::string(mode)=="bad-path")CHECK(r.counters.at("attempted_edges")==1);}
}
TEST_CASE("State value guards obey copy budgets and remain opt in","[llvm][guarded-state-values]") {
 for(const char *key:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture f;auto before=f.text();auto r=f.run({{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==before);}
 Fixture f;auto before=f.text();auto r=f.run({{"max_blocks",std::int64_t(2)}});CHECK_FALSE(r.changed);CHECK(r.counters.at("attempted_edges")==1);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==before);
 CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"guarded-state-values")==PassPipeline::defaultLLVM().end());
}
