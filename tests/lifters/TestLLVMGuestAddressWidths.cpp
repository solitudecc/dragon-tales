#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <cstdint>
#include <string>

using namespace dragon::llvmlifter;

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
constexpr std::uint64_t First = 0x1122334455667788ULL;
constexpr std::uint64_t Second = 0x8877665544332211ULL;

enum class Index {
  AddWrap, SubWrap, MulWrap, ShlSigned, LShr, AShr,
  WideTrunc, SExt, ZExt, AddNUW, AddNSW, SubNUW, MulNSW,
  ExactShift, InvalidShift8, InvalidShift64, DisjointOr,
  FlaggedTrunc, FlaggedZExt, FrozenFlaggedTrunc, Deep,
};

struct View {
  llvm::Function *function;
  llvm::Value *data;
  llvm::LoadInst *load;
};

void verify(const llvm::Module &module) {
  std::string error;
  llvm::raw_string_ostream stream(error);
  const bool invalid = llvm::verifyModule(module, &stream);
  INFO(error);
  REQUIRE_FALSE(invalid);
}

llvm::Value *indexFor(Builder &b, Index mode) {
  switch (mode) {
  case Index::AddWrap: return b.CreateAdd(b.getInt8(200), b.getInt8(100));
  case Index::SubWrap: return b.CreateSub(b.getInt8(1), b.getInt8(200));
  case Index::MulWrap: return b.CreateMul(b.getInt8(20), b.getInt8(20));
  case Index::ShlSigned: return b.CreateShl(b.getInt8(1), b.getInt8(7));
  case Index::LShr: return b.CreateLShr(b.getInt8(255), b.getInt8(1));
  case Index::AShr: return b.CreateAShr(b.getInt8(128), b.getInt8(1));
  case Index::WideTrunc: {
    auto *wide = b.getIntNTy(128);
    auto *value = b.CreateAdd(llvm::ConstantInt::get(wide,
        llvm::APInt(128, 1).shl(64)), llvm::ConstantInt::get(wide, 3));
    return b.CreateTrunc(value, b.getInt64Ty());
  }
  case Index::SExt: return b.CreateSExt(b.getInt8(255), b.getInt64Ty());
  case Index::ZExt: return b.CreateZExt(b.getInt8(255), b.getInt64Ty());
  case Index::AddNUW: return b.CreateNUWAdd(b.getInt8(255), b.getInt8(1));
  case Index::AddNSW: return b.CreateNSWAdd(b.getInt8(127), b.getInt8(1));
  case Index::SubNUW: return b.CreateNUWSub(b.getInt8(0), b.getInt8(1));
  case Index::MulNSW: return b.CreateNSWMul(b.getInt8(20), b.getInt8(20));
  case Index::ExactShift: return b.CreateLShr(b.getInt8(3), b.getInt8(1), "", true);
  case Index::InvalidShift8: return b.CreateShl(b.getInt8(1), b.getInt8(8));
  case Index::InvalidShift64: return b.CreateLShr(b.getInt64(1), b.getInt64(64));
  case Index::DisjointOr: {
    auto *value = llvm::cast<llvm::PossiblyDisjointInst>(
        b.CreateOr(b.getInt8(1), b.getInt8(1)));
    value->setIsDisjoint(true);
    return value;
  }
  case Index::FlaggedTrunc:
  case Index::FrozenFlaggedTrunc: {
    auto *value = llvm::cast<llvm::TruncInst>(b.CreateTrunc(b.getInt16(256), b.getInt8Ty()));
    value->setHasNoUnsignedWrap(true);
    return mode == Index::FrozenFlaggedTrunc ? b.CreateFreeze(value) : value;
  }
  case Index::FlaggedZExt: {
    auto *value = llvm::cast<llvm::ZExtInst>(b.CreateZExt(b.getInt8(128), b.getInt64Ty()));
    value->setNonNeg(true);
    return value;
  }
  case Index::Deep: {
    llvm::Value *value = b.getInt64(8);
    for (unsigned index = 0; index != 128; ++index)
      value = b.CreateAdd(value, b.getInt64(0));
    return value;
  }
  }
  llvm_unreachable("unknown index fixture");
}

