#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include "../../lib/GuardedTransition.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();llvm::Function *function;
  Fixture(bool defaultSame=false,bool multiply=false,bool after=false) {
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);
    function=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt64Ty(c),{ptr,ptr,llvm::Type::getInt64Ty(c),llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,function);};auto *entry=block("entry"),*dispatch=block("dispatch"),*yes=block("yes"),*central=block("central"),*miss=block("miss");
    llvm::IRBuilder<llvm::NoFolder> ir(entry);auto *guest=ir.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));
    ir.CreateCondBr(function->getArg(3),dispatch,miss);
    ir.SetInsertPoint(dispatch);llvm::Value *scaled=multiply?ir.CreateMul(ir.getInt64(8),function->getArg(2)):ir.CreateShl(function->getArg(2),ir.getInt64(3));
    auto *index=ir.CreateAdd(scaled,ir.getInt64(128));auto *address=ir.CreateGEP(ir.getInt8Ty(),guest,index);auto *actual=ir.CreateLoad(ir.getInt64Ty(),address,"actual");actual->setAlignment(llvm::Align(1));
    if(after)ir.CreateStore(ir.getInt64(13),address)->setAlignment(llvm::Align(1));
    auto *sw=ir.CreateSwitch(actual,defaultSame?yes:miss);sw->addCase(ir.getInt64(7),yes);sw->addCase(ir.getInt64(9),yes);sw->addCase(ir.getInt64(13),central);
    ir.SetInsertPoint(yes);auto *value=ir.CreatePHI(ir.getInt64Ty(),defaultSame?3:2,"selected");value->addIncoming(actual,dispatch);value->addIncoming(actual,dispatch);if(defaultSame)value->addIncoming(actual,dispatch);ir.CreateRet(value);
    ir.SetInsertPoint(central);ir.CreateStore(ir.getInt64(13),function->getArg(0))->setAlignment(llvm::Align(1));ir.CreateRet(ir.getInt64(13));
    ir.SetInsertPoint(miss);ir.CreateRet(ir.getInt64(-1));
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,"{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0x800\",\"bytes\":\"aa\"}]}")));
    module.getOrInsertNamedMetadata("dragon.guarded-entry.memory")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,"untouched-entry-sentinel")));
    hints({7,9,7});REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  void manifest(const std::string &text) {
    if(auto *old=module.getNamedMetadata("dragon.indirect-target-hints"))module.eraseNamedMetadata(old);
    module.getOrInsertNamedMetadata("dragon.indirect-target-hints")->addOperand(llvm::MDNode::get(module.getContext(),llvm::MDString::get(module.getContext(),text)));
  }
  void hints(const std::vector<std::uint64_t> &values) {
    const char *hex="0123456789abcdef";std::string bytes;
    for(auto value:values)for(unsigned n=0;n<8;++n){auto byte=(value>>(8*n))&255;bytes+=hex[byte>>4];bytes+=hex[byte&15];}
    manifest("{\"function\":\"view\",\"tables\":[{\"base\":\"0x80\",\"bytes\":\""+bytes+"\"}]}");
  }
  std::string text(){std::string s;llvm::raw_string_ostream os(s);module.print(os,nullptr);return s;}
  Result run(Options options={}) {PassPipeline p;REQUIRE(p.add("guarded-target-sets",options));auto report=p.run(lifter);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs.front().result;}
};
llvm::SwitchInst *fastSwitch(Fixture &f) {
  llvm::SwitchInst *result=nullptr;
  for(auto &b:*f.function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator())) {
    auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());
    if(sw->getMetadata(Generated) && load && load->getParent()==&b) {REQUIRE(result==nullptr);result=sw;}
  }
  REQUIRE(result);return result;
}
}
TEST_CASE("Guarded target sets preserve actual load fallback and repeated case PHIs","[llvm][guarded-target-sets]") {
  for(bool defaultSame:{false,true}) {
    Fixture f(defaultSame);auto *function=f.function;auto *argument=function->getArg(0);auto *entryHints=f.module.getNamedMetadata("dragon.guarded-entry.memory")->getOperand(0);auto *entryManifest=f.module.getNamedMetadata("dragon.guarded-entry")->getOperand(0);
    auto *saved=new llvm::GlobalVariable(f.module,function->getType(),false,llvm::GlobalValue::ExternalLinkage,function,"saved.view");
    auto r=f.run();REQUIRE(r.changed);CHECK(r.counters.at("rewritten_edges")==1);CHECK(r.counters.at("fast_switches_created")==1);CHECK(r.counters.at("fallback_switches_created")==1);
    CHECK(f.module.getFunction("view")==function);CHECK(function->getArg(0)==argument);CHECK(saved->getInitializer()==function);CHECK(f.module.getNamedMetadata("dragon.guarded-entry.memory")->getOperand(0)==entryHints);CHECK(f.module.getNamedMetadata("dragon.guarded-entry")->getOperand(0)==entryManifest);
    auto *fast=fastSwitch(f);REQUIRE(fast->getNumCases()==2);auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(fast->getDefaultDest()->getTerminator());REQUIRE(fallback);CHECK(fallback->getNumCases()==3);CHECK(fallback->getCondition()==fast->getCondition());CHECK(fallback->getMetadata(Generated)!=nullptr);
    CHECK(fast->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt64Ty(f.module.getContext()),13))==fast->case_default());
    auto *yes=fast->case_begin()->getCaseSuccessor();auto *phi=llvm::dyn_cast<llvm::PHINode>(&yes->front());REQUIRE(phi);CHECK(phi->getNumIncomingValues()==(defaultSame?8:6));
    unsigned fastInputs=0,fallbackInputs=0;for(unsigned n=0;n<phi->getNumIncomingValues();++n){if(phi->getIncomingBlock(n)==fast->getParent()){++fastInputs;CHECK(phi->getIncomingValue(n)==fast->getCondition());}if(phi->getIncomingBlock(n)==fallback->getParent()){++fallbackInputs;CHECK(phi->getIncomingValue(n)==fast->getCondition());}}
    CHECK(fastInputs==2);CHECK(fallbackInputs==(defaultSame?3:2));
    const auto stable=f.text();auto again=f.run();CHECK_FALSE(again.changed);CHECK(f.text()==stable);
  }
}
TEST_CASE("Guarded target sets accept arbitrary index expressions and do not reread after stores","[llvm][guarded-target-sets]") {
  Fixture f(false,true,true);auto r=f.run();REQUIRE(r.changed);auto *fast=fastSwitch(f);auto *actual=llvm::cast<llvm::LoadInst>(fast->getCondition());
  unsigned loads=0,stores=0;bool seenLoad=false;
  for(auto &i:*fast->getParent()){if(llvm::isa<llvm::LoadInst>(i)){++loads;CHECK(&i==actual);seenLoad=true;}if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)){++stores;CHECK(seenLoad);CHECK(store->getPointerOperand()==actual->getPointerOperand());CHECK(llvm::cast<llvm::ConstantInt>(store->getValueOperand())->equalsInt(13));}}
  CHECK(loads==1);CHECK(stores==1);auto *fallback=llvm::cast<llvm::SwitchInst>(fast->getDefaultDest()->getTerminator());CHECK(fallback->getCondition()==actual);
}
TEST_CASE("Guarded target sets treat wrong candidate bytes as predictions only","[llvm][guarded-target-sets]") {
  Fixture f;f.hints({13});auto r=f.run();REQUIRE(r.changed);auto *fast=fastSwitch(f);REQUIRE(fast->getNumCases()==1);CHECK(fast->case_begin()->getCaseValue()->equalsInt(13));
  auto *fallback=llvm::cast<llvm::SwitchInst>(fast->getDefaultDest()->getTerminator());CHECK(fallback->getNumCases()==3);CHECK(fallback->getCondition()==fast->getCondition());
  Fixture unknown;unknown.hints({777});const auto before=unknown.text();auto rejected=unknown.run();CHECK_FALSE(rejected.changed);CHECK(rejected.status==Status::Incomplete);CHECK(unknown.text()==before);
}
TEST_CASE("Guarded target sets keep entry facts separate and enforce bounded metadata","[llvm][guarded-target-sets]") {
  Fixture noHints;noHints.module.eraseNamedMetadata(noHints.module.getNamedMetadata("dragon.indirect-target-hints"));const auto before=noHints.text();auto missing=noHints.run();CHECK(missing.status==Status::Unavailable);CHECK_FALSE(missing.changed);CHECK(noHints.text()==before);
  for(const auto &text:std::vector<std::string>{"{}","{","{\"function\":\"view\",\"tables\":[{\"base\":\"0x80\",\"bytes\":\"zz\"}]}","{\"function\":\"view\",\"tables\":[{\"base\":\"0xffffffffffffffff\",\"bytes\":\"0700000000000000\"}]}"}) {Fixture f;f.manifest(text);const auto old=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(f.text()==old);}
  Fixture many;std::vector<std::uint64_t> values;for(unsigned n=0;n<17;++n)values.push_back(n);many.hints(values);const auto old=many.text();auto r=many.run();CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(many.text()==old);
  CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"guarded-target-sets")==PassPipeline::defaultLLVM().end());
}
TEST_CASE("Guarded target sets respect zero transaction budgets","[llvm][guarded-target-sets]") {
  for(const char *option:{"max_candidates","max_rewrites","max_blocks","max_instructions"}) {Fixture f;const auto old=f.text();auto r=f.run({{option,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==old);}
}
TEST_CASE("Guarded target set extension preserves the legacy single guard shape","[llvm][guarded-target-sets]") {
  Fixture f;auto *seed=&f.function->getEntryBlock();auto *target=seed->getTerminator()->getSuccessor(0);
  dragon::llvmlifter::detail::GuardedTransitionRequest request{seed,0,target,{target},{},llvm::ConstantInt::get(llvm::Type::getInt64Ty(f.module.getContext()),7)};
  auto result=dragon::llvmlifter::detail::cloneGuardedTransition(*f.function,request);REQUIRE(result.changed);REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));
  // Function body pointers were invalidated; recover from the stable Function.
  auto *entry=&f.function->getEntryBlock();auto *newTarget=entry->getTerminator()->getSuccessor(0);
  auto *branch=llvm::dyn_cast<llvm::BranchInst>(newTarget->getTerminator());REQUIRE(branch);CHECK(branch->isConditional());auto *cmp=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());REQUIRE(cmp);CHECK(cmp->getPredicate()==llvm::CmpInst::ICMP_EQ);
  auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(branch->getSuccessor(1)->getTerminator());REQUIRE(fallback);CHECK(fallback->getCondition()==cmp->getOperand(0));CHECK(fallback->getNumCases()==3);
}
