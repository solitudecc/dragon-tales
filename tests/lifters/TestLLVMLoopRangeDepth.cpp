#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace dragon::llvmlifter::detail;
namespace {
// New named API: an older SDK lacks its symbol. Do not label that link failure
// an old execution error. The legacy API precision control below uses its real,
// unchanged entry point on exactly this same fixture.
struct RangeDepthFixture {
  enum Mode { Normal, DuplicateInput, PrefixVersusEnd, MissingMask, MissingDefinition,
              PartialIndex, WideMask, ClobberRestore, UnknownCall, OtherBase,
              BadSP, BadInitialState, BadIndex, GrayDefinition, CallerTag,
              TwoCandidates, BadBackedgeRange, CallerGlobalTag, ReverseConstantChain, ReverseConstantChainBadMagic, FailedFirstCandidate };
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  static constexpr unsigned ReverseLayers=24;
  static std::uint64_t salt(unsigned n) {
    return UINT64_C(0x9e3779b97f4a7c15) ^ (std::uint64_t(n+1)*UINT64_C(0xbf58476d1ce4e5b9));
  }
  static std::uint64_t foldedChain(std::uint64_t value) {
    for(unsigned n=0;n<ReverseLayers;++n) value=(value*UINT64_C(3))^salt(n);
    return value;
  }
  explicit RangeDepthFixture(Mode mode=Normal) {
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
    if (mode==BadSP)
      replace("b.end:\n  br label %head","b.end:\n  %oldsp = load i64, ptr %slot\n  %newsp = add i64 %oldsp, 1\n  store i64 %newsp, ptr %slot\n  br label %head");
    if (mode==ReverseConstantChain || mode==ReverseConstantChainBadMagic) {
      std::string chain="  br label %overlay.chain.0\n";
      for(unsigned at=ReverseLayers;at;--at) {
        const unsigned n=at-1;
        const auto number=std::to_string(n);
        const auto input=n ? "%overlay.v."+std::to_string(n-1) : "%pointer";
        chain+="overlay.chain."+number+":\n";
        chain+="  %overlay.mul."+number+" = mul i64 "+input+", 3\n";
        chain+="  %overlay.v."+number+" = xor i64 %overlay.mul."+number+", "+std::to_string(salt(n))+"\n";
        if(n+1==ReverseLayers) {
          const auto expected=mode==ReverseConstantChainBadMagic ? UINT64_C(8192) : UINT64_C(32768);
          const auto magic=foldedChain(UINT64_C(32768))^expected;
          chain+="  %overlay.result = xor i64 %overlay.v."+number+", "+std::to_string(magic)+"\n";
          chain+="  %output = getelementptr i8, ptr %guest, i64 %overlay.result\n";
          chain+="  store i32 123, ptr %output, align 1\n";
          chain+="  br label %a.end\n";
        } else chain+="  br label %overlay.chain."+std::to_string(n+1)+"\n";
      }
      const std::string original="  %output = getelementptr i8, ptr %guest, i64 %pointer\n"
          "  store i32 123, ptr %output, align 1\n  \n  br label %a.end\n";
      REQUIRE(text.find(original)!=std::string::npos);
      replace(original,chain);
    }
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

TEST_CASE("Depth accounting fixture copy: Loop image proof threads every input and preserves all lookup reads",
          "[llvm][image-loop-range-depth]") {
  for (auto mode:{RangeDepthFixture::Normal,RangeDepthFixture::DuplicateInput,RangeDepthFixture::PrefixVersusEnd,
                  RangeDepthFixture::GrayDefinition})
    DYNAMIC_SECTION("mode "<<mode) {
      RangeDepthFixture f(mode);auto *view=f.module->getFunction("view");auto *arg=view->getArg(0);
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

TEST_CASE("Depth accounting fixture copy: Loop image local ranges never bypass missing definitions or memory effects",
          "[llvm][image-loop-range-depth]") {
  for (auto mode:{RangeDepthFixture::MissingMask,RangeDepthFixture::MissingDefinition,RangeDepthFixture::PartialIndex,
                  RangeDepthFixture::WideMask,RangeDepthFixture::ClobberRestore,RangeDepthFixture::UnknownCall,
                  RangeDepthFixture::OtherBase,RangeDepthFixture::BadSP,RangeDepthFixture::BadInitialState,RangeDepthFixture::BadIndex,
                  RangeDepthFixture::CallerTag,RangeDepthFixture::BadBackedgeRange,
                  RangeDepthFixture::CallerGlobalTag})
    DYNAMIC_SECTION("mode "<<mode) {
      RangeDepthFixture f(mode);auto before=f.ir();auto *view=f.module->getFunction("view");
      auto *entry=&view->getEntryBlock();auto *arg=view->getArg(0);
      auto result=runLoopImageTableContext(*f.module,f.request());
      INFO(result.message);CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);
      CHECK(f.ir()==before);CHECK(f.module->getFunction("view")==view);
      CHECK(&view->getEntryBlock()==entry);CHECK(view->getArg(0)==arg);
      REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
    }
}

TEST_CASE("Depth accounting fixture copy: Loop image tiny budgets refuse without publishing a private batch",
          "[llvm][image-loop-range-depth]") {
  for (std::size_t limit:{std::size_t(1),std::size_t(32),std::size_t(256)}) {
    RangeDepthFixture f;auto before=f.ir();auto request=f.request();request.maxWork=limit;
    auto result=runLoopImageTableContext(*f.module,request);
    CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(result.budgetExhausted);
    CHECK(result.work<=limit);CHECK(f.ir()==before);
  }
}

TEST_CASE("Depth accounting fixture copy: Legacy image entry retains its original local range refusal",
          "[llvm][image-loop-range-depth]") {
  RangeDepthFixture f;const auto before=f.ir();
  auto result=runClosedImageTableContext(*f.module,f.request());
  CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(f.ir()==before);
}

TEST_CASE("Depth accounting fixture copy: Loop image consumes the first fully closed cell candidate",
          "[llvm][image-loop-range-depth]") {
  RangeDepthFixture f(RangeDepthFixture::TwoCandidates);
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

TEST_CASE("Depth accounting fixture copy: Loop image refuses a truly clobbered candidate without leaking constants",
          "[llvm][image-loop-range-depth]") {
  RangeDepthFixture f(RangeDepthFixture::FailedFirstCandidate);
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
constexpr std::size_t ReverseChainWork = 1000000;
llvm::BasicBlock *overlayBlock(llvm::Function &f,const std::string &name) {
  for(auto &block:f) if(block.getName()==name) return &block;
  return nullptr;
}
llvm::Instruction *overlayInstruction(llvm::Function &f,const std::string &name) {
  for(auto &block:f) for(auto &i:block) if(i.getName()==name) return &i;
  return nullptr;
}
void checkReverseChainInput(RangeDepthFixture &fixture) {
  auto &f=*fixture.module->getFunction("view");
  std::vector<unsigned> physical;
  for(auto &block:f) if(block.getName().starts_with("overlay.chain."))
    physical.push_back(unsigned(std::stoul(block.getName().drop_front(14).str())));
  REQUIRE(physical.size()==RangeDepthFixture::ReverseLayers);
  for(unsigned n=0;n<physical.size();++n) CHECK(physical[n]==physical.size()-1-n);
  auto *pointer=llvm::dyn_cast_or_null<llvm::LoadInst>(overlayInstruction(f,"pointer"));REQUIRE(pointer);
  llvm::Value *previous=pointer;
  for(unsigned n=0;n<RangeDepthFixture::ReverseLayers;++n) {
    const auto number=std::to_string(n);
    auto *block=overlayBlock(f,"overlay.chain."+number);REQUIRE(block);
    auto *multiply=llvm::dyn_cast_or_null<llvm::BinaryOperator>(overlayInstruction(f,"overlay.mul."+number));
    auto *value=llvm::dyn_cast_or_null<llvm::BinaryOperator>(overlayInstruction(f,"overlay.v."+number));
    REQUIRE(multiply);REQUIRE(value);CHECK(multiply->getParent()==block);CHECK(value->getParent()==block);
    CHECK(multiply->getOpcode()==llvm::Instruction::Mul);CHECK_FALSE(multiply->hasPoisonGeneratingAnnotations());
    CHECK(multiply->getOperand(0)==previous);
    auto *three=llvm::dyn_cast<llvm::ConstantInt>(multiply->getOperand(1));REQUIRE(three);CHECK(three->getZExtValue()==3);
    CHECK(value->getOpcode()==llvm::Instruction::Xor);CHECK_FALSE(value->hasPoisonGeneratingAnnotations());
    CHECK(value->getOperand(0)==multiply);
    auto *salt=llvm::dyn_cast<llvm::ConstantInt>(value->getOperand(1));REQUIRE(salt);
    CHECK(salt->getZExtValue()==RangeDepthFixture::salt(n));
    auto *branch=llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());REQUIRE(branch);REQUIRE(branch->isUnconditional());
    CHECK(branch->getSuccessor(0)==overlayBlock(f,n+1==RangeDepthFixture::ReverseLayers ? "a.end" : "overlay.chain."+std::to_string(n+1)));
    previous=value;
  }
  auto *result=llvm::dyn_cast_or_null<llvm::BinaryOperator>(overlayInstruction(f,"overlay.result"));REQUIRE(result);
  CHECK(result->getOpcode()==llvm::Instruction::Xor);CHECK(result->getOperand(0)==previous);
}
void checkProtectedEffectsRemain(RangeDepthFixture &fixture) {
  auto &f=*fixture.module->getFunction("view");unsigned targetReads=0;bool pointerRead=false,outputWrite=false;
  for(auto &block:f)for(auto &i:block) {
    if(llvm::isa<llvm::LoadInst>(i) && i.getName().starts_with("target")) ++targetReads;
    if(llvm::isa<llvm::LoadInst>(i) && i.getName()=="pointer") pointerRead=true;
    auto *store=llvm::dyn_cast<llvm::StoreInst>(&i);if(!store)continue;
    auto *value=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    if(!value || value->getBitWidth()!=32 || value->getZExtValue()!=123)continue;
    auto *pointer=llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());REQUIRE(pointer);
    REQUIRE(pointer->getNumIndices()==1);auto *index=llvm::dyn_cast<llvm::ConstantInt>(pointer->idx_begin()->get());
    REQUIRE(index);CHECK(index->getZExtValue()==32768);outputWrite=true;
  }
  CHECK(targetReads>=3);CHECK(pointerRead);CHECK(outputWrite);
  // The original PC-default error remains outside the accepted loop. The
  // table-switch default may become unreachable only after full closure;
  // this test does not require an impossible old target edge to survive.
  auto *error=overlayBlock(f,"unsupported");REQUIRE(error);
  CHECK(llvm::isa<llvm::ReturnInst>(error->getTerminator()));
  bool entryErrorEdge=false;auto *entryTerminator=f.getEntryBlock().getTerminator();
  for(unsigned slot=0;slot<entryTerminator->getNumSuccessors();++slot)entryErrorEdge|=entryTerminator->getSuccessor(slot)==error;
  CHECK(entryErrorEdge);
  auto *fallback=overlayBlock(f,"fallback");REQUIRE(fallback);
  auto *call=llvm::dyn_cast<llvm::CallInst>(&fallback->front());REQUIRE(call);CHECK(call->getCalledFunction()==fixture.module->getFunction("ordinary"));
  CHECK(llvm::isa<llvm::ReturnInst>(fallback->getTerminator()));
  CHECK(fixture.ir().find("dragon.loop.image.private")==std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*fixture.module,&llvm::errs()));
}
}

TEST_CASE("Loop range accounting separates CFG distance from scalar dependency depth",
          "[llvm][image-loop-range-depth][image-loop-depth-chain]") {
  RangeDepthFixture fixture(RangeDepthFixture::ReverseConstantChain);checkReverseChainInput(fixture);
  auto *f=fixture.module->getFunction("view");auto *arg=f->getArg(0);auto *ordinary=fixture.module->getFunction("ordinary");
  auto request=fixture.request();REQUIRE(request.maxWork==ReverseChainWork);
  auto result=runLoopImageTableContext(*fixture.module,request);INFO(result.message);INFO(result.work);
  REQUIRE(result.closed);REQUIRE(result.changed);CHECK_FALSE(result.budgetExhausted);CHECK(result.work<=ReverseChainWork);
  CHECK(fixture.module->getFunction("view")==f);CHECK(f->getArg(0)==arg);CHECK(fixture.module->getFunction("ordinary")==ordinary);
  checkProtectedEffectsRemain(fixture);
}

TEST_CASE("Loop range depth accounting never accepts a protected-table output",
          "[llvm][image-loop-range-depth][image-loop-depth-chain]") {
  RangeDepthFixture fixture(RangeDepthFixture::ReverseConstantChainBadMagic);checkReverseChainInput(fixture);
  const auto before=fixture.ir();auto *f=fixture.module->getFunction("view");auto *arg=f->getArg(0);auto *entry=&f->getEntryBlock();
  std::vector<llvm::Instruction *> instructions;for(auto &block:*f)for(auto &i:block)instructions.push_back(&i);
  auto request=fixture.request();REQUIRE(request.maxWork==ReverseChainWork);
  auto result=runLoopImageTableContext(*fixture.module,request);INFO(result.message);INFO(result.work);
  CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(result.work<=ReverseChainWork);CHECK(fixture.ir()==before);
  CHECK(fixture.module->getFunction("view")==f);CHECK(f->getArg(0)==arg);CHECK(&f->getEntryBlock()==entry);
  std::vector<llvm::Instruction *> after;for(auto &block:*f)for(auto &i:block)after.push_back(&i);CHECK(after==instructions);
  REQUIRE_FALSE(llvm::verifyModule(*fixture.module,&llvm::errs()));
}

TEST_CASE("Loop range depth tiny budgets keep the exact source module",
          "[llvm][image-loop-range-depth][image-loop-depth-chain]") {
  for(std::size_t limit:{std::size_t(1),std::size_t(32),std::size_t(256)}) {
    RangeDepthFixture fixture(RangeDepthFixture::ReverseConstantChain);const auto before=fixture.ir();
    auto request=fixture.request();request.maxWork=limit;
    auto result=runLoopImageTableContext(*fixture.module,request);INFO(result.message);
    CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(result.budgetExhausted);CHECK(result.work<=limit);CHECK(fixture.ir()==before);
    REQUIRE_FALSE(llvm::verifyModule(*fixture.module,&llvm::errs()));
  }
}
