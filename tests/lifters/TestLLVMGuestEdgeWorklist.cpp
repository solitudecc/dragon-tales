#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;
using namespace dragon::passes;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Shape { Direct, Hub, Pointer };
struct View {
  std::vector<llvm::BasicBlock *> sources, originalTargets;
  llvm::BasicBlock *unknown, *join, *loaded;
  llvm::LoadInst *load;
};
void init(LLVMLifter &lifter) {
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  lifter.module().setDataLayout("e-p:64:64");
}
View addFixture(LLVMLifter &lifter, const char *name, Shape shape, unsigned bad = 0) {
  auto &module = lifter.module(); auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *f = llvm::Function::Create(llvm::FunctionType::get(i64, {ptr,ptr,i64,i64,i1}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  for (auto &arg : f->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto block = [&](const char *n) { return llvm::BasicBlock::Create(context, n, f); };
  auto *entry = block("entry"), *unknown = block("unknown");
  auto *join = block("checked.join"), *loaded = block("checked.load");
  auto *done = block("done"), *failure = block("failure");
  Builder b(entry);
  auto *base = b.CreateLoad(ptr, f->getArg(1), "guest.base"); base->setAlignment(llvm::Align(1));
  auto *route = b.CreateSwitch(f->getArg(3), unknown);
  b.SetInsertPoint(join); auto *selected = b.CreatePHI(i64, 4, "selected.frame");
  View result{{},{},unknown,join,loaded,nullptr};
  for (unsigned n = 0; n < 3; ++n) {
    auto *source = block("store.source");
    llvm::Value *frame = nullptr, *address = nullptr;
    if (shape == Shape::Pointer) {
      auto *left = block("anchor.left"), *right = block("anchor.right");
      route->addCase(b.getInt64(n*2),left); route->addCase(b.getInt64(n*2+1),right);
      b.SetInsertPoint(left);
      auto *lf = b.CreateAdd(f->getArg(2),b.getInt64(n*32));
      auto *lp = b.CreateGEP(b.getInt8Ty(),base,b.CreateAdd(lf,b.getInt64(16))); b.CreateBr(source);
      b.SetInsertPoint(right);
      auto *rf = b.CreateAdd(f->getArg(2),b.getInt64(n*32+8));
      auto *rp = b.CreateGEP(b.getInt8Ty(),base,b.CreateAdd(rf,b.getInt64(16))); b.CreateBr(source);
      b.SetInsertPoint(source);
      auto *fp = b.CreatePHI(i64,2,"paired.frame");fp->addIncoming(lf,left);fp->addIncoming(rf,right);
      auto *pp = b.CreatePHI(ptr,2,"paired.pointer");pp->addIncoming(lp,left);pp->addIncoming(rp,right);
      frame=fp;address=pp;
    } else {
      route->addCase(b.getInt64(n*2),source); b.SetInsertPoint(source);
      frame=b.CreateAdd(f->getArg(2),b.getInt64(n*32));
      address=b.CreateGEP(b.getInt8Ty(),base,b.CreateAdd(frame,b.getInt64(16)));
    }
    b.CreateStore(b.getInt64(14+n),address)->setAlignment(llvm::Align(1));
    if (n == 1 && bad == 1)
      b.CreateCall(module.getOrInsertFunction("unknown.writer",llvm::FunctionType::get(b.getVoidTy(),{ptr},false)),{base});
    if (n == 1 && bad == 2)
      b.CreateStore(b.getInt64(99),f->getArg(0))->setAlignment(llvm::Align(1));
    auto *target=join;
    if (shape == Shape::Hub) {
      target=block("hub"); auto *other=block("hub.unknown");
      route->addCase(b.getInt64(n*2+1),other); b.CreateBr(target);
      b.SetInsertPoint(other);b.CreateBr(target);
      b.SetInsertPoint(target);auto *hf=b.CreatePHI(i64,2,"hub.frame");
      hf->addIncoming(frame,source);hf->addIncoming(f->getArg(2),other);frame=hf;b.CreateBr(join);
    } else b.CreateBr(join);
    selected->addIncoming(frame,target==join?source:target);
    result.sources.push_back(source);result.originalTargets.push_back(target);
  }
  b.SetInsertPoint(unknown);b.CreateBr(join);selected->addIncoming(f->getArg(2),unknown);
  b.SetInsertPoint(join);auto *index=b.CreateAdd(selected,b.getInt64(16));
  b.CreateCondBr(f->getArg(4),loaded,failure);
  b.SetInsertPoint(loaded);auto *address=b.CreateGEP(b.getInt8Ty(),base,index);
  result.load=b.CreateLoad(i64,address,"actual.load");result.load->setAlignment(llvm::Align(1));b.CreateBr(done);
  b.SetInsertPoint(done);b.CreateRet(result.load);b.SetInsertPoint(failure);b.CreateRet(b.getInt64(0));
  REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return result;
}
unsigned redirected(const View &view) {
  unsigned count=0;
  for(unsigned n=0;n<view.sources.size();++n)
    count += view.sources[n]->getTerminator()->getSuccessor(0)!=view.originalTargets[n];
  return count;
}
Report run(LLVMLifter &lifter, bool enabled, unsigned budget=65536) {
  PassPipeline pipeline;
  REQUIRE(pipeline.add("guest-memory",{{"run_mem2reg",std::int64_t{0}},
      {"edge_worklist",std::int64_t{enabled}}, {"max_rewrites",std::int64_t{budget}}}));
  const auto result=pipeline.run(lifter);
  REQUIRE(result.ok);REQUIRE(result.runs.size()==1);
  REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));return result;
}
}
TEST_CASE("Guest edge worklist consumes distinct original incoming STORE edges", "[llvm][guest-memory-ssa][guest-edge-worklist]") {
  for(auto shape:{Shape::Direct,Shape::Hub,Shape::Pointer})for(bool enabled:{false,true})
    DYNAMIC_SECTION("shape "<<static_cast<int>(shape)<<" enabled "<<enabled) {
      LLVMLifter lifter{Architecture::X86_64};init(lifter);auto view=addFixture(lifter,"multiple",shape);
      auto report=run(lifter,enabled);CHECK(redirected(view)==(enabled?3:1));
      CHECK(report.runs[0].result.counters.at("loads_forwarded")== (enabled?3:1));
      CHECK(view.unknown->getTerminator()->getSuccessor(0)==view.join);
      CHECK(view.load->getParent()==view.loaded);
    }
}
TEST_CASE("Guest edge worklist keeps unknown calls and aliases conservative", "[llvm][guest-memory-ssa][guest-edge-worklist]") {
  for(unsigned bad:{1u,2u})for(auto shape:{Shape::Direct,Shape::Hub,Shape::Pointer})
    DYNAMIC_SECTION("shape "<<static_cast<int>(shape)<<" bad "<<bad) {
      LLVMLifter lifter{Architecture::X86_64};init(lifter);auto view=addFixture(lifter,"barrier",shape,bad);
      run(lifter,true);CHECK(redirected(view)==2);
      CHECK(view.sources[1]->getTerminator()->getSuccessor(0)==view.originalTargets[1]);
      CHECK(view.unknown->getTerminator()->getSuccessor(0)==view.join);
      CHECK(view.load->getParent()==view.loaded);
    }
}
TEST_CASE("Guest edge worklist preserves first sweep and rotates across functions", "[llvm][guest-memory-ssa][guest-edge-worklist]") {
  for(unsigned budget:{2u,3u})DYNAMIC_SECTION("budget "<<budget) {
    LLVMLifter lifter{Architecture::X86_64};init(lifter);
    auto first=addFixture(lifter,"first",Shape::Pointer);
    auto second=addFixture(lifter,"second",Shape::Hub);
    auto report=run(lifter,true,budget);
    CHECK(redirected(first)==(budget==3?2:1));CHECK(redirected(second)==1);
    CHECK(report.runs[0].result.counters.at("loads_forwarded")==budget);
    CHECK(report.runs[0].result.status==Status::Incomplete);
  }
}
TEST_CASE("Guest edge worklist disabled preserves legacy output exactly", "[llvm][guest-memory-ssa][guest-edge-worklist]") {
  LLVMLifter legacy{Architecture::X86_64},explicitOff{Architecture::X86_64};init(legacy);init(explicitOff);
  addFixture(legacy,"same",Shape::Pointer);addFixture(explicitOff,"same",Shape::Pointer);
  MemoryForwardOptions options;options.runMem2Reg=false;const auto old=legacy.runGuestMemorySSA(options);
  const auto report=run(explicitOff,false);CHECK(legacy.ir()==explicitOff.ir());
  CHECK(old.loadsForwarded==report.runs[0].result.counters.at("loads_forwarded"));
}
TEST_CASE("Guest edge worklist validates numeric opt in before mutation", "[llvm][pipeline][guest-edge-worklist]") {
  LLVMLifter lifter{Architecture::X86_64};init(lifter);addFixture(lifter,"unchanged",Shape::Direct);
  const auto before=lifter.ir();
  for(const Parameter &bad:{Parameter{true},Parameter{std::string{"1"}},Parameter{std::int64_t{-1}},Parameter{std::int64_t{2}}}) {
    PassPipeline pipeline;CHECK_FALSE(pipeline.add("guest-memory",{{"edge_worklist",bad}}));CHECK(lifter.ir()==before);
  }
}
