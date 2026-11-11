#include <dragon/lifters/llvm/GuestMemorySSA.hpp>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum Mode { Plain, TwoGuards, Dynamic, LiveOut, Alias, Partial, UnknownCall,
            Fence, Volatile, Overwrite, Loop, ModeCount };
constexpr const char *names[] = {"plain", "two_guards", "dynamic", "liveout",
    "alias", "partial", "unknown_call", "fence", "volatile", "overwrite", "loop"};
struct View {
  llvm::Function *function;
  llvm::BasicBlock *known, *join;
  // Observe the original LOAD's lifetime, without following RAUW to a value
  // produced by another instruction. Existing regular SSA removes Overwrite.
  llvm::WeakVH targetLoad;
  llvm::BranchInst *guards[2];
};

llvm::Value *byte(Builder &b, llvm::Value *base, llvm::Value *offset) {
  return b.CreateGEP(b.getInt8Ty(), base, offset);
}
llvm::Value *byte(Builder &b, llvm::Value *base, std::uint64_t offset) {
  return byte(b, base, b.getInt64(offset));
}
void store(Builder &b, llvm::Value *value, llvm::Value *address) {
  b.CreateStore(value, address)->setAlignment(llvm::Align(1));
}
llvm::Value *read(Builder &b, llvm::Type *type, llvm::Value *address,
                  const char *name = "") {
  auto *load = b.CreateLoad(type, address, name);
  load->setAlignment(llvm::Align(1));
  return load;
}

