#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <set>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
enum Mode {Correlated,Compare,Freeze,Load,Three,Four,UnknownArm,Undef,Poison,Cycle,Historical,OldFinite};
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();llvm::Function *function;
  Fixture(Mode mode=Correlated) {
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);
    auto *i64=llvm::Type::getInt64Ty(c),*i1=llvm::Type::getInt1Ty(c);
    function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,i1,i1,i1,i1},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,function);};
    auto *entry=block("entry"),*head=block("head"),*dispatch=block("dispatch"),*latch=block("latch"),*exit=block("exit");
    llvm::IRBuilder<llvm::NoFolder> ir(entry);auto *guest=ir.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));ir.CreateBr(head);
    // Leave the real loop edge as the only candidate in history/cycle modes.
    if(mode==Historical || mode==Cycle)entry->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
    ir.SetInsertPoint(head);llvm::PHINode *old=nullptr;
    if(mode==Historical || mode==Cycle){old=ir.CreatePHI(i1,2,"old.condition");old->addIncoming(ir.getFalse(),entry);}
    llvm::Value *condition=function->getArg(3);
    if(mode==Compare)condition=ir.CreateICmpEQ(function->getArg(2),ir.getInt64(0),"condition");
    if(mode==Freeze)condition=ir.CreateFreeze(condition,"condition");
    if(mode==Load || mode==Historical){auto *p=ir.CreateGEP(ir.getInt8Ty(),guest,ir.getInt64(16));auto *v=ir.CreateLoad(i1,p,"fresh.condition");v->setAlignment(llvm::Align(1));condition=v;}
    if(mode==Undef)condition=llvm::UndefValue::get(i1);
    if(mode==Poison)condition=llvm::PoisonValue::get(i1);
    if(mode==Historical)old->addIncoming(condition,latch);
    if(mode==Cycle){old->addIncoming(old,latch);condition=old;}
    llvm::Value *state=nullptr;
    if(mode==Three || mode==Four) {
      state=ir.getInt64(0);for(unsigned n=0;n<(mode==Three?3u:4u);++n){auto *v=ir.CreateZExt(function->getArg(3+n),i64);state=ir.CreateOr(state,ir.CreateShl(v,ir.getInt64(n)));}
    } else if(mode==OldFinite)state=ir.CreateSelect(condition,ir.getInt64(1),ir.getInt64(8));
    else {
      llvm::Value *yes=mode==UnknownArm?static_cast<llvm::Value *>(function->getArg(2)):ir.getInt64(0);
      auto *choice=ir.CreateSelect(mode==Historical?old:condition,yes,ir.getInt64(8),"choice");
      state=ir.CreateOr(choice,ir.CreateZExt(condition,i64),"correlated.state");
    }
    ir.CreateBr(dispatch);ir.SetInsertPoint(dispatch);
    auto *index=ir.CreateAdd(ir.getInt64(1024),ir.CreateShl(state,ir.getInt64(3)));
    auto *address=ir.CreateGEP(ir.getInt8Ty(),guest,index);auto *actual=ir.CreateLoad(i64,address,"actual");actual->setAlignment(llvm::Align(1));
    auto *sw=ir.CreateSwitch(actual,exit);for(unsigned n=0;n<16;++n)sw->addCase(ir.getInt64(100+n),n==0?latch:exit);
    ir.SetInsertPoint(latch);ir.CreateBr(head);
    ir.SetInsertPoint(exit);auto *out=ir.CreatePHI(i64,16,"selected");for(unsigned n=0;n<16;++n)out->addIncoming(actual,dispatch);ir.CreateRet(out);
    std::string json="{\"view_symbol\":\"view\",\"memory\":[";
    const char *hex="0123456789abcdef";for(unsigned n=0;n<16;++n){if(n)json+=",";std::string offset;llvm::raw_string_ostream o(offset);o<<llvm::format_hex(1024+n*8,0);std::string bytes;std::uint64_t value=100+n;for(unsigned b=0;b<8;++b){auto v=(value>>(8*b))&255;bytes+=hex[v>>4];bytes+=hex[v&15];}json+="{\"offset\":\""+offset+"\",\"bytes\":\""+bytes+"\"}";}json+="]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));
    REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  std::string text(){std::string s;llvm::raw_string_ostream os(s);module.print(os,nullptr);return s;}
  Result run(Options options={}){PassPipeline pipeline;REQUIRE(pipeline.add("guarded-state-corridors",options));auto report=pipeline.run(lifter);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs[0].result;}
  std::vector<std::set<std::uint64_t>> sets(){std::vector<std::set<std::uint64_t>> out;for(auto &block:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());sw && sw->getMetadata(Generated))if(auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());load && load->getParent()==&block){auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(sw->getDefaultDest()->getTerminator());REQUIRE(fallback);CHECK(fallback->getCondition()==load);CHECK(fallback->getNumCases()==16);std::set<std::uint64_t> values;for(auto item:sw->cases())values.insert(item.getCaseValue()->getZExtValue());out.push_back(std::move(values));}return out;}
};
}
TEST_CASE("Guarded Boolean retries keep one condition correlated across select and cast","[llvm][guarded-correlated-booleans]") {
  for(auto mode:{Correlated,Compare,Freeze,Load}){Fixture f(mode);auto result=f.run();REQUIRE(result.changed);auto sets=f.sets();REQUIRE_FALSE(sets.empty());for(auto &values:sets)CHECK((values==std::set<std::uint64_t>{101,108}));auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);}
}
TEST_CASE("Guarded Boolean retries bound assignments and reject every unknown scenario","[llvm][guarded-correlated-booleans]") {
  Fixture three(Three);REQUIRE(three.run().changed);auto sets=three.sets();REQUIRE_FALSE(sets.empty());for(auto &values:sets)CHECK((values==std::set<std::uint64_t>{100,101,102,103,104,105,106,107}));
  for(auto mode:{Four,UnknownArm,Undef,Poison,Cycle}){Fixture f(mode);auto before=f.text();auto result=f.run();CHECK_FALSE(result.changed);CHECK(f.text()==before);if(mode==Four){CHECK(result.status==Status::Incomplete);CHECK(result.counters.at("budget_exhausted")>0);}}
}
TEST_CASE("Guarded Boolean retries distinguish old and fresh executions of the same LOAD","[llvm][guarded-correlated-booleans]") {
  Fixture f(Historical);auto *function=f.function;auto *arg=function->getArg(0);auto result=f.run();REQUIRE(result.changed);CHECK(function==f.module.getFunction("view"));CHECK(function->getArg(0)==arg);auto sets=f.sets();bool separated=false;for(auto &values:sets)separated|=values==std::set<std::uint64_t>{100,101,108,109};CHECK(separated);
}
TEST_CASE("Guarded Boolean retries preserve successful legacy predictions and zero budgets","[llvm][guarded-correlated-booleans]") {
  Fixture legacy(OldFinite);auto result=legacy.run();REQUIRE(result.changed);auto sets=legacy.sets();REQUIRE_FALSE(sets.empty());for(auto &values:sets)CHECK((values==std::set<std::uint64_t>{101,108}));
  for(const char *key:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture f;auto before=f.text();auto r=f.run({{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==before);}
}
