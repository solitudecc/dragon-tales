#include <catch2/catch_test_macros.hpp>
#include "../../lib/GuestMemoryEdges.hpp"
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <string>

using namespace dragon::llvmlifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Bad { None, Offset, Crossed, Base, Reload, UndefPointer, PoisonPointer,
                 UndefFrame, Inbounds, IndexFlag, CallAfter, AliasAfter, Volatile,
                 PureCycle, DeepIndex };
struct Config { bool cyclic = false, historical = false, callBefore = false;
  std::uint64_t offset = 16; Bad bad = Bad::None; };
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"pointer-phi", context};
  llvm::Function *function;
  llvm::BasicBlock *known, *join, *checked;
  llvm::Value *base;
  llvm::LoadInst *load;
  llvm::StoreInst *store;
  explicit Fixture(Config c = {}) {
    module.setDataLayout("e-p:64:64");
    auto *ptr = llvm::PointerType::getUnqual(context); auto *i64 = llvm::Type::getInt64Ty(context);
    function = llvm::Function::Create(llvm::FunctionType::get(i64,
        {ptr,ptr,i64,i64,llvm::Type::getInt1Ty(context),llvm::Type::getInt1Ty(context)},false),
        llvm::GlobalValue::ExternalLinkage,"pointer_phi",module);
    auto block=[&](const char *n){return llvm::BasicBlock::Create(context,n,function);};
    auto *entry=block("entry"), *a=block("anchor.a"), *bblock=block("anchor.b");
    auto *pair=block("paired"), *second=c.cyclic?block("paired.second"):nullptr;
    known=block("known.store"); join=block("checked.join"); checked=block("checked.load");
    auto *unknown=block("unknown"), *exit=block("exit");
    auto *pure=c.bad==Bad::PureCycle?block("unanchored"):nullptr;
    Builder b(entry);
    base=b.CreateLoad(ptr,function->getArg(1),"guest.base"); llvm::cast<llvm::LoadInst>(base)->setAlignment(llvm::Align(1));
    llvm::Value *otherBase=base;
    if(c.bad==Bad::Base)otherBase=function->getArg(0);
    if(c.bad==Bad::Reload){auto *reload=b.CreateLoad(ptr,function->getArg(1),"other.base");reload->setAlignment(llvm::Align(1));otherBase=reload;}
    llvm::Value *fa=function->getArg(2);
    if(c.bad==Bad::UndefFrame)fa=b.CreateAdd(llvm::UndefValue::get(i64),b.getInt64(0),"undefined.frame");
    if(c.bad==Bad::DeepIndex)for(unsigned n=0;n<80;++n)fa=b.CreateAdd(fa,b.getInt64(n+1),"deep.index");
    auto *fb=b.CreateAdd(function->getArg(2),b.getInt64(8),"frame.b");
    auto *unknownIndex=b.CreateAdd(function->getArg(2),b.getInt64(c.offset),"unknown.index");
    auto *unknownPointer=b.CreateGEP(b.getInt8Ty(),base,unknownIndex,"unknown.pointer");
    auto *route=b.CreateSwitch(function->getArg(3),unknown,2);route->addCase(b.getInt64(0),a);route->addCase(b.getInt64(1),bblock);
    b.SetInsertPoint(a);
    auto *ia=b.CreateAdd(fa,b.getInt64(c.offset),"index.a");
    auto *pa=b.CreateGEP(b.getInt8Ty(),base,ia,"pointer.a"); b.CreateBr(pair);
    b.SetInsertPoint(bblock);
    auto *ib=b.CreateAdd(fb,b.getInt64(c.offset+(c.bad==Bad::Offset?8:0)),"index.b");
    if(c.bad==Bad::IndexFlag)llvm::cast<llvm::BinaryOperator>(ib)->setHasNoUnsignedWrap();
    llvm::Value *pb=b.CreateGEP(b.getInt8Ty(),otherBase,ib,"pointer.b");
    if(c.bad==Bad::Inbounds)llvm::cast<llvm::GetElementPtrInst>(pb)->setIsInBounds(true);
    if(c.bad==Bad::UndefPointer)pb=llvm::UndefValue::get(ptr);
    if(c.bad==Bad::PoisonPointer)pb=llvm::PoisonValue::get(ptr);
    b.CreateBr(second?second:pair);
    b.SetInsertPoint(pair);
    auto *frame=b.CreatePHI(i64,3,"paired.frame"); auto *pointer=b.CreatePHI(ptr,3,"paired.pointer");
    frame->addIncoming(c.bad==Bad::Crossed?static_cast<llvm::Value *>(fb):fa,a);
    pointer->addIncoming(pa,a);
    llvm::PHINode *secondFrame=nullptr,*secondPointer=nullptr;
    if(second){
      b.SetInsertPoint(second);secondFrame=b.CreatePHI(i64,2,"second.frame");secondPointer=b.CreatePHI(ptr,2,"second.pointer");
      secondFrame->addIncoming(c.bad==Bad::Crossed?fa:fb,bblock);secondPointer->addIncoming(pb,bblock);
      secondFrame->addIncoming(frame,pair);secondPointer->addIncoming(pointer,pair);b.CreateBr(pair);
      frame->addIncoming(secondFrame,second);pointer->addIncoming(secondPointer,second);
    }else{
      frame->addIncoming(c.bad==Bad::Crossed?fa:fb,bblock);pointer->addIncoming(pb,bblock);
    }
    if(pure){
      b.SetInsertPoint(pure);auto *pf=b.CreatePHI(i64,1,"pure.frame");auto *pp=b.CreatePHI(ptr,1,"pure.pointer");
      pf->addIncoming(pf,pure);pp->addIncoming(pp,pure);b.CreateCondBr(function->getArg(4),pure,pair);
      frame->addIncoming(pf,pure);pointer->addIncoming(pp,pure);
    }
    b.SetInsertPoint(pair);
    if(c.historical)b.CreateBr(join);
    else if(second)b.CreateCondBr(function->getArg(4),second,known);
    else b.CreateBr(known);
    b.SetInsertPoint(unknown);b.CreateBr(join);
    b.SetInsertPoint(join);
    auto *selected=b.CreatePHI(i64,3,"selected.frame");
    auto *selectedPointer=b.CreatePHI(ptr,3,"selected.pointer");
    auto *source=c.historical?pair:known;
    selected->addIncoming(frame,source);selectedPointer->addIncoming(pointer,source);
    selected->addIncoming(function->getArg(2),unknown);selectedPointer->addIncoming(unknownPointer,unknown);
    auto *loadedIndex=b.CreateAdd(selected,b.getInt64(c.offset),"loaded.index");
    b.CreateCondBr(b.CreateICmpULT(loadedIndex,function->getArg(3)),checked,exit);
    b.SetInsertPoint(checked);auto *loadedPointer=b.CreateGEP(b.getInt8Ty(),base,loadedIndex);
    load=b.CreateLoad(i64,loadedPointer,"actual.load");load->setAlignment(llvm::Align(1));
    if(c.historical)b.CreateCondBr(function->getArg(5),known,exit);else b.CreateBr(exit);
    b.SetInsertPoint(known);
    auto call=[&]{auto callee=module.getOrInsertFunction("unknown_writer",llvm::FunctionType::get(b.getVoidTy(),{ptr},false));b.CreateCall(callee,{base});};
    if(c.callBefore)call();
    store=b.CreateStore(b.getInt64(14),c.historical?static_cast<llvm::Value *>(selectedPointer):pointer);
    store->setAlignment(llvm::Align(1));if(c.bad==Bad::Volatile)store->setVolatile(true);
    if(c.bad==Bad::CallAfter)call();
    if(c.bad==Bad::AliasAfter)b.CreateStore(b.getInt64(99),function->getArg(0))->setAlignment(llvm::Align(1));
    if(c.historical){
      // A history hub imports old checked.join frame/pointer values. Both
      // are also fresh definitions in the corridor copied on the next entry.
      auto *hub=block("history.hub"),*other=block("history.other");
      b.CreateBr(hub);
      auto *branch=llvm::cast<llvm::BranchInst>(checked->getTerminator());branch->setSuccessor(1,other);
      b.SetInsertPoint(other);b.CreateBr(hub);
      b.SetInsertPoint(hub);auto *hf=b.CreatePHI(i64,2,"history.frame");auto *hp=b.CreatePHI(ptr,2,"history.pointer");
      hf->addIncoming(selected,known);hp->addIncoming(selectedPointer,known);
      hf->addIncoming(selected,other);hp->addIncoming(selectedPointer,other);b.CreateBr(join);
      selected->addIncoming(hf,hub);selectedPointer->addIncoming(hp,hub);
    }else b.CreateBr(join);
    b.SetInsertPoint(exit);auto *result=b.CreatePHI(i64,2,"result");
    result->addIncoming(b.getInt64(0),join);
    if(!c.historical)result->addIncoming(load,checked);
    b.CreateRet(result);
  }
};
void verify(Fixture &f){REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));}
std::string text(Fixture &f){std::string s;llvm::raw_string_ostream out(s);f.module.print(out,nullptr);return s;}
MemoryForwardStats run(Fixture &f,detail::CheckedMemoryEdgeBudget &budget){MemoryForwardStats s;
  detail::forwardCheckedMemoryEdges(*f.function,{f.base},f.module.getDataLayout(),MemoryForwardOptions{65536,false},s,budget);return s;}
}
TEST_CASE("Checked Guest pointer PHIs prove correlated incoming addresses", "[llvm][guest-memory-ssa][guest-pointer-phi]"){
  for(bool cycle:{false,true})for(std::uint64_t offset:{std::uint64_t(16),~std::uint64_t(7),std::uint64_t(1)<<63})
    DYNAMIC_SECTION("cycle "<<cycle<<" offset "<<offset){Config c;c.cyclic=cycle;c.offset=offset;Fixture f(c);verify(f);
      detail::CheckedMemoryEdgeBudget budget;auto s=run(f,budget);verify(f);CHECK(s.complete);REQUIRE(s.loadsForwarded==1);CHECK(s.changed);
      CHECK(f.load->getParent()==f.checked);CHECK(f.store->getParent()==f.known);CHECK(f.known->getTerminator()->getSuccessor(0)!=f.join);
    }
}
TEST_CASE("Checked Guest pointer PHIs keep wrong incoming bases poison and effects unknown", "[llvm][guest-memory-ssa][guest-pointer-phi]"){
  for(bool cycle:{false,true})for(auto bad:{Bad::Offset,Bad::Crossed,Bad::Base,Bad::Reload,Bad::UndefPointer,Bad::PoisonPointer,Bad::UndefFrame,Bad::Inbounds,Bad::IndexFlag,Bad::CallAfter,Bad::AliasAfter,Bad::Volatile})
    DYNAMIC_SECTION("cycle "<<cycle<<" bad "<<static_cast<int>(bad)){Config c;c.cyclic=cycle;c.bad=bad;Fixture f(c);verify(f);auto before=text(f);
      detail::CheckedMemoryEdgeBudget budget;auto s=run(f,budget);verify(f);CHECK(s.loadsForwarded==0);CHECK_FALSE(s.changed);CHECK(text(f)==before);
    }
}
TEST_CASE("Checked Guest pointer PHIs reject unanchored subcycles", "[llvm][guest-memory-ssa][guest-pointer-phi]"){
  Config c;c.bad=Bad::PureCycle;Fixture f(c);verify(f);auto before=text(f);detail::CheckedMemoryEdgeBudget budget;auto s=run(f,budget);verify(f);
  CHECK(s.loadsForwarded==0);CHECK_FALSE(s.changed);CHECK(text(f)==before);
}
TEST_CASE("Checked Guest pointer PHIs support prior calls and historical entry snapshots", "[llvm][guest-memory-ssa][guest-pointer-phi]"){
  for(bool history:{false,true})DYNAMIC_SECTION("history "<<history){Config c;c.callBefore=!history;c.historical=history;Fixture f(c);verify(f);
    detail::CheckedMemoryEdgeBudget budget;auto s=run(f,budget);verify(f);REQUIRE(s.loadsForwarded==1);CHECK(s.changed);CHECK(s.complete);
    CHECK(f.load->getParent()==f.checked);CHECK(f.store->getParent()==f.known);
  }
}
TEST_CASE("Checked Guest pointer PHIs retain proof budgets", "[llvm][guest-memory-ssa][guest-pointer-phi]"){
  for(bool global:{false,true})DYNAMIC_SECTION("global "<<global){Config c;if(!global)c.bad=Bad::DeepIndex;Fixture f(c);verify(f);auto before=text(f);
    detail::CheckedMemoryEdgeBudget budget;if(global)budget.indexNodes=65536;auto s=run(f,budget);verify(f);
    CHECK(s.loadsForwarded==0);CHECK_FALSE(s.changed);CHECK_FALSE(s.complete);CHECK(s.budgetExhausted>0);CHECK(text(f)==before);
  }
}
