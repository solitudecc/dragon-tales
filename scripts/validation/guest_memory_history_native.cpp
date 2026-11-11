#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <string>

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
const char *Kinds[] = {"phi", "load", "freeze"};
const char *Depths[] = {"one", "deep", "branch"};
const char *Faults[] = {"none", "fresh_history", "stale_fresh", "drop_store", "bounds", "reentry", "liveout"};
llvm::Value *address(Builder &b, llvm::Value *base, llvm::Value *index) {
  return b.CreateGEP(b.getInt8Ty(), base, index);
}
llvm::Value *at(Builder &b, llvm::Value *base, unsigned index) { return address(b, base, b.getInt64(index)); }
llvm::LoadInst *load(Builder &b, llvm::Type *type, llvm::Value *pointer, const char *name = "") {
  auto *op = b.CreateLoad(type, pointer, name); op->setAlignment(llvm::Align(1)); return op;
}
void store(Builder &b, llvm::Value *value, llvm::Value *pointer) {
  b.CreateStore(value, pointer)->setAlignment(llvm::Align(1));
}
struct View { llvm::Function *function; llvm::BasicBlock *body, *hub, *seed; llvm::LoadInst *state; };
View build(llvm::Module &module, unsigned kind, unsigned depth, unsigned fault = 0) {
  const bool deep = depth != 0, branched = depth == 2;
  auto &ctx = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(ctx); auto *i64 = llvm::Type::getInt64Ty(ctx);
  const std::string name = fault ? std::string("control_") + Faults[fault]
      : std::string("history_") + Kinds[kind] + "_" + Depths[depth];
  auto *fn = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, ptr, i64, i64, i64, i64}, false), llvm::GlobalValue::ExternalLinkage, name, module);
  for (auto &arg : fn->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto block = [&](const char *name) { return llvm::BasicBlock::Create(ctx, name, fn); };
  auto *entry = block("entry"), *seed = block("unknown.seed"), *hub = block("history.hub");
  auto *middle = deep ? block("history.middle") : nullptr;
  auto *middleSeed = branched ? block("unknown.middle") : nullptr;
  auto *shared = block("shared.future"), *checked = block("checked"), *valid = block("valid");
  auto *body = block("body.store"), *side = block("side.reentry"), *done = block("done"), *failure = block("failure");
  Builder b(entry);
  auto *regs = fn->getArg(0), *memory = fn->getArg(1), *ex = fn->getArg(2);
  auto *path = fn->getArg(3), *sp = fn->getArg(4), *x = fn->getArg(5), *limit = fn->getArg(6);
  auto *base = load(b, ptr, memory, "guest.base");
  if (branched) {
    auto *route = b.CreateSwitch(path, shared, 2);
    route->addCase(b.getInt64(1), seed); route->addCase(b.getInt64(3), middleSeed);
  } else b.CreateCondBr(b.CreateICmpEQ(path, b.getInt64(1)), seed, shared);
  b.SetInsertPoint(seed); auto *unknownFrame = b.CreateAdd(sp, b.getInt64(32)); b.CreateBr(hub);
  llvm::Value *middleFrame = nullptr, *middleHistory = nullptr;
  if (branched) {
    b.SetInsertPoint(middleSeed); middleFrame = b.CreateAdd(sp, b.getInt64(64));
    middleHistory = b.CreateXor(x, b.getInt64(0x33)); b.CreateBr(middle);
  }
  b.SetInsertPoint(hub);
  auto *hubFrame = b.CreatePHI(i64, 2, "history.frame");
  auto *hubHistory = b.CreatePHI(i64, 2, "history.value");
  auto *hubCount = b.CreatePHI(i64, 2, "history.count");
  hubFrame->addIncoming(unknownFrame, seed); hubHistory->addIncoming(x, seed); hubCount->addIncoming(b.getInt64(0), seed);
  b.CreateBr(deep ? middle : shared);
  llvm::Value *nextFrame = hubFrame, *nextHistory = hubHistory, *nextCount = hubCount;
  if (deep) {
    b.SetInsertPoint(middle);
    auto make = [&](llvm::Value *v, llvm::Value *unknown, const char *name) {
      auto *p = b.CreatePHI(i64, branched ? 2 : 1, name); p->addIncoming(v, hub);
      if (branched) p->addIncoming(unknown, middleSeed); return p;
    };
    nextFrame = make(hubFrame, middleFrame, "middle.frame");
    nextHistory = make(hubHistory, middleHistory, "middle.history");
    nextCount = make(hubCount, b.getInt64(0), "middle.count");
    b.CreateBr(shared);
  }
  b.SetInsertPoint(shared);
  auto *frame = b.CreatePHI(i64, 3, "current.frame"), *history = b.CreatePHI(i64, 3, "old.history"), *count = b.CreatePHI(i64, 3, "current.count");
  frame->addIncoming(sp, entry); history->addIncoming(x, entry); count->addIncoming(b.getInt64(0), entry);
  frame->addIncoming(nextFrame, deep ? middle : hub); history->addIncoming(nextHistory, deep ? middle : hub); count->addIncoming(nextCount, deep ? middle : hub);
  auto *index = b.CreateSub(frame, b.getInt64(168), "future.index");
  llvm::Value *fresh = kind == 0 ? b.CreateXor(frame, count, "future.scalar")
      : static_cast<llvm::Value *>(load(b, i64, at(b, base, 0x400), "fresh.load"));
  if (kind == 2) fresh = b.CreateFreeze(b.CreateXor(fresh, count), "fresh.freeze");
  b.CreateBr(checked);
  b.SetInsertPoint(checked);
  auto *size = load(b, i64, at(b, memory, 8), "guest.size");
  auto *fits = b.CreateAnd(b.CreateICmpUGE(size, b.getInt64(8)), b.CreateICmpULE(index, b.CreateSub(size, b.getInt64(8))));
  b.CreateCondBr(fault == 4 ? static_cast<llvm::Value *>(b.getTrue()) : fits, valid, failure);
  b.SetInsertPoint(valid);
  auto *slot = address(b, base, index); auto *state = load(b, i64, slot, "actual.state");
  state->setMetadata("history.target.load", llvm::MDNode::get(ctx, {}));
  auto *usedHistory = fault == 1 ? fresh : static_cast<llvm::Value *>(history);
  auto *usedFresh = fault == 2 ? static_cast<llvm::Value *>(history) : fresh;
  auto *result = b.CreateXor(state, b.CreateAdd(usedHistory, usedFresh));
  b.CreateCondBr(b.CreateICmpULT(count, limit), body, done);
  b.SetInsertPoint(body);
  store(b, b.CreateXor(x, b.CreateAdd(count, b.getInt64(0x55))), at(b, base, 0x400));
  if (fault != 3) store(b, b.getInt64(14), slot);
  auto *increment = b.CreateAdd(count, b.getInt64(1));
  auto *useSide = b.CreateAnd(b.CreateICmpEQ(path, b.getInt64(2)), b.CreateICmpEQ(count, b.getInt64(0)));
  b.CreateCondBr(useSide, side, hub);
  hubFrame->addIncoming(frame, body); hubHistory->addIncoming(fresh, body); hubCount->addIncoming(increment, body);
  b.SetInsertPoint(side);
  auto *sideFrame = fault == 5 ? static_cast<llvm::Value *>(frame) : b.CreateAdd(frame, b.getInt64(8));
  frame->addIncoming(sideFrame, side); history->addIncoming(fresh, side); count->addIncoming(increment, side); b.CreateBr(shared);
  auto finish = [&](llvm::BasicBlock *where, bool success) {
    b.SetInsertPoint(where);
    auto *value = success ? result : b.CreateXor(index, b.getInt64(0xbad0000000000000ULL));
    auto *live = fault == 6 ? static_cast<llvm::Value *>(x) : b.CreateXor(frame, b.CreateXor(fresh, history));
    store(b, value, at(b, regs, 0)); store(b, count, at(b, regs, 8)); store(b, live, at(b, regs, 16));
    store(b, fresh, at(b, regs, 24)); store(b, history, at(b, regs, 32)); store(b, index, at(b, regs, 40));
    store(b, value, at(b, base, 0x500)); store(b, live, at(b, base, 0x508));
    llvm::Value *fields[] = {b.getInt64(success ? 1 : 5), b.getInt64(0), b.getInt64(0x6000 + kind * 0x100 + depth * 0x40),
        index, b.getInt64(success), x, b.getInt64(success ? 0 : 3)};
    for (unsigned n = 0; n != 7; ++n) store(b, fields[n], at(b, ex, n * 8));
    b.CreateRet(value);
  };
  finish(done, true); finish(failure, false);
  return {fn, body, hub, seed, state};
}
void dump(llvm::Module &module, const std::filesystem::path &path) {
  std::error_code error; llvm::raw_fd_ostream out(path.string(), error, llvm::sys::fs::OF_Text);
  if (error) std::exit(2); module.print(out, nullptr);
}
}
int main(int argc, char **argv) {
  if (argc != 2) return 2; const std::filesystem::path out(argv[1]);
  for (unsigned kind = 0; kind != 3; ++kind) for (unsigned depth = 0; depth != 3; ++depth) {
    llvm::LLVMContext ctx; llvm::Module module("history-independent", ctx); module.setDataLayout("e-p:64:64");
    const auto v = build(module, kind, depth); const auto name = std::string(Kinds[kind]) + "_" + Depths[depth];
    if (llvm::verifyModule(module, &llvm::errs())) return 3;
    dump(module, out / (name + "-before.ll"));
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {});
    if (llvm::verifyModule(module, &llvm::errs())) return 4;
    dump(module, out / (name + "-after.ll"));
    unsigned targetLoads = 0;
    for (auto &bb : *v.function) for (auto &op : bb)
      if (llvm::isa<llvm::LoadInst>(op) && op.getMetadata("history.target.load")) ++targetLoads;
    llvm::outs() << "case=" << name << " available=" << stats.available << " forwarded=" << stats.loadsForwarded
        << " redirected=" << (v.body->getTerminator()->getSuccessor(1) != v.hub)
        << " unknown_retained=" << (v.seed->getTerminator()->getSuccessor(0) == v.hub)
        << " target_loads=" << targetLoads
        << " complete=" << stats.complete << " budget=" << stats.budgetExhausted << " invalid=0\n";
  }
  for (unsigned fault = 1; fault != 7; ++fault) {
    llvm::LLVMContext ctx; llvm::Module module("history-control", ctx); module.setDataLayout("e-p:64:64");
    build(module, 2, false, fault);
    if (llvm::verifyModule(module, &llvm::errs())) return 5;
    dump(module, out / (std::string("control_") + Faults[fault] + ".ll"));
  }
  for (unsigned kind = 0; kind != 3; ++kind) for (unsigned fault : {1u, 3u}) {
    llvm::LLVMContext ctx; llvm::Module module("history-branch-control", ctx); module.setDataLayout("e-p:64:64");
    auto view = build(module, kind, 2, fault);
    const auto name = std::string("control_") + Kinds[kind] + "_branch_" + Faults[fault];
    view.function->setName(name);
    if (llvm::verifyModule(module, &llvm::errs())) return 5;
    dump(module, out / (name + ".ll"));
  }
}
