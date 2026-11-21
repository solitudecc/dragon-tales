#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>

using namespace dragon::llvmlifter::detail;
namespace {
// New named API: an older SDK lacks its symbol. Do not label that link failure
// an old execution error. The legacy API precision control below uses its real,
// unchanged entry point on exactly this same fixture.
struct LoopFixture {
  enum Mode { Normal, DuplicateInput, PrefixVersusEnd, MissingMask, MissingDefinition,
              PartialIndex, WideMask, ClobberRestore, UnknownCall, OtherBase,
              BadSP, BadInitialState, BadIndex, GrayDefinition, CallerTag,
              TwoCandidates, BadBackedgeRange, CallerGlobalTag, FailedFirstCandidate,
              NoFrame, NoFrameUnguarded, NoFrameBadSP, NoFrameReloadedSP };
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit LoopFixture(Mode mode=Normal) {
    std::string text=R"IR(
target datalayout = "e-p:64:64"
declare void @ordinary(ptr, ptr, ptr, ptr)
declare void @unknown_call(ptr)
define void @view(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {
entry:
  %slot = alloca i64
  %pcslot = alloca i64
  %regdesc = getelementptr i8, ptr %state, i64 24
  %regs = load ptr, ptr %regdesc, align 1
  %guest = load ptr, ptr %memory, align 1
  %spaddr = getelementptr i8, ptr %regs, i64 32
  %initialsp = load i64, ptr %spaddr, align 1
  store i64 %initialsp, ptr %slot
  %outaddr = getelementptr i8, ptr %regs, i64 48
  %snapshot = load i64, ptr %outaddr, align 1
  %pcaddr = getelementptr i8, ptr %regs, i64 128
  %initialpc = load i64, ptr %pcaddr, align 1
  store i64 %initialpc, ptr %pcslot
  %pc = load i64, ptr %pcslot
  switch i64 %pc, label %unsupported [ i64 4096, label %bounds ]
bounds:
  %sizeaddr = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizeaddr, align 1
  %large = icmp uge i64 %size, 65536
  %nonnull = icmp ne ptr %guest, null
  %valid = and i1 %large, %nonnull
  br i1 %valid, label %spcheck, label %fallback
spcheck:
  %spok = icmp eq i64 %initialsp, 49152
  br i1 %spok, label %outcheck, label %fallback
outcheck:
  %outok = icmp eq i64 %snapshot, 32768
  br i1 %outok, label %table0, label %fallback
table0:
  %t0p = getelementptr i8, ptr %guest, i64 8192
  %t0 = load i64, ptr %t0p, align 1
  %t0ok = icmp eq i64 %t0, 4352
  br i1 %t0ok, label %table1, label %fallback
table1:
  %t1p = getelementptr i8, ptr %guest, i64 8200
  %t1 = load i64, ptr %t1p, align 1
  %t1ok = icmp eq i64 %t1, 4608
  br i1 %t1ok, label %accepted, label %fallback
accepted:
  br label %initialize
initialize:
  %sp0 = load i64, ptr %slot
  %frame = sub i64 %sp0, 64
  store i64 %frame, ptr %slot
  %cell = getelementptr i8, ptr %guest, i64 4096
  store i64 %snapshot, ptr %cell, align 1
  ; FIRST_CANDIDATE
  %indexcell = getelementptr i8, ptr %guest, i64 2048
  ; INITIAL_INDEX
  br label %head
head:
  %statevalue = phi i64 [ INITIAL_STATE, %initialize ], [ 1, %a.end ], [ B_STATE, %b.end ], [ %state.step, %head ]
  %state.step = add i64 %statevalue, 1
  %inrange = icmp ult i64 %statevalue, 2
  br i1 %inrange, label %read, label %head
read:
  %scale = shl i64 %statevalue, 3
  %tableoff = add i64 %scale, 8192
  %targetp = getelementptr i8, ptr %guest, i64 %tableoff
  %target = load i64, ptr %targetp, align 1
  switch i64 %target, label %unknown [ i64 4352, label %a i64 4608, label %b ]
a:
  %tracep = getelementptr i8, ptr %guest, i64 1536
  store i64 %statevalue, ptr %tracep, align 1
  ; FIRST_READ
  %datap = getelementptr i8, ptr %guest, i64 128
  %data = load i64, ptr %datap, align 1
  %bounded = and i64 %data, INDEX_MASK
  ; MASK_STORE
  ; EXTRA_EFFECT
  %index.a = load i64, ptr %indexcell, align 1
  %scaled.a = shl i64 %index.a, 2
  %offset.a = add i64 %scaled.a, 4008
  %work.a = getelementptr i8, ptr %guest, i64 %offset.a
  store i32 8192, ptr %work.a, align 1
  %pointer = load i64, ptr %cell, align 1
  %output = getelementptr i8, ptr %guest, i64 %pointer
  store i32 123, ptr %output, align 1
  ; RESTORE_EFFECT
  br label %a.end
a.end:
  br label %head
b:
  %index.b = load i64, ptr %indexcell, align 1
  %scaled.b = shl i64 %index.b, 2
  %offset.b = add i64 %scaled.b, 4008
  %work.b = getelementptr i8, ptr %guest, i64 %offset.b
  store i32 8192, ptr %work.b, align 1
  %pointer.b = load i64, ptr %cell, align 1
  %output.b = getelementptr i8, ptr %guest, i64 %pointer.b
  store i32 321, ptr %output.b, align 1
  ; AFTER_B_LOAD
  br label %b.end
b.end:
  br label %head
unknown:
  store i64 %target, ptr %exit, align 1
  ret void
unsupported:
  ret void
fallback:
  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)
  ret void
}
)IR";
    auto replace=[&](const std::string &from,const std::string &to) {
      std::size_t at=0;
      while ((at=text.find(from,at))!=std::string::npos) { text.replace(at,from.size(),to);at+=to.size(); }
    };
    replace("INITIAL_STATE",mode==BadIndex ? "2" : mode==BadInitialState ? "1" : "0");
    replace("B_STATE",mode==PrefixVersusEnd || mode==GrayDefinition || mode==BadBackedgeRange ? "1" : "0");
    replace("; FIRST_CANDIDATE",(mode==TwoCandidates || mode==FailedFirstCandidate) ?
            "%firstp = getelementptr i8, ptr %guest, i64 1024\n  store i64 500, ptr %firstp, align 1" : "");
    replace("; FIRST_READ",(mode==TwoCandidates || mode==FailedFirstCandidate) ?
            "%first.read = load i64, ptr %firstp, align 1\n  %first.mask = and i64 %first.read, 1\n  %first.trace = getelementptr i8, ptr %guest, i64 1504\n  store i64 %first.mask, ptr %first.trace, align 1" : "");
    replace("INDEX_MASK",mode==WideMask ? "31" : "7");
    replace("; INITIAL_INDEX",mode==MissingDefinition ? "" : "store i64 22, ptr %indexcell, align 1");
    replace("; MASK_STORE",mode==MissingMask || mode==MissingDefinition ? "" :
            mode==PartialIndex ? "%partial = trunc i64 %bounded to i32\n  store i32 %partial, ptr %indexcell, align 1" :
                                 "store i64 %bounded, ptr %indexcell, align 1");
    replace("; EXTRA_EFFECT",mode==ClobberRestore ? "store i64 8192, ptr %cell, align 1" :
            mode==UnknownCall ? "call void @unknown_call(ptr %guest)" :
            mode==OtherBase ? "store i64 0, ptr %state, align 1" :
            mode==FailedFirstCandidate ?
            "%clobber.low = and i64 %bounded, 1\n  %clobber.delta = shl i64 %clobber.low, 9\n"
            "  %clobber.offset = add i64 %clobber.delta, 1024\n"
            "  %clobber.pointer = getelementptr i8, ptr %guest, i64 %clobber.offset\n"
            "  store i64 501, ptr %clobber.pointer, align 1" : "");
    replace("; RESTORE_EFFECT",mode==ClobberRestore ? "store i64 32768, ptr %cell, align 1" : "");
    replace("; AFTER_B_LOAD",mode==BadBackedgeRange ? "store i64 22, ptr %indexcell, align 1" : mode==PrefixVersusEnd ?
            "%next.index = add i64 %index.b, 1\n  %new.index = and i64 %next.index, 7\n  store i64 %new.index, ptr %indexcell, align 1" : "");
    if (mode==DuplicateInput) {
      replace("[ 1, %a.end ]","[ 1, %a.end ], [ 1, %a.end ]");
      replace("a.end:\n  br label %head","a.end:\n  br i1 %inrange, label %head, label %head");
    }
    if (mode==NoFrame || mode==NoFrameUnguarded || mode==NoFrameBadSP || mode==NoFrameReloadedSP)
      replace("%frame = sub i64 %sp0, 64", "%frame = add i64 %sp0, 0");
    if (mode==NoFrameUnguarded)
      replace("br i1 %spok, label %outcheck, label %fallback",
              "br i1 true, label %outcheck, label %fallback");
    if (mode==NoFrameReloadedSP)
      replace("  store i64 22, ptr %indexcell, align 1",
              "  store i64 22, ptr %indexcell, align 1\n"
              "  %freshsp = load i64, ptr %spaddr, align 1\n"
              "  store i64 %freshsp, ptr %slot, align 1");
    if (mode==BadSP || mode==NoFrameBadSP)
      replace("b.end:\n  br label %head","b.end:\n  %oldsp = load i64, ptr %slot\n  %newsp = add i64 %oldsp, 1\n  store i64 %newsp, ptr %slot\n  br label %head");
    // GrayDefinition enters B after the real A mask and repeatedly returns
    // to B. The exact-observation proof must cover initialization and prove
    // every historical work STORE disjoint under that LOAD interval.
    llvm::SMDiagnostic error;
    module=llvm::parseAssemblyString(text,error,context);
    if (!module) { error.print("loop-image-test",llvm::errs()); }
    REQUIRE(module);
    REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
    if (mode==CallerTag) {
      for (auto &bb:*module->getFunction("view")) for (auto &i:bb)
        if (i.getName()=="target") i.setMetadata("dragon.loop.image.private",llvm::MDNode::get(context,{}));
    }
    if (mode==CallerGlobalTag) {
      auto *global=new llvm::GlobalVariable(*module,llvm::Type::getInt64Ty(context),false,
          llvm::GlobalValue::InternalLinkage,
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),0),"caller.global");
      global->setMetadata("dragon.loop.image.private",llvm::MDNode::get(context,{}));
    }
  }
  ImageTableContextRequest request() const { return {"view",8192,{4352,4608}}; }
  std::string ir() const { std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s; }
};
}

