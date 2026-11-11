#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <string>

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum Mode {
  Wrap8, Signed8, Scaled16, Wrap32, Shift8, Wrap64, LShr8, WideTrunc,
  AliasWord, AliasPartial, AliasByte, AliasGEP, NoAlias, PrivateAlloca,
  CapturedAlloca, TwoBasesByte, UnknownCall, AtomicAdd, CompareExchange,
  Fence, Volatile, LoopWord, LoopByte, SubWrap8, MulWrap8, ModeCount,
};
constexpr const char *names[] = {
  "wrap8", "signed8", "scaled16", "wrap32", "shift8", "wrap64", "lshr8", "wide_trunc",
  "alias_word", "alias_partial", "alias_byte", "alias_gep", "noalias", "private_alloca",
  "captured_alloca", "two_bases_byte", "unknown_call", "atomic_add", "cmpxchg",
  "fence", "volatile", "loop_word", "loop_byte", "sub_wrap8", "mul_wrap8",
};
constexpr std::uint64_t A = 0x1122334455667788ULL;
constexpr std::uint64_t B = 0x8877665544332211ULL;
struct View { llvm::Function *function; llvm::WeakVH target; };

llvm::Value *byte(Builder &b, llvm::Value *base, llvm::Value *index) {
  return b.CreateGEP(b.getInt8Ty(), base, index);
}
llvm::Value *byte(Builder &b, llvm::Value *base, std::uint64_t index) {
  return byte(b, base, b.getInt64(index));
}
void store(Builder &b, llvm::Value *value, llvm::Value *pointer) {
  b.CreateStore(value, pointer)->setAlignment(llvm::Align(1));
}
llvm::LoadInst *read(Builder &b, llvm::Type *type, llvm::Value *pointer,
                     const char *name = "") {
  auto *value = b.CreateLoad(type, pointer, name);
  value->setAlignment(llvm::Align(1));
  return value;
}

