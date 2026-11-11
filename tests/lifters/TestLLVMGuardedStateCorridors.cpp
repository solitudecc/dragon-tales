#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Format.h>
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();llvm::Function *function;
  Fixture(unsigned mode=0,bool duplicate=false) {
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);
    function=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt64Ty(c),{ptr,ptr,llvm::Type::getInt64Ty(c),llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,function);};auto *entry=block("entry"),*left=block("left"),*right=block("right"),*unknown=block("unknown"),*join=block("join"),*bridge=block("bridge"),*dispatch=block("dispatch"),*yes=block("yes"),*miss=block("miss");
    llvm::IRBuilder<llvm::NoFolder> ir(entry);auto *guest=ir.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));
    auto *choose=ir.CreateSwitch(function->getArg(2),unknown);choose->addCase(ir.getInt64(0),left);if(mode!=4)choose->addCase(ir.getInt64(1),right);
    ir.SetInsertPoint(left);if(duplicate)ir.CreateCondBr(function->getArg(3),join,join);else ir.CreateBr(join);
    ir.SetInsertPoint(right);ir.CreateBr(join);ir.SetInsertPoint(unknown);ir.CreateBr(join);
    ir.SetInsertPoint(join);llvm::Value *state=nullptr;
    if(mode==1 || mode==2 || mode==3) {
      state=ir.CreateSelect(function->getArg(3),ir.getInt32(0),ir.getInt32(50),"choice");
      state=ir.CreateZExt(state,ir.getInt64Ty());
      if(mode==2)state=ir.CreateFreeze(state);
      if(mode==3)state=ir.CreateShl(state,ir.getInt64(64));
    } else {
      auto *phi=ir.CreatePHI(ir.getInt64Ty(),duplicate?4:3,"state");phi->addIncoming(ir.getInt64(mode==5?0:34),left);if(duplicate)phi->addIncoming(ir.getInt64(34),left);phi->addIncoming(mode==4?static_cast<llvm::Value *>(phi):ir.getInt64(mode==5?50:37),right);phi->addIncoming(function->getArg(2),unknown);state=phi;
      if(mode==5)state=ir.CreateZExt(ir.CreateTrunc(state,ir.getInt32Ty()),ir.getInt64Ty());
    }
    if(mode==4)ir.CreateCondBr(function->getArg(3),bridge,right);else ir.CreateBr(bridge);ir.SetInsertPoint(bridge);llvm::Value *through=state;if(mode==0 || mode==4 || mode==5){auto *phi=ir.CreatePHI(ir.getInt64Ty(),1,"through");phi->addIncoming(state,join);through=phi;}ir.CreateBr(dispatch);
    ir.SetInsertPoint(dispatch);auto *address=ir.CreateGEP(ir.getInt8Ty(),guest,ir.CreateAdd(ir.getInt64(128),ir.CreateShl(through,ir.getInt64(3))));auto *actual=ir.CreateLoad(ir.getInt64Ty(),address,"actual");actual->setAlignment(llvm::Align(1));
    auto *sw=ir.CreateSwitch(actual,miss);sw->addCase(ir.getInt64(7),yes);sw->addCase(ir.getInt64(9),yes);sw->addCase(ir.getInt64(13),miss);
    ir.SetInsertPoint(yes);auto *selected=ir.CreatePHI(ir.getInt64Ty(),2);selected->addIncoming(actual,dispatch);selected->addIncoming(actual,dispatch);ir.CreateRet(selected);
    ir.SetInsertPoint(miss);ir.CreateRet(ir.getInt64(-1));
    hints(7,9);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  void hints(std::uint64_t first,std::uint64_t second) {
    auto *old=module.getNamedMetadata("dragon.guarded-entry");if(old)module.eraseNamedMetadata(old);
    auto bytes=[](std::uint64_t v){std::string s;const char *hex="0123456789abcdef";for(unsigned n=0;n<8;++n){unsigned b=(v>>(8*n))&255;s+=hex[b>>4];s+=hex[b&15];}return s;};
    std::string json="{\"view_symbol\":\"view\",\"memory\":[";
    const unsigned states[]={0,34,37,50};for(unsigned n=0;n<4;++n){if(n)json+=",";std::string address;llvm::raw_string_ostream out(address);out<<llvm::format_hex(128+states[n]*8,0);json+="{\"offset\":\""+address+"\",\"bytes\":\""+bytes(n<2?first:second)+"\"}";}json+="]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(module.getContext(),llvm::MDString::get(module.getContext(),json)));
  }
  std::string text(){std::string s;llvm::raw_string_ostream os(s);module.print(os,nullptr);return s;}
  Result run(Options options={}) {PassPipeline p;REQUIRE(p.add("guarded-state-corridors",options));auto report=p.run(lifter);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs.front().result;}
  llvm::BasicBlock *named(llvm::StringRef name){for(auto &b:*function)if(b.getName()==name)return &b;return nullptr;}
  std::vector<llvm::SwitchInst *> fast() {std::vector<llvm::SwitchInst *> out;for(auto &b:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator()))if(sw->getMetadata(Generated))if(auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());load && load->getParent()==&b)out.push_back(sw);return out;}
};
void fallback(llvm::SwitchInst *fast) {auto *full=llvm::dyn_cast<llvm::SwitchInst>(fast->getDefaultDest()->getTerminator());REQUIRE(full);CHECK(full->getCondition()==fast->getCondition());CHECK(full->getNumCases()==3);CHECK(full->getMetadata(Generated));}
}
TEST_CASE("Guarded SSA corridors select distinct real incoming PHI states","[llvm][guarded-state-corridors]") {
  Fixture f;auto *function=f.function;auto *arg=function->getArg(0);auto *metadata=f.module.getNamedMetadata("dragon.guarded-entry")->getOperand(0);auto r=f.run();REQUIRE(r.changed);CHECK(r.counters.at("rewritten_edges")==2);CHECK(function==f.module.getFunction("view"));CHECK(function->getArg(0)==arg);CHECK(f.module.getNamedMetadata("dragon.guarded-entry")->getOperand(0)==metadata);
  std::set<std::uint64_t> values;for(auto *fast:f.fast()){REQUIRE(fast->getNumCases()==1);values.insert(fast->case_begin()->getCaseValue()->getZExtValue());fallback(fast);}CHECK((values==std::set<std::uint64_t>{7,9}));
  CHECK(f.named("left")->getTerminator()->getSuccessor(0)!=f.named("join"));CHECK(f.named("right")->getTerminator()->getSuccessor(0)!=f.named("join"));CHECK(f.named("unknown")->getTerminator()->getSuccessor(0)==f.named("join"));const auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
}
TEST_CASE("Guarded SSA corridors enumerate finite select casts without reading its condition","[llvm][guarded-state-corridors]") {
  Fixture phi(5);auto pr=phi.run();REQUIRE(pr.changed);CHECK(pr.counters.at("rewritten_edges")==2);std::set<std::uint64_t> pv;for(auto *sw:phi.fast()){REQUIRE(sw->getNumCases()==1);pv.insert(sw->case_begin()->getCaseValue()->getZExtValue());fallback(sw);}CHECK((pv==std::set<std::uint64_t>{7,9}));
  Fixture f(1);auto r=f.run();REQUIRE(r.changed);auto fast=f.fast();REQUIRE(fast.size()==1);CHECK(fast[0]->getNumCases()==2);fallback(fast[0]);
  Fixture wrong(1);wrong.hints(13,13);REQUIRE(wrong.run().changed);auto predicted=wrong.fast();REQUIRE(predicted.size()==1);CHECK(predicted[0]->case_begin()->getCaseValue()->equalsInt(13));fallback(predicted[0]);
}
TEST_CASE("Guarded SSA corridors preserve duplicate original slots and resume budgets","[llvm][guarded-state-corridors]") {
  Fixture f(0,true);auto first=f.run({{"max_rewrites",std::int64_t(1)}});REQUIRE(first.changed);CHECK(first.status==Status::Incomplete);CHECK(first.counters.at("rewritten_edges")==1);
  auto second=f.run();CHECK(second.changed);CHECK(second.counters.at("rewritten_edges")==2);CHECK(f.fast().size()==3);for(auto *fast:f.fast())fallback(fast);auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
}
TEST_CASE("Guarded SSA corridors retain unknown freeze poison shift and generated seeds","[llvm][guarded-state-corridors]") {
  for(unsigned mode:{2u,3u}) {Fixture f(mode);auto old=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(f.text()==old);}
  Fixture generated;for(const char *name:{"left","right"})generated.named(name)->getTerminator()->setMetadata(Generated,llvm::MDNode::get(generated.module.getContext(),{}));auto old=generated.text();auto r=generated.run();CHECK_FALSE(r.changed);CHECK(generated.text()==old);
}
TEST_CASE("Guarded SSA corridors preserve defaults metadata and zero budgets","[llvm][guarded-state-corridors]") {
  for(const char *key:{"max_candidates","max_rewrites","max_blocks","max_instructions"}) {Fixture f;auto old=f.text();auto r=f.run({{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==old);}
  Fixture absent;absent.module.eraseNamedMetadata(absent.module.getNamedMetadata("dragon.guarded-entry"));auto old=absent.text();CHECK(absent.run().status==Status::Unavailable);CHECK(absent.text()==old);
  CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"guarded-state-corridors")==PassPipeline::defaultLLVM().end());
}