TEST_CASE("Loop image proof threads every input and preserves all lookup reads",
          "[llvm][image-loop-context]") {
  for (auto mode:{LoopFixture::Normal,LoopFixture::DuplicateInput,LoopFixture::PrefixVersusEnd,
                  LoopFixture::GrayDefinition})
    DYNAMIC_SECTION("mode "<<mode) {
      LoopFixture f(mode);auto *view=f.module->getFunction("view");auto *arg=view->getArg(0);
      auto *ordinary=f.module->getFunction("ordinary");
      const auto result=runLoopImageTableContext(*f.module,f.request());
      INFO(result.message);REQUIRE(result.closed);REQUIRE(result.changed);CHECK_FALSE(result.budgetExhausted);
      CHECK(result.work<=f.request().maxWork);CHECK(f.module->getFunction("view")==view);
      CHECK(view->getArg(0)==arg);CHECK(f.module->getFunction("ordinary")==ordinary);
      unsigned reads=0;bool seededRead=false;
      for (auto &bb:*view) for (auto &i:bb) {
        if (llvm::isa<llvm::LoadInst>(i) && i.getName().starts_with("target")) ++reads;
        if (llvm::isa<llvm::LoadInst>(i) && i.getName()=="pointer") seededRead=true;
      }
      CHECK(reads>=3);CHECK(seededRead);
      CHECK(f.ir().find("dragon.loop.image.private")==std::string::npos);
      CHECK(f.ir().find("call void @ordinary")!=std::string::npos);
      REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
    }
}

