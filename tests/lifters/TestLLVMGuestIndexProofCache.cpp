#include <catch2/catch_test_macros.hpp>
#include "../../lib/GuestMemoryEdges.hpp"
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>
#include <utility>
using namespace dragon::llvmlifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Tail { Normal, StoreNUW, StoreNSW, StoreUndef, StorePoison, DifferentOffset };
struct Fixture {
 llvm::LLVMContext context;
 llvm::Module module{"index-proof-cache", context};
 llvm::Function *function;
 llvm::BasicBlock *known, *unknown, *join, *read, *second = nullptr;
 llvm::Value *base;
 llvm::LoadInst *target;
 explicit Fixture(Tail tail=Tail::Normal, bool secondCandidate=false) {
  module.setDataLayout("e-p:64:64");
  auto *i64=llvm::Type::getInt64Ty(context);auto *ptr=llvm::PointerType::getUnqual(context);
  std::vector<llvm::Type *> arguments{ptr};arguments.insert(arguments.end(),64,i64);
  arguments.push_back(llvm::Type::getInt1Ty(context));arguments.push_back(i64);arguments.push_back(i64);
  function=llvm::Function::Create(llvm::FunctionType::get(i64,arguments,false),llvm::GlobalValue::ExternalLinkage,"cached_history",module);
  auto block=[&](const char *name){return llvm::BasicBlock::Create(context,name,function);};
  auto *entry=block("entry");known=block("known");unknown=block("unknown");join=block("join");read=block("read");auto *fail=block("fail");auto *done=block("done");
  if(secondCandidate)second=block("second.poison.candidate");
  Builder b(entry);base=b.CreateLoad(ptr,function->getArg(0),"guest.base");llvm::cast<llvm::LoadInst>(base)->setAlignment(llvm::Align(1));
  // 64 distinct leaves and 63 balanced additions: below the depth cap and
  // small enough once, but traversing it twice exhausts the 256-node proof.
  std::vector<llvm::Value *> level;
  for(unsigned i=1;i<=64;++i)level.push_back(function->getArg(i));
  while(level.size()>1){std::vector<llvm::Value *> next;for(unsigned i=0;i<level.size();i+=2)next.push_back(b.CreateAdd(level[i],level[i+1],"frame.part"));level=std::move(next);}
  auto *frame=level.front();
  llvm::Value *storedFrame=frame;
  if(tail==Tail::StoreUndef||tail==Tail::StorePoison)
   storedFrame=b.CreateAdd(frame,tail==Tail::StoreUndef?static_cast<llvm::Value *>(llvm::UndefValue::get(i64)):static_cast<llvm::Value *>(llvm::PoisonValue::get(i64)),"bad.store.frame");
  auto *index=llvm::cast<llvm::BinaryOperator>(b.CreateAdd(storedFrame,b.getInt64(tail==Tail::DifferentOffset?24:16),"store.index"));
  if(tail==Tail::StoreNUW)index->setHasNoUnsignedWrap();
  if(tail==Tail::StoreNSW)index->setHasNoSignedWrap();
  auto *stored=b.CreateGEP(b.getInt8Ty(),base,index,"stored.ptr");
  llvm::Value *secondFrame=nullptr,*secondAddress=nullptr;
  if(secondCandidate){
   secondFrame=b.CreateAdd(frame,llvm::PoisonValue::get(i64),"second.frame");
   auto *secondIndex=b.CreateAdd(secondFrame,b.getInt64(16),"second.index");
   secondAddress=b.CreateGEP(b.getInt8Ty(),base,secondIndex,"second.ptr");
  }
  b.CreateCondBr(function->getArg(65),known,unknown);
  b.SetInsertPoint(known);b.CreateStore(b.getInt64(43),stored)->setAlignment(llvm::Align(1));b.CreateBr(join);
  b.SetInsertPoint(unknown);
  if(secondCandidate){b.CreateCondBr(b.CreateICmpEQ(function->getArg(66),b.getInt64(0)),second,join);b.SetInsertPoint(second);b.CreateStore(b.getInt64(44),secondAddress)->setAlignment(llvm::Align(1));b.CreateBr(join);}
  else b.CreateBr(join);
  b.SetInsertPoint(join);auto *selected=b.CreatePHI(i64,2,"selected.frame");selected->addIncoming(frame,known);selected->addIncoming(function->getArg(66),unknown);
  if(secondCandidate)selected->addIncoming(secondFrame,second);
  auto *loadIndex=b.CreateAdd(selected,b.getInt64(16),"load.index");b.CreateCondBr(b.CreateICmpULT(loadIndex,function->getArg(67)),read,fail);
  b.SetInsertPoint(read);auto *address=b.CreateGEP(b.getInt8Ty(),base,loadIndex,"loaded.ptr");target=b.CreateLoad(i64,address,"target");target->setAlignment(llvm::Align(1));b.CreateBr(done);
  b.SetInsertPoint(fail);b.CreateBr(done);
  b.SetInsertPoint(done);auto *result=b.CreatePHI(i64,2,"result");result->addIncoming(target,read);result->addIncoming(b.getInt64(0),fail);b.CreateRet(result);
 }
 std::string text(){std::string s;llvm::raw_string_ostream out(s);module.print(out,nullptr);return s;}
 MemoryForwardStats run(detail::CheckedMemoryEdgeBudget &budget){MemoryForwardStats stats;detail::forwardCheckedMemoryEdges(*function,{base},module.getDataLayout(),MemoryForwardOptions{65536,false},stats,budget);return stats;}
};
}
TEST_CASE("Checked index proofs share successful historical dependencies within one candidate", "[llvm][guest-memory-ssa][guest-index-cache]") {
 Fixture f;REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));auto *old=f.known->getTerminator()->getSuccessor(0);
 detail::CheckedMemoryEdgeBudget budget;auto stats=f.run(budget);
 REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));
 CHECK(stats.loadsForwarded==1);CHECK(stats.changed);CHECK(stats.complete);CHECK(stats.budgetExhausted==0);
 CHECK(f.known->getTerminator()->getSuccessor(0)!=old);
 CHECK(f.unknown->getTerminator()->getSuccessor(0)==f.join);
 CHECK(f.target->getParent()==f.read);CHECK(llvm::pred_size(f.join)==1);
 // This is a count of genuinely visited obligations, not a forged complete flag.
 CHECK(budget.indexNodes>0);CHECK(budget.indexNodes<256);
}
TEST_CASE("Checked index dependency reuse retains store-only poison flags and address mismatches", "[llvm][guest-memory-ssa][guest-index-cache]") {
 for(auto tail:{Tail::StoreNUW,Tail::StoreNSW,Tail::StoreUndef,Tail::StorePoison,Tail::DifferentOffset})DYNAMIC_SECTION(static_cast<int>(tail)) {
  Fixture f(tail);REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));auto before=f.text();detail::CheckedMemoryEdgeBudget budget;auto stats=f.run(budget);
  REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));CHECK(stats.loadsForwarded==0);CHECK_FALSE(stats.changed);CHECK(f.text()==before);
 }
}

TEST_CASE("Checked index dependency caches do not survive a candidate or invocation", "[llvm][guest-memory-ssa][guest-index-cache]") {
 Fixture f(Tail::Normal,true);REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));
 detail::CheckedMemoryEdgeBudget firstBudget;auto first=f.run(firstBudget);
 REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));CHECK(first.loadsForwarded==1);
 CHECK(f.second->getTerminator()->getSuccessor(0)==f.join);
 auto beforeSecond=f.text();detail::CheckedMemoryEdgeBudget secondBudget;auto second=f.run(secondBudget);
 REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));CHECK(second.loadsForwarded==0);CHECK_FALSE(second.changed);CHECK(f.text()==beforeSecond);
 CHECK(f.second->getTerminator()->getSuccessor(0)==f.join);
}
