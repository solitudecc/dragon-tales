#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <array>
#include <memory>
#include <vector>

using namespace dragon::llvmlifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;

// Allocate the same CFG in different heap orders while keeping textual block
// order fixed. Retain modules until the matrix ends so freed blocks cannot
// collapse every case to the same allocator layout.
struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"nested-loop-membership", context};
  llvm::StoreInst *observed = nullptr;
  explicit Fixture(unsigned rotation, unsigned stride, bool twoLatches) {
    module.setDataLayout("e-p:64:64");
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *i1 = llvm::Type::getInt1Ty(context);
    auto *fn = llvm::Function::Create(llvm::FunctionType::get(
        llvm::Type::getVoidTy(context), {ptr,ptr,i64,i1,i1,i1}, false),
        llvm::GlobalValue::ExternalLinkage,"nested",module);
    // Nine is coprime with the selected strides. B2 is a normal side path
    // without a second latch in the single-latch fixture.
    std::array<llvm::BasicBlock *,9> b{};
    constexpr std::array<const char *,9> names={"entry","outer","prefix",
        "inner","body","latch","exit","body2","choose"};
    for (unsigned n=0;n!=b.size();++n) {
      const auto role=(rotation+n*stride)%b.size();
      b[role]=llvm::BasicBlock::Create(context,names[role]);
    }
    for(auto *block:b)block->insertInto(fn);
    Builder ir(b[0]);
    auto *base=ir.CreateLoad(ptr,fn->getArg(1),"guest.base");base->setAlignment(llvm::Align(1));
    auto *slot=ir.CreateGEP(ir.getInt8Ty(),base,fn->getArg(2),"dynamic.slot");
    ir.CreateBr(b[1]);
    ir.SetInsertPoint(b[1]);ir.CreateCondBr(fn->getArg(3),b[2],b[6]);
    ir.SetInsertPoint(b[2]);
    ir.CreateStore(ir.getInt64(50),slot)->setAlignment(llvm::Align(1));
    auto *load=ir.CreateLoad(i64,slot,"prefix.observation");load->setAlignment(llvm::Align(1));
    observed=ir.CreateStore(load,fn->getArg(0));observed->setAlignment(llvm::Align(1));
    ir.CreateBr(b[3]);
    ir.SetInsertPoint(b[3]);ir.CreateCondBr(fn->getArg(4),b[8],b[5]);
    ir.SetInsertPoint(b[8]);ir.CreateCondBr(fn->getArg(5),b[4],b[7]);
    ir.SetInsertPoint(b[4]);ir.CreateStore(ir.getInt64(99),slot)->setAlignment(llvm::Align(1));ir.CreateBr(b[3]);
    ir.SetInsertPoint(b[7]);ir.CreateStore(ir.getInt64(98),slot)->setAlignment(llvm::Align(1));ir.CreateBr(twoLatches?b[3]:b[4]);
    ir.SetInsertPoint(b[5]);ir.CreateStore(ir.getInt64(51),slot)->setAlignment(llvm::Align(1));ir.CreateBr(b[1]);
    ir.SetInsertPoint(b[6]);ir.CreateRetVoid();
  }
};
}

TEST_CASE("Guest memory nested loop membership ignores block allocation order",
          "[llvm][guest-memory-ssa][guest-loop-membership]") {
  std::vector<std::unique_ptr<Fixture>> keep;
  for(bool twoLatches:{false,true})
    for(unsigned stride:{1u,2u,4u,5u,7u,8u})
      for(unsigned rotation=0;rotation!=9;++rotation) {
        auto fixture=std::make_unique<Fixture>(rotation,stride,twoLatches);
        INFO("rotation="<<rotation<<" stride="<<stride<<" two latches="<<twoLatches);
        REQUIRE_FALSE(llvm::verifyModule(fixture->module,&llvm::errs()));
        MemoryForwardOptions options;options.runMem2Reg=false;
        const auto stats=detail::runGuestMemorySSA(fixture->module,options);
        CHECK(stats.available);
        CHECK(stats.complete);
        CHECK(stats.loopBarriers==(twoLatches?4:3));
        // Neither natural loop has an invariant entry/backedge fact set.
        // Prefix is in the outer loop and must participate in deferred proof.
        // This is a conservative-admission regression, not an old wrong-result
        // claim: the old immediate local STORE-to-LOAD fold can itself be safe.
        CHECK(stats.loadsForwarded==0);
        CHECK(llvm::isa<llvm::LoadInst>(fixture->observed->getValueOperand()));
        REQUIRE_FALSE(llvm::verifyModule(fixture->module,&llvm::errs()));
        keep.push_back(std::move(fixture));
      }
}