View build(llvm::Module &module, Index mode, std::uint64_t actualOffset,
           std::uint64_t oldOffset, unsigned scale = 1, std::uint64_t bias = 512) {
  module.setDataLayout("e-p:64:64");
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                         "guest_index", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  Builder b(entry);
  auto *data = b.CreateLoad(ptr, function->getArg(1));
  auto put = [&](std::uint64_t offset, std::uint64_t value) {
    auto *address = b.CreateGEP(b.getInt8Ty(), data, b.getInt64(offset));
    b.CreateStore(b.getInt64(value), address)->setAlignment(llvm::Align(1));
  };
  put(actualOffset, First);
  if (oldOffset != actualOffset) put(oldOffset, Second);
  auto *base = bias ? b.CreateGEP(b.getInt8Ty(), data, b.getInt64(bias)) : data;
  auto *address = b.CreateGEP(b.getIntNTy(scale * 8), base, indexFor(b, mode));
  auto *load = b.CreateLoad(b.getInt64Ty(), address);
  load->setAlignment(llvm::Align(1));
  b.CreateRet(load);
  return {function, data, load};
}

void expectValue(Index mode, std::uint64_t actualOffset, std::uint64_t oldOffset,
                 unsigned scale = 1, std::uint64_t bias = 512) {
  llvm::LLVMContext context;
  llvm::Module module("guest-address-width", context);
  const auto view = build(module, mode, actualOffset, oldOffset, scale, bias);
  verify(module);
  const auto stats = detail::runGuestMemorySSA(module, {65536, false});
  CHECK(stats.available);
  CHECK(stats.complete);
  CHECK(stats.loadsForwarded == 1);
  const auto *ret = llvm::cast<llvm::ReturnInst>(view.function->back().getTerminator());
  const auto *value = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue());
  REQUIRE(value);
  CHECK(value->getZExtValue() == First);
  verify(module);
}

void expectUnknown(Index mode, std::uint64_t oldOffset) {
  llvm::LLVMContext context;
  llvm::Module module("guest-address-unknown", context);
  const auto view = build(module, mode, 32, oldOffset);
  verify(module);
  const auto stats = detail::runGuestMemorySSA(module, {65536, false});
  CHECK(stats.available);
  CHECK_FALSE(stats.changed);
  CHECK(stats.loadsForwarded == 0);
  const auto *ret = llvm::cast<llvm::ReturnInst>(view.function->back().getTerminator());
  CHECK(llvm::isa<llvm::LoadInst>(ret->getReturnValue()));
  verify(module);
}
} // namespace

TEST_CASE("Guest address folding preserves modular narrow arithmetic",
          "[llvm][guest-memory-ssa][guest-address-widths]") {
  expectValue(Index::AddWrap, 44, 300, 1, 0);
  expectValue(Index::SubWrap, 569, 313);
  expectValue(Index::MulWrap, 400, 912);
  expectValue(Index::ShlSigned, 384, 640);
  expectValue(Index::LShr, 639, 639);
  expectValue(Index::AShr, 448, 448);
}

TEST_CASE("Guest GEP indices sign extend before typed scaling",
          "[llvm][guest-memory-ssa][guest-address-widths]") {
  expectValue(Index::ShlSigned, 384, 640);
  expectValue(Index::ShlSigned, 256, 768, 2);
  expectValue(Index::SubWrap, 740, 18446744073709551332ULL, 4);
}

TEST_CASE("Guest address folding retains wide intermediates and cast widths",
          "[llvm][guest-memory-ssa][guest-address-widths]") {
  expectValue(Index::WideTrunc, 515, 1539);
  expectValue(Index::SExt, 511, 767);
  expectValue(Index::ZExt, 767, 511);
}

