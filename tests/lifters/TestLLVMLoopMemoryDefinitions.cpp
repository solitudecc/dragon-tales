#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace dragon::llvmlifter::detail;
namespace {
// Same installed API as frozen 0cff. A before failure here is an executed
// precision/refusal regression, not a missing-symbol or wrong-result claim.
struct MemoryGraphFixture {
  enum Mode { ReadOnlySCC, TwoEntryUnion, DuplicateEntryUnion, DeepCorridor,
              TypedFull, TerminalEffect, BadBackedge22, BadUnion22,
              MissingEntryDefinition, PartialWrite, UnknownCall, OtherBase,
              VolatileRead, AtomicRead, Fence, UnknownAlias,
              TypedTooWide, TypedVolatile, TypedAtomic, LatePartial,
              CyclicValue };
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit MemoryGraphFixture(Mode mode=ReadOnlySCC) {
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
    auto replace = [&](const std::string &from, const std::string &to) {
      std::size_t at=0; unsigned count=0;
      while ((at=text.find(from,at))!=std::string::npos) {
        text.replace(at,from.size(),to);at+=to.size();++count;
      }
      REQUIRE(count>0);
    };
    const bool unionMode=mode==TwoEntryUnion || mode==DuplicateEntryUnion || mode==BadUnion22;
    replace("INITIAL_STATE","0");
    replace("B_STATE","1");
    replace("; FIRST_CANDIDATE","");
    replace("; FIRST_READ","");
    replace("INDEX_MASK","7");
    replace("; INITIAL_INDEX",mode==MissingEntryDefinition ? "" : "store i64 22, ptr %indexcell, align 1");
    if (unionMode) {
      replace("; MASK_STORE",
          "%choose.high = icmp ult i64 %data, 9223372036854775808\n"
          "  br i1 %choose.high, label %a.left, label %a.right\n"
          "a.left:\n  store i64 2, ptr %indexcell, align 1\n  br label %a.join\n"
          "a.right:\n  store i64 "+std::string(mode==BadUnion22 ? "22" : "15")+
          ", ptr %indexcell, align 1\n  br label %a.join\n"
          "a.join:");
    } else {
      replace("; MASK_STORE",mode==MissingEntryDefinition ? "" :
          "store i64 %bounded, ptr %indexcell, align 1");
    }
    replace("; EXTRA_EFFECT","");
    replace("; RESTORE_EFFECT","");
    replace("; AFTER_B_LOAD",mode==BadBackedge22 ? "store i64 22, ptr %indexcell, align 1" :
            mode==LatePartial ? "store i32 0, ptr %indexcell, align 1" :
            mode==CyclicValue ? "%next.index = add i64 %index.b, 1\n  store i64 %next.index, ptr %indexcell, align 1" : "");
    if (mode==DuplicateEntryUnion) {
      replace("a.left:\n  store i64 2, ptr %indexcell, align 1\n  br label %a.join",
              "a.left:\n  store i64 2, ptr %indexcell, align 1\n  br i1 %choose.high, label %a.join, label %a.join");
    }
    std::string prefix;
    if (mode==PartialWrite)
      prefix="  store i32 0, ptr %indexcell, align 1\n";
    if (mode==UnknownCall)
      prefix="  call void @unknown_call(ptr %guest)\n";
    if (mode==OtherBase)
      prefix="  store i64 0, ptr %state, align 1\n";
    if (mode==Fence)
      prefix="  fence seq_cst\n";
    if (mode==UnknownAlias)
      prefix="  %alias.input = getelementptr i8, ptr %guest, i64 160\n"
             "  %alias.index = load i64, ptr %alias.input, align 1\n"
             "  %alias.destination = getelementptr i8, ptr %guest, i64 %alias.index\n"
             "  store i8 0, ptr %alias.destination, align 1\n";
    if (!prefix.empty()) replace("b:\n","b:\n"+prefix);
    if (mode==VolatileRead)
      replace("%index.b = load i64, ptr %indexcell, align 1",
              "%index.b = load volatile i64, ptr %indexcell, align 1");
    if (mode==AtomicRead)
      replace("%index.b = load i64, ptr %indexcell, align 1",
              "%index.b = load atomic i64, ptr %indexcell monotonic, align 8");
    if (mode==DeepCorridor) {
      std::string chain="b:\n  br label %memory.chain.0\n";
      for (unsigned n=0;n<40;++n) {
        chain+="memory.chain."+std::to_string(n)+":\n";
        // Keep a real ordinary LOAD in every block. The proof must inspect
        // every operation; no CFG-history depth is an arithmetic depth reset.
        chain+="  %chain.read."+std::to_string(n)+" = load i8, ptr %guest, align 1\n";
        chain+="  br label %"+std::string(n==39 ? "memory.read" : "memory.chain."+std::to_string(n+1))+"\n";
      }
      chain+="memory.read:\n";
      replace("b:\n",chain);
    }
    if (mode==TypedFull || mode==TypedTooWide || mode==TypedVolatile || mode==TypedAtomic) {
      replace("%data = load i64, ptr %datap, align 1",
              mode==TypedVolatile ? "%data = load volatile i64, ptr %outaddr, align 1" :
              mode==TypedAtomic ? "%data = load atomic i64, ptr %outaddr monotonic, align 8" :
                                  "%data = load i64, ptr %outaddr, align 1");
      replace("%bounded = and i64 %data, 7",
              "%typed.low = trunc i64 %data to i32\n"
              "  %typed.high = lshr i32 %typed.low, "+std::string(mode==TypedTooWide ? "27" : "28")+
              "\n  %bounded = zext i32 %typed.high to i64");
    }
    if (mode==TerminalEffect) {
      replace("b.end:\n  br label %head",
              "b.end:\n  %leave = icmp eq i64 %index.b, 0\n"
              "  br i1 %leave, label %terminal, label %head\n"
              "terminal:\n  call void @unknown_call(ptr %guest)\n"
              "  store i64 8192, ptr %cell, align 1\n  ret void");
    }
    llvm::SMDiagnostic error;
    module=llvm::parseAssemblyString(text,error,context);
    if (!module) error.print("loop-memory-definition-test",llvm::errs());
    REQUIRE(module);
    REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  ImageTableContextRequest request() const { return {"view",8192,{4352,4608}}; }
  std::string ir() const {
    std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;
  }
};
struct Identities {
  llvm::Function *view;
  llvm::Function *ordinary;
  std::vector<llvm::Argument *> arguments;
  std::vector<llvm::BasicBlock *> blocks;
  std::vector<llvm::Instruction *> instructions;
  explicit Identities(llvm::Module &m):view(m.getFunction("view")),ordinary(m.getFunction("ordinary")) {
    for (auto &a:view->args()) arguments.push_back(&a);
    for (auto &b:*view) { blocks.push_back(&b);for(auto &i:b) instructions.push_back(&i); }
  }
  void checkFunctions(llvm::Module &m) const {
    CHECK(m.getFunction("view")==view);CHECK(m.getFunction("ordinary")==ordinary);
    unsigned n=0;for(auto &a:view->args()) { REQUIRE(n<arguments.size());CHECK(&a==arguments[n++]); }
    CHECK(n==arguments.size());
  }
  void checkAll(llvm::Module &m) const {
    checkFunctions(m);
    std::vector<llvm::BasicBlock *> actualBlocks;
    std::vector<llvm::Instruction *> actualInstructions;
    for (auto &b:*view) { actualBlocks.push_back(&b);for(auto &i:b) actualInstructions.push_back(&i); }
    CHECK(actualBlocks==blocks);CHECK(actualInstructions==instructions);
  }
};

std::set<std::string> normalizedIndexLoads(const llvm::Module &original) {
  // Independent upstream frontend control, not a Dragon optimizer invocation.
  // Only mem2reg + InstCombine may forward the raw same-block index.a LOAD.
  // The production proof is required to retain every observation that this
  // control still needs; an absolute raw-IR LOAD count is not that contract.
  auto control=llvm::CloneModule(original);
  auto *view=control->getFunction("view");REQUIRE(view);
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerLoopAnalyses(loops);builder.registerFunctionAnalyses(functions);
  builder.registerCGSCCAnalyses(cgscc);builder.registerModuleAnalyses(modules);
  builder.crossRegisterProxies(loops,functions,cgscc,modules);
  llvm::FunctionPassManager frontend;
  frontend.addPass(llvm::PromotePass());
  frontend.addPass(llvm::InstCombinePass());
  frontend.run(*view,functions);
  REQUIRE_FALSE(llvm::verifyModule(*control,&llvm::errs()));
  std::set<std::string> names;
  for(auto &block:*view)for(auto &i:block)
    if(llvm::isa<llvm::LoadInst>(i) && i.getName().starts_with("index."))
      names.insert(i.getName().str());
  REQUIRE(names.contains("index.b")); // This test must still exercise D history.
  return names;
}
}

TEST_CASE("Loop memory definition graphs close readonly strongly connected histories",
          "[llvm][image-loop-memory-graph]") {
  for (auto mode:{MemoryGraphFixture::ReadOnlySCC,MemoryGraphFixture::TwoEntryUnion,
                  MemoryGraphFixture::DuplicateEntryUnion,MemoryGraphFixture::DeepCorridor,
                  MemoryGraphFixture::TypedFull,MemoryGraphFixture::TerminalEffect})
    DYNAMIC_SECTION("mode "<<mode) {
      MemoryGraphFixture f(mode);Identities ids(*f.module);
      auto *note=f.module->getOrInsertNamedMetadata("caller.note");
      note->addOperand(llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"unchanged")}));
      const auto raw=f.ir();const auto requiredIndexLoads=normalizedIndexLoads(*f.module);
      CHECK(f.ir()==raw);
      std::string controlNames;
      for(const auto &name:requiredIndexLoads)controlNames+=name+" ";
      INFO("upstream mem2reg+InstCombine surviving index LOADs: "<<controlNames);
      auto result=runLoopImageTableContext(*f.module,f.request());
      INFO(result.message);REQUIRE(result.closed);REQUIRE(result.changed);
      CHECK_FALSE(result.budgetExhausted);CHECK(result.work<=f.request().maxWork);
      ids.checkFunctions(*f.module);
      CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);
      unsigned targetReads=0,pointerReads=0;
      std::set<std::string> actualIndexLoads;
      for (auto &b:*ids.view) for(auto &i:b) if (llvm::isa<llvm::LoadInst>(i)) {
        targetReads+=i.getName().starts_with("target");
        if(i.getName().starts_with("index."))actualIndexLoads.insert(i.getName().str());
        pointerReads+=i.getName().starts_with("pointer");
      }
      CHECK(targetReads>=3);CHECK(pointerReads>=1);
      for(const auto &name:requiredIndexLoads)CHECK(actualIndexLoads.contains(name));
      const auto after=f.ir();
      CHECK(after.find("call void @ordinary")!=std::string::npos);
      CHECK(after.find("dragon.loop.image.private")==std::string::npos);
      CHECK(after.find("llvm.assume")==std::string::npos);
      CHECK(after.find("unreachable")==std::string::npos);
      if (mode==MemoryGraphFixture::TerminalEffect) {
        CHECK(after.find("call void @unknown_call")!=std::string::npos);
        CHECK(after.find("store i64 8192, ptr %cell")!=std::string::npos);
      }
      if (mode==MemoryGraphFixture::TypedFull) {
        bool actualFreshRead=false;
        for(auto &b:*ids.view) for(auto &i:b)
          actualFreshRead |= i.getName()=="data" && llvm::isa<llvm::LoadInst>(i);
        CHECK(actualFreshRead);
      }
      REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
    }
}

