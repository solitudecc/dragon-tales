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
enum class Mode {Same,Fresh,Two,Undef,Poison,Cycle,UnknownArm,Four,Finite};
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();llvm::Function *function;
  Fixture(Mode mode) {
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);
    auto *i64=llvm::Type::getInt64Ty(c),*i1=llvm::Type::getInt1Ty(c);
    function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,i1,i1,i1},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *name){return llvm::BasicBlock::Create(c,name,function);};
    auto *entry=block("entry"),*route=block("route"),*left=block("left"),*right=block("right"),*join=block("old.join");
    auto *seed=block("seed"),*other=block("other"),*head=block("head"),*dispatch=block("dispatch"),*exit=block("exit");
    llvm::IRBuilder<llvm::NoFolder> b(entry);auto *guest=b.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));b.CreateCondBr(function->getArg(3),route,other);
    b.SetInsertPoint(route);b.CreateCondBr(function->getArg(4),left,right);
    b.SetInsertPoint(left);b.CreateBr(join);b.SetInsertPoint(right);b.CreateBr(join);
    b.SetInsertPoint(join);std::vector<llvm::PHINode *> historical;
    unsigned count=mode==Mode::Four?3:mode==Mode::Two?2:1;
    for(unsigned n=0;n<count;++n){auto *p=b.CreatePHI(i1,3,"old.boolean");p->addIncoming(function->getArg(4),left);p->addIncoming(function->getArg(5),right);historical.push_back(p);}
    if(mode==Mode::Undef)historical[0]->setIncomingValue(0,llvm::UndefValue::get(i1));
    if(mode==Mode::Poison)historical[0]->setIncomingValue(0,llvm::PoisonValue::get(i1));
    if(mode==Mode::Cycle){historical[0]->addIncoming(historical[0],join);b.CreateCondBr(function->getArg(3),join,seed);}else b.CreateBr(seed);
    b.SetInsertPoint(seed);b.CreateBr(head);b.SetInsertPoint(other);b.CreateBr(head);
    b.SetInsertPoint(head);std::vector<llvm::PHINode *> prior;
    for(auto *old:historical){auto *p=b.CreatePHI(i1,2,"selected.old");p->addIncoming(old,seed);p->addIncoming(b.getFalse(),other);prior.push_back(p);}
    auto *bp=b.CreateGEP(b.getInt8Ty(),guest,b.getInt64(16));auto *fresh=b.CreateLoad(i1,bp,"fresh.boolean");fresh->setAlignment(llvm::Align(1));
    llvm::Value *condition=mode==Mode::Finite?static_cast<llvm::Value *>(function->getArg(3)):prior[0];
    llvm::Value *yes=mode==Mode::UnknownArm?static_cast<llvm::Value *>(function->getArg(2)):b.getInt64(0);
    auto *choice=b.CreateSelect(condition,yes,b.getInt64(8));
    llvm::Value *bit=condition;
    if(mode==Mode::Fresh)bit=fresh;
    if(mode==Mode::Two || mode==Mode::Four)bit=prior[1];
    llvm::Value *state=b.CreateOr(choice,b.CreateZExt(bit,i64),"state");
    if(mode==Mode::Four){state=b.CreateOr(state,b.CreateShl(b.CreateZExt(prior[2],i64),b.getInt64(1)));state=b.CreateOr(state,b.CreateShl(b.CreateZExt(fresh,i64),b.getInt64(2)));}
    b.CreateBr(dispatch);b.SetInsertPoint(dispatch);
    auto *index=b.CreateAdd(b.getInt64(1024),b.CreateShl(state,b.getInt64(3)));
    auto *address=b.CreateGEP(b.getInt8Ty(),guest,index);auto *actual=b.CreateLoad(i64,address,"actual");actual->setAlignment(llvm::Align(1));
    auto *sw=b.CreateSwitch(actual,exit);for(unsigned n=0;n<16;++n)sw->addCase(b.getInt64(100+n),exit);
    b.SetInsertPoint(exit);auto *out=b.CreatePHI(i64,17,"result");for(unsigned n=0;n<17;++n)out->addIncoming(actual,dispatch);b.CreateRet(out);
    for(auto *skip:{entry,route,left,right,join,other})skip->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
    std::string json="{\"view_symbol\":\"view\",\"memory\":[";const char *hex="0123456789abcdef";
    for(unsigned n=0;n<16;++n){if(n)json+=",";std::string offset;llvm::raw_string_ostream o(offset);o<<llvm::format_hex(1024+n*8,0);std::string bytes;for(unsigned k=0;k<8;++k){unsigned v=((std::uint64_t(100+n))>>(k*8))&255;bytes+=hex[v>>4];bytes+=hex[v&15];}json+="{\"offset\":\""+offset+"\",\"bytes\":\""+bytes+"\"}";}json+="]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));
    REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
  Result run(Options options={}){PassPipeline p;REQUIRE(p.add("guarded-state-corridors",options));auto result=p.run(lifter);REQUIRE(result.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return result.runs[0].result;}
  std::set<std::uint64_t> targets(){std::set<std::uint64_t> values;unsigned copies=0;for(auto &block:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());sw && sw->getMetadata(Generated)){auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(sw->getDefaultDest()->getTerminator());if(!fallback)continue;++copies;CHECK(sw->getCondition()==fallback->getCondition());CHECK(fallback->getNumCases()==16);for(auto item:sw->cases())values.insert(item.getCaseValue()->getZExtValue());}CHECK(copies==1);return values;}
};
}
TEST_CASE("Historical Boolean join has one scenario identity","[llvm][guarded-historical-booleans]") {
  Fixture f(Mode::Same);auto *function=f.function;auto *arg=function->getArg(0);auto result=f.run();REQUIRE(result.changed);CHECK(result.counters.at("rewritten_edges")==1);CHECK(f.module.getFunction("view")==function);CHECK(function->getArg(0)==arg);CHECK((f.targets()==std::set<std::uint64_t>{101,108}));auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
}
TEST_CASE("Historical Boolean joins distinguish separate values and fresh execution","[llvm][guarded-historical-booleans]") {
  for(auto mode:{Mode::Fresh,Mode::Two}){Fixture f(mode);REQUIRE(f.run().changed);CHECK((f.targets()==std::set<std::uint64_t>{100,101,108,109}));}
}
TEST_CASE("Historical Boolean identity rejects invalid and unknown scenarios","[llvm][guarded-historical-booleans]") {
  for(auto mode:{Mode::Undef,Mode::Poison,Mode::Cycle,Mode::UnknownArm,Mode::Four}){Fixture f(mode);auto before=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==before);}
  for(const char *budget:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture f(Mode::Same);auto before=f.text();auto r=f.run({{budget,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==before);}
}
TEST_CASE("Historical Boolean retry leaves earlier successful finite prediction intact","[llvm][guarded-historical-booleans]") {
  Fixture f(Mode::Finite);auto r=f.run();REQUIRE(r.changed);CHECK((f.targets()==std::set<std::uint64_t>{101,108}));
}