TEST_CASE("Guest address proofs reject poison flags and invalid shifts",
          "[llvm][guest-memory-ssa][guest-address-widths]") {
  expectUnknown(Index::AddNUW, 768);
  expectUnknown(Index::AddNSW, 640);
  expectUnknown(Index::SubNUW, 511);
  expectUnknown(Index::MulNSW, 912);
  expectUnknown(Index::ExactShift, 513);
  expectUnknown(Index::InvalidShift8, 768);
  expectUnknown(Index::InvalidShift64, 512);
  expectUnknown(Index::DisjointOr, 513);
  expectUnknown(Index::FlaggedTrunc, 512);
  expectUnknown(Index::FlaggedZExt, 640);
  expectUnknown(Index::FrozenFlaggedTrunc, 512);
}

TEST_CASE("Guest address expression budgets retain dynamic identity",
          "[llvm][guest-memory-ssa][guest-address-widths]") {
  expectUnknown(Index::Deep, 520);
}

TEST_CASE("Guest address proofs do not replace scalable type size with its minimum",
          "[llvm][guest-memory-ssa][guest-address-widths]") {
  llvm::LLVMContext context;
  llvm::Module module("guest-scalable-index", context);
  module.setDataLayout("e-p:64:64");
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                         "guest_scalable", module);
  Builder b(llvm::BasicBlock::Create(context, "entry", function));
  auto *data = b.CreateLoad(ptr, function->getArg(1));
  for (const auto offset : {16, 32}) {
    auto *slot = b.CreateGEP(b.getInt8Ty(), data, b.getInt64(offset));
    b.CreateStore(b.getInt64(offset == 16 ? First : Second), slot);
  }
  auto *vector = llvm::ScalableVectorType::get(b.getInt32Ty(), 4);
  auto *index = b.CreateAdd(b.getInt64(1), b.getInt64(0));
  auto *address = b.CreateGEP(vector, data, index);
  b.CreateRet(b.CreateLoad(b.getInt64Ty(), address));
  verify(module);
  const auto stats = detail::runGuestMemorySSA(module, {65536, false});
  CHECK(stats.available);
  CHECK_FALSE(stats.changed);
  CHECK(stats.loadsForwarded == 0);
  CHECK(llvm::isa<llvm::LoadInst>(llvm::cast<llvm::ReturnInst>(
      function->back().getTerminator())->getReturnValue()));
  verify(module);
}

TEST_CASE("Guest address keys keep narrower index layouts conservative",
          "[llvm][guest-memory-ssa][guest-address-widths][guest-address-narrow-layout]") {
  for (const auto *dataLayout : {"e-p:64:64:64:32", "e-p:128:128:128:64"}) {
    CAPTURE(dataLayout);
    llvm::LLVMContext context;
    llvm::Module module("guest-index-layout", context);
    module.setDataLayout(dataLayout);
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), {ptr, ptr}, false);
    auto *function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                           "guest_index32", module);
    Builder b(llvm::BasicBlock::Create(context, "entry", function));
    auto *data = b.CreateLoad(ptr, function->getArg(1));
    auto *small = b.CreateGEP(b.getInt8Ty(), data, b.getInt64(4));
    auto *wrapped = b.CreateGEP(b.getInt8Ty(), data, b.getInt64((1ULL << 32) + 4));
    b.CreateStore(b.getInt64(First), small)->setAlignment(llvm::Align(1));
    b.CreateStore(b.getInt64(Second), wrapped)->setAlignment(llvm::Align(1));
    b.CreateRet(b.CreateLoad(b.getInt64Ty(), small));
    verify(module);
    const auto stats = detail::runGuestMemorySSA(module, {65536, false});
    CHECK(stats.available);
    CHECK(stats.loadsForwarded == 0);
    CHECK(llvm::isa<llvm::LoadInst>(llvm::cast<llvm::ReturnInst>(
        function->back().getTerminator())->getReturnValue()));
    verify(module);
  }
}
