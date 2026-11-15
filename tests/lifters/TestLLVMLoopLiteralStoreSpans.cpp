#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace dragon::llvmlifter::detail;
namespace {
// Independent from the earlier six unresolved alias/value-cycle positives.
// These cases exercise fixed STORE spans only, through the same public API.
struct LiteralSpanFixture {
  enum Mode { Safe, PartialD, TableClobber, WrappedSpan, UnknownCall };
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit LiteralSpanFixture(Mode mode=Safe) {
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
      std::size_t at=0;unsigned count=0;
      while ((at=text.find(from,at))!=std::string::npos) {
        text.replace(at,from.size(),to);at+=to.size();++count;
      }
      REQUIRE(count>0);
    };
    replace("INITIAL_STATE","0");replace("B_STATE","1");
    replace("; FIRST_CANDIDATE","");replace("; FIRST_READ","");
    replace("INDEX_MASK","7");
    replace("; INITIAL_INDEX","store i64 22, ptr %indexcell, align 1");
    std::string stores="store i64 %bounded, ptr %indexcell, align 1\n";
    for(unsigned n=0;n<128;++n) {
      stores+="  %literal.address."+std::to_string(n)+" = getelementptr i8, ptr %guest, i64 "+
              std::to_string(16384+n*8)+"\n";
      stores+="  store i64 "+std::to_string(500+n)+", ptr %literal.address."+std::to_string(n)+", align 1\n";
    }
    if(mode==PartialD) stores+="  store i8 22, ptr %indexcell, align 1\n";
    if(mode==TableClobber) stores+="  %bad.table = getelementptr i8, ptr %guest, i64 8200\n"
                                  "  store i64 4352, ptr %bad.table, align 1\n";
    if(mode==WrappedSpan) stores+="  %wrapped = getelementptr i8, ptr %guest, i64 -4\n"
                                  "  store i64 123, ptr %wrapped, align 1\n";
    if(mode==UnknownCall) stores+="  call void @unknown_call(ptr %guest)\n";
    replace("; MASK_STORE",stores);
    replace("; EXTRA_EFFECT","");replace("; RESTORE_EFFECT","");replace("; AFTER_B_LOAD","");
    // This explicit original mask makes B's work STORE disjoint without
    // recursively requesting a numeric fact for this very index.b LOAD.
    replace("%scaled.b = shl i64 %index.b, 2",
            "%bounded.b = and i64 %index.b, 7\n  %scaled.b = shl i64 %bounded.b, 2");
    llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);
    if (!module) error.print("literal-store-span-test",llvm::errs());
    REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  ImageTableContextRequest request() const { return {"view",8192,{4352,4608}}; }
  std::string ir() const {std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;}
};
llvm::Instruction *named(llvm::Function &f,llvm::StringRef name) {
  for(auto &b:f) for(auto &i:b) if(i.getName()==name)return &i;
  return nullptr;
}
std::pair<unsigned,unsigned> guestEffects(llvm::Function &f) {
  auto *guest=named(f,"guest");REQUIRE(guest);
  unsigned reads=0,writes=0;
  for(auto &b:f) for(auto &i:b) {
    llvm::Value *pointer=nullptr;
    if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&i))pointer=load->getPointerOperand();
    if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i))pointer=store->getPointerOperand();
    auto *gep=llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(pointer);
    if(!gep || gep->getPointerOperand()!=guest)continue;
    reads+=llvm::isa<llvm::LoadInst>(i);writes+=llvm::isa<llvm::StoreInst>(i);
  }
  return {reads,writes};
}
std::map<std::uint64_t,std::uint64_t> literalEffects(llvm::Function &f) {
  auto *guest=named(f,"guest");REQUIRE(guest);
  std::map<std::uint64_t,std::uint64_t> result;
  for(auto &b:f) for(auto &i:b) if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)) {
    auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
    auto *value=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    auto *index=gep && gep->getNumIndices()==1 ? llvm::dyn_cast<llvm::ConstantInt>(gep->idx_begin()->get()) : nullptr;
    if(!gep || gep->getPointerOperand()!=guest || !index || !value || !value->getType()->isIntegerTy(64))continue;
    const auto at=index->getZExtValue();
    if(at>=16384 && at<16384+128*8) {
      CHECK(store->isSimple());CHECK(store->getAlign().value()==1);
      CHECK(result.emplace(at,value->getZExtValue()).second);
    }
  }
  return result;
}
struct SpanIdentities {
  llvm::Function *view,*ordinary;
  std::vector<llvm::Argument *> args;
  std::vector<llvm::BasicBlock *> blocks;
  std::vector<llvm::Instruction *> instructions;
  explicit SpanIdentities(llvm::Module &m):view(m.getFunction("view")),ordinary(m.getFunction("ordinary")) {
    for(auto &a:view->args())args.push_back(&a);
    for(auto &b:*view) {blocks.push_back(&b);for(auto &i:b)instructions.push_back(&i);}
  }
  void checkFunctions(llvm::Module &m) const {
    CHECK(m.getFunction("view")==view);CHECK(m.getFunction("ordinary")==ordinary);
    unsigned n=0;for(auto &a:view->args()){REQUIRE(n<args.size());CHECK(&a==args[n++]);}CHECK(n==args.size());
  }
  void checkAll(llvm::Module &m) const {
    checkFunctions(m);std::vector<llvm::BasicBlock *> b;std::vector<llvm::Instruction *> i;
    for(auto &block:*view){b.push_back(&block);for(auto &inst:block)i.push_back(&inst);}
    CHECK(b==blocks);CHECK(i==instructions);
  }
};
}