TEST_CASE("Guarded SSA corridors do not reuse fresh PHI selection for historical cycles","[llvm][guarded-state-corridors]") {
  Fixture f(4);auto r=f.run();REQUIRE(r.changed);CHECK(r.counters.at("rewritten_edges")==1);auto fast=f.fast();REQUIRE(fast.size()==1);CHECK(fast[0]->case_begin()->getCaseValue()->equalsInt(7));fallback(fast[0]);CHECK(f.named("right")->getTerminator()->getSuccessor(0)==f.named("join"));
  auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
}

TEST_CASE("Guarded SSA corridors account repeated prediction work before any transaction","[llvm][guarded-state-corridors]") {
  Fixture f;auto &c=f.module.getContext();auto *unknown=f.named("unknown"),*join=f.named("join"),*miss=f.named("miss");
  auto *guest=&f.function->getEntryBlock().front();unknown->getTerminator()->eraseFromParent();llvm::IRBuilder<llvm::NoFolder> ir(unknown);
  std::function<llvm::Value *(unsigned)> expression=[&](unsigned depth)->llvm::Value * {
    if(!depth)return f.function->getArg(2);
    auto *left=expression(depth-1),*right=expression(depth-1);return ir.CreateAdd(left,right);
  };
  auto *index=expression(6);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,f.function);};
  std::function<llvm::BasicBlock *(unsigned)> tree=[&](unsigned depth) {
    auto *b=block(depth?"split":"terminal");
    if(depth){auto *left=tree(depth-1),*right=tree(depth-1);ir.SetInsertPoint(b);ir.CreateCondBr(f.function->getArg(3),left,right);}
    else {ir.SetInsertPoint(b);auto *p=ir.CreateGEP(ir.getInt8Ty(),guest,index);auto *value=ir.CreateLoad(ir.getInt64Ty(),p);value->setAlignment(llvm::Align(1));auto *sw=ir.CreateSwitch(value,miss);sw->addCase(ir.getInt64(7),miss);}
    return b;
  };
  auto *root=tree(10);ir.SetInsertPoint(unknown);ir.CreateCondBr(f.function->getArg(3),join,root);
  REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));const auto before=f.text();auto result=f.run();
  CHECK_FALSE(result.changed);CHECK(result.status==Status::Incomplete);CHECK(result.counters.at("budget_exhausted")>0);CHECK(result.counters.at("rejected")==1);
  CHECK(result.message=="state-corridor total scan budget exhausted");CHECK(f.text()==before);
}