TEST_CASE("Loop image local ranges never bypass missing definitions or memory effects",
          "[llvm][image-loop-context]") {
  for (auto mode:{LoopFixture::MissingMask,LoopFixture::MissingDefinition,LoopFixture::PartialIndex,
                  LoopFixture::WideMask,LoopFixture::ClobberRestore,LoopFixture::UnknownCall,
                  LoopFixture::OtherBase,LoopFixture::BadSP,LoopFixture::BadInitialState,LoopFixture::BadIndex,
                  LoopFixture::CallerTag,LoopFixture::BadBackedgeRange,
                  LoopFixture::CallerGlobalTag})
    DYNAMIC_SECTION("mode "<<mode) {
      LoopFixture f(mode);auto before=f.ir();auto *view=f.module->getFunction("view");
      auto *entry=&view->getEntryBlock();auto *arg=view->getArg(0);
      auto result=runLoopImageTableContext(*f.module,f.request());
      INFO(result.message);CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);
      CHECK(f.ir()==before);CHECK(f.module->getFunction("view")==view);
      CHECK(&view->getEntryBlock()==entry);CHECK(view->getArg(0)==arg);
      REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
    }
}

TEST_CASE("Loop image tiny budgets refuse without publishing a private batch",
          "[llvm][image-loop-context]") {
  for (std::size_t limit:{std::size_t(1),std::size_t(32),std::size_t(256)}) {
    LoopFixture f;auto before=f.ir();auto request=f.request();request.maxWork=limit;
    auto result=runLoopImageTableContext(*f.module,request);
    CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(result.budgetExhausted);
    CHECK(result.work<=limit);CHECK(f.ir()==before);
  }
}