TEST_CASE("Loop image literal STORE spans do not consume scalar expression nodes",
          "[llvm][image-loop-literal-span]") {
  LiteralSpanFixture f;SpanIdentities ids(*f.module);
  auto *note=f.module->getOrInsertNamedMetadata("caller.note");
  note->addOperand(llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"preserved")}));
  const auto oldEffects=guestEffects(*ids.view);const auto oldLiterals=literalEffects(*ids.view);
  REQUIRE(oldLiterals.size()==128);
  auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
  REQUIRE(result.closed);REQUIRE(result.changed);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.work<=1000000);ids.checkFunctions(*f.module);
  CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);
  const auto effects=guestEffects(*ids.view);
  CHECK(effects.first>=oldEffects.first);CHECK(effects.second==oldEffects.second);
  CHECK(literalEffects(*ids.view)==oldLiterals);
  for(auto name:{"data","index.a","index.b","pointer","pointer.b","t0","t1"}) {
    auto *observation=named(*ids.view,name);REQUIRE(observation);CHECK(llvm::isa<llvm::LoadInst>(observation));
  }
  unsigned actualLookups=0;for(auto &b:*ids.view)for(auto &i:b)
    actualLookups+=llvm::isa<llvm::LoadInst>(i) && i.getName().starts_with("target");
  CHECK(actualLookups>=3);
  const auto after=f.ir();CHECK(after.find("call void @ordinary")!=std::string::npos);
  CHECK(after.find("dragon.loop.image.private")==std::string::npos);
  CHECK(after.find("llvm.assume")==std::string::npos);CHECK(after.find("unreachable")==std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
}

TEST_CASE("Loop image literal STORE span exceptions refuse without a partial publication",
          "[llvm][image-loop-literal-span]") {
  for(auto mode:{LiteralSpanFixture::PartialD,LiteralSpanFixture::TableClobber,
                 LiteralSpanFixture::WrappedSpan,LiteralSpanFixture::UnknownCall})
    DYNAMIC_SECTION("mode "<<mode) {
      LiteralSpanFixture f(mode);SpanIdentities ids(*f.module);
      auto *note=f.module->getOrInsertNamedMetadata("caller.note");
      note->addOperand(llvm::MDNode::get(f.context,{llvm::MDString::get(f.context,"preserved")}));
      const auto before=f.ir();const auto oldEffects=guestEffects(*ids.view);
      const auto oldLiterals=literalEffects(*ids.view);REQUIRE(oldLiterals.size()==128);
      auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
      CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(result.work<=1000000);
      CHECK(f.ir()==before);ids.checkAll(*f.module);
      CHECK(guestEffects(*ids.view)==oldEffects);CHECK(literalEffects(*ids.view)==oldLiterals);
      CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);
      REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
    }
}
