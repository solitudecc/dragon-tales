#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/lifters/llvm/CallEffectSummary.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <algorithm>
#include <limits>
#include <string>

using namespace dragon;
using namespace dragon::llvmlifter;
using namespace dragon::passes;
namespace {
using Builder=llvm::IRBuilder<llvm::NoFolder>;
void init(LLVMLifter &l,bool legacy=false) {
  if(!legacy)REQUIRE(l.setExecutionMode(ExecutionMode::StaticAnalysis));
  l.module().setDataLayout("e-p:64:64");
}
void local(LLVMLifter &l,unsigned loads=1,bool call=false,bool stack=false) {
  auto &c=l.module().getContext();auto *ptr=llvm::PointerType::getUnqual(c);
  auto *i64=llvm::Type::getInt64Ty(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr},false),llvm::GlobalValue::ExternalLinkage,"local",l.module());
  Builder b(llvm::BasicBlock::Create(c,"entry",f));
  auto *base=b.CreateLoad(ptr,f->getArg(1));base->setAlignment(llvm::Align(1));
  llvm::Value *result=b.getInt64(0);
  if(stack) {
    auto *slot=b.CreateAlloca(i64);b.CreateStore(b.getInt64(42),slot);result=b.CreateLoad(i64,slot);
  }
  for(unsigned n=0;n<loads;++n) {
    auto *address=b.CreateGEP(b.getInt8Ty(),base,b.getInt64(n*8));
    b.CreateStore(b.getInt64(17+n),address)->setAlignment(llvm::Align(1));
    if(call)b.CreateCall(l.module().getOrInsertFunction("unknown",llvm::FunctionType::get(b.getVoidTy(),{},false)));
    auto *load=b.CreateLoad(i64,address);load->setAlignment(llvm::Align(1));
    result=b.CreateAdd(result,load);
  }
  b.CreateRet(result);
}
void joined(LLVMLifter &l) {
  auto &c=l.module().getContext();auto *ptr=llvm::PointerType::getUnqual(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt64Ty(c),{ptr,ptr,llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"joined",l.module());
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,f);};
  auto *entry=block("entry"),*left=block("left"),*right=block("right"),*join=block("join");
  Builder b(entry);auto *base=b.CreateLoad(ptr,f->getArg(1));base->setAlignment(llvm::Align(1));
  auto *address=b.CreateGEP(b.getInt8Ty(),base,b.getInt64(0));b.CreateCondBr(f->getArg(2),left,right);
  b.SetInsertPoint(left);b.CreateStore(b.getInt64(17),address)->setAlignment(llvm::Align(1));b.CreateBr(join);
  b.SetInsertPoint(right);b.CreateStore(b.getInt64(23),address)->setAlignment(llvm::Align(1));b.CreateBr(join);
  b.SetInsertPoint(join);auto *load=b.CreateLoad(b.getInt64Ty(),address);load->setAlignment(llvm::Align(1));b.CreateRet(load);
}
unsigned loads(LLVMLifter &l) {
  unsigned n=0;for(const auto &f:l.module())for(const auto &b:f)for(const auto &i:b)
    if(const auto *v=llvm::dyn_cast<llvm::LoadInst>(&i))n+=v->getType()->isIntegerTy();
  return n;
}
Report run(LLVMLifter &l,const Options &options={},RunOptions limits={}) {
  PassPipeline p;REQUIRE(p.add("guest-memory",options));auto r=p.run(l,limits);
  REQUIRE(r.ok);REQUIRE_FALSE(llvm::verifyModule(l.module(),&llvm::errs()));return r;
}
Counters counters(const MemoryForwardStats &s) {
  return {{"available",s.available},{"complete",s.complete},{"changed",s.changed},
    {"blocks_visited",s.blocksVisited},{"guest_loads",s.guestLoads},{"guest_stores",s.guestStores},
    {"loads_forwarded",s.loadsForwarded},{"loads_reused",s.loadsReused},{"call_barriers",s.callBarriers},
    {"alias_barriers",s.aliasBarriers},{"partial_overlaps",s.partialOverlaps},{"phis_inserted",s.phisInserted},
    {"join_barriers",s.joinBarriers},{"loop_barriers",s.loopBarriers},{"budget_exhausted",s.budgetExhausted},
    {"byte_rebuilds",s.byteRebuilds},{"leaf_calls_preserved",s.leafCallsPreserved},
    {"summary_calls_preserved",s.summaryCallsPreserved},{"summary_calls_rejected",s.summaryCallsRejected}};
}
}
TEST_CASE("Guest memory builtin matches direct API defaults and full statistics","[llvm][pipeline][guest-memory-pipeline]") {
  LLVMLifter manual{Architecture::X86_64},managed{Architecture::X86_64};init(manual);init(managed);local(manual);local(managed);
  const auto s=manual.runGuestMemorySSA();auto r=run(managed);REQUIRE(r.runs.size()==1);
  CHECK(s.loadsForwarded==1);CHECK(loads(managed)==0);CHECK(r.runs[0].result.counters==counters(s));CHECK(manual.ir()==managed.ir());
  CHECK(r.runs[0].result.status==Status::Success);
}
TEST_CASE("Guest memory builtin preserves default sweep and custom SSA name","[llvm][pipeline][guest-memory-pipeline]") {
  const std::vector<std::string> expected{"mem2reg","instcombine","gvn","mba","predicates","dce"};
  CHECK(PassPipeline::defaultLLVM()==expected);
  PassPipeline p;bool called=false;REQUIRE(p.registerLLVM("guest-memory-ssa",[&](auto &,const auto &){called=true;return Result{};}));
  REQUIRE(p.add("guest-memory"));REQUIRE(p.add("guest-memory-ssa"));LLVMLifter l{Architecture::X86_64};init(l);local(l);
  REQUIRE(p.run(l).ok);CHECK(called);CHECK(loads(l)==0);
}
TEST_CASE("Guest memory builtin reports unavailable for legacy and empty modules","[llvm][pipeline][guest-memory-pipeline]") {
  for(bool empty:{false,true})DYNAMIC_SECTION("empty "<<empty) {
    LLVMLifter l{Architecture::X86_64};init(l,!empty);if(!empty)local(l);const auto before=l.ir();auto r=run(l);
    REQUIRE(r.runs.size()==1);CHECK(r.runs[0].result.status==Status::Unavailable);CHECK_FALSE(r.runs[0].result.message.empty());
    CHECK(r.runs[0].result.counters.at("available")==0);CHECK_FALSE(r.changed);CHECK(l.ir()==before);
  }
}
TEST_CASE("Guest memory builtin validates exact numeric gates before mutation","[llvm][pipeline][guest-memory-pipeline]") {
  LLVMLifter l{Architecture::X86_64};init(l);local(l);const auto before=l.ir();
  for(const auto *name:{"run_mem2reg","enable_cross_block","enable_phi","consume_call_effect_summaries"}) {
    for(const Parameter &bad:{Parameter{true},Parameter{std::int64_t{-1}},Parameter{std::int64_t{2}},Parameter{std::string{"1"}}}) {
      PassPipeline p;CHECK_FALSE(p.add("guest-memory",{{name,bad}}));CHECK(l.ir()==before);
    }
    for(auto good:{0,1}){PassPipeline p;CHECK(p.add("guest-memory",{{name,std::int64_t{good}}}));}
  }
  for(const auto *name:{"max_rewrites"}) {
    for(const Parameter &bad:{Parameter{true},Parameter{std::int64_t{-1}},Parameter{std::int64_t{4294967296LL}},Parameter{std::string{"1"}}}) {
      PassPipeline p;CHECK_FALSE(p.add("guest-memory",{{name,bad}}));CHECK(l.ir()==before);
    }
    PassPipeline p;CHECK(p.add("guest-memory",{{name,std::int64_t{0}}}));
    CHECK(p.add("guest-memory",{{name,std::int64_t{4294967295LL}}}));
  }
  for(const auto *name:{"prove_leaf_calls","max_call_blocks","max_call_operations","max_call_facts"}) {
    PassPipeline p;CHECK_FALSE(p.add("guest-memory",{{name,std::int64_t{1}}}));CHECK(l.ir()==before);
  }
  PassPipeline p;CHECK_FALSE(p.add("guest-memory",{{"misspelled",std::int64_t{1}}}));CHECK(l.ir()==before);
}
TEST_CASE("Guest memory builtin retains unknown calls with explicit call options","[llvm][pipeline][guest-memory-pipeline]") {
  LLVMLifter manual{Architecture::X86_64},managed{Architecture::X86_64};init(manual);init(managed);local(manual,1,true);local(managed,1,true);
  MemoryForwardOptions o;o.runMem2Reg=false;o.consumeCallEffectSummaries=true;
  const auto s=manual.runGuestMemorySSA(o);
  auto r=run(managed,{{"run_mem2reg",std::int64_t{0}},{"consume_call_effect_summaries",std::int64_t{1}}});
  CHECK(loads(managed)==1);CHECK(s.callBarriers>0);CHECK(s.loadsForwarded==0);
  CHECK(r.runs[0].result.counters==counters(s));CHECK(manual.ir()==managed.ir());
}
TEST_CASE("Guest memory builtin honors cross block and PHI gates","[llvm][pipeline][guest-memory-pipeline]") {
  for(bool cross:{false,true})for(bool phi:{false,true})DYNAMIC_SECTION("cross "<<cross<<" phi "<<phi) {
    LLVMLifter l{Architecture::X86_64};init(l);joined(l);
    auto r=run(l,{{"run_mem2reg",std::int64_t{0}},{"enable_cross_block",std::int64_t{cross}},{"enable_phi",std::int64_t{phi}}});
    CHECK(loads(l)==((cross&&phi)?0:1));CHECK(r.runs[0].result.counters.at("loads_forwarded")==((cross&&phi)?1:0));
  }
}
TEST_CASE("Guest memory builtin detects promotion without a memory rewrite","[llvm][pipeline][guest-memory-pipeline]") {
  for(bool promote:{false,true})DYNAMIC_SECTION("promote "<<promote) {
    LLVMLifter l{Architecture::X86_64};init(l);local(l,0,false,true);
    auto r=run(l,{{"run_mem2reg",std::int64_t{promote}}});CHECK(r.changed==promote);
    CHECK(loads(l)==(promote?0:1));CHECK(r.runs[0].result.counters.at("loads_forwarded")==0);
  }
}
TEST_CASE("Guest memory builtin bounded iterations make actual progress and retain incomplete records","[llvm][pipeline][guest-memory-pipeline]") {
  LLVMLifter l{Architecture::X86_64};init(l);local(l,2);
  auto r=run(l,{{"max_rewrites",std::int64_t{1}},{"run_mem2reg",std::int64_t{0}}},{3,3,true});
  REQUIRE(r.runs.size()==3);CHECK(r.converged);CHECK_FALSE(r.budgetExhausted);CHECK(loads(l)==0);
  CHECK(r.runs[0].result.status==Status::Incomplete);CHECK(r.runs[1].result.status==Status::Incomplete);
  CHECK(r.runs[0].result.counters.at("loads_forwarded")==1);CHECK(r.runs[1].result.counters.at("loads_forwarded")==1);
  CHECK_FALSE(r.runs[2].result.changed);CHECK(r.runs[2].result.counters.at("loads_forwarded")==0);
}
TEST_CASE("Guest memory builtin zero rewrite and pass budgets preserve honest partial progress","[llvm][pipeline][guest-memory-pipeline]") {
  LLVMLifter zero{Architecture::X86_64};init(zero);local(zero,2);auto before=zero.ir();
  auto r=run(zero,{{"max_rewrites",std::int64_t{0}},{"run_mem2reg",std::int64_t{0}}});CHECK_FALSE(r.changed);CHECK(zero.ir()==before);
  LLVMLifter partial{Architecture::X86_64};init(partial);local(partial,2);
  r=run(partial,{{"max_rewrites",std::int64_t{1}},{"run_mem2reg",std::int64_t{0}}},{3,1,true});
  CHECK(r.budgetExhausted);CHECK_FALSE(r.converged);REQUIRE(r.runs.size()==1);CHECK(loads(partial)==1);
  CHECK(r.runs[0].result.status==Status::Incomplete);CHECK(r.changed);
}

TEST_CASE("Guest memory builtin consumes only explicitly enabled proved call summaries","[llvm][pipeline][guest-memory-pipeline]") {
  for(bool consume:{false,true})DYNAMIC_SECTION("consume "<<consume) {
    LLVMLifter l{Architecture::X86_64};init(l);local(l,1,true);
    unsigned attached=0;
    for(auto &f:l.module())for(auto &b:f)for(auto &i:b)if(auto *call=llvm::dyn_cast<llvm::CallBase>(&i)) {
      CallEffectSummary summary;summary.proven=true;summary.memory=CallMemoryEffectKind::NoMemory;
      REQUIRE(attachCallEffectSummary(*call,summary));++attached;
    }
    REQUIRE(attached==1);
    auto r=run(l,{{"run_mem2reg",std::int64_t{0}},{"consume_call_effect_summaries",std::int64_t{consume}}});
    CHECK(loads(l)==(consume?0:1));CHECK(r.runs[0].result.counters.at("loads_forwarded")==(consume?1:0));
    CHECK((r.runs[0].result.counters.at("summary_calls_preserved")>0)==consume);
  }
}