TEST_CASE("Legacy image entry retains its original local range refusal",
          "[llvm][image-loop-context]") {
  LoopFixture f;const auto before=f.ir();
  auto result=runClosedImageTableContext(*f.module,f.request());
  CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(f.ir()==before);
}

TEST_CASE("Loop image consumes the first fully closed cell candidate",
          "[llvm][image-loop-context]") {
  LoopFixture f(LoopFixture::TwoCandidates);
  auto *note=f.module->getOrInsertNamedMetadata("caller.note");
  note->addOperand(llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"preserved")}));
  auto result=runLoopImageTableContext(*f.module,f.request());
  INFO(result.message);REQUIRE(result.closed);REQUIRE(result.changed);
  CHECK(result.constantValues>=1);
  llvm::LoadInst *first=nullptr;
  for (auto &bb:*f.module->getFunction("view")) for (auto &i:bb)
    if (i.getName()=="first.read") first=llvm::dyn_cast<llvm::LoadInst>(&i);
  REQUIRE(first);CHECK(first->use_empty());
  CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);
  CHECK(f.ir().find("dragon.loop.image.private")==std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
}

TEST_CASE("Loop image refuses a truly clobbered candidate without leaking constants",
          "[llvm][image-loop-context]") {
  LoopFixture f(LoopFixture::FailedFirstCandidate);
  auto *note=f.module->getOrInsertNamedMetadata("caller.note");
  note->addOperand(llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"preserved")}));
  auto result=runLoopImageTableContext(*f.module,f.request());
  INFO(result.message);REQUIRE(result.closed);REQUIRE(result.changed);
  CHECK(result.constantValues>=1);
  llvm::LoadInst *first=nullptr;
  for (auto &bb:*f.module->getFunction("view")) for (auto &i:bb)
    if (i.getName()=="first.read") first=llvm::dyn_cast<llvm::LoadInst>(&i);
  REQUIRE(first);CHECK_FALSE(first->use_empty());
  CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);
  CHECK(f.ir().find("dragon.loop.image.private")==std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
}

namespace {
std::unique_ptr<dragon::llvmlifter::LLVMLifter> pipelineFixture(const LoopFixture &fixture) {
  using namespace dragon::llvmlifter;
  auto lifter=std::make_unique<LLVMLifter>(dragon::Architecture::X86_64,"loop-pipeline-test");
  REQUIRE(lifter->setExecutionMode(ExecutionMode::StaticAnalysis));
  const auto text=fixture.ir();llvm::SMDiagnostic error;
  const bool failed=llvm::parseAssemblyInto(llvm::MemoryBufferRef(text,"loop-pipeline-fixture"),
      &lifter->module(),nullptr,error);
  if(failed)error.print("loop-pipeline-test",llvm::errs());
  REQUIRE_FALSE(failed);REQUIRE_FALSE(llvm::verifyModule(lifter->module(),&llvm::errs()));
  return lifter;
}
dragon::passes::Options pipelineRequest() {
  return {{"view_symbol",std::string("view")},{"table_offset",std::string("0x2000")},
          {"targets",std::string("0x1100, 4608")}};
}
}

