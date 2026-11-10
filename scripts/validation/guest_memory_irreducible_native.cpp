#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <string>

namespace {
enum Mode {
  Exact, Narrow, UnknownCall, UnknownAlias, Fence, AtomicWrite,
  VolatileLoad, VolatileStore, Partial, OtherGuestBase, CrossBlock,
};
constexpr const char *names[] = {
    "exact", "narrow", "unknown_call", "unknown_alias", "fence", "atomic_write",
    "volatile_load", "volatile_store", "partial", "other_guest_base", "cross_block",
};

void build(llvm::Module &module, Mode mode) {
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *type = llvm::FunctionType::get(i64, {ptr, ptr, i1, i32}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                         names[mode], module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *exit = llvm::BasicBlock::Create(context, "exit", function);
  llvm::IRBuilder<> builder(entry);
  auto *data = builder.CreateLoad(ptr, function->getArg(1));
  auto *otherData = mode == OtherGuestBase
      ? builder.CreateLoad(ptr, function->getArg(1)) : nullptr;
  auto *slot = builder.CreateGEP(builder.getInt8Ty(), data, builder.getInt64(8));
  auto *initialCount = builder.CreateAnd(function->getArg(3), builder.getInt32(7));
  builder.CreateCondBr(function->getArg(2), left, right);
  auto *leftCount = llvm::PHINode::Create(i32, 2, "left.count", left);
  auto *rightCount = llvm::PHINode::Create(i32, 2, "right.count", right);
  leftCount->addIncoming(initialCount, entry);
  rightCount->addIncoming(initialCount, entry);
  llvm::Value *results[2]{};
  for (unsigned index = 0; index != 2; ++index) {
    builder.SetInsertPoint(index ? right : left);
    const auto value = index ? 0x8877665544332211ULL : 0x1122334455667788ULL;
    if (mode != CrossBlock || !index) {
      auto *store = mode == Partial
          ? builder.CreateStore(builder.getInt8(value & 0xff), slot)
          : builder.CreateStore(builder.getInt64(value), slot);
      store->setAlignment(llvm::Align(1));
      if (mode == VolatileStore) store->setVolatile(true);
    }
    switch (mode) {
    case UnknownCall: {
      auto callee = module.getOrInsertFunction(
          "unknown_writer", llvm::FunctionType::get(builder.getVoidTy(),
                                                     {ptr}, false));
      builder.CreateCall(callee, {data});
      break;
    }
    case UnknownAlias:
      builder.CreateStore(builder.getInt64(3), function->getArg(0))
          ->setAlignment(llvm::Align(1));
      break;
    case Fence:
      builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case AtomicWrite:
      builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, slot, builder.getInt64(1),
                              llvm::MaybeAlign(8),
                              llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case OtherGuestBase: {
      auto *otherSlot = builder.CreateGEP(builder.getInt8Ty(), otherData,
                                          builder.getInt64(8));
      builder.CreateStore(builder.getInt64(0), otherSlot)
          ->setAlignment(llvm::Align(1));
      break;
    }
    default: break;
    }
    if (mode == CrossBlock && !index) {
      results[index] = builder.getInt64(value);
    } else {
      const bool narrow = mode == Narrow || mode == OtherGuestBase;
      auto *address = narrow
          ? builder.CreateGEP(builder.getInt8Ty(), data, builder.getInt64(11))
          : slot;
      auto *load = builder.CreateLoad(narrow ? builder.getInt16Ty() : i64, address);
      load->setAlignment(llvm::Align(1));
      if (mode == VolatileLoad) load->setVolatile(true);
      results[index] = builder.CreateZExtOrTrunc(load, i64);
    }
    auto *count = index ? rightCount : leftCount;
    auto *next = builder.CreateSub(count, builder.getInt32(1));
    (index ? leftCount : rightCount)->addIncoming(next, index ? right : left);
    builder.CreateCondBr(builder.CreateICmpNE(count, builder.getInt32(0)),
                         index ? left : right, exit);
  }
  builder.SetInsertPoint(exit);
  auto *result = builder.CreatePHI(i64, 2, "result");
  result->addIncoming(results[0], left);
  result->addIncoming(results[1], right);
  builder.CreateRet(result);
}

void dump(const llvm::Module &module, const std::string &path) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_Text);
  if (error) { llvm::errs() << error.message() << '\n'; std::exit(2); }
  module.print(stream, nullptr);
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  llvm::LLVMContext context;
  llvm::Module module("guest-memory-irreducible-native", context);
  module.setDataLayout("e-p:64:64");
  for (unsigned mode = Exact; mode <= CrossBlock; ++mode)
    build(module, static_cast<Mode>(mode));
  if (llvm::verifyModule(module, &llvm::errs())) return 3;
  dump(module, std::string(argv[1]) + "/before.ll");
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(
      module, {65536, false});
  const bool invalid = llvm::verifyModule(module, &llvm::errs());
  dump(module, std::string(argv[1]) + "/after.ll");
  llvm::outs() << "changed=" << stats.changed
               << " blocks=" << stats.blocksVisited
               << " loads_forwarded=" << stats.loadsForwarded
               << " byte_rebuilds=" << stats.byteRebuilds
               << " phis=" << stats.phisInserted
               << " complete=" << stats.complete
               << " verify_invalid=" << invalid << '\n';
  return invalid ? 3 : 0;
}
