#define main historical_original_main
#include "guest_memory_history_native.cpp"
#undef main

namespace pointer_phi {
const char *Names[] = {"different", "same", "alias_before", "alias_after", "call_before",
    "call_after", "base_reload", "different_base", "uncorrelated", "history", "history_mixed", "history_noanchor"};
const char *FaultNames[] = {"none", "wrong_phi", "drop_store", "drop_alias", "drop_call", "stale_pointer", "bounds"};
struct PView {
  llvm::Function *function;
  llvm::BasicBlock *source, *join, *unknown;
  unsigned sourceEdge;
};

PView historical(llvm::Module &module, unsigned mode, unsigned fault) {
  auto old = build(module, 2, 0);
  auto *fn = old.function;
  fn->setName(fault ? std::string("control_") + FaultNames[fault] : mode == 12 ? "structural_undef" : std::string("pointer_") + Names[mode]);
  llvm::BasicBlock *shared = nullptr, *side = nullptr;
  llvm::Value *base = nullptr;
  for (auto &block : *fn) {
    if (block.getName() == "shared.future") shared = &block;
    if (block.getName() == "side.reentry") side = &block;
  }
  for (auto &op : fn->getEntryBlock()) if (op.getName() == "guest.base") base = &op;
  auto *frame = llvm::cast<llvm::PHINode>(&*shared->phis().begin());
  Builder e(fn->getEntryBlock().getTerminator());
  auto makePointer = [&](Builder &b, llvm::Value *guest, llvm::Value *index) {
    if (mode == 11) return b.CreateIntToPtr(b.CreateAdd(b.CreatePtrToInt(guest, b.getInt64Ty()), index), b.getPtrTy());
    return address(b, guest, index);
  };
  auto *entryPointer = makePointer(e, base, e.CreateSub(fn->getArg(4), e.getInt64(168)));
  Builder u(old.seed->getTerminator());
  auto *hubFrame = llvm::cast<llvm::PHINode>(frame->getIncomingValueForBlock(old.hub));
  auto *seedBase = mode == 10 ? at(u, base, 64) : base;
  auto *seedPointer = mode == 12 ? static_cast<llvm::Value *>(llvm::UndefValue::get(u.getPtrTy()))
      : makePointer(u, seedBase, u.CreateSub(hubFrame->getIncomingValueForBlock(old.seed), u.getInt64(168)));
  Builder s(shared, shared->getFirstNonPHIIt());
  auto *pointer = s.CreatePHI(s.getPtrTy(), 3, "current.pointer");
  Builder h(old.hub, old.hub->getFirstNonPHIIt());
  auto *hubPointer = h.CreatePHI(h.getPtrTy(), 2, "historical.pointer");
  hubPointer->addIncoming(seedPointer, old.seed); hubPointer->addIncoming(pointer, old.body);
  pointer->addIncoming(entryPointer, &fn->getEntryBlock()); pointer->addIncoming(hubPointer, old.hub);
  Builder r(side->getTerminator());
  auto *sidePointer = makePointer(r, base, r.CreateSub(frame->getIncomingValueForBlock(side), r.getInt64(168)));
  pointer->addIncoming(sidePointer, side);
  for (auto &op : *old.body) if (auto *write = llvm::dyn_cast<llvm::StoreInst>(&op))
    if (auto *value = llvm::dyn_cast<llvm::ConstantInt>(write->getValueOperand()); value && value->equalsInt(14))
      write->setOperand(1, fault == 5 ? entryPointer : pointer);
  old.state->setMetadata("pointer.target.load", llvm::MDNode::get(module.getContext(), {}));
  return {fn, old.body, old.hub, old.seed, 1};
}

PView make(llvm::Module &module, unsigned mode, unsigned fault = 0, bool suffix = false) {
  if (mode >= 9) return historical(module, mode, fault);
  auto &ctx = module.getContext(); auto *ptr = llvm::PointerType::getUnqual(ctx);
  auto *i64 = llvm::Type::getInt64Ty(ctx);
  auto *fn = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, ptr, i64, i64, i64, ptr}, false), llvm::GlobalValue::ExternalLinkage,
      fault ? std::string(suffix ? "control_suffix_" : "control_") + FaultNames[fault]
            : std::string("pointer_") + Names[mode] + (suffix ? "_suffix" : ""), module);
  for (auto &arg : fn->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto block = [&](const char *name) { return llvm::BasicBlock::Create(ctx, name, fn); };
  auto *entry = block("entry"), *left = block("left"), *right = block("right");
  auto *source = block("source.join"), *unknown = block("unknown"), *join = block("checked.join");
  auto *valid = block("valid"), *failure = block("failure");
  Builder b(entry);
  auto *regs = fn->getArg(0), *desc = fn->getArg(1), *ex = fn->getArg(2), *path = fn->getArg(3), *sp = fn->getArg(4), *x = fn->getArg(5);
  auto *base = load(b, ptr, desc, "guest.base");
  llvm::Value *storeBase = base;
  if (mode == 6) storeBase = load(b, ptr, desc, "guest.other.base");
  if (mode == 7) storeBase = fn->getArg(6);
  auto *route = b.CreateSwitch(path, unknown, 2); route->addCase(b.getInt64(0), left); route->addCase(b.getInt64(1), right);
  b.SetInsertPoint(left);
  auto *leftPointer = address(b, storeBase, b.CreateSub(sp, b.getInt64(128)));
  auto *leftFrame = mode == 8 ? b.CreateAdd(sp, b.getInt64(32)) : static_cast<llvm::Value *>(sp);
  b.CreateBr(source);
  b.SetInsertPoint(right);
  auto *rightFrame = mode == 1 || mode == 8 ? static_cast<llvm::Value *>(sp) : b.CreateAdd(sp, b.getInt64(32));
  auto *rightPointer = address(b, storeBase, b.CreateSub(mode == 1 ? static_cast<llvm::Value *>(sp) : b.CreateAdd(sp, b.getInt64(32)), b.getInt64(128)));
  if (fault == 1) {
    // Rebuild the wrong left address here so it still dominates this PHI edge.
    rightPointer = address(b, storeBase, b.CreateSub(sp, b.getInt64(128)));
  }
  b.CreateBr(source);
  b.SetInsertPoint(source);
  auto *selectedFrame = b.CreatePHI(i64, 2, "selected.frame");
  selectedFrame->addIncoming(leftFrame, left); selectedFrame->addIncoming(rightFrame, right);
  auto *selectedPointer = b.CreatePHI(ptr, 2, "selected.pointer");
  selectedPointer->addIncoming(leftPointer, left); selectedPointer->addIncoming(rightPointer, right);
  auto aliasWrite = [&]() { store(b, b.CreateXor(x, b.getInt64(0x5555)), fn->getArg(6)); };
  auto call = [&]() {
    auto callee = module.getOrInsertFunction("edge_unknown_writer", llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr, ptr, i64}, false));
    b.CreateCall(callee, {regs, base, selectedPointer, x});
  };
  if (mode == 2) aliasWrite();
  if (mode == 4) call();
  if (fault != 2) store(b, b.getInt64(50), selectedPointer);
  if (mode == 3 && fault != 3) aliasWrite();
  if (mode == 5 && fault != 4) call();
  b.CreateBr(join);
  b.SetInsertPoint(unknown); auto *unknownFrame = b.CreateAdd(sp, b.getInt64(64)); b.CreateBr(join);
  b.SetInsertPoint(join);
  auto *frame = b.CreatePHI(i64, 2, "checked.frame"); frame->addIncoming(selectedFrame, source); frame->addIncoming(unknownFrame, unknown);
  auto *index = b.CreateSub(frame, b.getInt64(128), "checked.index");
  auto *size = load(b, i64, at(b, desc, 8), "guest.size");
  auto *fits = b.CreateAnd(b.CreateICmpUGE(size, b.getInt64(8)), b.CreateICmpULE(index, b.CreateSub(size, b.getInt64(8))));
  b.CreateCondBr(fault == 6 ? static_cast<llvm::Value *>(b.getTrue()) : fits, valid, failure);
  b.SetInsertPoint(valid);
  auto *state = load(b, i64, address(b, base, index), "actual.state");
  state->setMetadata("pointer.target.load", llvm::MDNode::get(ctx, {}));
  auto *result = b.CreateXor(state, x);
  auto finish = [&](llvm::BasicBlock *where, llvm::Value *value, bool success) {
    b.SetInsertPoint(where);
    store(b, value, at(b, regs, 0)); store(b, frame, at(b, regs, 8)); store(b, index, at(b, regs, 16)); store(b, path, at(b, regs, 24));
    store(b, value, at(b, base, 0x500));
    llvm::Value *fields[] = {b.getInt64(success ? 1 : 5), b.getInt64(0), b.getInt64(0x7000 + mode * 0x100), index, b.getInt64(success), x, b.getInt64(success ? 0 : 3)};
    for (unsigned n = 0; n != 7; ++n) store(b, fields[n], at(b, ex, n * 8));
    b.CreateRet(value);
  };
  auto *output = suffix ? block("output") : valid;
  if (suffix) b.CreateBr(output);
  finish(output, result, true);
  b.SetInsertPoint(failure); auto *bad = b.CreateXor(index, b.getInt64(0xbad0000000000000ULL)); finish(failure, bad, false);
  return {fn, source, join, unknown, 0};
}
}