TEST_CASE("Loop memory definition graphs reject every unanchored or clobbering path",
          "[llvm][image-loop-memory-graph]") {
  for (auto mode:{MemoryGraphFixture::BadBackedge22,MemoryGraphFixture::BadUnion22,
                  MemoryGraphFixture::MissingEntryDefinition,MemoryGraphFixture::PartialWrite,
                  MemoryGraphFixture::UnknownCall,MemoryGraphFixture::OtherBase,
                  MemoryGraphFixture::VolatileRead,MemoryGraphFixture::AtomicRead,
                  MemoryGraphFixture::Fence,MemoryGraphFixture::UnknownAlias,
                  MemoryGraphFixture::TypedTooWide,MemoryGraphFixture::TypedVolatile,
                  MemoryGraphFixture::TypedAtomic,MemoryGraphFixture::LatePartial,
                  MemoryGraphFixture::CyclicValue})
    DYNAMIC_SECTION("mode "<<mode) {
      MemoryGraphFixture f(mode);Identities ids(*f.module);
      auto *note=f.module->getOrInsertNamedMetadata("caller.note");
      note->addOperand(llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"unchanged")}));
      const auto before=f.ir();auto result=runLoopImageTableContext(*f.module,f.request());
      INFO(result.message);CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);
      CHECK(result.work<=f.request().maxWork);CHECK(f.ir()==before);
      ids.checkAll(*f.module);
      CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);
      REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
    }
}

TEST_CASE("Loop memory definition graph limits refuse atomically under the shared work cap",
          "[llvm][image-loop-memory-graph]") {
  for (std::size_t work:{std::size_t(1),std::size_t(32),std::size_t(256)}) {
    MemoryGraphFixture f(MemoryGraphFixture::DeepCorridor);Identities ids(*f.module);
    const auto before=f.ir();auto request=f.request();request.maxWork=work;
    auto result=runLoopImageTableContext(*f.module,request);
    CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(result.budgetExhausted);
    CHECK(result.work<=work);CHECK(f.ir()==before);ids.checkAll(*f.module);
    REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
  }
}