View build(llvm::Module &module, Mode mode, const std::string &overrideName = "") {
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *type = llvm::FunctionType::get(i64, {ptr, ptr, ptr, i64, ptr, i32}, false);
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
      overrideName.empty() ? std::string("address_") + names[mode] : overrideName, module);
  for (auto &argument : function->args()) argument.addAttr(llvm::Attribute::NoUndef);
  if (mode == NoAlias) function->getArg(4)->addAttr(llvm::Attribute::NoAlias);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  Builder b(entry);
  auto *regs = function->getArg(0);
  auto *descriptor = function->getArg(1);
  auto *exit = function->getArg(2);
  auto *x = function->getArg(3);
  auto *alias = function->getArg(4);
  auto *rounds = function->getArg(5);
  auto *data = read(b, ptr, descriptor, "guest.base");
  auto *initialReg = read(b, i64, byte(b, regs, 32), "initial.reg32");
  auto *valueB = b.CreateXor(x, b.getInt64(B), "second.value");
  llvm::Value *observed = nullptr;
  llvm::Value *iterations = b.getInt64(1);
  llvm::Value *auxiliary = b.getInt64(0);
  llvm::LoadInst *target = nullptr;

  if (mode <= WideTrunc || mode == SubWrap8 || mode == MulWrap8) {
    llvm::Value *index = nullptr;
    std::uint64_t anchor = 0, actual = 0, distractor = 0;
    llvm::Type *element = b.getInt8Ty();
    switch (mode) {
    case Wrap8:
      index = b.CreateAdd(b.getInt8(200), b.getInt8(100));
      actual = 44; distractor = 300;
      break;
    case Signed8:
      index = b.CreateAdd(b.getInt8(127), b.getInt8(1));
      anchor = 512; actual = 384; distractor = 640;
      break;
    case Scaled16:
      index = b.CreateAdd(b.getInt8(127), b.getInt8(1));
      element = b.getInt16Ty();
      anchor = 512; actual = 256; distractor = 768;
      break;
    case Wrap32:
      index = b.CreateAdd(b.getInt32(0xffffffffU), b.getInt32(2));
      anchor = 512; actual = 513; distractor = 641;
      break;
    case Shift8:
      index = b.CreateShl(b.getInt8(128), b.getInt8(1));
      anchor = 512; actual = 512; distractor = 768;
      break;
    case Wrap64:
      index = b.CreateAdd(b.getInt64(~std::uint64_t{0}), b.getInt64(2));
      anchor = 512; actual = 513; distractor = 641;
      break;
    case LShr8:
      index = b.CreateLShr(b.getInt8(255), b.getInt8(4));
      anchor = 512; actual = 527; distractor = 641;
      break;
    case WideTrunc: {
      auto *wide = b.CreateAdd(b.getIntN(128, ~std::uint64_t{0}), b.getIntN(128, 2));
      index = b.CreateTrunc(wide, i64);
      anchor = 512; actual = 513; distractor = 641;
      break;
    }
    case SubWrap8:
      index = b.CreateSub(b.getInt8(1), b.getInt8(200));
      anchor = 512; actual = 569; distractor = 313;
      break;
    case MulWrap8:
      index = b.CreateMul(b.getInt8(20), b.getInt8(20));
      anchor = 512; actual = 400; distractor = 912;
      break;
    default: llvm_unreachable("not a width mode");
    }
    auto *computed = b.CreateGEP(element, byte(b, data, anchor), index, "computed.slot");
    if (mode == Wrap32) {
      // Both native addresses are data+513; the old key incorrectly separates
      // the computed narrow wrap from the constant byte offset by 2^32.
      store(b, b.getInt64(A), computed);
      store(b, valueB, byte(b, data, actual));
    } else {
      store(b, b.getInt64(A), byte(b, data, actual));
      store(b, valueB, byte(b, data, distractor));
    }
    target = read(b, i64, computed, "observed");
    observed = target;
  } else if (mode == CapturedAlloca) {
    // Generic LLVM detail-pass case, not a V2 machine path: the pointer loaded
    // through the descriptor is a captured local allocation during the test.
    // Restore the descriptor before returning to the host.
    auto *local = b.CreateAlloca(b.getInt8Ty(), b.getInt64(16), "captured.local");
    store(b, local, descriptor);
    auto *captured = read(b, ptr, descriptor, "captured.guest.base");
    store(b, b.getInt64(A), byte(b, captured, std::uint64_t{0}));
    store(b, valueB, local);
    target = read(b, i64, byte(b, captured, std::uint64_t{0}), "observed");
    observed = target;
    store(b, data, descriptor);
  } else if (mode == LoopWord || mode == LoopByte) {
    auto *header = llvm::BasicBlock::Create(context, "header", function);
    auto *body = llvm::BasicBlock::Create(context, "body", function);
    auto *done = llvm::BasicBlock::Create(context, "done", function);
    auto *slot = byte(b, data, 8);
    store(b, b.getInt64(A), slot);
    auto *limit = b.CreateAdd(b.CreateAnd(rounds, b.getInt32(3)), b.getInt32(1));
    b.CreateBr(header);
    b.SetInsertPoint(header);
    auto *count = b.CreatePHI(i32, 2, "iteration");
    auto *sum = b.CreatePHI(i64, 2, "sum");
    count->addIncoming(b.getInt32(0), entry);
    sum->addIncoming(b.getInt64(0), entry);
    target = read(b, mode == LoopByte ? b.getInt16Ty() : i64,
        mode == LoopByte ? byte(b, data, 11) : slot, "observed");
    auto *sumNext = b.CreateAdd(sum, b.CreateZExtOrTrunc(target, i64));
    auto *next = b.CreateAdd(count, b.getInt32(1));
    b.CreateCondBr(b.CreateICmpULT(next, limit), body, done);
    b.SetInsertPoint(body);
    store(b, valueB, alias);
    count->addIncoming(next, body);
    sum->addIncoming(sumNext, body);
    b.CreateBr(header);
    b.SetInsertPoint(done);
    observed = sumNext;
    iterations = b.CreateZExt(next, i64);
  } else {
    auto *slot = byte(b, data, 8);
    auto *secondBase = mode == TwoBasesByte ? read(b, ptr, descriptor, "second.guest.base") : nullptr;
    llvm::Value *local = mode == PrivateAlloca
        ? static_cast<llvm::Value *>(b.CreateAlloca(i64, nullptr, "private.local")) : nullptr;
    store(b, b.getInt64(A), slot);
    switch (mode) {
    case AliasWord: case AliasByte: case NoAlias:
      store(b, valueB, alias);
      break;
    case AliasPartial:
      store(b, b.CreateTrunc(valueB, b.getInt16Ty()), alias);
      break;
    case AliasGEP:
      store(b, valueB, byte(b, alias, 8));
      break;
    case PrivateAlloca:
      store(b, valueB, local);
      auxiliary = read(b, i64, local, "private.value");
      break;
    case TwoBasesByte:
      store(b, valueB, byte(b, secondBase, 8));
      break;
    case UnknownCall: {
      auto callee = module.getOrInsertFunction("address_unknown_writer",
          llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr, i64}, false));
      b.CreateCall(callee, {regs, data, valueB});
      break;
    }
    case AtomicAdd:
      b.CreateAtomicRMW(llvm::AtomicRMWInst::Add, slot, b.getInt64(1), llvm::MaybeAlign(8),
          llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case CompareExchange:
      b.CreateAtomicCmpXchg(slot, b.getInt64(A), valueB, llvm::MaybeAlign(8),
          llvm::AtomicOrdering::SequentiallyConsistent,
          llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case Fence:
      b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    case Volatile:
      read(b, i64, slot, "volatile.observation")->setVolatile(true);
      break;
    default: llvm_unreachable("not a scalar mode");
    }
    const bool narrow = mode == AliasByte || mode == TwoBasesByte;
    target = read(b, narrow ? b.getInt16Ty() : i64,
        narrow ? byte(b, data, 11) : slot, "observed");
    observed = b.CreateZExtOrTrunc(target, i64);
  }
  auto *value = b.CreateXor(b.CreateXor(observed, x), auxiliary, "result");
  store(b, value, regs);
  store(b, observed, byte(b, regs, 8));
  store(b, iterations, byte(b, regs, 16));
  store(b, b.CreateXor(initialReg, value), byte(b, regs, 32));
  store(b, value, byte(b, data, 0x600));
  store(b, observed, byte(b, data, 0x608));
  llvm::Value *fields[] = {b.getInt64(1), b.getInt64(0),
      b.getInt64(0x8000 + mode * 16), b.CreateAdd(value, b.getInt64(0x1000)),
      b.getInt64(1), b.CreateXor(x, b.getInt64(0xdeadbeef)), iterations};
  for (unsigned index = 0; index != 7; ++index)
    store(b, fields[index], byte(b, exit, index * 8));
  b.CreateRet(value);
  return {function, target};
}