TEST_CASE("Loop image pipeline publishes exactly the direct public proof",
          "[llvm][image-loop-pipeline]") {
  using namespace dragon::passes;
  for(auto mode:{LoopFixture::Normal,LoopFixture::DuplicateInput,LoopFixture::PrefixVersusEnd}) {
    LoopFixture fixture(mode);auto direct=pipelineFixture(fixture),managed=pipelineFixture(fixture);
    const auto expected=runLoopImageTableContext(direct->module(),fixture.request());
    REQUIRE(expected.closed);PassPipeline pipeline;
    REQUIRE(pipeline.add("loop-image-context",pipelineRequest()));
    const auto report=pipeline.run(*managed);REQUIRE(report.ok);REQUIRE(report.runs.size()==1);
    const auto &result=report.runs.front().result;
    CHECK(result.status==Status::Success);CHECK(result.changed==expected.changed);
    CHECK(result.message==expected.message);CHECK(managed->ir()==direct->ir());
    const Counters counters{{"available",1},{"complete",1},{"closed",1},
      {"budget_exhausted",expected.budgetExhausted},{"guarded_bytes",expected.guardedBytes},
      {"preserved_stores",expected.preservedStores},{"constant_values",expected.constantValues},
      {"work",expected.work}};
    CHECK(result.counters==counters);
    CHECK_FALSE(llvm::verifyModule(managed->module(),&llvm::errs()));
  }
}

TEST_CASE("Loop image pipeline retains direct refusals and budget reports",
          "[llvm][image-loop-pipeline]") {
  using namespace dragon::passes;
  for(auto mode:{LoopFixture::Normal,LoopFixture::UnknownCall,LoopFixture::MissingDefinition,
                 LoopFixture::ClobberRestore,LoopFixture::CallerTag}) {
    LoopFixture fixture(mode);auto direct=pipelineFixture(fixture),managed=pipelineFixture(fixture);
    auto request=fixture.request();auto options=pipelineRequest();
    if(mode==LoopFixture::Normal) {
      request.maxWork=1;options.emplace("max_work",std::int64_t(1));
    }
    const auto before=managed->ir();const auto expected=runLoopImageTableContext(direct->module(),request);
    CHECK_FALSE(expected.closed);PassPipeline pipeline;
    REQUIRE(pipeline.add("loop-image-context",options));
    const auto report=pipeline.run(*managed);REQUIRE(report.ok);REQUIRE(report.runs.size()==1);
    const auto &result=report.runs.front().result;
    CHECK(result.status==Status::Incomplete);CHECK_FALSE(result.changed);
    CHECK(result.message==expected.message);CHECK(result.counters.at("available")==1);
    CHECK(result.counters.at("closed")==0);CHECK(result.counters.at("complete")==0);
    CHECK(result.counters.at("budget_exhausted")==expected.budgetExhausted);
    CHECK(result.counters.at("work")==expected.work);
    CHECK(managed->ir()==before);CHECK(managed->ir()==direct->ir());
    CHECK_FALSE(llvm::verifyModule(managed->module(),&llvm::errs()));
  }
}

TEST_CASE("Loop image accepts an unchanged exact guarded SP snapshot without a frame",
          "[llvm][image-loop-context][image-loop-no-frame]") {
  LoopFixture fixture(LoopFixture::NoFrame);
  auto *view=fixture.module->getFunction("view");
  auto *ordinary=fixture.module->getFunction("ordinary");
  auto *external=fixture.module->getFunction("unknown_call");
  auto printed=[](llvm::Function &f){std::string s;llvm::raw_string_ostream out(s);f.print(out);return s;};
  const auto ordinaryBefore=printed(*ordinary),externalBefore=printed(*external);
  std::vector<llvm::Argument*>arguments;for(auto &argument:view->args())arguments.push_back(&argument);
  auto result=runLoopImageTableContext(*fixture.module,fixture.request());
  INFO(result.message);REQUIRE(result.closed);REQUIRE(result.changed);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.work<=fixture.request().maxWork);CHECK(fixture.module->getFunction("view")==view);
  for(unsigned n=0;n<arguments.size();++n)CHECK(view->getArg(n)==arguments[n]);
  CHECK(fixture.module->getFunction("ordinary")==ordinary);CHECK(printed(*ordinary)==ordinaryBefore);
  CHECK(fixture.module->getFunction("unknown_call")==external);CHECK(printed(*external)==externalBefore);
  llvm::LoadInst *snapshot=nullptr;llvm::ICmpInst *guard=nullptr;unsigned targets=0;
  for(auto &block:*view)for(auto &instruction:block){
    if(instruction.getName()=="initialsp")snapshot=llvm::dyn_cast<llvm::LoadInst>(&instruction);
    if(instruction.getName()=="spok")guard=llvm::dyn_cast<llvm::ICmpInst>(&instruction);
    targets+=llvm::isa<llvm::LoadInst>(instruction)&&instruction.getName().starts_with("target");
  }
  REQUIRE(snapshot);CHECK(snapshot->isSimple());CHECK(snapshot->getType()->isIntegerTy(64));
  REQUIRE(guard);CHECK(guard->getPredicate()==llvm::ICmpInst::ICMP_EQ);
  const auto other=guard->getOperand(0)==snapshot?1u:0u;
  CHECK(guard->getOperand(1u-other)==snapshot);
  auto *expected=llvm::dyn_cast<llvm::ConstantInt>(guard->getOperand(other));
  REQUIRE(expected);CHECK(expected->getZExtValue()==49152);
  auto *branch=llvm::dyn_cast<llvm::BranchInst>(guard->getParent()->getTerminator());
  REQUIRE(branch);REQUIRE(branch->isConditional());CHECK(branch->getCondition()==guard);
  CHECK(branch->getSuccessor(0)->getName()=="outcheck");CHECK(branch->getSuccessor(1)->getName()=="fallback");
  CHECK(targets>=3);CHECK(fixture.ir().find("call void @ordinary")!=std::string::npos);
  CHECK(fixture.ir().find("dragon.loop.image.private")==std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*fixture.module,&llvm::errs()));
}

