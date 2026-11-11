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
 dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};llvm::Module &module=lifter.module();llvm::Function *function;
 Fixture(const std::string &mode="normal",bool duplicate=false){
  module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);
  function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,llvm::Type::getInt1Ty(c),llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,function);};auto *entry=block("entry"),*post=block("post.call"),*other=block("other"),*head=block("head"),*dispatch=block("dispatch"),*hit=block("hit"),*miss=block("miss");
  llvm::IRBuilder<llvm::NoFolder> ir(entry);auto *guest=ir.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));auto helper=module.getOrInsertFunction("external_maywrite",llvm::FunctionType::get(ir.getVoidTy(),{ptr,ptr},false));ir.CreateCall(helper,{function->getArg(0),guest});auto *sizeptr=ir.CreateGEP(ir.getInt8Ty(),function->getArg(1),ir.getInt64(8));auto *size=ir.CreateLoad(i64,sizeptr);size->setAlignment(llvm::Align(1));ir.CreateCondBr(function->getArg(4),post,other);
  ir.SetInsertPoint(post);llvm::PHINode *cycle=nullptr;if(mode=="cycle"){cycle=ir.CreatePHI(i64,2,"cycle");cycle->addIncoming(cycle,post);cycle->addIncoming(function->getArg(2),entry);}
  auto *reload=ir.CreateLoad(i64,function->getArg(0),"after.call.reload");reload->setAlignment(llvm::Align(1));llvm::Value *unknown=reload;
  if(mode=="undef")unknown=llvm::UndefValue::get(i64);if(mode=="poison")unknown=llvm::PoisonValue::get(i64);if(cycle)unknown=cycle;if(mode=="finite")unknown=ir.getInt64(8);
  llvm::Value *known=mode=="none"?static_cast<llvm::Value *>(function->getArg(2)):ir.getInt64(50);
  llvm::Value *state=ir.CreateSelect(function->getArg(3),unknown,known,"partial.state");
  if(mode=="outside")state=ir.CreateAdd(ir.CreateSelect(function->getArg(3),ir.getInt64(8),ir.getInt64(50)),reload);
  if(mode=="cycle")ir.CreateCondBr(function->getArg(4),post,head);else if(duplicate)ir.CreateCondBr(function->getArg(3),head,head);else ir.CreateBr(head);
  ir.SetInsertPoint(other);ir.CreateBr(head);ir.SetInsertPoint(head);auto *phi=ir.CreatePHI(i64,duplicate?3:2,"state");phi->addIncoming(state,post);if(duplicate)phi->addIncoming(state,post);phi->addIncoming(function->getArg(2),other);
  if(mode=="call-inside")ir.CreateCall(helper,{function->getArg(0),guest});
  auto *offset=ir.CreateAdd(ir.getInt64(1024),ir.CreateShl(phi,ir.getInt64(3)));auto *enough=ir.CreateICmpUGT(size,ir.getInt64(7));auto *range=ir.CreateICmpULE(offset,ir.CreateSub(size,ir.getInt64(8)));ir.CreateCondBr(ir.CreateAnd(enough,range),dispatch,miss);
  ir.SetInsertPoint(dispatch);auto *table=ir.CreateGEP(ir.getInt8Ty(),guest,offset);if(mode=="table-before")ir.CreateStore(function->getArg(2),table)->setAlignment(llvm::Align(1));auto *actual=ir.CreateLoad(i64,table,"actual.target");actual->setAlignment(llvm::Align(1));if(mode=="table-after")ir.CreateStore(function->getArg(2),table)->setAlignment(llvm::Align(1));
  auto *sw=ir.CreateSwitch(actual,miss);sw->addCase(ir.getInt64(100),hit);sw->addCase(ir.getInt64(200),hit);sw->addCase(ir.getInt64(300),miss);ir.SetInsertPoint(hit);auto *out=ir.CreatePHI(i64,2);out->addIncoming(actual,dispatch);out->addIncoming(actual,dispatch);ir.CreateStore(out,function->getArg(0))->setAlignment(llvm::Align(1));ir.CreateRet(out);ir.SetInsertPoint(miss);ir.CreateRet(ir.getInt64(-1));
  const char *hex="0123456789abcdef";std::string json="{\"view_symbol\":\"view\",\"memory\":[";unsigned states[]={8,50},targets[]={100,mode=="wrong-hint"?300u:200u};for(unsigned n=0;n<2;++n){if(n)json+=",";std::string offsetText,bytes;llvm::raw_string_ostream os(offsetText);os<<llvm::format_hex(1024+8*states[n],0);for(unsigned b=0;b<8;++b){auto v=(static_cast<std::uint64_t>(targets[n])>>(8*b))&255;bytes+=hex[v>>4];bytes+=hex[v&15];}json+="{\"offset\":\""+offsetText+"\",\"bytes\":\""+bytes+"\"}";}json+="]}";
  module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
 }
 std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
 llvm::BasicBlock *named(llvm::StringRef name){for(auto &b:*function)if(b.getName()==name)return &b;return nullptr;}
 Result run(const std::string &name="guarded-partial-corridors",Options options={}){PassPipeline p;REQUIRE(p.add(name,options));auto report=p.run(lifter);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs.front().result;}
 std::vector<llvm::SwitchInst *> fast(){std::vector<llvm::SwitchInst *> result;for(auto &b:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator());sw && sw->getMetadata(Generated))if(auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());load && load->getParent()==&b)result.push_back(sw);return result;}
};
void check(Fixture &f,const std::set<std::uint64_t> &expected,unsigned count=1){auto list=f.fast();REQUIRE(list.size()==count);for(auto *fast:list){std::set<std::uint64_t> values;for(auto item:fast->cases())values.insert(item.getCaseValue()->getZExtValue());CHECK(values==expected);auto *full=llvm::dyn_cast<llvm::SwitchInst>(fast->getDefaultDest()->getTerminator());REQUIRE(full);CHECK(full->getCondition()==fast->getCondition());CHECK(full->getNumCases()==3);}}
}
TEST_CASE("Partial corridors preserve unknown CALL reload and count only committed hints","[llvm][guarded-partial-corridors]"){
 Fixture f;auto before=f.text();CHECK_FALSE(f.run("guarded-state-corridors").changed);CHECK(f.text()==before);CHECK_FALSE(f.run("guarded-store-corridors").changed);CHECK(f.text()==before);auto r=f.run();REQUIRE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("partial_candidate_edges")==1);CHECK(r.counters.at("partial_attempted_edges")==1);CHECK(r.counters.at("partial_rewritten_edges")==1);CHECK(r.counters.at("partial_unresolved_edges")==0);CHECK(r.counters.at("budget_exhausted")==0);check(f,{200});CHECK(f.named("other")->getTerminator()->getSuccessor(0)==f.named("head"));unsigned calls=0,reloads=0;for(auto &b:*f.function)for(auto &i:b){if(auto *call=llvm::dyn_cast<llvm::CallBase>(&i)){++calls;CHECK(call->getCalledFunction()->getName()=="external_maywrite");CHECK(b.getName()=="entry");}if(i.getName()=="after.call.reload")++reloads;}CHECK(calls==1);CHECK(reloads==1);auto stable=f.text();auto again=f.run();CHECK_FALSE(again.changed);CHECK(again.counters.at("partial_rewritten_edges")==0);CHECK(f.text()==stable);
}
TEST_CASE("Partial corridors keep wrong hints table writes and same loaded fallback","[llvm][guarded-partial-corridors]"){
 for(const char *mode:{"wrong-hint","table-before","table-after"}){Fixture f(mode);REQUIRE(f.run().changed);check(f,std::string(mode)=="wrong-hint"?std::set<std::uint64_t>{300}:std::set<std::uint64_t>{200});if(std::string(mode)=="table-after"){auto *sw=f.fast().front();auto *actual=llvm::cast<llvm::LoadInst>(sw->getCondition());bool seen=false,wrote=false;for(auto &i:*sw->getParent()){seen|=&i==actual;if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i);store && store->getPointerOperand()==actual->getPointerOperand()){CHECK(seen);wrote=true;}}CHECK(wrote);}}
}
TEST_CASE("Partial corridors reject missing arms unknown outer values cycles and calls","[llvm][guarded-partial-corridors]"){
 for(const char *mode:{"none","outside","undef","poison","cycle","call-inside"}){INFO(mode);Fixture f(mode);auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.counters.at("partial_rewritten_edges")==0);CHECK(f.text()==before);}
}
TEST_CASE("Partial corridors retain finite compatibility duplicate slots and bounded continuation","[llvm][guarded-partial-corridors]"){
 Fixture finite("finite"),legacy("finite");auto old=legacy.run("guarded-state-corridors");auto next=finite.run();REQUIRE(old.changed);REQUIRE(next.changed);CHECK(next.counters.at("partial_candidate_edges")==0);CHECK(next.counters.at("partial_rewritten_edges")==0);CHECK(finite.text()==legacy.text());check(finite,{100,200});for(auto &[key,value]:old.counters)CHECK(next.counters.at(key)==value);
 Fixture duplicate("normal",true);auto first=duplicate.run("guarded-partial-corridors",{{"max_rewrites",std::int64_t(1)}});REQUIRE(first.changed);CHECK(first.counters.at("partial_candidate_edges")==2);CHECK(first.counters.at("partial_rewritten_edges")==1);CHECK(first.counters.at("partial_unresolved_edges")==1);REQUIRE(duplicate.run().changed);check(duplicate,{200},2);
 for(const char *key:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture f;auto before=f.text();auto r=f.run("guarded-partial-corridors",{{key,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.counters.at("partial_rewritten_edges")==0);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==before);}
 CHECK(std::find(PassPipeline::defaultLLVM().begin(),PassPipeline::defaultLLVM().end(),"guarded-partial-corridors")==PassPipeline::defaultLLVM().end());
}