void dump(llvm::Module &module, const std::filesystem::path &path) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path.string(), error, llvm::sys::fs::OF_Text);
  if (error) { llvm::errs() << error.message() << '\n'; std::exit(2); }
  module.print(stream, nullptr);
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::filesystem::path out(argv[1]);
  for (unsigned mode = 0; mode != ModeCount; ++mode) {
    llvm::LLVMContext context;
    llvm::Module module("guest-memory-address-native", context);
    module.setDataLayout("e-p:64:64");
    auto view = build(module, static_cast<Mode>(mode));
    if (llvm::verifyModule(module, &llvm::errs())) return 3;
    dump(module, out / (std::string(names[mode]) + "-before.ll"));
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {65536, false});
    const bool invalid = llvm::verifyModule(module, &llvm::errs());
    dump(module, out / (std::string(names[mode]) + "-after.ll"));
    llvm::outs() << "case=" << names[mode] << " available=" << stats.available
                 << " forwarded=" << stats.loadsForwarded
                 << " bytes=" << stats.byteRebuilds << " phis=" << stats.phisInserted
                 << " changed=" << stats.changed << " complete=" << stats.complete
                 << " original_load=" << bool(view.target) << " invalid=" << invalid << '\n';
    if (invalid) return 3;
  }
  for (const auto mode : {Wrap8, AliasWord}) {
    llvm::LLVMContext context;
    llvm::Module module("guest-memory-address-fault-control", context);
    module.setDataLayout("e-p:64:64");
    const std::string name = mode == Wrap8 ? "control_width" : "control_alias";
    auto view = build(module, mode, name);
    auto *load = llvm::cast<llvm::LoadInst>(static_cast<llvm::Value *>(view.target));
    llvm::IRBuilder<> b(load);
    llvm::Value *wrong = mode == Wrap8
        ? b.CreateXor(view.function->getArg(3), b.getInt64(B))
        : static_cast<llvm::Value *>(b.getInt64(A));
    load->replaceAllUsesWith(wrong);
    load->eraseFromParent();
    if (llvm::verifyModule(module, &llvm::errs())) return 3;
    dump(module, out / (name + ".ll"));
  }
}
