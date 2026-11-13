#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
 dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
 llvm::Module &module=lifter.module();llvm::Function *function;
 explicit Fixture(const std::string &mode="mask") {
  module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);auto *i1=llvm::Type::getInt1Ty(c);
  function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,i1,i1},false),llvm::GlobalValue::ExternalLinkage,"view",module);
  auto block=[&](const char *name){return llvm::BasicBlock::Create(c,name,function);};
  auto *entry=block("entry"),*seed=block("seed"),*other=block("other"),*head=block("head"),*dispatch=block("dispatch"),*hit=block("hit"),*miss=block("miss");
  llvm::IRBuilder<llvm::NoFolder> b(entry);auto *guest=b.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));
  auto helper=module.getOrInsertFunction("external_maywrite",llvm::FunctionType::get(b.getVoidTy(),{ptr,ptr},false));
  b.CreateCall(helper,{function->getArg(0),guest});auto *sizep=b.CreateGEP(b.getInt8Ty(),function->getArg(1),b.getInt64(8));auto *size=b.CreateLoad(i64,sizep,"size");size->setAlignment(llvm::Align(1));b.CreateCondBr(function->getArg(4),seed,other);
  // Only the selected seed is a candidate; no generated marker is a value fact.
  entry->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
  b.SetInsertPoint(seed);llvm::PHINode *cycle=nullptr;
  if(mode=="cycle"){cycle=b.CreatePHI(i64,2,"cycle");cycle->addIncoming(cycle,seed);cycle->addIncoming(function->getArg(2),entry);}
  auto *reload=b.CreateLoad(i64,function->getArg(0),"after.call.reload");reload->setAlignment(llvm::Align(1));
  llvm::Value *unknown=reload,*bit=function->getArg(3);
  if(mode=="undef")unknown=llvm::UndefValue::get(i64);
  if(mode=="poison")unknown=llvm::PoisonValue::get(i64);
  if(mode=="cycle")unknown=cycle;
  if(mode=="shift-finite"||mode=="shift-unknown")unknown=b.CreateShl(mode=="shift-finite"?static_cast<llvm::Value *>(b.getInt64(1)):reload,b.getInt64(64),"invalid.shift");
  if(mode=="depth")for(unsigned n=0;n<48;++n)unknown=b.CreateAdd(unknown,b.getInt64(0),"deep");
  if(mode=="values"){
   unknown=b.getInt64(0);
   for(unsigned n=0;n<4;++n){auto *test=b.CreateICmpNE(b.CreateAnd(reload,b.getInt64(1u<<n)),b.getInt64(0));auto *value=b.CreateSelect(test,b.getInt64(1u<<n),b.getInt64(0));unknown=b.CreateAdd(unknown,value);}
  }
  if(mode=="bool-undef")bit=llvm::UndefValue::get(i1);
  if(mode=="bool-poison")bit=llvm::PoisonValue::get(i1);
  if(mode=="bool-icmp-poison")bit=b.CreateICmpEQ(llvm::PoisonValue::get(i64),reload,"poison.bit");
  if(mode=="bool-load-undefptr"){auto *load=b.CreateLoad(i1,llvm::UndefValue::get(ptr),"undef.pointer.bit");load->setAlignment(llvm::Align(1));bit=load;}
  if(mode=="freeze")bit=b.CreateFreeze(llvm::UndefValue::get(i1),"defined.bit");
  auto *mask=b.CreateSExt(bit,i64,"boolean.mask");auto *masked=b.CreateAnd(unknown,mask,"masked.unknown");
  auto *finite=b.CreateSelect(bit,b.getInt64(0),b.getInt64(40),"finite.arm");llvm::Value *state=b.CreateOr(masked,finite,"masked.state");
  if(mode=="outer-add")state=b.CreateAdd(state,reload,"unknown.outer.add");
  if(mode=="flags"){auto *add=llvm::cast<llvm::BinaryOperator>(b.CreateAdd(state,b.getInt64(0),"retained.flags"));add->setHasNoSignedWrap();add->setHasNoUnsignedWrap();state=add;}
  if(mode=="finite")state=b.CreateSelect(bit,b.getInt64(0),b.getInt64(40),"finite.state");
  if(mode=="old-partial")state=b.CreateSelect(bit,reload,b.getInt64(40),"old.partial");
  if(mode=="cycle")b.CreateCondBr(function->getArg(4),seed,head);else b.CreateBr(head);
  b.SetInsertPoint(other);b.CreateBr(head);other->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
  b.SetInsertPoint(head);auto *phi=b.CreatePHI(i64,2,"state");phi->addIncoming(state,seed);phi->addIncoming(function->getArg(2),other);
  if(mode=="call-inside")b.CreateCall(helper,{function->getArg(0),guest});
  auto *index=b.CreateAdd(b.getInt64(1024),b.CreateShl(phi,b.getInt64(3)),"table.index");auto *range=b.CreateAnd(b.CreateICmpUGT(size,b.getInt64(7)),b.CreateICmpULE(index,b.CreateSub(size,b.getInt64(8))));b.CreateCondBr(range,dispatch,miss);
  b.SetInsertPoint(dispatch);auto *address=b.CreateGEP(b.getInt8Ty(),guest,index,"table.address");
  if(mode=="table-before")b.CreateStore(function->getArg(2),address)->setAlignment(llvm::Align(1));
  auto *actual=b.CreateLoad(i64,address,"actual.target");actual->setAlignment(llvm::Align(1));
  if(mode=="table-after")b.CreateStore(function->getArg(2),address)->setAlignment(llvm::Align(1));
  auto *sw=b.CreateSwitch(actual,miss);sw->addCase(b.getInt64(100),hit);sw->addCase(b.getInt64(200),hit);sw->addCase(b.getInt64(300),miss);
  b.SetInsertPoint(hit);auto *out=b.CreatePHI(i64,2,"result");out->addIncoming(actual,dispatch);out->addIncoming(actual,dispatch);b.CreateStore(out,function->getArg(0))->setAlignment(llvm::Align(1));b.CreateRet(out);
  b.SetInsertPoint(miss);b.CreateRet(b.getInt64(-1));
  std::string hint="{\"view_symbol\":\"view\",\"memory\":[";const char *hex="0123456789abcdef";
  const unsigned states[]={0,40},targets[]={100,mode=="wrong-hint"?300u:200u};
  for(unsigned row=0;row<2;++row){if(row)hint+=",";std::string offset,bytes;llvm::raw_string_ostream os(offset);os<<llvm::format_hex(1024+8*states[row],0);for(unsigned n=0;n<8;++n){auto v=(static_cast<std::uint64_t>(targets[row])>>(8*n))&255;bytes+=hex[v>>4];bytes+=hex[v&15];}hint+="{\"offset\":\""+offset+"\",\"bytes\":\""+bytes+"\"}";}hint+="]}";
  module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,hint)));verify();
 }
 void verify(){REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));}
 std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
 std::string instruction(llvm::StringRef name){for(auto &block:*function)for(auto &i:block)if(i.getName()==name){std::string s;llvm::raw_string_ostream out(s);i.print(out);return s;}return {};}
 Result run(const char *name="guarded-partial-masks",Options options={}){PassPipeline p;REQUIRE(p.add(name,options));RunOptions opts;opts.maxIterations=1;opts.maxPassRuns=1;opts.verifyLLVM=true;auto report=p.run(lifter,opts);REQUIRE(report.ok);REQUIRE(report.runs.size()==1);verify();return report.runs.front().result;}
 llvm::SwitchInst *fast(){llvm::SwitchInst *found=nullptr;for(auto &block:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());sw&&sw->getMetadata(Generated))if(auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());load&&load->getParent()==&block){REQUIRE(found==nullptr);found=sw;}REQUIRE(found);return found;}
 void check(const std::set<std::uint64_t> &expected){auto *sw=fast();std::set<std::uint64_t> actual;for(auto c:sw->cases())actual.insert(c.getCaseValue()->getZExtValue());CHECK(actual==expected);auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(sw->getDefaultDest()->getTerminator());REQUIRE(fallback);CHECK(fallback->getCondition()==sw->getCondition());CHECK(fallback->getNumCases()==3);CHECK(fallback->getDefaultDest()->getName()=="miss");}
};
}
TEST_CASE("Partial masks predict Cartesian zero mask hints while preserving real source","[llvm][guarded-partial-masks]") {
 for(const char *mode:{"mask","flags","freeze"}){INFO(mode);Fixture f(mode);std::vector<std::pair<std::string,std::string>> source;for(const char *n:{"after.call.reload","boolean.mask","masked.unknown","finite.arm","masked.state","retained.flags","defined.bit"}){auto text=f.instruction(n);if(!text.empty())source.emplace_back(n,text);}auto *function=f.function;auto *arg=function->getArg(0);auto r=f.run();REQUIRE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("partial_candidate_edges")==1);CHECK(r.counters.at("partial_attempted_edges")==1);CHECK(r.counters.at("partial_rewritten_edges")==1);CHECK(r.counters.at("partial_unresolved_edges")==0);CHECK(r.counters.at("budget_exhausted")==0);f.check({100,200});CHECK(f.module.getFunction("view")==function);CHECK(function->getArg(0)==arg);for(const auto &[n,s]:source)CHECK(f.instruction(n)==s);unsigned calls=0;for(auto &block:*function)for(auto &i:block)if(auto *call=llvm::dyn_cast<llvm::CallBase>(&i)){++calls;CHECK(call->getCalledFunction()->getName()=="external_maywrite");CHECK(block.getName()=="entry");CHECK_FALSE(call->doesNotAccessMemory());}CHECK(calls==1);for(auto &a:function->args())CHECK_FALSE(a.hasAttribute(llvm::Attribute::NoAlias));auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);}
}
TEST_CASE("Partial masks retain wrong predictions and store order with same read fallback","[llvm][guarded-partial-masks]") {
 for(const char *mode:{"wrong-hint","table-before","table-after"}){INFO(mode);Fixture f(mode);REQUIRE(f.run().changed);f.check(std::string(mode)=="wrong-hint"?std::set<std::uint64_t>{100,300}:std::set<std::uint64_t>{100,200});if(std::string(mode)!="wrong-hint"){auto *sw=f.fast();auto *load=llvm::cast<llvm::LoadInst>(sw->getCondition());bool seen=false,wrote=false;for(auto &i:*sw->getParent()){seen|=&i==load;if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)){CHECK(store->getPointerOperand()==load->getPointerOperand());CHECK(seen==(std::string(mode)=="table-after"));wrote=true;}}CHECK(wrote);}}
}
TEST_CASE("Partial masks reject fatal dependencies unknown outer arithmetic and resource limits","[llvm][guarded-partial-masks]") {
 for(const char *mode:{"undef","poison","bool-undef","bool-poison","bool-icmp-poison","bool-load-undefptr","shift-finite","shift-unknown","cycle","depth","values","outer-add","call-inside"}){INFO(mode);Fixture f(mode);auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.counters.at("partial_rewritten_edges")==0);CHECK(f.text()==before);}
 for(const char *key:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture f;auto before=f.text();auto r=f.run("guarded-partial-masks",{{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(r.counters.at("partial_rewritten_edges")==0);CHECK(f.text()==before);}
}
TEST_CASE("Partial masks preserve legacy finite and partial controller bytes and counters","[llvm][guarded-partial-masks]") {
 for(const char *mode:{"finite","old-partial"}){Fixture legacy(mode),next(mode);auto old=legacy.run("guarded-partial-corridors"),fresh=next.run();REQUIRE(old.changed);REQUIRE(fresh.changed);CHECK(next.text()==legacy.text());CHECK(fresh.counters==old.counters);CHECK(fresh.status==old.status);}
 CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"guarded-partial-masks")==PassPipeline::defaultLLVM().end());
}
TEST_CASE("Legacy controllers keep masked state unresolved without mutating source","[llvm][guarded-partial-masks][guarded-partial-masks-legacy]") {
 Fixture finite,store,partial;auto before=finite.text();auto f=finite.run("guarded-state-corridors"),h=store.run("guarded-store-corridors"),j=partial.run("guarded-partial-corridors");CHECK_FALSE(f.changed);CHECK_FALSE(h.changed);CHECK_FALSE(j.changed);CHECK(f.counters.at("candidate_edges")==0);CHECK(h.counters.at("candidate_edges")==0);CHECK(j.counters.at("candidate_edges")==0);CHECK(f.counters==h.counters);for(const auto &[k,v]:f.counters)CHECK(j.counters.at(k)==v);CHECK(j.counters.at("partial_candidate_edges")==0);CHECK(finite.text()==before);CHECK(store.text()==before);CHECK(partial.text()==before);
}
