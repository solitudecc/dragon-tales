#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();
  llvm::Function *function;
  Fixture() {
    auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);
    function=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt64Ty(c),{ptr,llvm::Type::getInt1Ty(c),llvm::Type::getInt64Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"subject",module);
  }
  llvm::BasicBlock *block(const char *name){return llvm::BasicBlock::Create(module.getContext(),name,function);}
  std::string text(){std::string s;llvm::raw_string_ostream os(s);module.print(os,nullptr);return s;}
  Result run(Options options={}) {
    REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
    PassPipeline p;REQUIRE(p.add("constant-branches",options));
    auto report=p.run(lifter);REQUIRE(report.runs.size()==1);
    REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
    return report.runs.front().result;
  }
  void duplicateBranch(llvm::Value *condition=nullptr) {
    auto *entry=block("entry"),*join=block("join");llvm::IRBuilder<llvm::NoFolder> ir(entry);
    auto *branch=ir.CreateCondBr(condition?condition:ir.getFalse(),join,join);
    branch->setMetadata(Generated,llvm::MDNode::get(module.getContext(),{}));
    ir.SetInsertPoint(join);auto *phi=ir.CreatePHI(ir.getInt64Ty(),2);phi->addIncoming(ir.getInt64(17),entry);phi->addIncoming(ir.getInt64(17),entry);ir.CreateRet(phi);
  }
};
}
TEST_CASE("Constant branches fold false duplicate slots without changing external identities","[llvm][constant-branches]") {
  Fixture f;f.duplicateBranch();auto *module=&f.module;auto *function=f.function;auto *argument=function->getArg(0);
  auto *saved=new llvm::GlobalVariable(f.module,function->getType(),false,llvm::GlobalValue::ExternalLinkage,function,"saved.subject");
  auto *caller=llvm::Function::Create(function->getFunctionType(),llvm::GlobalValue::ExternalLinkage,"caller",f.module);
  auto *cb=llvm::BasicBlock::Create(f.module.getContext(),"entry",caller);llvm::IRBuilder<> ir(cb);
  auto *call=ir.CreateCall(function,{caller->getArg(0),caller->getArg(1),caller->getArg(2)});ir.CreateRet(call);
  auto *md=llvm::MDNode::get(f.module.getContext(),llvm::MDString::get(f.module.getContext(),"provenance"));f.module.getOrInsertNamedMetadata("test.provenance")->addOperand(md);
  auto result=f.run();CHECK(result.changed);CHECK(result.counters.at("folded_branches")==1);CHECK(result.counters.at("folded_switches")==0);
  CHECK(&f.lifter.module()==module);CHECK(f.module.getFunction("subject")==function);CHECK(function->getArg(0)==argument);CHECK(saved->getInitializer()==function);CHECK(call->getCalledFunction()==function);CHECK(f.module.getNamedMetadata("test.provenance")->getOperand(0)==md);
  auto *branch=llvm::dyn_cast<llvm::BranchInst>(function->getEntryBlock().getTerminator());REQUIRE(branch);CHECK(branch->isUnconditional());CHECK(branch->getMetadata(Generated)!=nullptr);
  auto *ret=llvm::dyn_cast<llvm::ReturnInst>(branch->getSuccessor(0)->getTerminator());REQUIRE(ret);CHECK(llvm::cast<llvm::ConstantInt>(ret->getReturnValue())->equalsInt(17));
  const auto stable=f.text();auto again=f.run();CHECK_FALSE(again.changed);CHECK(f.text()==stable);
}
TEST_CASE("Constant switches repair repeated case PHIs and remove only unreachable stores","[llvm][constant-branches]") {
  Fixture f;auto *entry=f.block("entry"),*yes=f.block("yes"),*dead=f.block("dead");llvm::IRBuilder<llvm::NoFolder> ir(entry);
  auto *sw=ir.CreateSwitch(ir.getInt64(9),dead);sw->addCase(ir.getInt64(7),yes);sw->addCase(ir.getInt64(9),yes);sw->setMetadata(Generated,llvm::MDNode::get(f.module.getContext(),{}));
  ir.SetInsertPoint(yes);auto *phi=ir.CreatePHI(ir.getInt64Ty(),2);phi->addIncoming(ir.getInt64(11),entry);phi->addIncoming(ir.getInt64(11),entry);ir.CreateStore(phi,f.function->getArg(0))->setAlignment(llvm::Align(1));ir.CreateRet(phi);
  ir.SetInsertPoint(dead);ir.CreateStore(ir.getInt64(99),f.function->getArg(0))->setAlignment(llvm::Align(1));ir.CreateRet(ir.getInt64(99));
  auto r=f.run();CHECK(r.changed);CHECK(r.counters.at("folded_switches")==1);CHECK(r.counters.at("removed_blocks")==1);CHECK(f.function->size()==2);
  CHECK(f.function->getEntryBlock().getTerminator()->getMetadata(Generated)!=nullptr);
  unsigned stores=0;for(auto &b:*f.function)for(auto &i:b)if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i)){++stores;CHECK(llvm::cast<llvm::ConstantInt>(s->getValueOperand())->equalsInt(11));}CHECK(stores==1);
}
TEST_CASE("Constant branch cleanup preserves dynamic target guards and natural switches","[llvm][constant-branches]") {
  Fixture f;auto *entry=f.block("entry"),*dead=f.block("dead"),*guard=f.block("guard"),*fallback=f.block("fallback"),*natural=f.block("natural"),*yes=f.block("yes"),*other=f.block("other");llvm::IRBuilder<llvm::NoFolder> ir(entry);
  ir.CreateCondBr(ir.getFalse(),dead,guard);ir.SetInsertPoint(dead);ir.CreateRet(ir.getInt64(-1));
  ir.SetInsertPoint(guard);auto *actual=ir.CreateLoad(ir.getInt64Ty(),f.function->getArg(0));actual->setAlignment(llvm::Align(1));auto *eq=ir.CreateICmpEQ(actual,ir.getInt64(7));auto *gb=ir.CreateCondBr(eq,yes,fallback);gb->setMetadata(Generated,llvm::MDNode::get(f.module.getContext(),{}));
  ir.SetInsertPoint(fallback);auto *fs=ir.CreateSwitch(actual,other);fs->addCase(ir.getInt64(7),yes);fs->addCase(ir.getInt64(9),natural);fs->setMetadata(Generated,llvm::MDNode::get(f.module.getContext(),{}));
  ir.SetInsertPoint(natural);auto *ns=ir.CreateSwitch(f.function->getArg(2),other);ns->addCase(ir.getInt64(3),yes);
  ir.SetInsertPoint(yes);ir.CreateRet(actual);ir.SetInsertPoint(other);ir.CreateRet(ir.getInt64(0));
  auto r=f.run();CHECK(r.counters.at("folded_branches")==1);CHECK(r.counters.at("removed_blocks")==1);
  CHECK(guard->getTerminator()==gb);CHECK(gb->getCondition()==eq);CHECK(fallback->getTerminator()==fs);CHECK(fs->getCondition()==actual);CHECK(fs->getNumCases()==2);CHECK(natural->getTerminator()==ns);CHECK(ns->getCondition()==f.function->getArg(2));
}
TEST_CASE("Constant branches preserve unknown undef poison and freeze conditions","[llvm][constant-branches]") {
  for(unsigned kind=0;kind<4;++kind) {
    Fixture f;auto &c=f.module.getContext();llvm::Value *condition=nullptr;
    if(kind==0)condition=f.function->getArg(1);
    if(kind==1)condition=llvm::UndefValue::get(llvm::Type::getInt1Ty(c));
    if(kind==2)condition=llvm::PoisonValue::get(llvm::Type::getInt1Ty(c));
    if(kind==3) {
      auto *entry=f.block("entry"),*join=f.block("join");llvm::IRBuilder<llvm::NoFolder> ir(entry);condition=ir.CreateFreeze(llvm::UndefValue::get(ir.getInt1Ty()));ir.CreateCondBr(condition,join,join);ir.SetInsertPoint(join);ir.CreateRet(ir.getInt64(17));
    } else f.duplicateBranch(condition);
    const auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.status==Status::Success);CHECK(r.counters.at("candidate_terminators")==0);CHECK(f.text()==before);
  }
}
TEST_CASE("Constant branches reject zero budgets before changing IR","[llvm][constant-branches]") {
  for(const char *option:{"max_folds","max_blocks","max_instructions"}) {Fixture f;f.duplicateBranch();const auto before=f.text();auto r=f.run({{option,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")==1);CHECK(f.text()==before);}
  PassPipeline p;CHECK_FALSE(p.add("constant-branches",{{"max_folds",std::int64_t(65537)}}));
}
TEST_CASE("Constant branches conservatively reject blockaddress and EH identity","[llvm][constant-branches]") {
  for(bool blockaddress:{false,true}) {
    Fixture f;f.duplicateBranch();
    if(blockaddress) {
      auto *address=llvm::BlockAddress::get(f.function,&f.function->back());new llvm::GlobalVariable(f.module,address->getType(),false,llvm::GlobalValue::ExternalLinkage,address,"saved.block");
    } else {
      auto *personality=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt32Ty(f.module.getContext()),false),llvm::GlobalValue::ExternalLinkage,"personality",f.module);f.function->setPersonalityFn(personality);
    }
    const auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("rejected")==1);CHECK(f.text()==before);
  }
}
