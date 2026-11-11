#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();llvm::Function *function;
  Fixture(const std::string &mode="normal",bool duplicate=false) {
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);
    function=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt64Ty(c),{ptr,ptr,llvm::Type::getInt64Ty(c),llvm::Type::getInt1Ty(c),llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,function);};
    auto *entry=block("entry"),*seed=block("seed"),*unknown=block("unknown"),*head=block("head"),*readA=block("read.a"),*writeB=block("write.b"),*after=block("after"),*readB=block("read.b"),*index=block("index"),*bounds=block("bounds"),*dispatch=block("dispatch"),*hit=block("hit"),*miss=block("miss");
    llvm::IRBuilder<llvm::NoFolder> ir(entry);auto *guest=ir.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));auto *sizeptr=ir.CreateGEP(ir.getInt8Ty(),function->getArg(1),ir.getInt64(8));auto *size=ir.CreateLoad(ir.getInt64Ty(),sizeptr);size->setAlignment(llvm::Align(1));
    auto address=[&](llvm::Value *offset){return ir.CreateGEP(ir.getInt8Ty(),guest,offset);};
    auto store=[&](llvm::Value *v,llvm::Value *p){auto *s=ir.CreateStore(v,p);s->setAlignment(llvm::Align(1));return s;};
    auto load=[&](llvm::Value *p){auto *l=ir.CreateLoad(ir.getInt64Ty(),p);l->setAlignment(llvm::Align(1));return l;};
    ir.CreateCondBr(function->getArg(4),seed,unknown);ir.SetInsertPoint(seed);
    llvm::Value *state=mode=="unknown"?function->getArg(2):ir.CreateSelect(function->getArg(3),ir.getInt64(8),ir.getInt64(38),"next.state");
    store(state,address(ir.getInt64(256)));
    if(mode=="unrelated")store(ir.CreateSelect(function->getArg(3),ir.getInt64(24),ir.getInt64(25)),address(ir.getInt64(280)));
    if(duplicate)ir.CreateCondBr(function->getArg(3),head,head);else ir.CreateBr(head);
    ir.SetInsertPoint(unknown);ir.CreateBr(head);ir.SetInsertPoint(head);auto *phi=ir.CreatePHI(ir.getInt64Ty(),duplicate?3:2,"external.input");phi->addIncoming(function->getArg(2),seed);if(duplicate)phi->addIncoming(function->getArg(2),seed);phi->addIncoming(function->getArg(2),unknown);
    ir.CreateCondBr(ir.CreateICmpUGT(size,ir.getInt64(287)),readA,miss);
    ir.SetInsertPoint(readA);auto *a=load(address(ir.getInt64(256)));ir.CreateBr(writeB);
    ir.SetInsertPoint(writeB);store(a,address(ir.getInt64(264)));ir.CreateBr(after);
    ir.SetInsertPoint(after);
    if(mode=="overwrite-state")store(function->getArg(2),address(ir.getInt64(264)));
    if(mode=="call") {auto callee=module.getOrInsertFunction("external",llvm::FunctionType::get(ir.getVoidTy(),{ptr},false));ir.CreateCall(callee,{guest});}
    ir.CreateBr(readB);ir.SetInsertPoint(readB);auto *b=load(address(ir.getInt64(264)));ir.CreateBr(index);
    ir.SetInsertPoint(index);auto *scaled=ir.CreateShl(b,ir.getInt64(3));auto *offset=mode=="or-address"?ir.CreateOr(scaled,ir.getInt64(1024)):ir.CreateAdd(scaled,ir.getInt64(1024));ir.CreateBr(bounds);
    ir.SetInsertPoint(bounds);auto *enough=ir.CreateICmpUGT(size,ir.getInt64(7));auto *range=ir.CreateICmpULE(offset,ir.CreateSub(size,ir.getInt64(8)));ir.CreateCondBr(ir.CreateAnd(enough,range),dispatch,miss);
    ir.SetInsertPoint(dispatch);auto *table=address(offset);
    if(mode=="overwrite-table")store(function->getArg(2),table);
    auto *actual=load(table);actual->setName("actual.target");if(mode=="after-load")store(function->getArg(2),table);
    auto *sw=ir.CreateSwitch(actual,miss);for(unsigned v:{100u,200u,300u,400u})sw->addCase(ir.getInt64(v),hit);
    ir.SetInsertPoint(hit);auto *result=ir.CreatePHI(ir.getInt64Ty(),4);for(unsigned n=0;n<4;++n)result->addIncoming(actual,dispatch);ir.CreateRet(result);ir.SetInsertPoint(miss);ir.CreateRet(ir.getInt64(-1));
    const char *hex="0123456789abcdef";std::string json="{\"view_symbol\":\"view\",\"memory\":[";unsigned states[]={8,38,24,25};unsigned values[]={100,200,300,400};
    for(unsigned n=0;n<4;++n){if(n)json+=",";std::string offsetText,bytes;llvm::raw_string_ostream os(offsetText);os<<llvm::format_hex(1024+8*states[n],0);for(unsigned k=0;k<8;++k){unsigned byte=(static_cast<std::uint64_t>(values[n])>>(8*k))&255;bytes+=hex[byte>>4];bytes+=hex[byte&15];}json+="{\"offset\":\""+offsetText+"\",\"bytes\":\""+bytes+"\"}";}json+="]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
  Result run(bool stores=true,Options options={}) {PassPipeline p;REQUIRE(p.add(stores?"guarded-store-corridors":"guarded-state-corridors",options));auto report=p.run(lifter);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs.front().result;}
  std::vector<llvm::SwitchInst *> fast(){std::vector<llvm::SwitchInst *> out;for(auto &b:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator()))if(sw->getMetadata(Generated))if(auto *l=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());l && l->getParent()==&b)out.push_back(sw);return out;}
};
void check(Fixture &f,const std::set<std::uint64_t> &expected,unsigned copies=1) {
  auto fast=f.fast();REQUIRE(fast.size()==copies);
  for(auto *sw:fast){std::set<std::uint64_t> values;for(auto c:sw->cases())values.insert(c.getCaseValue()->getZExtValue());CHECK(values==expected);auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(sw->getDefaultDest()->getTerminator());REQUIRE(fallback);CHECK(fallback->getCondition()==sw->getCondition());CHECK(fallback->getNumCases()==4);CHECK(fallback->getMetadata(Generated));}
}
}
TEST_CASE("Guarded STORE corridor predictions leave the existing SSA builtin byte exact","[llvm][guarded-store-corridors]") {
  Fixture f;const auto before=f.text();auto old=f.run(false);CHECK_FALSE(old.changed);CHECK(old.status==Status::Success);CHECK(f.text()==before);
  for(const char *key:{"candidate_edges","attempted_edges","rewritten_edges","unresolved_edges","rejected","budget_exhausted","cloned_blocks","cloned_instructions","original_switches_retained","fallback_switches_created","fast_switches_created"})CHECK(old.counters.at(key)==0);
  auto r=f.run();REQUIRE(r.changed);CHECK(r.counters.at("rewritten_edges")==1);check(f,{100,200});
  unsigned loads=0,stores=0;for(auto &b:*f.function)if(b.getTerminator()->getMetadata(Generated))for(auto &i:b){loads+=llvm::isa<llvm::LoadInst>(i);stores+=llvm::isa<llvm::StoreInst>(i);}CHECK(loads>=3);CHECK(stores>=1);
  const auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
}
TEST_CASE("Guarded STORE predictions keep wrong hints and actual later writes executable","[llvm][guarded-store-corridors]") {
  for(const char *mode:{"unrelated","overwrite-state","overwrite-table","after-load","or-address"}) {Fixture f(mode);REQUIRE(f.run().changed);check(f,std::string(mode)=="unrelated"?std::set<std::uint64_t>{300,400}:std::set<std::uint64_t>{100,200});
    if(std::string(mode)=="after-load"){auto *sw=f.fast().front();auto *actual=llvm::cast<llvm::LoadInst>(sw->getCondition());bool seen=false,after=false;for(auto &i:*sw->getParent()){if(&i==actual)seen=true;if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i);store && store->getPointerOperand()==actual->getPointerOperand()){CHECK(seen);after=true;}}CHECK(after);}
  }
}
TEST_CASE("Guarded STORE corridors refuse unknown values and effectful paths","[llvm][guarded-store-corridors]") {
  for(const char *mode:{"unknown","call"}){Fixture f(mode);auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(f.text()==before);}
  CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"guarded-store-corridors")==PassPipeline::defaultLLVM().end());
}
TEST_CASE("Guarded STORE corridors share budgets and resume duplicate original slots","[llvm][guarded-store-corridors]") {
  Fixture f("normal",true);auto first=f.run(true,{{"max_rewrites",std::int64_t(1)}});REQUIRE(first.changed);CHECK(first.status==Status::Incomplete);check(f,{100,200});auto second=f.run();REQUIRE(second.changed);check(f,{100,200},2);auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);
  for(const char *key:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture zero;auto before=zero.text();auto r=zero.run(true,{{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(zero.text()==before);}
}