int main(int argc, char **argv) {
  if (argc != 2) return 2; const std::filesystem::path out(argv[1]);
  auto runMode = [&](unsigned mode, bool suffix) {
    llvm::LLVMContext ctx; llvm::Module module("pointer-phi-independent", ctx); module.setDataLayout("e-p:64:64");
    auto view = pointer_phi::make(module, mode, 0, suffix);
    const std::string name = std::string(pointer_phi::Names[mode]) + (suffix ? "_suffix" : "");
    if (llvm::verifyModule(module, &llvm::errs())) return 3;
    dump(module, out / (name + "-before.ll"));
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {});
    if (llvm::verifyModule(module, &llvm::errs())) return 4;
    dump(module, out / (name + "-after.ll"));
    unsigned targets = 0;
    for (auto &block : *view.function) for (auto &op : block)
      if (llvm::isa<llvm::LoadInst>(op) && op.getMetadata("pointer.target.load")) ++targets;
    llvm::outs() << "case=" << name << " available=" << stats.available << " forwarded=" << stats.loadsForwarded
        << " redirected=" << (view.source->getTerminator()->getSuccessor(view.sourceEdge) != view.join)
        << " unknown_retained=" << (view.unknown->getTerminator()->getSuccessor(0) == view.join)
        << " target_loads=" << targets << " complete=" << stats.complete << " budget=" << stats.budgetExhausted << " invalid=0\n";
    return 0;
  };
  for (unsigned mode = 0; mode != std::size(pointer_phi::Names); ++mode)
    if (auto rc = runMode(mode, false)) return rc;
  for (unsigned mode = 0; mode != 9; ++mode)
    if (auto rc = runMode(mode, true)) return rc;
  for (unsigned fault = 1; fault != 7; ++fault) {
    llvm::LLVMContext ctx; llvm::Module module("pointer-phi-control", ctx); module.setDataLayout("e-p:64:64");
    pointer_phi::make(module, fault == 3 ? 3 : fault == 4 ? 5 : fault == 5 ? 9 : 0, fault);
    if (llvm::verifyModule(module, &llvm::errs())) return 5;
    dump(module, out / (std::string("control_") + pointer_phi::FaultNames[fault] + ".ll"));
  }
  for (unsigned fault : {1u, 2u, 3u, 4u, 6u}) {
    llvm::LLVMContext ctx; llvm::Module module("pointer-suffix-control", ctx); module.setDataLayout("e-p:64:64");
    pointer_phi::make(module, fault == 3 ? 3 : fault == 4 ? 5 : 0, fault, true);
    if (llvm::verifyModule(module, &llvm::errs())) return 5;
    dump(module, out / (std::string("control_suffix_") + pointer_phi::FaultNames[fault] + ".ll"));
  }
  {
    llvm::LLVMContext ctx; llvm::Module module("pointer-phi-undef-rejection", ctx); module.setDataLayout("e-p:64:64");
    pointer_phi::make(module, 12);
    if (llvm::verifyModule(module, &llvm::errs())) return 6;
    dump(module, out / "structural-undef-before.ll");
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {});
    if (llvm::verifyModule(module, &llvm::errs())) return 7;
    dump(module, out / "structural-undef-after.ll");
    llvm::outs() << "structural=undef forwarded=" << stats.loadsForwarded << " invalid=0\n";
  }
  {
    // A closed pointer SCC has no reachable entry value. It is an IR-only
    // robustness control, never an invented concrete native input.
    llvm::LLVMContext ctx; llvm::Module module("pointer-phi-closed-cycle", ctx); module.setDataLayout("e-p:64:64");
    auto *ptr = llvm::PointerType::getUnqual(ctx); auto *i64 = llvm::Type::getInt64Ty(ctx);
    auto *fn = llvm::Function::Create(llvm::FunctionType::get(i64, {ptr, ptr}, false), llvm::GlobalValue::ExternalLinkage, "closed_cycle", module);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn), *first = llvm::BasicBlock::Create(ctx, "first", fn), *second = llvm::BasicBlock::Create(ctx, "second", fn);
    Builder b(entry); auto *base = load(b, ptr, fn->getArg(1)); b.CreateRet(b.getInt64(0));
    b.SetInsertPoint(first); auto *p = b.CreatePHI(ptr, 1); b.CreateBr(second);
    b.SetInsertPoint(second); auto *q = b.CreatePHI(ptr, 1); q->addIncoming(p, first); p->addIncoming(q, second);
    store(b, b.getInt64(50), q); load(b, i64, at(b, base, 16)); b.CreateBr(first);
    if (llvm::verifyModule(module, &llvm::errs())) return 8;
    dump(module, out / "structural-closed-before.ll");
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, {});
    if (llvm::verifyModule(module, &llvm::errs())) return 9;
    dump(module, out / "structural-closed-after.ll");
    llvm::outs() << "structural=closed forwarded=" << stats.loadsForwarded << " invalid=0\n";
  }
}