TEST_CASE("Loop image never gives the guarded SP fact to missing checks changed PHIs or reloads",
          "[llvm][image-loop-context][image-loop-no-frame]") {
  for(auto mode:{LoopFixture::NoFrameUnguarded,LoopFixture::NoFrameBadSP,LoopFixture::NoFrameReloadedSP})
    DYNAMIC_SECTION("mode "<<mode){
      LoopFixture fixture(mode);const auto before=fixture.ir();auto *view=fixture.module->getFunction("view");
      auto *ordinary=fixture.module->getFunction("ordinary");
      std::vector<llvm::Argument*>arguments;std::vector<llvm::BasicBlock*>blocks;std::vector<llvm::Instruction*>instructions;
      for(auto &argument:view->args())arguments.push_back(&argument);
      for(auto &block:*view){blocks.push_back(&block);for(auto &instruction:block)instructions.push_back(&instruction);}
      const auto result=runLoopImageTableContext(*fixture.module,fixture.request());INFO(result.message);
      CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK_FALSE(result.budgetExhausted);
      CHECK(fixture.ir()==before);CHECK(fixture.module->getFunction("view")==view);
      CHECK(fixture.module->getFunction("ordinary")==ordinary);
      for(unsigned n=0;n<arguments.size();++n)CHECK(view->getArg(n)==arguments[n]);
      std::vector<llvm::BasicBlock*>afterBlocks;std::vector<llvm::Instruction*>afterInstructions;
      for(auto &block:*view){afterBlocks.push_back(&block);for(auto &instruction:block)afterInstructions.push_back(&instruction);}
      CHECK(afterBlocks==blocks);CHECK(afterInstructions==instructions);
      REQUIRE_FALSE(llvm::verifyModule(*fixture.module,&llvm::errs()));
    }
}

TEST_CASE("Loop image proof profiles keep default output and expose expanded through the lifter",
          "[llvm][image-loop-proof-profile]") {
  using namespace dragon::passes;
  using dragon::llvmlifter::detail::LoopImageProofProfile;
  LoopFixture fixture;
  auto direct=pipelineFixture(fixture),bridge=pipelineFixture(fixture),explicitDefault=pipelineFixture(fixture);
  const auto expected=runLoopImageTableContext(direct->module(),fixture.request());REQUIRE(expected.closed);
  const auto bridged=bridge->runLoopImageTableContext(fixture.request());REQUIRE(bridged.closed);
  const auto selected=explicitDefault->runLoopImageTableContext(fixture.request(),LoopImageProofProfile::Default);
  REQUIRE(selected.closed);CHECK(bridged.work==expected.work);CHECK(selected.work==expected.work);
  CHECK(bridge->ir()==direct->ir());CHECK(explicitDefault->ir()==direct->ir());
  for(const auto &profile:{std::string("default"),std::string("expanded")}) {
    auto managed=pipelineFixture(fixture),peer=pipelineFixture(fixture);
    auto request=fixture.request();auto options=pipelineRequest();options["proof_profile"]=profile;
    const auto selectedProfile=profile=="expanded"?LoopImageProofProfile::Expanded:LoopImageProofProfile::Default;
    if(profile=="expanded"){request.maxWork=16000000;options["max_work"]=std::int64_t(16000000);}
    const auto sameProfile=runLoopImageTableContext(peer->module(),request,selectedProfile);REQUIRE(sameProfile.closed);
    PassPipeline pipeline;REQUIRE(pipeline.add("loop-image-context",options));
    const auto report=pipeline.run(*managed);REQUIRE(report.ok);REQUIRE(report.runs.size()==1);
    const auto &result=report.runs.front().result;REQUIRE(result.status==Status::Success);
    CHECK(result.changed==sameProfile.changed);CHECK(result.message==sameProfile.message);
    const Counters sameCounters{{"available",1},{"complete",sameProfile.closed},{"closed",sameProfile.closed},
      {"budget_exhausted",sameProfile.budgetExhausted},{"guarded_bytes",sameProfile.guardedBytes},
      {"preserved_stores",sameProfile.preservedStores},{"constant_values",sameProfile.constantValues},{"work",sameProfile.work}};
    CHECK(result.counters==sameCounters);CHECK(managed->ir()==peer->ir());
    if(profile=="default")CHECK(managed->ir()==direct->ir());
    CHECK_FALSE(llvm::verifyModule(managed->module(),&llvm::errs()));
  }
}