View build(llvm::Module &module, Mode mode) {
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *type = llvm::FunctionType::get(i64, {ptr, ptr, ptr, i32, i64, i64, i32, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
      std::string("edge_") + names[mode], module);
  for (unsigned index = 0; index != function->arg_size(); ++index)
    function->addParamAttr(index, llvm::Attribute::NoUndef);
  auto *regs = function->getArg(0);
  auto *descriptor = function->getArg(1);
  auto *exit = function->getArg(2);
  auto *path = function->getArg(3);
  auto *sp = function->getArg(4);
  auto *x = function->getArg(5);
  auto *rounds = function->getArg(6);
  auto *alias = function->getArg(7);
  auto block = [&](const char *name) { return llvm::BasicBlock::Create(context, name, function); };
  auto *entry = block("entry");
  llvm::BasicBlock *incoming[] = {block("known"), block("unknown.one"),
                                block("unknown.two"), block("unknown.three")};
  auto *join = block("join");
  const bool singleGuard = mode == Plain || mode == Dynamic || mode == LiveOut;
  auto *second = singleGuard ? nullptr : block("guard.second");
  auto *loadBlock = block("checked.load");
  auto *success = block("success.liveout");
  auto *failureOne = block("failure.first");
  auto *failureTwo = singleGuard ? nullptr : block("failure.second");
  auto *early = mode == LiveOut ? block("early") : nullptr;
  auto *done = block("done");
  Builder b(entry);
  auto *data = read(b, ptr, descriptor, "guest.base");
  auto *size = read(b, i64, byte(b, descriptor, 8), "guest.size");
  auto *initial16 = read(b, i64, byte(b, regs, 16), "old.reg16");
  auto *knownRbp = b.CreateSub(sp, b.getInt64(8), "known.rbp");
  auto *slotIndex = b.CreateSub(knownRbp, b.getInt64(120), "store.index");
  auto *slot = byte(b, data, slotIndex);
  auto *roundLimit = b.CreateAdd(b.CreateAnd(rounds, b.getInt32(3)), b.getInt32(1));
  store(b, sp, regs);
  store(b, b.CreateXor(x, b.getInt64(0x11110000ULL + mode)), byte(b, regs, 8));
  auto *select = b.CreateSwitch(b.CreateAnd(path, b.getInt32(3)), incoming[3], 3);
  for (unsigned index = 0; index != 3; ++index) select->addCase(b.getInt32(index), incoming[index]);
  llvm::Value *rbps[4]{};
  for (unsigned index = 0; index != 4; ++index) {
    b.SetInsertPoint(incoming[index]);
    rbps[index] = index ? b.CreateAdd(sp, b.getInt64(24 + (index - 1) * 32))
                        : knownRbp;
    if (!index) {
      auto *value = mode == Dynamic
          ? b.CreateFreeze(b.CreateXor(x, b.getInt64(0x1357)), "stored.value")
          : static_cast<llvm::Value *>(b.getInt64(50));
      store(b, value, slot);
      switch (mode) {
      case Alias: store(b, b.CreateXor(x, b.getInt64(0x4567)), alias); break;
      case Partial: store(b, b.getInt8(0xab), byte(b, slot, 1)); break;
      case UnknownCall: {
        auto callee = module.getOrInsertFunction("edge_unknown_writer",
            llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr, ptr, i64}, false));
        b.CreateCall(callee, {regs, data, slot, x});
        break;
      }
      case Fence: b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent); break;
      default: break;
      }
    }
    if (mode == LiveOut && !index)
      b.CreateCondBr(b.CreateICmpEQ(b.CreateAnd(x, b.getInt64(1)), b.getInt64(0)), join, early);
    else b.CreateBr(join);
  }
  b.SetInsertPoint(join);
  auto *rbp = b.CreatePHI(i64, mode == Loop ? 5 : 4, "rbp");
  for (unsigned index = 0; index != 4; ++index) rbp->addIncoming(rbps[index], incoming[index]);
  auto *iteration = b.CreatePHI(i32, mode == Loop ? 5 : 4, "iteration");
  for (auto *pred : incoming) iteration->addIncoming(b.getInt32(0), pred);
  auto *loadIndex = b.CreateSub(rbp, b.getInt64(120), "load.index");
  auto *address = byte(b, data, loadIndex);
  if (mode == Overwrite) store(b, b.CreateXor(x, b.getInt64(0x987654321ULL)), address);
  auto *firstCondition = b.CreateICmpULE(loadIndex, size, "bounds.first");
  auto *secondCondition = b.CreateICmpUGE(b.CreateSub(size, loadIndex), b.getInt64(8), "bounds.second");
  auto *guardOne = b.CreateCondBr(singleGuard
                                    ? b.CreateAnd(firstCondition, secondCondition) : firstCondition,
                                singleGuard ? loadBlock : second,
                                failureOne);
  llvm::BranchInst *guardTwo = nullptr;
  if (second) {
    b.SetInsertPoint(second);
    guardTwo = b.CreateCondBr(secondCondition, loadBlock, failureTwo);
  }
  b.SetInsertPoint(loadBlock);
  auto *loaded = llvm::cast<llvm::LoadInst>(read(b, i64, address, "checked.value"));
  if (mode == Volatile) loaded->setVolatile(true);
  b.CreateBr(success);

  llvm::Value *results[4]{}, *offsets[4]{}, *iterations[4]{}, *tags[4]{};
  b.SetInsertPoint(success);
  store(b, b.CreateAdd(loaded, x), byte(b, data, 0x500));
  results[0] = b.CreateXor(loaded, x);
  offsets[0] = loadIndex;
  iterations[0] = b.CreateZExt(b.CreateAdd(iteration, b.getInt32(1)), i64);
  tags[0] = b.getInt64(0);
  if (mode == Loop) {
    auto *next = b.CreateAdd(iteration, b.getInt32(1));
    rbp->addIncoming(b.CreateAdd(sp, b.getInt64(24)), success);
    iteration->addIncoming(next, success);
    b.CreateCondBr(b.CreateICmpULT(next, roundLimit), join, done);
  } else b.CreateBr(done);

  for (unsigned index = 1; index != (singleGuard ? 2 : 3); ++index) {
    b.SetInsertPoint(index == 1 ? failureOne : failureTwo);
    store(b, b.CreateTrunc(b.CreateAdd(path, b.getInt32(index)), b.getInt8Ty()), byte(b, data, 0x510));
    results[index] = b.CreateXor(loadIndex, b.getInt64(0xbad0000000000000ULL + index));
    offsets[index] = loadIndex;
    iterations[index] = b.CreateZExt(iteration, i64);
    tags[index] = b.getInt64(index);
    b.CreateBr(done);
  }
  if (early) {
    b.SetInsertPoint(early);
    store(b, b.getInt8(0xe7), byte(b, data, 0x510));
    results[3] = b.CreateXor(x, b.getInt64(0xe411000000000000ULL));
    offsets[3] = slotIndex;
    iterations[3] = b.getInt64(0);
    tags[3] = b.getInt64(3);
    b.CreateBr(done);
  }
  b.SetInsertPoint(done);
  auto phi = [&](const char *name, llvm::Value **values) {
    auto *value = b.CreatePHI(i64, (singleGuard ? 2 : 3) + (early ? 1 : 0), name);
    value->addIncoming(values[0], success);
    value->addIncoming(values[1], failureOne);
    if (failureTwo) value->addIncoming(values[2], failureTwo);
    if (early) value->addIncoming(values[3], early);
    return value;
  };
  auto *result = phi("result", results);
  auto *offset = phi("observed.offset", offsets);
  auto *count = phi("observed.iterations", iterations);
  auto *tag = phi("exit.tag", tags);
  store(b, result, regs);
  store(b, offset, byte(b, regs, 8));
  store(b, b.CreateXor(initial16, result), byte(b, regs, 16));
  store(b, count, byte(b, regs, 24));
  store(b, b.CreateZExt(path, i64), byte(b, regs, 32));
  auto *isSuccess = b.CreateICmpEQ(tag, b.getInt64(0));
  auto *isEarly = b.CreateICmpEQ(tag, b.getInt64(3));
  llvm::Value *fields[] = {
      b.CreateSelect(isSuccess, b.getInt64(1), b.CreateSelect(isEarly, b.getInt64(2), b.getInt64(5))),
      b.CreateZExt(isEarly, i64),
      b.CreateAdd(b.getInt64(0x4000 + mode * 0x100), b.CreateMul(tag, b.getInt64(8))),
      b.CreateSelect(isSuccess, b.CreateAdd(result, b.getInt64(0x5000)), offset),
      b.CreateZExt(b.CreateOr(isSuccess, isEarly), i64),
      b.CreateXor(x, b.getInt64(0x9000)),
      b.CreateSelect(isSuccess, b.getInt64(0), b.CreateSelect(isEarly, b.getInt64(5), b.getInt64(3)))};
  for (unsigned index = 0; index != 7; ++index) store(b, fields[index], byte(b, exit, index * 8));
  b.CreateRet(result);
  return {function, incoming[0], join, loaded, {guardOne, guardTwo}};
}

