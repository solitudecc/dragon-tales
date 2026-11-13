#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <string>

namespace {
using namespace dragon::passes;
std::string stateDefinedText(const llvm::Value &value) {
 std::string text;llvm::raw_string_ostream out(text);value.print(out);return text;
}
struct DefinednessFixture {
 dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
 llvm::Module &module=lifter.module();llvm::Function *function;
 std::string stateBefore,comparisonBefore,effectBefore;unsigned originalFreezes=0;
 DefinednessFixture(const std::string &mode,unsigned width=64) {
  module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *i64=llvm::Type::getInt64Ty(c);auto *type=llvm::IntegerType::get(c,width);
  function=llvm::Function::Create(llvm::FunctionType::get(i64,{i64},false),llvm::GlobalValue::ExternalLinkage,"definedness",module);
  function->getArg(0)->setName("input");if(mode=="defined")function->addParamAttr(0,llvm::Attribute::NoUndef);
  auto block=[&](const char *name){return llvm::BasicBlock::Create(c,name,function);};auto *entry=block("entry"),*seed=block("seed"),*tail=block("tail"),*high=block("high"),*answer=block("answer"),*other=block("other");
  llvm::IRBuilder<llvm::NoFolder> b(entry);llvm::Value *state=nullptr;
  if(mode=="defined")state=width==64?static_cast<llvm::Value *>(function->getArg(0)):b.CreateTrunc(function->getArg(0),type,"input.narrow");
  else {
   auto *slot=b.CreateAlloca(type,nullptr,"uninitialized.slot");
   if(mode=="partial-byte"){
    auto *byte=b.CreateGEP(b.getInt8Ty(),slot,b.getInt64(1));b.CreateStore(b.getInt8(128),byte)->setAlignment(llvm::Align(1));
   }
   auto *read=b.CreateLoad(type,slot,"state.read");read->setAlignment(llvm::Align(1));state=read;
   if(mode=="existing-freeze"){state=b.CreateFreeze(state,"existing.state.freeze");originalFreezes=1;}
  }
  if(mode=="wide-partial")state=b.CreateLShr(state,llvm::ConstantInt::get(type,width-1),"state.highbit");
  state=b.CreateAnd(state,llvm::ConstantInt::get(type,3),"state.masked");stateBefore=stateDefinedText(*state);b.CreateBr(seed);
  b.SetInsertPoint(seed);auto *out=new llvm::GlobalVariable(module,i64,false,llvm::GlobalValue::ExternalLinkage,b.getInt64(0),"effect");auto *effect=b.CreateStore(function->getArg(0),out);effect->setAlignment(llvm::Align(1));effectBefore=stateDefinedText(*effect);
  auto *comparison=llvm::cast<llvm::ICmpInst>(b.CreateICmpUGT(state,llvm::ConstantInt::get(type,50),"original.cmp"));comparisonBefore=stateDefinedText(*comparison);b.CreateCondBr(comparison,high,tail);
  // LLVM itself proves the source condition false, despite the possibly
  // uninitialized LOAD. The new state-equality use is a separate UB consumer.
  auto *constant=llvm::dyn_cast_or_null<llvm::ConstantInt>(llvm::simplifyInstruction(comparison,llvm::SimplifyQuery(module.getDataLayout())));
  REQUIRE(constant);CHECK(constant->isZero());
  b.SetInsertPoint(tail);auto *table=new llvm::GlobalVariable(module,i64,true,llvm::GlobalValue::ExternalLinkage,b.getInt64(100),"table");auto *actual=b.CreateLoad(i64,table,"actual.target");actual->setAlignment(llvm::Align(1));auto *sw=b.CreateSwitch(actual,other);sw->addCase(b.getInt64(100),answer);
  b.SetInsertPoint(high);b.CreateRet(b.getInt64(99));b.SetInsertPoint(answer);b.CreateRet(b.getInt64(7));b.SetInsertPoint(other);b.CreateRet(b.getInt64(77));
  const std::string json="{\"hints\":[{\"function\":\"definedness\",\"block\":\"seed\",\"state\":\"0x2\",\"target\":\"0x64\"}]}";
  module.getOrInsertNamedMetadata("dragon.guarded-state-values")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));verify();
 }
 void verify(){REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));}
 llvm::BasicBlock *block(llvm::StringRef name){for(auto &b:*function)if(b.getName()==name)return &b;return nullptr;}
 std::string text(){std::string result;llvm::raw_string_ostream out(result);module.print(out,nullptr);return result;}
 Result run(const Options &options={}){PassPipeline p;REQUIRE(p.add("guarded-state-values",options));auto report=p.run(lifter);REQUIRE(report.ok);REQUIRE(report.runs.size()==1);verify();return report.runs.front().result;}
 void check() {
  auto *seed=block("seed");auto *branch=llvm::cast<llvm::BranchInst>(seed->getTerminator());llvm::DominatorTree dom(*function);
  CHECK(llvm::isGuaranteedNotToBeUndefOrPoison(branch->getCondition(),nullptr,branch,&dom));
  auto *defined=llvm::dyn_cast<llvm::FreezeInst>(branch->getCondition());CHECK(defined!=nullptr);if(!defined)return;
  CHECK(defined->getName()=="state.value.defined");auto *eq=llvm::dyn_cast<llvm::ICmpInst>(defined->getOperand(0));REQUIRE(eq);CHECK(eq->getPredicate()==llvm::ICmpInst::ICMP_EQ);CHECK(eq->getOperand(0)->getName()=="state.masked");CHECK(stateDefinedText(*eq->getOperand(0))==stateBefore);CHECK(llvm::cast<llvm::ConstantInt>(eq->getOperand(1))->getZExtValue()==2);
  auto *fallback=branch->getSuccessor(1);auto *original=llvm::cast<llvm::BranchInst>(fallback->getTerminator());CHECK(original->isConditional());CHECK(stateDefinedText(*original->getCondition())==comparisonBefore);CHECK(original->getSuccessor(0)==block("high"));CHECK(original->getSuccessor(1)==block("tail"));
  unsigned effects=0,freezes=0,newFreezes=0,stateReads=0;
  for(auto &b:*function)for(auto &i:b){if(llvm::isa<llvm::FreezeInst>(i)){++freezes;if(i.getName()=="state.value.defined")++newFreezes;}if(i.getName()=="state.read")++stateReads;if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i);s && s->getPointerOperand()==module.getGlobalVariable("effect")){++effects;CHECK(s->getParent()==seed);CHECK(stateDefinedText(*s)==effectBefore);}}
  CHECK(effects==1);CHECK(freezes==originalFreezes+1);CHECK(newFreezes==1);CHECK(stateReads<=1);
 }
};
}
TEST_CASE("State prediction branches do not newly consume undefined load bits","[llvm][guarded-state-definedness]") {
 for(const char *mode:{"uninitialized-mask","wide-partial","partial-byte","existing-freeze","defined"})for(unsigned width:{16U,64U}){
  INFO(mode);INFO(width);DefinednessFixture f(mode,width);auto r=f.run();REQUIRE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("rewritten_edges")==1);CHECK(r.counters.at("budget_exhausted")==0);f.check();
 }
}
TEST_CASE("State condition freeze consumes the shared instruction budget atomically","[llvm][guarded-state-definedness]") {
 for(std::int64_t budget:{3,4,7}){INFO(budget);DefinednessFixture f("uninitialized-mask");auto before=f.text();auto r=f.run({{"max_instructions",budget}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("attempted_edges")==1);CHECK(r.counters.at("budget_exhausted")>0);CHECK(r.counters.at("cloned_blocks")==0);CHECK(r.counters.at("cloned_instructions")==0);CHECK(f.text()==before);}
 DefinednessFixture f("uninitialized-mask");auto r=f.run({{"max_instructions",std::int64_t(8)}});REQUIRE(r.changed);CHECK(r.counters.at("cloned_instructions")==8);CHECK(r.counters.at("cloned_blocks")==4);f.check();
}