TEST_CASE("Loop image expanded profile retains atomic refusal and hard resource boundaries",
          "[llvm][image-loop-proof-profile]") {
  using namespace dragon::passes;
  using dragon::llvmlifter::detail::LoopImageProofProfile;
  for(unsigned mode=0;mode<5;++mode) {
    INFO(mode);LoopFixture fixture;auto managed=pipelineFixture(fixture);const auto before=managed->ir();
    auto request=fixture.request();request.maxWork=16000000;
    auto *function=managed->module().getFunction("view");auto *argument=function->getArg(0);
    dragon::llvmlifter::detail::ImageTableContextStats result;
    if(mode==0)result=runLoopImageTableContext(managed->module(),request); // old hard1M
    if(mode==1)result=managed->runLoopImageTableContext(request,LoopImageProofProfile::Default);
    if(mode==2)result=managed->runLoopImageTableContext(request,static_cast<LoopImageProofProfile>(99));
    if(mode==3){request.maxWork=16000001;result=managed->runLoopImageTableContext(request,LoopImageProofProfile::Expanded);}
    if(mode==4){request.maxWork=1;result=managed->runLoopImageTableContext(request,LoopImageProofProfile::Expanded);}
    CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(result.budgetExhausted==(mode==4));
    CHECK(result.work<=request.maxWork);CHECK(managed->ir()==before);
    CHECK(managed->module().getFunction("view")==function);CHECK(function->getArg(0)==argument);
    CHECK_FALSE(llvm::verifyModule(managed->module(),&llvm::errs()));
  }
  SECTION("an insufficient expanded pipeline still reports incomplete") {
    LoopFixture fixture;auto managed=pipelineFixture(fixture);const auto before=managed->ir();
    auto options=pipelineRequest();options["proof_profile"]=std::string("expanded");options["max_work"]=std::int64_t(1);
    PassPipeline pipeline;REQUIRE(pipeline.add("loop-image-context",options));const auto report=pipeline.run(*managed);
    REQUIRE(report.ok);REQUIRE(report.runs.size()==1);const auto &result=report.runs.front().result;
    CHECK(result.status==Status::Incomplete);CHECK_FALSE(result.changed);
    CHECK(result.counters.at("complete")==0);CHECK(result.counters.at("budget_exhausted")==1);
    CHECK(managed->ir()==before);
  }
  SECTION("the direct new enum overload rejects unknown values without entering a proof") {
    LoopFixture fixture;const auto before=fixture.ir();
    const auto result=runLoopImageTableContext(*fixture.module,fixture.request(),static_cast<LoopImageProofProfile>(99));
    CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK_FALSE(result.budgetExhausted);
    CHECK(result.work==0);CHECK_FALSE(result.message.empty());CHECK(fixture.ir()==before);
  }
  SECTION("the lifter bridge refuses nonstatic mode") {
    dragon::llvmlifter::LLVMLifter legacy(dragon::Architecture::X86_64);
    const auto before=legacy.ir();LoopFixture fixture;
    const auto result=legacy.runLoopImageTableContext(fixture.request(),LoopImageProofProfile::Expanded);
    CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(result.work==0);CHECK(legacy.ir()==before);
  }
}