void dump(const llvm::Module &module, const std::filesystem::path &path) {
  std::error_code error;
  llvm::raw_fd_ostream out(path.string(), error, llvm::sys::fs::OF_Text);
  if (error) { llvm::errs() << error.message() << '\n'; std::exit(2); }
  module.print(out, nullptr);
}
bool redirected(const View &view) {
  for (auto *successor : llvm::successors(view.known)) if (successor == view.join) return false;
  return true;
}
unsigned unknownPreserved(const View &view) {
  unsigned count = 0;
  for (const auto &block : *view.function) {
    if (block.getName() != "unknown.one" && block.getName() != "unknown.two" &&
        block.getName() != "unknown.three") continue;
    for (auto *successor : llvm::successors(&block))
      if (successor == view.join) ++count;
  }
  return count;
}
} // namespace

#ifndef DRAGON_CHECKED_EDGE_PRODUCER_LIBRARY
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::filesystem::path out(argv[1]);
  for (unsigned index = 0; index != ModeCount; ++index) {
    llvm::LLVMContext context;
    llvm::Module module("guest-memory-checked-edges-native", context);
    module.setDataLayout("e-p:64:64");
    const auto view = build(module, static_cast<Mode>(index));
    if (llvm::verifyModule(module, &llvm::errs())) return 3;
    dump(module, out / (std::string(names[index]) + "-before.ll"));
    const auto blocks = view.function->size();
    auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {65536, false});
    const bool invalid = llvm::verifyModule(module, &llvm::errs());
    dump(module, out / (std::string(names[index]) + "-after.ll"));
    llvm::outs() << "case=" << names[index] << " changed=" << stats.changed
        << " forwarded=" << stats.loadsForwarded << " phis=" << stats.phisInserted
        << " redirected=" << redirected(view)
        << " blocks_added=" << (view.function->size() - blocks)
        << " original_load=" << llvm::isa_and_nonnull<llvm::LoadInst>(static_cast<llvm::Value *>(view.targetLoad))
        << " unknown_edges=" << unknownPreserved(view)
        << " complete=" << stats.complete << " invalid=" << invalid << '\n';
    if (invalid) return 3;
  }
  for (const auto *control : {"global", "guard"}) {
    llvm::LLVMContext context;
    llvm::Module module("checked-edge-fault-control", context);
    module.setDataLayout("e-p:64:64");
    auto view = build(module, Plain);
    if (std::string(control) == "global") {
      auto *load = llvm::cast<llvm::LoadInst>(view.targetLoad);
      load->replaceAllUsesWith(llvm::ConstantInt::get(load->getType(), 50));
      load->eraseFromParent();
    } else {
      for (auto *guard : view.guards)
        if (guard) guard->setCondition(llvm::ConstantInt::getTrue(context));
    }
    if (llvm::verifyModule(module, &llvm::errs())) return 3;
    dump(module, out / (std::string("control-") + control + ".ll"));
  }
}
#endif
