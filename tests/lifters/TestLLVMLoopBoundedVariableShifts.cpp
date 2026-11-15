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
struct VariableShiftFixture {
  enum Mode { Right32_28, Right32_31, Right64_63, LiteralRight31, LiteralLeft3,
              Arithmetic32, WrapLeft64, SingletonRight,
              BadRightUpperC, BadRightLowerTable, BadLeftUpperC,
              Width32, Width64, Excess32, UndefCount };
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  explicit VariableShiftFixture(Mode mode=Right32_28) {
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
    auto replace=[&](const std::string &from,const std::string &to){
      std::size_t at=0;unsigned count=0;
      while((at=text.find(from,at))!=std::string::npos){text.replace(at,from.size(),to);at+=to.size();++count;}
      REQUIRE(count>0);
    };
    const bool narrow=mode==Right32_28 || mode==Right32_31 || mode==Arithmetic32 ||
                      mode==Width32 || mode==Excess32 || mode==UndefCount;
    const bool left=mode==LiteralLeft3 || mode==WrapLeft64 || mode==BadLeftUpperC;
    const bool arithmetic=mode==Arithmetic32;
    const bool endpoint=mode==BadRightUpperC || mode==BadRightLowerTable || mode==BadLeftUpperC;
    const bool finalMask=!endpoint && mode!=LiteralRight31 && mode!=LiteralLeft3;
    const std::string type=narrow ? "i32" : "i64";
    const unsigned countMask=mode==Right32_28 || mode==Arithmetic32 || mode==UndefCount ? 28 :
        mode==Right32_31 ? 31 : mode==Right64_63 || mode==Excess32 ? 63 :
        mode==Width32 ? 32 : mode==Width64 ? 64 : 3;
    replace("INITIAL_STATE","0");replace("B_STATE","1");replace("INDEX_MASK","7");
    replace("; FIRST_CANDIDATE","");replace("; FIRST_READ","");
    replace("; INITIAL_INDEX","store i64 22, ptr %indexcell, align 1\n"
        "  %ecell = getelementptr i8, ptr %guest, i64 1024\n  store i64 22, ptr %ecell, align 1");
    replace("  %pointer = load i64, ptr %cell, align 1\n","");
    std::string code="store i64 %bounded, ptr %indexcell, align 1\n"
        "  %pointer = load i64, ptr %cell, align 1\n"
        "  %count.address = getelementptr i8, ptr %guest, i64 192\n"
        "  %count.input64 = load i64, ptr %count.address, align 1\n";
    if(narrow)code+="  %input.narrow = trunc i64 %data to i32\n"
                    "  %count.input = trunc i64 %count.input64 to i32\n"
                    "  %count.mask.input = trunc i64 %pointer to i32\n";
    if(endpoint){
      code+="  %count.choose = icmp eq i64 %count.input64, 0\n";
      if(mode==BadRightUpperC){
        code+="  %count.one = xor i64 %pointer, 32769\n"
              "  %count.two = sub i64 %pointer, 32766\n"
              "  %count.value = select i1 %count.choose, i64 %count.one, i64 %count.two\n";
      } else {
        code+="  %count.zero = sub i64 %pointer, 32768\n"
              "  %count.one = xor i64 %pointer, 32769\n"
              "  %count.value = select i1 %count.choose, i64 %count.zero, i64 %count.one\n";
      }
    } else if(mode==SingletonRight) {
      code+="  %count.value = xor i64 %pointer, 32770\n";
    } else {
      code+="  %count.mask = xor "+type+" "+std::string(narrow ? "%count.mask.input" : "%pointer")+
            ", "+std::to_string(32768u^countMask)+"\n";
      std::string raw=narrow ? "%count.input" : "%count.input64";
      if(mode==Right32_28 || mode==Arithmetic32 || mode==Width32){
        code+="  %count.scaled = shl i32 %count.input, 2\n";raw="%count.scaled";
      }
      if(mode==UndefCount){
        code+="  br label %count.loop\ncount.loop:\n"
              "  %count.phi = phi i32 [ undef, %a ], [ %count.next, %count.back ]\n"
              "  %count.next = xor i32 %count.phi, %count.input\n"
              "  %count.again = icmp eq i64 %count.input64, 0\n"
              "  br i1 %count.again, label %count.back, label %count.use\n"
              "count.back:\n  %count.trace = getelementptr i8, ptr %guest, i64 1568\n"
              "  store i32 %count.next, ptr %count.trace, align 1\n  br label %count.loop\ncount.use:\n";
        raw="%count.phi";
      }
      code+="  %count.value = and "+type+" "+raw+", %count.mask\n";
    }
    const std::string input=mode==LiteralRight31 ? "31" : mode==LiteralLeft3 ? "3" :
        mode==BadRightUpperC ? "84" : mode==BadRightLowerTable ? "2132" :
        mode==BadLeftUpperC ? "21" : narrow ? "%input.narrow" : "%data";
    code+="  %shift.value = "+std::string(left ? "shl" : arithmetic ? "ashr" : "lshr")+
          " "+type+" "+input+", %count.value\n";
    if(narrow)code+="  %shift.trace.value = zext i32 %shift.value to i64\n";
    code+="  %shift.trace.address = getelementptr i8, ptr %guest, i64 1552\n"
          "  store i64 "+std::string(narrow ? "%shift.trace.value" : "%shift.value")+
          ", ptr %shift.trace.address, align 1\n";
    std::string bound="%shift.value";
    if(finalMask){code+="  %e.low = and "+type+" %shift.value, 15\n";bound="%e.low";}
    if(narrow){code+="  %e.wide = zext i32 "+bound+" to i64\n";bound="%e.wide";}
    code+="  store i64 "+bound+", ptr %ecell, align 1";
    replace("; MASK_STORE",code);
    replace("; EXTRA_EFFECT","");replace("; RESTORE_EFFECT","");replace("; AFTER_B_LOAD","");
    replace("b:\n","b:\n  %e.b = load i64, ptr %ecell, align 1\n");
    replace("  %scaled.b = shl i64 %index.b, 2\n  %offset.b = add i64 %scaled.b, 4008",
            "  %work.e.scale = shl i64 %e.b, 2\n  %offset.b = add i64 %work.e.scale, 3928");
    llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);
    if(!module)error.print("bounded-variable-shift-test",llvm::errs());
    REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  ImageTableContextRequest request()const{return {"view",8192,{4352,4608}};}
  std::string ir()const{std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;}
};
llvm::Instruction *shiftNamed(llvm::Function &f,llvm::StringRef name){for(auto &b:f)for(auto &i:b)if(i.getName()==name)return &i;return nullptr;}
unsigned shiftGuestStores(llvm::Function &f){
  auto *guest=shiftNamed(f,"guest");REQUIRE(guest);unsigned n=0;
  for(auto&b:f)for(auto&i:b)if(auto*s=llvm::dyn_cast<llvm::StoreInst>(&i))
    if(auto*p=llvm::dyn_cast<llvm::GetElementPtrInst>(s->getPointerOperand());p&&p->getPointerOperand()==guest)++n;
  return n;
}
void upstreamShiftControl(const llvm::Module &original,bool requireUndef=false){
  auto control=llvm::CloneModule(original);auto *f=control->getFunction("view");REQUIRE(f);
  llvm::LoopAnalysisManager loops;llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;llvm::ModuleAnalysisManager modules;llvm::PassBuilder builder;
  builder.registerLoopAnalyses(loops);builder.registerFunctionAnalyses(functions);builder.registerCGSCCAnalyses(cgscc);builder.registerModuleAnalyses(modules);
  builder.crossRegisterProxies(loops,functions,cgscc,modules);
  llvm::FunctionPassManager passes;passes.addPass(llvm::PromotePass());passes.addPass(llvm::InstCombinePass());passes.run(*f,functions);
  REQUIRE_FALSE(llvm::verifyModule(*control,&llvm::errs()));
  auto *shift=llvm::dyn_cast_or_null<llvm::BinaryOperator>(shiftNamed(*f,"shift.value"));REQUIRE(shift);
  REQUIRE((shift->getOpcode()==llvm::Instruction::LShr || shift->getOpcode()==llvm::Instruction::Shl || shift->getOpcode()==llvm::Instruction::AShr));
  REQUIRE_FALSE(llvm::isa<llvm::ConstantInt>(shift->getOperand(1)));
  auto *e=shiftNamed(*f,"e.b");REQUIRE(e);REQUIRE(llvm::isa<llvm::LoadInst>(e));
  if(requireUndef){
    bool visible=false;for(auto&b:*f)for(auto&i:b)for(auto&o:i.operands())visible|=llvm::isa<llvm::UndefValue>(o.get()) && !llvm::isa<llvm::PoisonValue>(o.get());
    REQUIRE(visible); // Never call an optimized-away undef a hazard test.
  }
}
struct ShiftIdentities{
  llvm::Function *view,*ordinary;std::vector<llvm::Argument*>args;std::vector<llvm::BasicBlock*>blocks;std::vector<llvm::Instruction*>instructions;
  explicit ShiftIdentities(llvm::Module&m):view(m.getFunction("view")),ordinary(m.getFunction("ordinary")){
    for(auto&a:view->args())args.push_back(&a);for(auto&b:*view){blocks.push_back(&b);for(auto&i:b)instructions.push_back(&i);}
  }
  void functions(llvm::Module&m)const{
    CHECK(m.getFunction("view")==view);CHECK(m.getFunction("ordinary")==ordinary);unsigned n=0;
    for(auto&a:view->args()){REQUIRE(n<args.size());CHECK(&a==args[n++]);}CHECK(n==args.size());
  }
  void all(llvm::Module&m)const{
    functions(m);std::vector<llvm::BasicBlock*>b;std::vector<llvm::Instruction*>i;
    for(auto&bb:*view){b.push_back(&bb);for(auto&ii:bb)i.push_back(&ii);}CHECK(b==blocks);CHECK(i==instructions);
  }
};
}
TEST_CASE("Loop image accepts bounded nonsingleton shift counts without new assumptions","[llvm][image-loop-variable-shift]"){
  for(auto mode:{VariableShiftFixture::Right32_28,VariableShiftFixture::Right32_31,VariableShiftFixture::Right64_63,
                 VariableShiftFixture::LiteralRight31,VariableShiftFixture::LiteralLeft3,VariableShiftFixture::Arithmetic32,
                 VariableShiftFixture::WrapLeft64,VariableShiftFixture::SingletonRight})DYNAMIC_SECTION("mode "<<mode){
    VariableShiftFixture f(mode);ShiftIdentities ids(*f.module);const auto raw=f.ir();upstreamShiftControl(*f.module);CHECK(f.ir()==raw);
    const auto stores=shiftGuestStores(*ids.view);auto *note=f.module->getOrInsertNamedMetadata("caller.note");note->addOperand(llvm::MDNode::get(f.context,{}));
    auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
    REQUIRE(result.closed);REQUIRE(result.changed);CHECK_FALSE(result.budgetExhausted);CHECK(result.work<=1000000);
    ids.functions(*f.module);CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);CHECK(shiftGuestStores(*ids.view)==stores);
    auto *e=llvm::dyn_cast_or_null<llvm::LoadInst>(shiftNamed(*ids.view,"e.b"));REQUIRE(e);CHECK(e->isSimple());CHECK_FALSE(e->use_empty());
    auto *shift=llvm::dyn_cast_or_null<llvm::BinaryOperator>(shiftNamed(*ids.view,"shift.value"));REQUIRE(shift);
    CHECK((shift->getOpcode()==llvm::Instruction::LShr || shift->getOpcode()==llvm::Instruction::Shl || shift->getOpcode()==llvm::Instruction::AShr));
    CHECK_FALSE(shift->use_empty());
    unsigned targets=0;for(auto&b:*ids.view)for(auto&i:b)targets+=llvm::isa<llvm::LoadInst>(i)&&i.getName().starts_with("target");CHECK(targets>=3);
    const auto after=f.ir();CHECK(after.find("call void @ordinary")!=std::string::npos);CHECK(after.find("dragon.loop.image.private")==std::string::npos);
    CHECK(after.find("llvm.assume")==std::string::npos);CHECK(after.find("unreachable")==std::string::npos);
    REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
  }
}
TEST_CASE("Loop image variable shift ranges reject clobbering endpoints and illegal counts","[llvm][image-loop-variable-shift]"){
  for(auto mode:{VariableShiftFixture::BadRightUpperC,VariableShiftFixture::BadRightLowerTable,VariableShiftFixture::BadLeftUpperC,
                 VariableShiftFixture::Width32,VariableShiftFixture::Width64,VariableShiftFixture::Excess32,
                 VariableShiftFixture::UndefCount})DYNAMIC_SECTION("mode "<<mode){
    VariableShiftFixture f(mode);ShiftIdentities ids(*f.module);const auto raw=f.ir();upstreamShiftControl(*f.module,mode==VariableShiftFixture::UndefCount);CHECK(f.ir()==raw);
    const auto stores=shiftGuestStores(*ids.view);auto *note=f.module->getOrInsertNamedMetadata("caller.note");note->addOperand(llvm::MDNode::get(f.context,{}));const auto before=f.ir();
    auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
    CHECK_FALSE(result.changed);CHECK_FALSE(result.closed);CHECK(result.work<=1000000);CHECK(f.ir()==before);ids.all(*f.module);
    CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);CHECK(shiftGuestStores(*ids.view)==stores);
    REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
  }
}
