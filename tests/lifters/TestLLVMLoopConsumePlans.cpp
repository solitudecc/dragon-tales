#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
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
#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
using namespace dragon::llvmlifter::detail;
namespace {
struct ConsumePlanFixture {
  enum Mode { Normal, UnknownCall, Poison, IllegalShift };
  static constexpr unsigned ChainBlocks=768;
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  std::vector<std::uint64_t> expectedTrace;
  explicit ConsumePlanFixture(Mode mode=Normal) {
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
    replace("INITIAL_STATE","0");replace("B_STATE","1");replace("INDEX_MASK","7");
    replace("; FIRST_CANDIDATE","");replace("; FIRST_READ","");
    replace("; INITIAL_INDEX","store i64 22, ptr %indexcell, align 1");
    replace("; MASK_STORE","store i64 %bounded, ptr %indexcell, align 1");
    replace("; EXTRA_EFFECT",mode==UnknownCall ? "call void @unknown_call(ptr %guest)" : "");
    replace("; RESTORE_EFFECT","");replace("; AFTER_B_LOAD","");
    replace("  %scaled.b = shl i64 %index.b, 2", "  %bounded.b = and i64 %index.b, 7\n  %scaled.b = shl i64 %bounded.b, 2");
    // Logical chain0->...->chain767; physically declared in reverse order.
    // Each operation is flag-free modular i64 arithmetic. Raw C is unknown,
    // so alternating xor/mul cannot fold to an absolute address before H_C.
    std::vector<std::string> blocks;
    std::uint64_t evaluated=32768;
    for(unsigned n=0;n<ChainBlocks;++n){
      const std::uint64_t x=UINT64_C(0x9e3779b97f4a7c15)^((std::uint64_t(n)+1)*UINT64_C(0x100000001b3));
      const std::uint64_t m=(UINT64_C(0xbf58476d1ce4e5b9)+2*std::uint64_t(n))|1;
      evaluated=(evaluated^x)*m;
      expectedTrace.push_back(evaluated);
      const auto previous=n ? "%consume.m."+std::to_string(n-1) : "%consume.seed";
      const auto next=n+1==ChainBlocks ? "%consume.tail" : "%consume.chain."+std::to_string(n+1);
      blocks.push_back("consume.chain."+std::to_string(n)+":\n  %consume.x."+std::to_string(n)+" = xor i64 "+previous+", "+std::to_string(x)+"\n  %consume.m."+std::to_string(n)+" = mul i64 %consume.x."+std::to_string(n)+", "+std::to_string(m)+"\n  %consume.trace.p."+std::to_string(n)+" = getelementptr i8, ptr %guest, i64 "+std::to_string(16384+8*n)+"\n  store i64 %consume.m."+std::to_string(n)+", ptr %consume.trace.p."+std::to_string(n)+", align 1\n  br label "+next+"\n");
    }
    std::string chain="  %pointer = load i64, ptr %cell, align 1\n";
    chain+=mode==Poison ? "  %consume.seed = xor i64 %pointer, poison\n" :
           mode==IllegalShift ? "  %consume.seed = shl i64 %pointer, 64\n" :
           "  %consume.seed = xor i64 %pointer, 0\n";
    chain+="  br label %consume.chain.0\n";
    for(auto it=blocks.rbegin();it!=blocks.rend();++it)chain+=*it;
    chain+="consume.tail:\n  %consume.offset = sub i64 %consume.m."+std::to_string(ChainBlocks-1)+", "+std::to_string(evaluated)+"\n"
           "  %consume.address = add i64 %consume.offset, 32768\n"
           "  %output = getelementptr i8, ptr %guest, i64 %consume.address\n"
           "  store i32 123, ptr %output, align 1";
    replace("  %pointer = load i64, ptr %cell, align 1\n  %output = getelementptr i8, ptr %guest, i64 %pointer\n  store i32 123, ptr %output, align 1",chain);
    llvm::SMDiagnostic error;module=llvm::parseAssemblyString(text,error,context);
    if(!module)error.print("loop-consume-plan-test",llvm::errs());
    REQUIRE(module);REQUIRE_FALSE(llvm::verifyModule(*module,&llvm::errs()));
  }
  ImageTableContextRequest request()const{return {"view",8192,{4352,4608}};}
  std::string ir()const{std::string s;llvm::raw_string_ostream out(s);module->print(out,nullptr);return s;}
};
llvm::Instruction *consumeNamed(llvm::Function &f,llvm::StringRef name){for(auto &b:f)for(auto &i:b)if(i.getName()==name)return &i;return nullptr;}
unsigned consumeGuestStores(llvm::Function &f){
  auto *guest=consumeNamed(f,"guest");REQUIRE(guest);unsigned n=0;
  for(auto&b:f)for(auto&i:b)if(auto*s=llvm::dyn_cast<llvm::StoreInst>(&i))
    if(auto*p=llvm::dyn_cast<llvm::GetElementPtrInst>(s->getPointerOperand());p&&p->getPointerOperand()==guest)++n;
  return n;
}
struct ConsumeOffset{std::uint64_t constant=0;std::vector<llvm::Value*>dynamic;};
std::optional<ConsumeOffset> consumeGuestOffset(llvm::Value *pointer,llvm::Value *guest){
  ConsumeOffset result;std::set<llvm::Value*>visited;
  for(unsigned depth=0;depth<32;++depth){
    if(pointer==guest)return result;
    if(!visited.insert(pointer).second)return {};
    auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
    if(!gep || gep->getNumIndices()!=1 || !gep->getSourceElementType()->isIntegerTy(8))return {};
    auto *index=gep->idx_begin()->get();if(!index->getType()->isIntegerTy(64))return {};
    if(auto *c=llvm::dyn_cast<llvm::ConstantInt>(index))result.constant+=c->getZExtValue();
    else result.dynamic.push_back(index);
    pointer=gep->getPointerOperand();
  }
  return {};
}
ConsumeOffset consumeActualOutputOffset(llvm::Function &f){
  auto *guest=consumeNamed(f,"guest");REQUIRE(guest);
  llvm::StoreInst *output=nullptr;
  for(auto&b:f)for(auto&i:b)if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)){
    auto *value=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    if(!value || !value->getType()->isIntegerTy(32) || value->getZExtValue()!=123)continue;
    REQUIRE_FALSE(output);output=store;
  }
  REQUIRE(output);
  auto address=consumeGuestOffset(output->getPointerOperand(),guest);REQUIRE(address.has_value());return *address;
}
void consumeTraceEffects(llvm::Function &f,const std::vector<std::uint64_t>&expected,bool consumed){
  auto *guest=consumeNamed(f,"guest");REQUIRE(guest);std::vector<unsigned>seen(expected.size());
  for(auto&b:f)for(auto&i:b)if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)){
    auto at=consumeGuestOffset(store->getPointerOperand(),guest);
    if(!at || !at->dynamic.empty() || at->constant<16384 || at->constant>=16384+8*expected.size())continue;
    REQUIRE((at->constant-16384)%8==0);auto n=(at->constant-16384)/8;REQUIRE(n<seen.size());CHECK(++seen[n]==1);
    CHECK(store->isSimple());CHECK(store->getAlign()==llvm::Align(1));REQUIRE(store->getValueOperand()->getType()->isIntegerTy(64));
    if(consumed){auto *value=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());REQUIRE(value);CHECK(value->getZExtValue()==expected[n]);}
    else{auto *value=llvm::dyn_cast<llvm::BinaryOperator>(store->getValueOperand());REQUIRE(value);CHECK(value->getOpcode()==llvm::Instruction::Mul);CHECK(value->getName()=="consume.m."+std::to_string(n));CHECK(value->getParent()==store->getParent());}
  }
  for(auto count:seen)CHECK(count==1);
}
std::vector<std::pair<std::string,unsigned>> consumeNormalizerControl(const llvm::Module &original,const std::vector<std::uint64_t>&expected){
  auto clone=llvm::CloneModule(original);auto *f=clone->getFunction("view");REQUIRE(f);
  llvm::LoopAnalysisManager loops;llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;llvm::ModuleAnalysisManager modules;llvm::PassBuilder builder;
  builder.registerLoopAnalyses(loops);builder.registerFunctionAnalyses(functions);builder.registerCGSCCAnalyses(cgscc);builder.registerModuleAnalyses(modules);
  builder.crossRegisterProxies(loops,functions,cgscc,modules);
  llvm::FunctionPassManager passes;passes.addPass(llvm::PromotePass());passes.addPass(llvm::InstCombinePass());passes.run(*f,functions);
  REQUIRE_FALSE(llvm::verifyModule(*clone,&llvm::errs()));
  auto *observation=llvm::dyn_cast_or_null<llvm::LoadInst>(consumeNamed(*f,"pointer"));REQUIRE(observation);CHECK_FALSE(observation->use_empty());
  std::vector<std::pair<std::string,unsigned>> chain;
  for(auto&b:*f)for(auto&i:b)if(i.getName().starts_with("consume.x.")||i.getName().starts_with("consume.m."))chain.emplace_back(i.getName().str(),i.getOpcode());
  REQUIRE(chain.size()>=ConsumePlanFixture::ChainBlocks);
  const auto output=consumeActualOutputOffset(*f);CHECK_FALSE(output.dynamic.empty());
  consumeTraceEffects(*f,expected,false);
  llvm::DominatorTree dom(*f);std::map<llvm::BasicBlock*,unsigned>ordinals;unsigned ordinal=0;
  for(auto&b:*f)ordinals.emplace(&b,ordinal++);
  llvm::Instruction *previous=nullptr;
  for(unsigned n=0;n<ConsumePlanFixture::ChainBlocks;++n){
    auto *current=consumeNamed(*f,"consume.m."+std::to_string(n));REQUIRE(current);
    CHECK(current->getParent()->getName()=="consume.chain."+std::to_string(n));
    if(previous){CHECK(dom.dominates(previous,current));CHECK(ordinals.at(previous->getParent())>ordinals.at(current->getParent()));
      auto *branch=llvm::dyn_cast<llvm::BranchInst>(previous->getParent()->getTerminator());REQUIRE(branch);REQUIRE(branch->isUnconditional());CHECK(branch->getSuccessor(0)==current->getParent());}
    previous=current;
  }
  return chain;
}
struct ConsumeIdentities{
  llvm::Function *view,*ordinary;std::vector<llvm::Argument*>args;std::vector<llvm::BasicBlock*>blocks;std::vector<llvm::Instruction*>instructions;
  explicit ConsumeIdentities(llvm::Module&m):view(m.getFunction("view")),ordinary(m.getFunction("ordinary")){
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
TEST_CASE("Loop image consumes proved reverse-order nonlinear constants without repeated folding","[llvm][image-loop-consume-plan]"){
  ConsumePlanFixture f;ConsumeIdentities ids(*f.module);const auto raw=f.ir();const auto chain=consumeNormalizerControl(*f.module,f.expectedTrace);CHECK(f.ir()==raw);
  const auto stores=consumeGuestStores(*ids.view);auto *note=f.module->getOrInsertNamedMetadata("caller.note");note->addOperand(llvm::MDNode::get(f.context,{}));
  auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);INFO(result.work);
  REQUIRE(result.closed);REQUIRE(result.changed);CHECK_FALSE(result.budgetExhausted);CHECK(result.work<=1000000);CHECK(result.constantValues>0);
  ids.functions(*f.module);CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);CHECK(consumeGuestStores(*ids.view)==stores);
  for(const auto&[name,opcode]:chain){auto *i=consumeNamed(*ids.view,name);INFO(name);REQUIRE(i);CHECK(i->getOpcode()==opcode);}
  for(auto name:{"pointer","pointer.b"}){auto *i=llvm::dyn_cast_or_null<llvm::LoadInst>(consumeNamed(*ids.view,name));INFO(name);REQUIRE(i);CHECK(i->isSimple());}
  const auto output=consumeActualOutputOffset(*ids.view);CHECK(output.dynamic.empty());CHECK(output.constant==32768);
  consumeTraceEffects(*ids.view,f.expectedTrace,true);
  unsigned targets=0;for(auto&b:*ids.view)for(auto&i:b)targets+=llvm::isa<llvm::LoadInst>(i)&&i.getName().starts_with("target");CHECK(targets>=3);
  const auto after=f.ir();CHECK(after.find("call void @ordinary")!=std::string::npos);CHECK(after.find("dragon.loop.image.private")==std::string::npos);
  CHECK(after.find("llvm.assume")==std::string::npos);CHECK(after.find("unreachable")==std::string::npos);REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
}
TEST_CASE("Loop image constant consumption preserves all identities on refusal and low budget","[llvm][image-loop-consume-plan]"){
  for(auto mode:{ConsumePlanFixture::UnknownCall,ConsumePlanFixture::Poison,ConsumePlanFixture::IllegalShift})DYNAMIC_SECTION("mode "<<mode){
    ConsumePlanFixture f(mode);ConsumeIdentities ids(*f.module);const auto stores=consumeGuestStores(*ids.view);
    auto *note=f.module->getOrInsertNamedMetadata("caller.note");note->addOperand(llvm::MDNode::get(f.context,{}));const auto before=f.ir();
    auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);INFO(result.work);
    CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(f.ir()==before);ids.all(*f.module);CHECK(consumeGuestStores(*ids.view)==stores);
    CHECK(f.module->getNamedMetadata("caller.note")==note);CHECK(note->getNumOperands()==1);CHECK(result.work<=1000000);REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
  }
  SECTION("bounded shared work failure has no publication"){
    ConsumePlanFixture f;ConsumeIdentities ids(*f.module);const auto before=f.ir();auto request=f.request();request.maxWork=128;
    auto result=runLoopImageTableContext(*f.module,request);INFO(result.message);INFO(result.work);
    CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(result.budgetExhausted);CHECK(result.work==request.maxWork);CHECK(f.ir()==before);ids.all(*f.module);REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
  }
}
