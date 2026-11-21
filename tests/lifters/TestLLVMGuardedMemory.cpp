#include "LoopRangeArithmeticFixture.hpp"
#include "../../lib/LLVMImageTableContext.hpp"
#include <llvm/IR/IRBuilder.h>

namespace {
struct GuardedMemoryFixture {
  enum Mode { Normal, Clobber, Restore, Alias, Call, Atomic, Bypass, Unchecked };
  ArithmeticFixture source{ArithmeticFixture::LowOnesSelf};
  llvm::Function *function=nullptr;
  llvm::LoadInst *observation=nullptr;
  explicit GuardedMemoryFixture(Mode mode=Normal) {
    auto proof=runLoopImageTableContext(*source.module,source.request());REQUIRE(proof.closed);
    function=source.module->getFunction("view");
    auto value=[&](llvm::StringRef name) -> llvm::Value * {
      for(auto &arg:function->args())if(arg.getName()==name)return &arg;
      for(auto &block:*function)for(auto &i:block)if(i.getName()==name)return &i;
      FAIL("fixture value missing: "<<name.str());return nullptr;
    };
    auto block=[&](llvm::StringRef name) -> llvm::BasicBlock * {
      for(auto &b:*function)if(b.getName()==name)return &b;
      FAIL("fixture block missing: "<<name.str());return nullptr;
    };
    auto *guest=value("guest");auto *accepted=block("accepted");
    llvm::IRBuilder<> b(source.context);
    if(mode!=Unchecked) {
      auto *previous=accepted->getSinglePredecessor();REQUIRE(previous);
      auto *guard=llvm::BasicBlock::Create(source.context,"memory.extra.guard",function,accepted);
      previous->getTerminator()->replaceSuccessorWith(accepted,guard);
      b.SetInsertPoint(guard);
      auto *p=b.CreateConstGEP1_64(b.getInt8Ty(),guest,7000);
      auto *read=b.CreateAlignedLoad(b.getInt32Ty(),p,llvm::Align(1));
      auto *same=b.CreateICmpEQ(read,b.getInt32(0x12345678));
      b.CreateCondBr(same,accepted,mode==Bypass?accepted:block("fallback"));
    }
    b.SetInsertPoint(&*block("a")->getFirstInsertionPt());
    auto *pointer=b.CreateConstGEP1_64(b.getInt8Ty(),guest,7000);
    auto *unknownp=b.CreateConstGEP1_64(b.getInt8Ty(),guest,256);
    auto *unknown=b.CreateAlignedLoad(b.getInt32Ty(),unknownp,llvm::Align(1));
    if(mode==Clobber || mode==Restore)b.CreateAlignedStore(unknown,pointer,llvm::Align(1));
    if(mode==Atomic) {
      auto *store=b.CreateAlignedStore(unknown,pointer,llvm::Align(4));
      store->setAtomic(llvm::AtomicOrdering::Monotonic);
    }
    if(mode==Alias) {
      auto *offsetp=b.CreateConstGEP1_64(b.getInt8Ty(),guest,272);
      auto *offset=b.CreateAlignedLoad(b.getInt64Ty(),offsetp,llvm::Align(1));
      auto *alias=b.CreateGEP(b.getInt8Ty(),guest,offset);
      b.CreateAlignedStore(b.getInt8(1),alias,llvm::Align(1));
    }
    if(mode==Call)b.CreateCall(source.module->getFunction("unknown_call"),{guest});
    observation=b.CreateAlignedLoad(b.getInt32Ty(),pointer,llvm::Align(1),"memory.extra.read");
    auto *output=b.CreateConstGEP1_64(b.getInt8Ty(),guest,1600);
    b.CreateAlignedStore(observation,output,llvm::Align(1));
    if(mode==Restore)b.CreateAlignedStore(b.getInt32(0x12345678),pointer,llvm::Align(1));
    REQUIRE_FALSE(llvm::verifyModule(*source.module,&llvm::errs()));
  }
};
}

TEST_CASE("Guarded memory consumes only checked preserved entry bytes",
          "[llvm][guarded-memory-context]") {
  GuardedMemoryFixture f;auto *original=f.function;auto *arg=original->getArg(0);
  const auto result=propagateGuardedMemory(*f.source.module,"view");INFO(result.message);
  REQUIRE(result.closed);CHECK(result.changed);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.guardedBytes==20);CHECK(result.constantValues>=1);CHECK(result.preservedStores>0);
  CHECK(f.source.module->getFunction("view")==original);CHECK(original->getArg(0)==arg);
  bool load=false,constant=false;
  for(auto &block:*original)for(auto &i:block) {
    load|=llvm::isa<llvm::LoadInst>(i)&&i.getName()=="memory.extra.read";
    if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i))
      if(auto *value=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand()))
        constant|=value->getBitWidth()==32&&value->getZExtValue()==0x12345678;
  }
  CHECK(load);CHECK(constant);CHECK(f.source.ir().find("call void @ordinary")!=std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*f.source.module,&llvm::errs()));
}

TEST_CASE("Guarded memory never assumes unchecked byte contents",
          "[llvm][guarded-memory-context]") {
  GuardedMemoryFixture f(GuardedMemoryFixture::Unchecked);
  const auto result=propagateGuardedMemory(*f.source.module,"view");INFO(result.message);
  REQUIRE(result.closed);CHECK(result.guardedBytes==16);
  bool retained=false;
  for(auto &block:*f.function)for(auto &i:block)
    if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i))
      retained|=llvm::isa<llvm::LoadInst>(store->getValueOperand())&&
                store->getValueOperand()->getName()=="memory.extra.read";
  CHECK(retained);REQUIRE_FALSE(llvm::verifyModule(*f.source.module,&llvm::errs()));
}

TEST_CASE("Guarded memory rejects clobber restore unknown aliases and guard bypass",
          "[llvm][guarded-memory-context]") {
  for(auto mode:{GuardedMemoryFixture::Clobber,GuardedMemoryFixture::Restore,
      GuardedMemoryFixture::Alias,GuardedMemoryFixture::Call,
      GuardedMemoryFixture::Atomic,GuardedMemoryFixture::Bypass}) DYNAMIC_SECTION("mode "<<mode) {
    GuardedMemoryFixture f(mode);const auto before=f.source.ir();auto *entry=&f.function->getEntryBlock();
    auto *arg=f.function->getArg(0);const auto result=propagateGuardedMemory(*f.source.module,"view");
    INFO(result.message);CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);
    CHECK(f.source.ir()==before);CHECK(f.source.module->getFunction("view")==f.function);
    CHECK(&f.function->getEntryBlock()==entry);CHECK(f.function->getArg(0)==arg);
    REQUIRE_FALSE(llvm::verifyModule(*f.source.module,&llvm::errs()));
  }
}

TEST_CASE("Guarded memory proof refuses insufficient and invalid work limits atomically",
          "[llvm][guarded-memory-context]") {
  for(auto limit:{std::size_t(0),std::size_t(1),std::size_t(16000001)}) {
    GuardedMemoryFixture f;const auto before=f.source.ir();
    const auto result=propagateGuardedMemory(*f.source.module,"view",limit);
    CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(result.budgetExhausted==(limit==1));
    CHECK(f.source.ir()==before);CHECK(result.work<=limit);
  }
}