TEST_CASE("Guest memory self loop stops its closure at its header",
          "[llvm][guest-memory-ssa][guest-loop-membership]") {
  llvm::LLVMContext context;llvm::Module module("self-loop-membership",context);
  module.setDataLayout("e-p:64:64");
  auto *ptr=llvm::PointerType::getUnqual(context);auto *i64=llvm::Type::getInt64Ty(context);
  auto *fn=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context),
      {ptr,ptr,i64,llvm::Type::getInt1Ty(context)},false),llvm::GlobalValue::ExternalLinkage,"self",module);
  auto *entry=llvm::BasicBlock::Create(context,"entry",fn);
  auto *header=llvm::BasicBlock::Create(context,"header",fn);
  auto *exit=llvm::BasicBlock::Create(context,"exit",fn);
  Builder ir(entry);auto *base=ir.CreateLoad(ptr,fn->getArg(1));
  auto *slot=ir.CreateGEP(ir.getInt8Ty(),base,fn->getArg(2));ir.CreateBr(header);
  ir.SetInsertPoint(header);ir.CreateStore(ir.getInt64(50),slot);
  auto *load=ir.CreateLoad(i64,slot);auto *observed=ir.CreateStore(load,fn->getArg(0));
  ir.CreateStore(ir.getInt64(99),slot);ir.CreateCondBr(fn->getArg(3),header,exit);
  ir.SetInsertPoint(exit);ir.CreateRetVoid();
  REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  MemoryForwardOptions options;options.runMem2Reg=false;
  auto stats=detail::runGuestMemorySSA(module,options);
  CHECK(stats.complete);CHECK(stats.loopBarriers==2);CHECK(stats.loadsForwarded==0);
  CHECK(llvm::isa<llvm::LoadInst>(observed->getValueOperand()));
  REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
}

TEST_CASE("Guest byte facts stop between blocks inside a nested outer loop",
          "[llvm][guest-memory-ssa][guest-loop-membership]") {
  // P writes two halves; Q reads their word. Only the byte walker could rebuild
  // this LOAD. P and Q are both in the outer natural loop, so their edge must
  // not carry byte facts under the current conservative loop contract.
  struct ByteFixture {
    llvm::LLVMContext context;
    llvm::Module module{"nested-byte-membership", context};
    llvm::StoreInst *observed = nullptr;
  };
  std::vector<std::unique_ptr<ByteFixture>> keep;
  for (unsigned stride : {1u, 3u, 5u, 7u})
    for (unsigned rotation = 0; rotation != 8; ++rotation) {
      auto fixture = std::make_unique<ByteFixture>();
      auto &context = fixture->context;
      auto &module = fixture->module;
      module.setDataLayout("e-p:64:64");
      auto *ptr = llvm::PointerType::getUnqual(context);
      auto *i1 = llvm::Type::getInt1Ty(context);
      auto *fn = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                  {ptr, ptr, i1, i1}, false),
          llvm::GlobalValue::ExternalLinkage, "nested_bytes", module);
      constexpr std::array<const char *, 8> names = {
          "entry", "outer", "prefix.write", "prefix.read", "inner",
          "body", "latch", "exit"};
      std::array<llvm::BasicBlock *, 8> blocks{};
      for (unsigned n = 0; n != blocks.size(); ++n) {
        auto role = (rotation + n * stride) % blocks.size();
        blocks[role] = llvm::BasicBlock::Create(context, names[role]);
      }
      for (auto *block : blocks) block->insertInto(fn);
      Builder ir(blocks[0]);
      auto *base = ir.CreateLoad(ptr, fn->getArg(1));
      base->setAlignment(llvm::Align(1));
      auto *slot = ir.CreateGEP(ir.getInt8Ty(), base, ir.getInt64(64));
      auto *upper = ir.CreateGEP(ir.getInt8Ty(), base, ir.getInt64(68));
      ir.CreateBr(blocks[1]);
      ir.SetInsertPoint(blocks[1]);
      ir.CreateCondBr(fn->getArg(2), blocks[2], blocks[7]);
      ir.SetInsertPoint(blocks[2]);
      ir.CreateStore(ir.getInt32(0x89abcdef), slot)->setAlignment(llvm::Align(1));
      ir.CreateStore(ir.getInt32(0x01234567), upper)->setAlignment(llvm::Align(1));
      ir.CreateBr(blocks[3]);
      ir.SetInsertPoint(blocks[3]);
      auto *load = ir.CreateLoad(ir.getInt64Ty(), slot);
      load->setAlignment(llvm::Align(1));
      fixture->observed = ir.CreateStore(load, fn->getArg(0));
      fixture->observed->setAlignment(llvm::Align(1));
      ir.CreateBr(blocks[4]);
      ir.SetInsertPoint(blocks[4]);
      ir.CreateCondBr(fn->getArg(3), blocks[5], blocks[6]);
      ir.SetInsertPoint(blocks[5]);
      ir.CreateStore(ir.getInt64(99), slot)->setAlignment(llvm::Align(1));
      ir.CreateBr(blocks[4]);
      ir.SetInsertPoint(blocks[6]);
      ir.CreateStore(ir.getInt64(51), slot)->setAlignment(llvm::Align(1));
      ir.CreateBr(blocks[1]);
      ir.SetInsertPoint(blocks[7]);
      ir.CreateRetVoid();
      INFO("byte rotation=" << rotation << " stride=" << stride);
      REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
      MemoryForwardOptions options;
      options.runMem2Reg = false;
      options.enablePhi = false; // Isolate the byte walker from checked copies.
      const auto stats = detail::runGuestMemorySSA(module, options);
      CHECK(stats.available);
      CHECK(stats.complete);
      CHECK(stats.loopBarriers == 3);
      CHECK(stats.loadsForwarded == 0);
      CHECK(stats.byteRebuilds == 0);
      CHECK(llvm::isa<llvm::LoadInst>(fixture->observed->getValueOperand()));
      REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
      keep.push_back(std::move(fixture));
    }
}
