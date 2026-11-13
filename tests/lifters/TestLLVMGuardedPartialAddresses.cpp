#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct Fixture {
 dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};llvm::Module &module=lifter.module();llvm::Function *function;
 Fixture(const std::string &mode="missing-last") {
  module.setDataLayout("e-p:64:64");auto &c=module.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);
  function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,function);};auto *entry=block("entry"),*seed=block("seed"),*other=block("other"),*head=block("head"),*dispatch=block("dispatch"),*exit=block("exit");
  llvm::IRBuilder<llvm::NoFolder> b(entry);auto *guest=b.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));auto *sizep=b.CreateGEP(b.getInt8Ty(),function->getArg(1),b.getInt64(8));auto *size=b.CreateLoad(i64,sizep,"size");size->setAlignment(llvm::Align(1));b.CreateCondBr(function->getArg(3),seed,other);
  entry->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
  b.SetInsertPoint(seed);auto *state=b.CreateSelect(function->getArg(3),b.getInt64(1),b.getInt64(2),"finite.state");b.CreateBr(head);
  b.SetInsertPoint(other);b.CreateBr(head);other->getTerminator()->setMetadata(Generated,llvm::MDNode::get(c,{}));
  b.SetInsertPoint(head);auto *selected=b.CreatePHI(i64,2,"state");selected->addIncoming(state,seed);selected->addIncoming(function->getArg(2),other);
  auto *offset=b.CreateAdd(b.getInt64(1024),b.CreateShl(selected,b.getInt64(3)),"table.index");auto *range=b.CreateAnd(b.CreateICmpUGT(size,b.getInt64(7)),b.CreateICmpULE(offset,b.CreateSub(size,b.getInt64(8))));b.CreateCondBr(range,dispatch,exit);
  b.SetInsertPoint(dispatch);auto *table=b.CreateGEP(b.getInt8Ty(),guest,offset);if(mode=="table-before")b.CreateStore(function->getArg(2),table)->setAlignment(llvm::Align(1));auto *actual=b.CreateLoad(i64,table,"actual.target");actual->setAlignment(llvm::Align(1));if(mode=="table-after")b.CreateStore(function->getArg(2),table)->setAlignment(llvm::Align(1));
  auto *sw=b.CreateSwitch(actual,exit);for(unsigned value:{100,200,300})sw->addCase(b.getInt64(value),exit);
  b.SetInsertPoint(exit);auto *result=b.CreatePHI(i64,5,"result");result->addIncoming(b.getInt64(666),head);for(unsigned n=0;n<4;++n)result->addIncoming(actual,dispatch);b.CreateRet(result);
  struct Row {unsigned state,target,bytes;};std::vector<Row> rows;
  if(mode=="missing-first")rows={{2,200,8}};
  else if(mode=="short")rows={{1,100,8},{2,200,7}};
  else if(mode=="noncase")rows={{1,100,8},{2,999,8}};
  else if(mode=="all-missing")rows={{9,100,8}};
  else if(mode=="all-noncase")rows={{1,999,8},{2,888,8}};
  else if(mode=="malformed")rows={{1,100,8},{1,200,8}};
  else if(mode=="wrong-hint")rows={{1,300,8}};
  else if(mode=="complete")rows={{1,100,8},{2,200,8}};
  else rows={{1,100,8}};
  const char *hex="0123456789abcdef";std::string json="{\"view_symbol\":\"view\",\"memory\":[";bool first=true;
  for(const auto &row:rows){if(!first)json+=",";first=false;std::string offsetText,bytes;llvm::raw_string_ostream os(offsetText);os<<llvm::format_hex(1024+8*row.state,0);for(unsigned n=0;n<row.bytes;++n){auto value=(static_cast<std::uint64_t>(row.target)>>(8*n))&255;bytes+=hex[value>>4];bytes+=hex[value&15];}json+="{\"offset\":\""+offsetText+"\",\"bytes\":\""+bytes+"\"}";}json+="]}";
  module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,json)));verify();
 }
 void verify(){REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));}
 std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
 Result run(const char *name="guarded-partial-corridors",Options options={}){PassPipeline p;REQUIRE(p.add(name,options));auto report=p.run(lifter);REQUIRE(report.runs.size()==1);verify();return report.runs.front().result;}
 llvm::SwitchInst *fast(){llvm::SwitchInst *found=nullptr;for(auto &b:*function)if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator());sw && sw->getMetadata(Generated))if(auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());load && load->getParent()==&b){REQUIRE(found==nullptr);found=sw;}REQUIRE(found);return found;}
 void check(const std::set<std::uint64_t> &expected){auto *sw=fast();std::set<std::uint64_t> actual;for(auto item:sw->cases())actual.insert(item.getCaseValue()->getZExtValue());CHECK(actual==expected);auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(sw->getDefaultDest()->getTerminator());REQUIRE(fallback);CHECK(fallback->getNumCases()==3);CHECK(fallback->getCondition()==sw->getCondition());CHECK(fallback->getDefaultDest()->getName()=="exit");}
};
}
TEST_CASE("Partial addresses retain only existing mapped target subset","[llvm][guarded-partial-addresses]") {
 for(const char *mode:{"missing-last","missing-first","short","noncase"}){INFO(mode);Fixture f(mode);auto *function=f.function;auto *arg=function->getArg(0);auto r=f.run();REQUIRE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("partial_candidate_edges")==1);CHECK(r.counters.at("partial_rewritten_edges")==1);CHECK(r.counters.at("partial_unresolved_edges")==0);CHECK(r.counters.at("budget_exhausted")==0);CHECK(function==f.module.getFunction("view"));CHECK(function->getArg(0)==arg);f.check(std::string(mode)=="missing-first"?std::set<std::uint64_t>{200}:std::set<std::uint64_t>{100});auto stable=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==stable);}
}
TEST_CASE("Partial addresses never turn hints or table writes into facts","[llvm][guarded-partial-addresses]") {
 for(const char *mode:{"wrong-hint","table-before","table-after"}){INFO(mode);Fixture f(mode);REQUIRE(f.run().changed);f.check(std::string(mode)=="wrong-hint"?std::set<std::uint64_t>{300}:std::set<std::uint64_t>{100});if(std::string(mode)!="wrong-hint"){auto *sw=f.fast();auto *load=llvm::cast<llvm::LoadInst>(sw->getCondition());bool seen=false,wrote=false;for(auto &i:*sw->getParent()){seen|=&i==load;if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)){CHECK(store->getPointerOperand()==load->getPointerOperand());CHECK(seen==(std::string(mode)=="table-after"));wrote=true;}}CHECK(wrote);}}
}
TEST_CASE("Partial addresses reject empty invalid and budgeted mappings","[llvm][guarded-partial-addresses]") {
 for(const char *mode:{"all-missing","all-noncase","malformed"}){Fixture f(mode);auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(r.counters.at("partial_rewritten_edges")==0);CHECK(f.text()==before);if(std::string(mode)=="malformed"){CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("rejected")>0);}}
 for(const char *option:{"max_candidates","max_rewrites","max_blocks","max_instructions"}){Fixture f;auto before=f.text();auto r=f.run("guarded-partial-corridors",{{option,std::int64_t(0)}});CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(r.counters.at("partial_rewritten_edges")==0);CHECK(f.text()==before);}
}
TEST_CASE("Partial address mapping preserves strict controllers and full mapping bytes","[llvm][guarded-partial-addresses]") {
 for(const char *mode:{"missing-last","missing-first","short","noncase","wrong-hint"}){Fixture f(mode);auto before=f.text();auto a=f.run("guarded-state-corridors");auto b=f.run("guarded-store-corridors");CHECK_FALSE(a.changed);CHECK_FALSE(b.changed);CHECK(a.counters==b.counters);CHECK(f.text()==before);}
 Fixture finite("complete"),store("complete"),partial("complete");auto a=finite.run("guarded-state-corridors"),b=store.run("guarded-store-corridors"),c=partial.run();REQUIRE(a.changed);REQUIRE(b.changed);REQUIRE(c.changed);CHECK(finite.text()==store.text());CHECK(finite.text()==partial.text());CHECK(a.counters==b.counters);for(const auto &[key,value]:a.counters)CHECK(c.counters.at(key)==value);CHECK(c.counters.at("partial_candidate_edges")==0);CHECK(c.counters.at("partial_rewritten_edges")==0);CHECK(c.status==a.status);partial.check({100,200});
}
