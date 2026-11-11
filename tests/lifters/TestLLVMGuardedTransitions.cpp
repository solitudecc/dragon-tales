#include <catch2/catch_test_macros.hpp>
#include "../../lib/GuardedTransitions.hpp"
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <string>
#include <vector>
using namespace dragon::llvmlifter::detail;
namespace {
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"transitions",context};
  llvm::Function *function;
  Fixture(bool duplicate=false,bool two=true) {
    module.setDataLayout("e-p:64:64");
    auto *ptr=llvm::PointerType::getUnqual(context);
    auto *i64=llvm::Type::getInt64Ty(context);
    function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,llvm::Type::getInt1Ty(context)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *name){return llvm::BasicBlock::Create(context,name,function);};
    auto *entry=block("entry");
    auto *a=duplicate?nullptr:block("a"),*b=duplicate||!two?nullptr:block("b");
    auto *dispatch=block("dispatch"),*yes=block("yes"),*other=block("other"),*fail=block("fail");
    llvm::IRBuilder<llvm::NoFolder> ir(entry);
    auto *guest=ir.CreateLoad(ptr,function->getArg(1));guest->setAlignment(llvm::Align(1));
    if(duplicate)ir.CreateCondBr(function->getArg(2),dispatch,dispatch);
    else {
      if(two)ir.CreateCondBr(function->getArg(2),a,b);else ir.CreateBr(a);
      ir.SetInsertPoint(a);ir.CreateBr(dispatch);
      if(b){ir.SetInsertPoint(b);ir.CreateBr(dispatch);}
    }
    ir.SetInsertPoint(dispatch);
    auto *address=ir.CreateGEP(ir.getInt8Ty(),guest,ir.getInt64(16));
    auto *value=ir.CreateLoad(i64,address,"actual");value->setAlignment(llvm::Align(1));
    auto *jump=ir.CreateSwitch(value,fail);jump->addCase(ir.getInt64(7),yes);jump->addCase(ir.getInt64(9),other);
    ir.SetInsertPoint(yes);ir.CreateRet(ir.getInt64(7));
    ir.SetInsertPoint(other);ir.CreateRet(ir.getInt64(9));
    ir.SetInsertPoint(fail);ir.CreateRet(ir.getInt64(-1));
    hint("0700000000000000");
    REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  void manifest(const std::string &text) {
    if(auto *old=module.getNamedMetadata("dragon.guarded-entry"))module.eraseNamedMetadata(old);
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(context,llvm::MDString::get(context,text)));
  }
  void hint(const std::string &bytes) {manifest("{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0x10\",\"bytes\":\""+bytes+"\"}]}");}
  std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
};
std::vector<std::string> names(llvm::Function &f){std::vector<std::string> r;for(auto &b:f)r.push_back(b.getName().str());return r;}
void checkFallbacks(Fixture &f,std::size_t expected,std::uint64_t candidate) {
  std::size_t count=0;
  for(auto &b:*f.function)if(auto *jump=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator())) {
    if(!jump->getMetadata("dragon.guarded-transitions.generated"))continue;
    ++count; REQUIRE(jump->getNumCases()==2);
    auto *load=llvm::dyn_cast<llvm::LoadInst>(jump->getCondition());REQUIRE(load);REQUIRE(load->getParent()!=&b);
    auto *branch=llvm::dyn_cast<llvm::BranchInst>(load->getParent()->getTerminator());REQUIRE(branch);REQUIRE(branch->isConditional());
    REQUIRE(branch->getSuccessor(1)==&b);
    auto *eq=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());REQUIRE(eq);CHECK(eq->getPredicate()==llvm::CmpInst::ICMP_EQ);
    CHECK(eq->getOperand(0)==load);CHECK(llvm::cast<llvm::ConstantInt>(eq->getOperand(1))->getZExtValue()==candidate);
  }
  CHECK(count==expected);CHECK_FALSE(llvm::verifyModule(f.module,&llvm::errs()));
}
}
TEST_CASE("Guarded transitions rediscover original ordinals after each transaction","[llvm][guarded-transitions]") {
  for(bool duplicate:{false,true}) {
    Fixture f(duplicate);const auto before=names(*f.function);
    auto s=runGuardedTransitions(f.module);CHECK(s.complete);CHECK(s.candidateEdges==2);CHECK(s.attemptedEdges==2);CHECK(s.rewrittenEdges==2);
    CHECK(s.originalSwitchesRetained==1);CHECK(s.fallbackSwitchesCreated==2);CHECK(s.unresolvedEdges==0);
    const auto after=names(*f.function);REQUIRE(after.size()>before.size());
    CHECK(std::vector<std::string>(after.begin(),after.begin()+before.size())==before);
    checkFallbacks(f,2,7);
    const auto stable=f.text();auto second=runGuardedTransitions(f.module);
    CHECK(second.complete);CHECK_FALSE(second.changed);CHECK(second.rewrittenEdges==0);CHECK(f.text()==stable);
  }
}
TEST_CASE("Guarded transitions treats stale and malicious manifest bytes only as hints","[llvm][guarded-transitions]") {
  Fixture f;f.hint("0900000000000000");auto s=runGuardedTransitions(f.module);
  CHECK(s.rewrittenEdges==2);checkFallbacks(f,2,9); // Actual memory need not equal this hint.
  for(const auto &hint:std::vector<std::string>{
      "{}", "{", "{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0x10\",\"bytes\":\"zz\"}]}",
      "{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0x10\",\"bytes\":\"07\"},{\"offset\":\"0x10\",\"bytes\":\"07\"}]}",
      "{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0xffffffffffffffff\",\"bytes\":\"0700\"}]}"}) {
    Fixture bad;bad.manifest(hint);auto old=bad.text();auto r=runGuardedTransitions(bad.module);
    CHECK_FALSE(r.changed);CHECK_FALSE(r.complete);CHECK(r.rejected==1);CHECK(bad.text()==old);
  }
  for(const char *layout:{"e-p:32:32", "e-p:64:64:64:32", "E-p:64:64"}) {
    Fixture narrow;narrow.module.setDataLayout(layout);const auto old=narrow.text();
    auto r=runGuardedTransitions(narrow.module);CHECK_FALSE(r.changed);CHECK_FALSE(r.complete);CHECK(r.rejected==1);CHECK(narrow.text()==old);
  }
  Fixture unknown;unknown.hint("0800000000000000");const auto old=unknown.text();
  auto r=runGuardedTransitions(unknown.module);CHECK(r.complete);CHECK(r.candidateEdges==0);CHECK(unknown.text()==old);
  Fixture suppressed;
  for(auto &b:*suppressed.function)b.getTerminator()->setMetadata("dragon.guarded-transitions.generated",llvm::MDNode::get(suppressed.context,{}));
  const auto unchanged=suppressed.text();CHECK_FALSE(runGuardedTransitions(suppressed.module).changed);CHECK(suppressed.text()==unchanged);
}
TEST_CASE("Guarded transitions reports zero and exact cumulative budgets","[llvm][guarded-transitions]") {
  Fixture reference(false,false);auto cost=runGuardedTransitions(reference.module);REQUIRE(cost.rewrittenEdges==1);
  for(unsigned which=0;which<4;++which) {
    Fixture f;GuardedTransitionsOptions o;
    if(which==0)o.maxCandidates=0;if(which==1)o.maxRewrites=0;if(which==2)o.maxBlocks=0;if(which==3)o.maxInstructions=0;
    const auto old=f.text();auto r=runGuardedTransitions(f.module,o);
    CHECK_FALSE(r.changed);CHECK_FALSE(r.complete);CHECK(r.budgetExhausted>0);CHECK(r.unresolvedEdges==2);CHECK(r.originalSwitchesRetained==1);CHECK(f.text()==old);
  }
  Fixture exact(false,false);GuardedTransitionsOptions o{1,1,cost.clonedBlocks,cost.clonedInstructions};
  auto r=runGuardedTransitions(exact.module,o);CHECK(r.complete);CHECK(r.rewrittenEdges==1);checkFallbacks(exact,1,7);
  Fixture less(false,false);--o.maxInstructions;auto old=less.text();r=runGuardedTransitions(less.module,o);
  CHECK_FALSE(r.changed);CHECK_FALSE(r.complete);CHECK(r.budgetExhausted>0);CHECK(less.text()==old);
  Fixture partial;auto p=runGuardedTransitions(partial.module,{64,1,128,16384});
  CHECK(p.rewrittenEdges==1);CHECK_FALSE(p.complete);CHECK(p.unresolvedEdges==1);checkFallbacks(partial,1,7);
}
TEST_CASE("Guarded transitions builtin is opt in and bounds public options","[llvm][guarded-transitions]") {
  using namespace dragon::passes;
  CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"guarded-transitions")==PassPipeline::defaultLLVM().end());
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};PassPipeline p;
  REQUIRE(p.add("guarded-transitions"));auto r=p.run(lifter);REQUIRE(r.runs.size()==1);CHECK(r.runs[0].result.status==Status::Unavailable);
  PassPipeline bad;CHECK_FALSE(bad.add("guarded-transitions",{{"max_candidates",std::int64_t(4097)}}));
}
