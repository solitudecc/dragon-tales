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
#include <string>
#include <vector>
using namespace dragon::llvmlifter::detail;
namespace {
struct MaskMemoryFixture {
  enum Mode { Mask32, Mask64, Mask16, Typed32, AlreadyBoundedNoDefinition,
              LowMaskNoDefinition, TooWide, PartialE, MissingEntry, UnknownCall,
              CyclicE, Overshift, VolatileE, AtomicE, PoisonOther, Fence };
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit MaskMemoryFixture(Mode mode=Mask32) {
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
      while((at=text.find(from,at))!=std::string::npos){text.replace(at,from.size(),to);at+=to.size();++count;}
      REQUIRE(count>0);
    };
    const bool narrow=mode==Typed32;
    const bool noDefinition=mode==AlreadyBoundedNoDefinition || mode==LowMaskNoDefinition;
    const std::string type=narrow ? "i32" : "i64";
    const std::uint64_t mask=mode==Mask64 ? UINT64_C(18446744073709551612) :
                             mode==Mask16 ? UINT64_C(65520) :
                             mode==LowMaskNoDefinition ? UINT64_C(7) : UINT64_C(4294967292);
    const unsigned shift=mode==Overshift ? 64 : mode==Mask16 ? 4 : 2;
    const unsigned lowMask=mode==Mask16 ? 15 : 3;
    const unsigned base=mode==Mask16 ? 3728 : 3928;
    replace("INITIAL_STATE","0");replace("B_STATE","1");replace("INDEX_MASK","7");
    replace("; FIRST_CANDIDATE","");replace("; FIRST_READ","");
    replace("; INITIAL_INDEX","store i64 22, ptr %indexcell, align 1\n"
        "  %ecell = getelementptr i8, ptr %guest, i64 1024\n"+
        std::string(noDefinition || mode==MissingEntry ? "" : "  store "+type+" 22, ptr %ecell, align 1"));
    std::string definition="store i64 %bounded, ptr %indexcell, align 1\n";
    if(!noDefinition) {
      definition+="  %e.bound = and i64 %data, "+std::string(mode==TooWide ? "63" : "15")+"\n";
      if(narrow)definition+="  %e.bound32 = trunc i64 %e.bound to i32\n";
      const auto value=narrow ? "%e.bound32" : "%e.bound";
      if(mode==MissingEntry)definition+=
          "  %e.choose = icmp eq i64 %bounded, 0\n"
          "  br i1 %e.choose, label %e.define, label %e.bypass\n"
          "e.define:\n  store "+type+" "+value+", ptr %ecell, align 1\n  br label %e.join\n"
          "e.bypass:\n  br label %e.join\ne.join:\n";
      else definition+="  store "+type+" "+value+", ptr %ecell, align 1\n";
    }
    replace("; MASK_STORE",definition);
    replace("; EXTRA_EFFECT","");replace("; RESTORE_EFFECT","");
    replace("; AFTER_B_LOAD",mode==CyclicE ?
        "%e.next = add i64 %e.b, 1\n  store i64 %e.next, ptr %ecell, align 1" : "");
    replace("  %pointer.b = load i64, ptr %cell, align 1\n","");
    std::string prefix="b:\n  %pointer.b = load i64, ptr %cell, align 1\n";
    if(mode==PartialE)prefix+="  store i8 42, ptr %ecell, align 1\n";
    if(mode==UnknownCall)prefix+="  call void @unknown_call(ptr %guest)\n";
    if(mode==Fence)prefix+="  fence seq_cst\n";
    prefix+="  %e.b = load "+std::string(mode==VolatileE ? "volatile " : mode==AtomicE ? "atomic " : "")+
            type+", ptr %ecell"+std::string(mode==AtomicE ? " monotonic, align 8\n" : ", align 1\n");
    prefix+="  %other.address = getelementptr i8, ptr %guest, i64 192\n"
            "  %other.data = load i64, ptr %other.address, align 1\n";
    prefix+="  %other.low = and i64 "+std::string(mode==PoisonOther ? "poison" : "%other.data")+
            ", "+std::to_string(lowMask)+"\n";
    replace("b:\n",prefix);
    std::string expression;
    std::string input="%e.b";
    if(mode==AlreadyBoundedNoDefinition){expression+="  %e.prebounded = and i64 %e.b, 15\n";input="%e.prebounded";}
    expression+="  %e.scaled = shl "+type+" "+input+", "+std::to_string(shift)+"\n";
    if(narrow)expression+="  %e.trace.value = zext i32 %e.scaled to i64\n";
    expression+="  %e.trace.address = getelementptr i8, ptr %guest, i64 1552\n"
                "  store i64 "+std::string(narrow ? "%e.trace.value" : "%e.scaled")+", ptr %e.trace.address, align 1\n";
    // The constant mask is an independently checked C observation, not an IR
    // literal before normalization. InstCombine cannot delete/reassociate the
    // mask by assuming this Guest LOAD equals the guard's snapshot value.
    if(narrow)expression+="  %mask.input = trunc i64 %pointer.b to i32\n";
    expression+="  %mask.value = xor "+type+" "+std::string(narrow ? "%mask.input" : "%pointer.b")+
                ", "+std::to_string(mask^UINT64_C(32768))+"\n";
    expression+="  %e.projected = and "+type+" %e.scaled, %mask.value\n";
    if(narrow)expression+="  %e.wide = zext i32 %e.projected to i64\n";
    expression+="  %e.term = or i64 "+std::string(narrow ? "%e.wide" : "%e.projected")+", %other.low\n"
                "  %offset.b = add i64 %e.term, "+std::to_string(base);
    replace("  %scaled.b = shl i64 %index.b, 2\n  %offset.b = add i64 %scaled.b, 4008",expression);
    llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);
    if(!module)error.print("mask-memory-refinement-test",llvm::errs());
    REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  ImageTableContextRequest request()const{return {"view",8192,{4352,4608}};}
  std::string ir()const{std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;}
};
llvm::Instruction *maskNamed(llvm::Function &f,llvm::StringRef name){for(auto &b:f)for(auto &i:b)if(i.getName()==name)return &i;return nullptr;}
unsigned guestStores(llvm::Function &f){
  auto *guest=maskNamed(f,"guest");REQUIRE(guest);unsigned n=0;
  for(auto &b:f)for(auto &i:b)if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i))
    if(auto *p=llvm::dyn_cast<llvm::GetElementPtrInst>(s->getPointerOperand());p&&p->getPointerOperand()==guest)++n;
  return n;
}
void upstreamEControl(const llvm::Module &original){
  auto control=llvm::CloneModule(original);auto *view=control->getFunction("view");REQUIRE(view);
  llvm::LoopAnalysisManager loops;llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;llvm::ModuleAnalysisManager modules;llvm::PassBuilder builder;
  builder.registerLoopAnalyses(loops);builder.registerFunctionAnalyses(functions);
  builder.registerCGSCCAnalyses(cgscc);builder.registerModuleAnalyses(modules);
  builder.crossRegisterProxies(loops,functions,cgscc,modules);
  llvm::FunctionPassManager frontend;frontend.addPass(llvm::PromotePass());frontend.addPass(llvm::InstCombinePass());frontend.run(*view,functions);
  REQUIRE_FALSE(llvm::verifyModule(*control,&llvm::errs()));
  auto *e=maskNamed(*view,"e.b");REQUIRE(e);REQUIRE(llvm::isa<llvm::LoadInst>(e));
}
struct MaskIdentities{
  llvm::Function *view,*ordinary;std::vector<llvm::Argument*>args;
  std::vector<llvm::BasicBlock*>blocks;std::vector<llvm::Instruction*>instructions;
  explicit MaskIdentities(llvm::Module&m):view(m.getFunction("view")),ordinary(m.getFunction("ordinary")){
    for(auto&a:view->args())args.push_back(&a);for(auto&b:*view){blocks.push_back(&b);for(auto&i:b)instructions.push_back(&i);}
  }
  void functions(llvm::Module&m)const{
    CHECK(m.getFunction("view")==view);CHECK(m.getFunction("ordinary")==ordinary);
    unsigned n=0;for(auto&a:view->args()){REQUIRE(n<args.size());CHECK(&a==args[n++]);}CHECK(n==args.size());
  }
  void all(llvm::Module&m)const{
    functions(m);std::vector<llvm::BasicBlock*>b;std::vector<llvm::Instruction*>i;
    for(auto&bb:*view){b.push_back(&bb);for(auto&ii:bb)i.push_back(&ii);}CHECK(b==blocks);CHECK(i==instructions);
  }
};
}
TEST_CASE("Loop image refines low-discarding masks only for ambiguous write spans","[llvm][image-loop-mask-memory]"){
  for(auto mode:{MaskMemoryFixture::Mask32,MaskMemoryFixture::Mask64,MaskMemoryFixture::Mask16,
                 MaskMemoryFixture::Typed32,MaskMemoryFixture::AlreadyBoundedNoDefinition,
                 MaskMemoryFixture::LowMaskNoDefinition})DYNAMIC_SECTION("mode "<<mode){
    MaskMemoryFixture f(mode);MaskIdentities ids(*f.module);const auto raw=f.ir();
    upstreamEControl(*f.module);CHECK(f.ir()==raw);const auto stores=guestStores(*ids.view);
    auto *note=f.module->getOrInsertNamedMetadata("caller.note");note->addOperand(llvm::MDNode::get(f.context,{}));
    auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
    REQUIRE(result.closed);REQUIRE(result.changed);CHECK_FALSE(result.budgetExhausted);CHECK(result.work<=1000000);
    ids.functions(*f.module);CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);
    CHECK(guestStores(*ids.view)==stores);
    auto *e=llvm::dyn_cast_or_null<llvm::LoadInst>(maskNamed(*ids.view,"e.b"));REQUIRE(e);CHECK(e->isSimple());CHECK_FALSE(e->use_empty());
    CHECK(e->getType()->isIntegerTy(mode==MaskMemoryFixture::Typed32?32:64));
    auto *pointer=maskNamed(*ids.view,"pointer.b");REQUIRE(pointer);CHECK(llvm::isa<llvm::LoadInst>(pointer));
    unsigned targets=0;for(auto&b:*ids.view)for(auto&i:b)targets+=llvm::isa<llvm::LoadInst>(i)&&i.getName().starts_with("target");CHECK(targets>=3);
    const auto after=f.ir();CHECK(after.find("call void @ordinary")!=std::string::npos);
    CHECK(after.find("dragon.loop.image.private")==std::string::npos);CHECK(after.find("llvm.assume")==std::string::npos);CHECK(after.find("unreachable")==std::string::npos);
    REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
  }
}
TEST_CASE("Loop image mask refinement keeps initialization alias and hazard refusals","[llvm][image-loop-mask-memory]"){
  for(auto mode:{MaskMemoryFixture::TooWide,MaskMemoryFixture::PartialE,MaskMemoryFixture::MissingEntry,
                 MaskMemoryFixture::UnknownCall,MaskMemoryFixture::CyclicE,MaskMemoryFixture::Overshift,
                 MaskMemoryFixture::VolatileE,MaskMemoryFixture::AtomicE,MaskMemoryFixture::PoisonOther,
                 MaskMemoryFixture::Fence})DYNAMIC_SECTION("mode "<<mode){
    MaskMemoryFixture f(mode);MaskIdentities ids(*f.module);auto *note=f.module->getOrInsertNamedMetadata("caller.note");
    note->addOperand(llvm::MDNode::get(f.context,{}));const auto before=f.ir();const auto stores=guestStores(*ids.view);
    auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
    CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(result.work<=1000000);CHECK(f.ir()==before);
    ids.all(*f.module);CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);CHECK(guestStores(*ids.view)==stores);
    REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
  }
}
