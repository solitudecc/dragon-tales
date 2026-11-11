#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <cstdint>
#include <string>

using namespace dragon::llvmlifter;

namespace {
enum class Write {
  Argument, ArgumentGEP, NoAliasArgument, PrivateAlloca, CapturedAlloca,
  OtherGuestBase, AtomicRMW, CmpXchg, AtomicStore, VolatileStore,
  AtomicLoad, VolatileLoad, Fence,
};

struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"guest-alias-barriers", context};
  llvm::Function *function = nullptr;
  llvm::WeakVH target;

  Fixture(Write kind, bool narrow) {
    module.setDataLayout("e-p:64:64");
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                        {ptr, ptr, ptr}, false);
    function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                     "alias_case", module);
    if (kind == Write::NoAliasArgument)
      function->getArg(2)->addAttr(llvm::Attribute::NoAlias);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    llvm::IRBuilder<> b(entry);
    llvm::Value *local = nullptr;
    if (kind == Write::PrivateAlloca || kind == Write::CapturedAlloca) {
      local = b.CreateAlloca(b.getInt8Ty(), b.getInt64(24), "private.bytes");
      if (kind == Write::CapturedAlloca)
        b.CreateStore(local, function->getArg(1))->setAlignment(llvm::Align(1));
    }
    auto *data = b.CreateLoad(ptr, function->getArg(1), "guest.base");
    data->setAlignment(llvm::Align(1));
    llvm::Value *other = nullptr;
    if (kind == Write::OtherGuestBase) {
      other = b.CreateLoad(ptr, function->getArg(1), "second.guest.base");
      llvm::cast<llvm::LoadInst>(other)->setAlignment(llvm::Align(1));
    }
    auto *slot = b.CreateGEP(b.getInt8Ty(), data, b.getInt64(8));
    auto *first = b.getInt64(UINT64_C(0x1122334455667788));
    auto *second = b.getInt64(UINT64_C(0x8877665544332211));
    b.CreateStore(first, slot)->setAlignment(llvm::Align(1));
    auto simpleStore = [&](llvm::Value *address) {
      b.CreateStore(second, address)->setAlignment(llvm::Align(1));
    };
    switch (kind) {
    case Write::Argument:
    case Write::NoAliasArgument:
      simpleStore(function->getArg(2));
      break;
    case Write::ArgumentGEP:
      simpleStore(b.CreateGEP(b.getInt8Ty(), function->getArg(2), b.getInt64(8)));
      break;
    case Write::PrivateAlloca:
    case Write::CapturedAlloca:
      simpleStore(b.CreateGEP(b.getInt8Ty(), local, b.getInt64(8)));
      break;
    case Write::OtherGuestBase:
      simpleStore(b.CreateGEP(b.getInt8Ty(), other, b.getInt64(8)));
      break;
    case Write::AtomicRMW:
      b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, slot, b.getInt64(1),
                        llvm::MaybeAlign(8), llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case Write::CmpXchg:
      b.CreateAtomicCmpXchg(slot, first, second, llvm::MaybeAlign(8),
                           llvm::AtomicOrdering::SequentiallyConsistent,
                           llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case Write::AtomicStore: {
      auto *store = b.CreateStore(second, slot);
      store->setAlignment(llvm::Align(8));
      store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    }
    case Write::VolatileStore:
      b.CreateStore(second, slot)->setVolatile(true);
      break;
    case Write::AtomicLoad: {
      auto *load = b.CreateLoad(b.getInt64Ty(), slot);
      load->setAlignment(llvm::Align(8));
      load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    }
    case Write::VolatileLoad:
      b.CreateLoad(b.getInt64Ty(), slot)->setVolatile(true);
      break;
    case Write::Fence:
      b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    }
    auto *address = narrow
        ? b.CreateGEP(b.getInt8Ty(), data, b.getInt64(11)) : slot;
    auto *load = b.CreateLoad(narrow ? b.getInt16Ty() : b.getInt64Ty(), address,
                             "observed.value");
    target = load;
    load->setAlignment(llvm::Align(1));
    b.CreateRet(narrow ? b.CreateZExt(load, b.getInt64Ty()) : load);
  }
};

void verify(const llvm::Module &module) {
  std::string message;
  llvm::raw_string_ostream stream(message);
  const bool invalid = llvm::verifyModule(module, &stream);
  INFO(message);
  CHECK_FALSE(invalid);
}

bool retainsTarget(const Fixture &fixture) {
  if (!fixture.target) return false;
  for (const auto &block : *fixture.function)
    for (const auto &instruction : block)
      if (&instruction == static_cast<llvm::Value *>(fixture.target)) return true;
  return false;
}

MemoryForwardStats run(Fixture &fixture) {
  verify(fixture.module);
  MemoryForwardOptions options{65536, false};
  options.enableCrossBlock = false;
  options.enablePhi = false;
  auto result = detail::runGuestMemorySSA(fixture.module, options);
  CHECK(result.available);
  CHECK(result.complete);
  verify(fixture.module);
  return result;
}
}

TEST_CASE("Guest memory facts stop at unproved parameter and captured alloca writes",
          "[llvm][guest-memory-ssa][guest-memory-alias]") {
  for (const auto kind : {Write::Argument, Write::ArgumentGEP, Write::CapturedAlloca})
    for (const bool narrow : {false, true}) {
      CAPTURE(static_cast<unsigned>(kind), narrow);
      Fixture fixture(kind, narrow);
      const auto result = run(fixture);
      CHECK_FALSE(result.changed);
      CHECK(result.loadsForwarded == 0);
      CHECK(result.aliasBarriers >= 1);
      CHECK(retainsTarget(fixture));
    }
}

TEST_CASE("Guest memory facts retain real private and explicit noalias stores",
          "[llvm][guest-memory-ssa][guest-memory-alias]") {
  for (const auto kind : {Write::PrivateAlloca, Write::NoAliasArgument})
    for (const bool narrow : {false, true}) {
      CAPTURE(static_cast<unsigned>(kind), narrow);
      Fixture fixture(kind, narrow);
      const auto result = run(fixture);
      CHECK(result.changed);
      CHECK(result.loadsForwarded == 1);
      CHECK_FALSE(retainsTarget(fixture));
    }
}

TEST_CASE("Guest byte facts do not treat separate Guest base loads as disjoint",
          "[llvm][guest-memory-ssa][guest-memory-alias]") {
  for (const bool narrow : {false, true}) {
    CAPTURE(narrow);
    Fixture fixture(Write::OtherGuestBase, narrow);
    const auto result = run(fixture);
    CHECK_FALSE(result.changed);
    CHECK(result.loadsForwarded == 0);
    CHECK(retainsTarget(fixture));
  }
}

TEST_CASE("Guest memory facts stop at atomic writes and synchronization",
          "[llvm][guest-memory-ssa][guest-memory-alias]") {
  for (const auto kind : {Write::AtomicRMW, Write::CmpXchg, Write::AtomicStore,
                          Write::VolatileStore, Write::AtomicLoad,
                          Write::VolatileLoad, Write::Fence})
    for (const bool narrow : {false, true}) {
      CAPTURE(static_cast<unsigned>(kind), narrow);
      Fixture fixture(kind, narrow);
      const auto result = run(fixture);
      CHECK_FALSE(result.changed);
      CHECK(result.loadsForwarded == 0);
      CHECK(retainsTarget(fixture));
    }
}
