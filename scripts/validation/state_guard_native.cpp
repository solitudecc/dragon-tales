#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <string>

// This producer deliberately does not use the unit-test builders or a machine
// lifter. Its input IR is compiled before and after the optional detail pass;
// an independent byte model in state_guard_diff.py defines the expected state.
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum Mode { Basic, SizeLoad, OuterGuard, Loop, Narrow8, Narrow32, Signed64,
            OrdinaryLoad, UnknownCall, Store, Atomic, Volatile, Fence, Count };
constexpr const char *names[] = {"basic", "size_load", "outer_guard", "loop",
    "narrow8", "narrow32", "signed64", "ordinary_load", "unknown_call",
    "store", "atomic", "volatile", "fence"};
struct View {
  llvm::Function *fn;
  llvm::BasicBlock *guard, *known, *unknown[3], *checked;
  llvm::Instruction *flags;
  llvm::BranchInst *stateBranch, *sizeBranch;
};
llvm::Value *byte(Builder &b, llvm::Value *base, unsigned index) {
  return b.CreateGEP(b.getInt8Ty(), base, b.getInt64(index));
}
llvm::LoadInst *read(Builder &b, llvm::Type *type, llvm::Value *ptr,
                     const char *name = "") {
  auto *result = b.CreateLoad(type, ptr, name);
  result->setAlignment(llvm::Align(1));
  return result;
}
void store(Builder &b, llvm::Value *value, llvm::Value *ptr) {
  b.CreateStore(value, ptr)->setAlignment(llvm::Align(1));
}
View build(llvm::Module &m, Mode mode, const std::string &overrideName = "") {
  auto &c = m.getContext();
  auto *ptr = llvm::PointerType::getUnqual(c);
  auto *i64 = llvm::Type::getInt64Ty(c);
  auto *i32 = llvm::Type::getInt32Ty(c);
  auto *fn = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, ptr, i64, i32, i32}, false), llvm::GlobalValue::ExternalLinkage,
      overrideName.empty() ? std::string("guard_") + names[mode] : overrideName, m);
  for (auto &arg : fn->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto *regs = fn->getArg(0), *desc = fn->getArg(1), *exit = fn->getArg(2);
  auto *x = fn->getArg(3), *route = fn->getArg(4), *rounds = fn->getArg(5);
  regs->setName("regs"); desc->setName("memory"); exit->setName("exit");
  x->setName("x"); route->setName("route"); rounds->setName("rounds");
  auto block = [&](const char *name) { return llvm::BasicBlock::Create(c, name, fn); };
  auto *entry = block("entry"), *select = block("choose.incoming");
  auto *known = block("known.fifty");
  llvm::BasicBlock *unknown[] = {block("unknown.one"), block("unknown.two"), block("unknown.three")};
  auto *guard = block("state.guard"), *stateFail = block("state.failure");
  auto *checked = block("checked.memory"), *memoryFail = block("memory.failure");
  auto *payload = block("payload"), *loopBack = block("loop.back");
  auto *outerFail = block("outer.failure"), *finish = block("finish");
  Builder b(entry);
  auto *data = read(b, ptr, desc, "guest.base");
  auto *initial = read(b, i64, byte(b, regs, 32), "initial.reg32");
  llvm::Value *size = nullptr;
  if (mode != SizeLoad) size = read(b, i64, byte(b, desc, 8), "entry.size");
  if (mode == OuterGuard)
    b.CreateCondBr(b.CreateICmpEQ(b.CreateAnd(x, b.getInt64(1)), b.getInt64(0)), select, outerFail);
  else b.CreateBr(select);
  b.SetInsertPoint(select);
  auto *dispatch = b.CreateSwitch(route, unknown[2], 3);
  dispatch->addCase(b.getInt32(0), known);
  dispatch->addCase(b.getInt32(1), unknown[0]);
  dispatch->addCase(b.getInt32(2), unknown[1]);
  auto width = mode == Narrow8 ? 8U : mode == Narrow32 ? 32U : 64U;
  auto *stateTy = b.getIntNTy(width);
  llvm::Value *inputs[4] = {b.getIntN(width, 50), nullptr, nullptr, nullptr};
  b.SetInsertPoint(known); b.CreateBr(guard);
  for (unsigned index = 0; index != 3; ++index) {
    b.SetInsertPoint(unknown[index]);
    auto *base = b.CreateTruncOrBitCast(x, stateTy);
    inputs[index + 1] = index == 0 ? base : index == 1
        ? b.CreateXor(base, b.getIntN(width, 0x55))
        : b.CreateAdd(base, b.getIntN(width, 1));
    b.CreateBr(guard);
  }
  b.SetInsertPoint(guard);
  auto *state = b.CreatePHI(stateTy, 5, "state");
  state->addIncoming(inputs[0], known);
  for (unsigned index = 0; index != 3; ++index) state->addIncoming(inputs[index + 1], unknown[index]);
  auto *iteration = b.CreatePHI(i64, 5, "iteration");
  iteration->addIncoming(b.getInt64(0), known);
  for (auto *pred : unknown) iteration->addIncoming(b.getInt64(0), pred);
  auto *wideState = b.CreateZExtOrBitCast(state, i64, "wide.state");
  if (mode == SizeLoad) size = read(b, i64, byte(b, desc, 8), "guard.size");
  llvm::Value *extra = b.getInt64(0);
  if (mode == OrdinaryLoad) extra = read(b, i64, byte(b, data, 0x500), "ordinary.payload");
  if (mode == UnknownCall) {
    auto helper = m.getOrInsertFunction("state_guard_unknown_writer",
        llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr, i64}, false));
    b.CreateCall(helper, {regs, data, x});
  } else if (mode == Store) store(b, x, byte(b, data, 0x520));
  else if (mode == Atomic || mode == Volatile) {
    auto *observation = read(b, i64, byte(b, data, 0x500), "barrier.observation");
    if (mode == Atomic) observation->setAtomic(llvm::AtomicOrdering::Acquire);
    else observation->setVolatile(true);
    extra = observation;
  } else if (mode == Fence) b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
  auto *flags = llvm::cast<llvm::Instruction>(b.CreateXor(
      b.CreateXor(wideState, b.getInt64(0xf00dfacecafebeefULL)),
      b.CreateXor(size, extra), "business.flags"));
  auto *condition = mode == Signed64 ? b.CreateICmpSGT(state, b.getIntN(width, 50))
                                    : b.CreateICmpUGT(state, b.getIntN(width, 50));
  auto *stateBranch = b.CreateCondBr(condition, stateFail, checked);

  // Both successors have outside PHIs, including a literal constant arm. The
  // edge copy must repair all of them, plus the loop live-outs below.
  b.SetInsertPoint(stateFail);
  auto *failState = b.CreatePHI(i64, 1, "failure.state"); failState->addIncoming(wideState, guard);
  auto *failFlags = b.CreatePHI(i64, 1, "failure.flags"); failFlags->addIncoming(flags, guard);
  auto *failIteration = b.CreatePHI(i64, 1, "failure.iteration"); failIteration->addIncoming(iteration, guard);
  auto *failConstant = b.CreatePHI(i64, 1, "failure.constant"); failConstant->addIncoming(b.getInt64(17), guard);
  b.CreateBr(finish);
  b.SetInsertPoint(checked);
  auto phi = [&](llvm::Value *v, const char *name) {auto *p = b.CreatePHI(i64, 1, name); p->addIncoming(v, guard); return p;};
  auto *checkedState = phi(wideState, "checked.state");
  auto *checkedFlags = phi(flags, "checked.flags");
  auto *checkedIteration = phi(iteration, "checked.iteration");
  auto *checkedSize = phi(size, "checked.size");
  auto *checkedConstant = phi(b.getInt64(23), "checked.constant");
  auto *sizeBranch = b.CreateCondBr(b.CreateICmpUGE(checkedSize, b.getInt64(0x508)), payload, memoryFail);
  b.SetInsertPoint(memoryFail); b.CreateBr(finish);
  b.SetInsertPoint(payload);
  auto *loaded = read(b, i64, byte(b, data, 0x500), "loaded.payload");
  if (mode == Loop) {
    auto *repeat = b.CreateICmpULT(checkedIteration, b.CreateZExt(rounds, i64));
    b.CreateCondBr(repeat, loopBack, finish);
  } else b.CreateBr(finish);
  b.SetInsertPoint(loopBack);
  if (mode == Loop) {
    auto *nextState = b.CreateTruncOrBitCast(x, stateTy);
    auto *nextIteration = b.CreateAdd(checkedIteration, b.getInt64(1));
    state->addIncoming(nextState, loopBack); iteration->addIncoming(nextIteration, loopBack);
    b.CreateBr(guard);
  } else b.CreateUnreachable();
  b.SetInsertPoint(outerFail); b.CreateBr(finish);
  b.SetInsertPoint(finish);
  auto finishPhi = [&](const char *name, llvm::Value *a, llvm::Value *z, llvm::Value *p,
                       llvm::Value *o) {
    auto *node = b.CreatePHI(i64, 4, name);
    node->addIncoming(a, stateFail); node->addIncoming(z, memoryFail);
    node->addIncoming(p, payload); node->addIncoming(o, outerFail); return node;
  };
  auto *outState = finishPhi("out.state", failState, checkedState, checkedState, b.getInt64(0xabc));
  auto *outFlags = finishPhi("out.flags", failFlags, checkedFlags, checkedFlags, b.getInt64(0xdef));
  auto *outIteration = finishPhi("out.iteration", failIteration, checkedIteration, checkedIteration, b.getInt64(0));
  auto *status = finishPhi("status", b.getInt64(2), b.getInt64(3), b.getInt64(1), b.getInt64(4));
  auto *value = finishPhi("value", failConstant, checkedConstant, loaded, b.getInt64(29));
  auto *result = b.CreateXor(b.CreateXor(outState, outFlags),
      b.CreateXor(b.CreateXor(status, value), b.CreateXor(outIteration, initial)), "result");
  store(b, result, regs); store(b, outState, byte(b, regs, 8));
  store(b, outFlags, byte(b, regs, 16)); store(b, outIteration, byte(b, regs, 24));
  store(b, b.CreateXor(initial, result), byte(b, regs, 32));
  store(b, result, byte(b, data, 0x600)); store(b, status, byte(b, data, 0x608));
  llvm::Value *fields[] = {status, b.getInt64(0), b.getInt64(0x9000 + mode * 16),
      b.CreateAdd(result, b.getInt64(0x1000)), b.getInt64(1),
      b.CreateXor(x, b.getInt64(0xdeadbeef)), outIteration};
  for (unsigned i = 0; i != 7; ++i) store(b, fields[i], byte(b, exit, i * 8));
  b.CreateRet(result);
  return {fn, guard, known, {unknown[0], unknown[1], unknown[2]}, checked,
          flags, stateBranch, sizeBranch};
}
void dump(llvm::Module &m, const std::filesystem::path &path) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path.string(), ec, llvm::sys::fs::OF_Text);
  if (ec) {llvm::errs() << ec.message() << '\n'; std::exit(2);}
  m.print(out, nullptr);
}
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::filesystem::path out(argv[1]);
  for (unsigned mode = 0; mode != Count; ++mode) {
    llvm::LLVMContext context;
    llvm::Module m("state-guard-independent-native", context);
    m.setDataLayout("e-p:64:64");
    auto view = build(m, static_cast<Mode>(mode));
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (std::string(names[mode]) + "-before.ll"));
    const auto blocksBefore = view.fn->size();
    const auto stats = dragon::llvmlifter::runDispatcherSpecialization(m);
    const bool invalid = llvm::verifyModule(m, &llvm::errs());
    dump(m, out / (std::string(names[mode]) + "-after.ll"));
    auto *knownTerminator = llvm::cast<llvm::BranchInst>(view.known->getTerminator());
    auto *next = knownTerminator->getSuccessor(0);
    auto *copyTerminator = llvm::dyn_cast<llvm::BranchInst>(next->getTerminator());
    const bool gain = next != view.guard && copyTerminator && copyTerminator->isUnconditional()
        && copyTerminator->getSuccessor(0) == view.checked;
    const bool unknownRetained = llvm::cast<llvm::BranchInst>(view.guard->getTerminator())->isConditional()
        && llvm::cast<llvm::BranchInst>(view.unknown[0]->getTerminator())->getSuccessor(0) == view.guard
        && llvm::cast<llvm::BranchInst>(view.unknown[1]->getTerminator())->getSuccessor(0) == view.guard
        && llvm::cast<llvm::BranchInst>(view.unknown[2]->getTerminator())->getSuccessor(0) == view.guard;
    llvm::outs() << "case=" << names[mode] << " available=" << stats.available
        << " changed=" << stats.changed << " complete=" << stats.complete
        << " branches=" << stats.branchesRewritten << " gain=" << gain
        << " unknown_retained=" << unknownRetained
        << " added_blocks=" << view.fn->size() - blocksBefore << " invalid=" << invalid << '\n';
    if (invalid) return 3;
  }
  for (unsigned control = 0; control != 3; ++control) {
    llvm::LLVMContext context;
    llvm::Module m("state-guard-fault-control", context); m.setDataLayout("e-p:64:64");
    const std::string name = control == 0 ? "control_unknown" : control == 1 ? "control_flags" : "control_failure";
    auto view = build(m, Basic, name);
    if (control == 0) view.stateBranch->setCondition(llvm::ConstantInt::getFalse(context));
    else if (control == 1) view.flags->replaceAllUsesWith(llvm::ConstantInt::get(view.flags->getType(), 0));
    else view.sizeBranch->setCondition(llvm::ConstantInt::getTrue(context));
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (name + ".ll"));
  }
}
