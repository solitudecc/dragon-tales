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
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();llvm::Function *function;
  Fixture(const std::string &mode="normal",bool duplicate=false){
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);
    function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *s){return llvm::BasicBlock::Create(c,s,function);};auto *entry=block("entry"),*seed=block("seed"),*unknown=block("unknown"),*head=block("head"),*read=block("read"),*dispatch=block("dispatch"),*hit=block("hit"),*miss=block("miss");
    llvm::IRBuilder<llvm::NoFolder> ir(entry);auto *guest=ir.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));auto *sizeptr=ir.CreateGEP(ir.getInt8Ty(),function->getArg(1),ir.getInt64(8));auto *size=ir.CreateLoad(i64,sizeptr);size->setAlignment(llvm::Align(1));
    auto address=[&](std::uint64_t n){return ir.CreateGEP(ir.getInt8Ty(),guest,ir.getInt64(n));};auto store=[&](llvm::Value *v,llvm::Value *p){auto *s=ir.CreateStore(v,p);s->setAlignment(llvm::Align(1));return s;};
    ir.CreateCondBr(function->getArg(3),seed,unknown);ir.SetInsertPoint(seed);store(ir.getInt32(50),address(240));
    if(mode=="legacy")store(ir.getInt64(8),address(256));
    if(duplicate)ir.CreateCondBr(function->getArg(3),head,head);else ir.CreateBr(head);
    ir.SetInsertPoint(unknown);ir.CreateBr(head);ir.SetInsertPoint(head);auto *state=ir.CreatePHI(i64,duplicate?3:2,"selected.state");state->addIncoming(ir.getInt64(50),seed);if(duplicate)state->addIncoming(ir.getInt64(50),seed);state->addIncoming(function->getArg(2),unknown);
    auto *write=store(state,address(mode=="unrelated"?280:256));if(mode=="volatile")write->setVolatile(true);
    if(mode=="overwrite-state")store(function->getArg(2),address(256));
    if(mode=="call"){auto callee=module.getOrInsertFunction("external",llvm::FunctionType::get(ir.getVoidTy(),{ptr},false));ir.CreateCall(callee,{guest});}
    ir.CreateCondBr(ir.CreateICmpUGT(size,ir.getInt64(287)),read,miss);ir.SetInsertPoint(read);auto *value=ir.CreateLoad(i64,address(256),"actual.state");value->setAlignment(llvm::Align(1));auto *offset=ir.CreateAdd(ir.getInt64(1024),ir.CreateShl(value,ir.getInt64(3)));auto *enough=ir.CreateICmpUGT(size,ir.getInt64(7));auto *range=ir.CreateICmpULE(offset,ir.CreateSub(size,ir.getInt64(8)));ir.CreateCondBr(ir.CreateAnd(enough,range),dispatch,miss);
    ir.SetInsertPoint(dispatch);auto *table=ir.CreateGEP(ir.getInt8Ty(),guest,offset);if(mode=="overwrite-table")store(function->getArg(2),table);auto *actual=ir.CreateLoad(i64,table,"actual.target");actual->setAlignment(llvm::Align(1));if(mode=="after-load")store(function->getArg(2),table);
    auto *sw=ir.CreateSwitch(actual,miss);sw->addCase(ir.getInt64(100),hit);sw->addCase(ir.getInt64(200),hit);sw->addCase(ir.getInt64(300),miss);
    ir.SetInsertPoint(hit);auto *result=ir.CreatePHI(i64,2);result->addIncoming(actual,dispatch);result->addIncoming(actual,dispatch);ir.CreateRet(result);ir.SetInsertPoint(miss);ir.CreateRet(ir.getInt64(-1));
    const char *hex="0123456789abcdef";std::string json="{\"view_symbol\":\"view\",\"memory\":[";unsigned states[]={8,50},targets[]={100,200};for(unsigned n=0;n<2;++n){if(n)json+=",";std::string offsetText,bytes;llvm::raw_string_ostream out(offsetText);out<<llvm::format_hex(1024+8*states[n],0);for(unsigned k=0;k<8;++k){auto b=(static_cast<std::uint64_t>(targets[n])>>(8*k))&255;bytes+=hex[b>>4];bytes+=hex[b&15];}json+="{\"offset\":\""+offsetText+"\",\"bytes\":\""+bytes+"\"}";}json+="]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
  llvm::BasicBlock *named(llvm::StringRef name){for(auto &b:*function)if(b.getName()==name)return &b;return nullptr;}
  Result run(bool stores=true,Options options={}){PassPipeline p;REQUIRE(p.add(stores?"guarded-store-corridors":"guarded-state-corridors",options));auto report=p.run(lifter);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs.front().result;}
  std::vector<llvm::SwitchInst *> fast(){std::vector<llvm::SwitchInst *> result;for(auto &b:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator());sw && sw->getMetadata(Generated))if(auto *l=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());l && l->getParent()==&b)result.push_back(sw);return result;}
};
void check(Fixture &f,std::uint64_t target,unsigned count=1){auto list=f.fast();REQUIRE(list.size()==count);for(auto *fast:list){REQUIRE(fast->getNumCases()==1);CHECK(fast->case_begin()->getCaseValue()->equalsInt(target));auto *full=llvm::dyn_cast<llvm::SwitchInst>(fast->getDefaultDest()->getTerminator());REQUIRE(full);CHECK(full->getCondition()==fast->getCondition());CHECK(full->getNumCases()==3);}}
}
TEST_CASE("Guarded path STORE predicts only the selected fresh PHI incoming","[llvm][guarded-path-stores]"){
 Fixture f;auto before=f.text();CHECK_FALSE(f.run(false).changed);CHECK(f.text()==before);auto *identity=f.function;auto *arg=identity->getArg(0);auto r=f.run();REQUIRE(r.changed);CHECK(r.counters.at("rewritten_edges")==1);check(f,200);CHECK(f.function==f.module.getFunction("view"));CHECK(identity->getArg(0)==arg);CHECK(f.named("seed")->getTerminator()->getSuccessor(0)!=f.named("head"));CHECK(f.named("unknown")->getTerminator()->getSuccessor(0)==f.named("head"));
 unsigned stores=0,loads=0;for(auto &b:*f.function)if(b.getTerminator()->getMetadata(Generated))for(auto &i:b){stores+=llvm::isa<llvm::StoreInst>(i);loads+=llvm::isa<llvm::LoadInst>(i);}CHECK(stores>=1);CHECK(loads>=2);auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
}
TEST_CASE("Guarded path STORE keeps seed priority wrong hints and write order","[llvm][guarded-path-stores]"){
 Fixture legacy("legacy");REQUIRE(legacy.run().changed);check(legacy,100);
 for(const char *mode:{"unrelated","overwrite-state","overwrite-table","after-load"}){Fixture f(mode);REQUIRE(f.run().changed);check(f,200);if(std::string(mode)=="after-load"){auto *sw=f.fast().front();auto *actual=llvm::cast<llvm::LoadInst>(sw->getCondition());bool seen=false,wrote=false;for(auto &i:*sw->getParent()){seen|=&i==actual;if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i);s && s->getPointerOperand()==actual->getPointerOperand()){CHECK(seen);wrote=true;}}CHECK(wrote);}}
}
TEST_CASE("Guarded path STORE retains effect barriers and finite original slot budgets","[llvm][guarded-path-stores]"){
 for(const char *mode:{"call","volatile"}){Fixture f(mode);auto before=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==before);}
 Fixture f("normal",true);auto first=f.run(true,{{"max_rewrites",std::int64_t(1)}});REQUIRE(first.changed);CHECK(first.status==Status::Incomplete);check(f,200);REQUIRE(f.run().changed);check(f,200,2);auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
 for(const char *key:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture zero;auto before=zero.text();auto r=zero.run(true,{{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(zero.text()==before);}
}
