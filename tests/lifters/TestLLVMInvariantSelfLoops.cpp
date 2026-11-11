#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <string>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
 dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};llvm::Module &module=lifter.module();llvm::Function *function;
 Fixture(const std::string &mode="normal",bool duplicate=false){
  auto &c=module.getContext();auto *i64=llvm::Type::getInt64Ty(c);auto *ptr=llvm::PointerType::getUnqual(c);
  function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,i64,i64,llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"subject",module);
  auto *entry=llvm::BasicBlock::Create(c,"entry",function),*head=llvm::BasicBlock::Create(c,"head",function),*exit=llvm::BasicBlock::Create(c,"exit",function);
  llvm::IRBuilder<llvm::NoFolder> ir(entry);ir.CreateStore(ir.getInt64(17),function->getArg(0))->setAlignment(llvm::Align(1));
  if(mode=="no-external")ir.CreateRet(ir.getInt64(9));else if(duplicate)ir.CreateCondBr(function->getArg(3),head,head);else ir.CreateBr(head);
  ir.SetInsertPoint(head);auto *state=ir.CreatePHI(i64,4,"state"),*data=ir.CreatePHI(i64,4,"data");
  if(mode!="no-external")for(unsigned n=0;n<(duplicate?2u:1u);++n){state->addIncoming(mode=="phi-undef"?static_cast<llvm::Value *>(llvm::UndefValue::get(i64)):function->getArg(1),entry);data->addIncoming(function->getArg(2),entry);}
  llvm::Value *next=state;if(mode=="changing")next=ir.CreateSub(state,ir.getInt64(1));
  for(unsigned n=0;n<(mode=="both-self"?2u:1u);++n){state->addIncoming(mode=="cross-phi"?data:next,head);data->addIncoming(mode=="cross-phi"?state:data,head);}
  if(mode=="load")ir.CreateLoad(i64,function->getArg(0))->setAlignment(llvm::Align(1));
  if(mode=="store")ir.CreateStore(data,function->getArg(0))->setAlignment(llvm::Align(1));
  if(mode=="call"){auto callee=module.getOrInsertFunction("effect",llvm::FunctionType::get(ir.getVoidTy(),false));ir.CreateCall(callee);}
  llvm::Value *left=mode=="freeze"?ir.CreateFreeze(state):state;
  llvm::Value *right=mode=="undef"?static_cast<llvm::Value *>(llvm::UndefValue::get(i64)):mode=="poison"?static_cast<llvm::Value *>(llvm::PoisonValue::get(i64)):ir.getInt64(50);
  auto *cmp=ir.CreateICmpUGT(left,right,"condition");auto *br=ir.CreateCondBr(mode=="unknown-condition"?function->getArg(3):cmp,mode=="false-self"?exit:head,mode=="false-self"||mode=="both-self"?head:exit);
  br->setMetadata(Generated,llvm::MDNode::get(c,{}));
  if(mode=="loop-metadata"){auto *md=llvm::MDNode::getDistinct(c,{nullptr});md->replaceOperandWith(0,md);br->setMetadata("llvm.loop",md);}
  if(mode=="mustprogress")function->addFnAttr(llvm::Attribute::MustProgress);
  if(mode=="willreturn")function->addFnAttr(llvm::Attribute::WillReturn);
  ir.SetInsertPoint(exit);ir.CreateStore(data,function->getArg(0))->setAlignment(llvm::Align(1));ir.CreateRet(state);
  if(mode=="blockaddress"){auto *address=llvm::BlockAddress::get(function,head);new llvm::GlobalVariable(module,address->getType(),false,llvm::GlobalValue::ExternalLinkage,address,"saved.block");}
  REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
 }
 llvm::BasicBlock *named(llvm::StringRef name){for(auto &b:*function)if(b.getName()==name)return &b;return nullptr;}
 std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
 Result run(Options options={}){PassPipeline p;REQUIRE(p.add("invariant-self-loops",options));auto report=p.run(lifter);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs.front().result;}
};
void structure(Fixture &f,unsigned external){
 auto *h=f.named("head"),*e=f.named("exit");REQUIRE(h);auto *br=llvm::dyn_cast<llvm::BranchInst>(h->getTerminator());REQUIRE(br);REQUIRE(br->isConditional());CHECK(br->getSuccessor(1)==e);auto *cmp=llvm::dyn_cast<llvm::ICmpInst>(br->getCondition());REQUIRE(cmp);CHECK(cmp->getPredicate()==llvm::ICmpInst::ICMP_UGT);CHECK(llvm::cast<llvm::ConstantInt>(cmp->getOperand(1))->equalsInt(50));CHECK(br->getMetadata(Generated));
 unsigned phis=0;for(auto &phi:h->phis()){++phis;CHECK(phi.getNumIncomingValues()==external);for(unsigned n=0;n<phi.getNumIncomingValues();++n)CHECK(phi.getIncomingBlock(n)==f.named("entry"));}CHECK(phis==2);
 auto *spin=br->getSuccessor(0);CHECK(spin!=h);CHECK(spin!=e);CHECK(spin->size()==1);auto *back=llvm::dyn_cast<llvm::BranchInst>(spin->getTerminator());REQUIRE(back);CHECK(back->isUnconditional());CHECK(back->getSuccessor(0)==spin);CHECK(back->getMetadata(Generated));CHECK_FALSE(back->getMetadata("llvm.loop"));CHECK_FALSE(f.function->hasFnAttribute(llvm::Attribute::MustProgress));CHECK_FALSE(f.function->hasFnAttribute(llvm::Attribute::WillReturn));
 CHECK(llvm::isa<llvm::StoreInst>(e->front()));CHECK(llvm::isa<llvm::ReturnInst>(e->getTerminator()));
}
}
TEST_CASE("Invariant self loops preserve first condition false exit and external identities","[llvm][invariant-self-loops]"){
 Fixture f;auto *module=&f.module;auto *function=f.function;auto *arg=function->getArg(0);auto *saved=new llvm::GlobalVariable(f.module,function->getType(),false,llvm::GlobalValue::ExternalLinkage,function,"saved.function");auto *caller=llvm::Function::Create(function->getFunctionType(),llvm::GlobalValue::ExternalLinkage,"caller",f.module);auto *entry=llvm::BasicBlock::Create(f.module.getContext(),"entry",caller);llvm::IRBuilder<> ir(entry);auto *call=ir.CreateCall(function,{caller->getArg(0),caller->getArg(1),caller->getArg(2),caller->getArg(3)});ir.CreateRet(call);auto *md=llvm::MDNode::get(f.module.getContext(),llvm::MDString::get(f.module.getContext(),"source"));f.module.getOrInsertNamedMetadata("test.provenance")->addOperand(md);
 auto result=f.run();REQUIRE(result.changed);CHECK(result.counters.at("isolated_loops")==1);CHECK(result.counters.at("removed_self_incomings")==2);CHECK(result.counters.at("added_blocks")==1);CHECK(&f.module==module);CHECK(f.module.getFunction("subject")==function);CHECK(function->getArg(0)==arg);CHECK(saved->getInitializer()==function);CHECK(call->getCalledFunction()==function);CHECK(f.module.getNamedMetadata("test.provenance")->getOperand(0)==md);structure(f,1);for(auto &fn:f.module)CHECK_FALSE(fn.getName().contains(".spin.trial"));auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
}
TEST_CASE("Invariant self loops retain duplicate external PHI slots","[llvm][invariant-self-loops]"){
 Fixture f("normal",true);REQUIRE(f.run().changed);structure(f,2);for(auto &phi:f.named("head")->phis())CHECK(phi.getIncomingValue(0)==phi.getIncomingValue(1));
}
TEST_CASE("Invariant self loops reject changing effects nondeterminism and progress promises","[llvm][invariant-self-loops]"){
 for(const char *mode:{"changing","cross-phi","load","store","call","freeze","undef","poison","phi-undef","unknown-condition","false-self","both-self","no-external","loop-metadata","mustprogress","willreturn","blockaddress"}){INFO(mode);Fixture f(mode);auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("rejected")>0);CHECK(f.text()==before);}
}
TEST_CASE("Invariant self loop budgets reject before mutation and builtin stays opt in","[llvm][invariant-self-loops]"){
 for(const char *key:{"max_loops","max_work","max_blocks"}){Fixture f;auto before=f.text();auto r=f.run({{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==before);}
 PassPipeline p;CHECK_FALSE(p.add("invariant-self-loops",{{"max_loops",std::int64_t(65537)}}));CHECK_FALSE(p.add("invariant-self-loops",{{"max_work",std::int64_t(1000001)}}));CHECK_FALSE(p.add("invariant-self-loops",{{"max_blocks",std::int64_t(65537)}}));CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"invariant-self-loops")==PassPipeline::defaultLLVM().end());
}
