#define main checked_edge_original_main
#include "guest_memory_checked_edges_native.cpp"
#undef main

#include <llvm/Transforms/Utils/Cloning.h>

namespace hub_context {
constexpr const char *Cases[] = {"plain", "deep", "two_guards", "dynamic", "liveout",
    "loop", "alias", "partial", "unknown_call", "fence", "volatile", "atomic",
    "overwrite", "history"};
enum Fault { Good, WrongPath, DropStore, Unknown, NoBounds, Reentry, Historical, LiveOutFault };
constexpr const char *Faults[] = {"none", "path", "drop_store", "unknown", "bounds",
    "reentry", "history", "liveout"};
struct Item { View view; llvm::BasicBlock *sources[4]; llvm::BasicBlock *first, *second; };

Mode baseMode(unsigned which) {
  const std::string n = Cases[which];
  return n == "two_guards" || n == "deep" ? TwoGuards : n == "dynamic" ? Dynamic
      : n == "liveout" ? LiveOut : n == "loop" || n == "history" ? Loop
      : n == "alias" ? Alias : n == "partial" ? Partial : n == "unknown_call" ? UnknownCall
      : n == "fence" ? Fence : n == "volatile" || n == "atomic" ? Volatile
      : n == "overwrite" ? Overwrite : Plain;
}

Item make(llvm::Module &m, unsigned which, Fault fault = Good) {
  const std::string name = Cases[which];
  auto v = build(m, baseMode(which));
  auto *f = v.function;
  f->setName(fault == Good ? "hub_" + name : std::string("control_") + Faults[fault]);
  llvm::BasicBlock *sources[4]{}, *success = nullptr;
  llvm::Value *data = nullptr;
  for (auto &block : *f) {
    if (block.getName() == "known") sources[0] = &block;
    if (block.getName() == "unknown.one") sources[1] = &block;
    if (block.getName() == "unknown.two") sources[2] = &block;
    if (block.getName() == "unknown.three") sources[3] = &block;
    if (block.getName() == "success.liveout") success = &block;
  }
  for (auto &op : f->getEntryBlock()) if (op.getName() == "guest.base") data = &op;
  auto *rbp = llvm::cast<llvm::PHINode>(&*v.join->phis().begin());
  llvm::Value *rbps[4];
  for (unsigned n = 0; n != 4; ++n) rbps[n] = rbp->getIncomingValueForBlock(sources[n]);
  const bool extraStores = name == "plain" || name == "two_guards" || name == "dynamic"
      || name == "liveout" || name == "loop";
  if (extraStores)
    for (unsigned n = 1; n != 3; ++n) {
      Builder b(sources[n]->getTerminator());
      auto *index = b.CreateSub(rbps[n], b.getInt64(120));
      store(b, b.getInt64(n == 1 ? 23 : 35), byte(b, data, index));
    }
  auto *first = llvm::BasicBlock::Create(m.getContext(), "hub.first", f, v.join);
  auto *second = llvm::BasicBlock::Create(m.getContext(), "hub.second", f, v.join);
  Builder b(first);
  std::vector<llvm::PHINode *> originalPhis, firstPhis, secondPhis;
  for (auto &phi : v.join->phis()) originalPhis.push_back(&phi);
  for (auto *phi : originalPhis) {
    auto *p = b.CreatePHI(phi->getType(), 2, "first." + phi->getName());
    p->addIncoming(phi->getIncomingValueForBlock(sources[0]), sources[0]);
    p->addIncoming(phi->getIncomingValueForBlock(sources[1]), sources[1]);
    firstPhis.push_back(p);
  }
  b.CreateBr(second);
  b.SetInsertPoint(second);
  for (unsigned n = 0; n != originalPhis.size(); ++n) {
    auto *p = b.CreatePHI(originalPhis[n]->getType(), 2, "second." + originalPhis[n]->getName());
    p->addIncoming(firstPhis[n], first);
    p->addIncoming(originalPhis[n]->getIncomingValueForBlock(sources[2]), sources[2]);
    secondPhis.push_back(p);
  }
  b.CreateBr(v.join);
  for (unsigned n = 0; n != 3; ++n) {
    auto *term = sources[n]->getTerminator();
    for (unsigned s = 0; s != term->getNumSuccessors(); ++s)
      if (term->getSuccessor(s) == v.join) term->setSuccessor(s, n == 2 ? second : first);
  }
  for (unsigned n = 0; n != originalPhis.size(); ++n) {
    for (unsigned p = 0; p != 3; ++p) originalPhis[n]->removeIncomingValue(sources[p], false);
    originalPhis[n]->addIncoming(secondPhis[n], second);
  }
  auto *load = llvm::cast<llvm::LoadInst>(static_cast<llvm::Value *>(v.targetLoad));
  load->setMetadata("hub.actual.load", llvm::MDNode::get(m.getContext(), {}));
  if (name == "atomic") {
    load->setVolatile(false); load->setAtomic(llvm::AtomicOrdering::Acquire); load->setAlignment(llvm::Align(8));
  }
  if (name == "history") {
    // Source zero can only execute after the old original LOAD. Its first-hub
    // PHI therefore has a valid prior-execution LOAD incoming, which must not
    // be interpreted as this copy's newly executed LOAD.
    auto *routing = llvm::cast<llvm::SwitchInst>(f->getEntryBlock().getTerminator());
    routing->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt32Ty(m.getContext()), 0))->setSuccessor(sources[1]);
    auto *back = llvm::cast<llvm::BranchInst>(success->getTerminator());
    llvm::Value *nextCount = originalPhis[1]->getIncomingValueForBlock(success);
    for (auto *phi : originalPhis) phi->removeIncomingValue(success, false);
    back->setSuccessor(0, sources[0]);
    firstPhis[1]->setIncomingValue(firstPhis[1]->getBasicBlockIndex(sources[0]), nextCount);
    Builder a(first, first->getFirstNonPHIIt());
    auto *old = a.CreatePHI(a.getInt64Ty(), 2, "historical.load");
    old->addIncoming(load, sources[0]); old->addIncoming(a.getInt64(42), sources[1]);
    Builder c(second, second->getFirstNonPHIIt());
    auto *middle = c.CreatePHI(c.getInt64Ty(), 2, "historical.middle");
    middle->addIncoming(old, first); middle->addIncoming(c.getInt64(43), sources[2]);
    Builder j(v.join, v.join->getFirstNonPHIIt());
    auto *joined = j.CreatePHI(j.getInt64Ty(), 2, "historical.joined");
    joined->addIncoming(middle, second); joined->addIncoming(j.getInt64(44), sources[3]);
    for (auto &block : *f) if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
      Builder out(ret);
      ret->setOperand(0, out.CreateXor(ret->getReturnValue(), fault == Historical
          ? static_cast<llvm::Value *>(out.getInt64(50)) : joined));
    }
  }
  if (fault == WrongPath)
    secondPhis[0]->setIncomingValue(secondPhis[0]->getBasicBlockIndex(sources[2]), rbps[0]);
  if (fault == DropStore)
    for (auto it = sources[0]->begin(); it != sources[0]->end();) {
      auto &op = *it++; if (llvm::isa<llvm::StoreInst>(op)) op.eraseFromParent();
    }
  if (fault == Unknown) {load->replaceAllUsesWith(llvm::ConstantInt::get(load->getType(), 50)); load->eraseFromParent();}
  if (fault == NoBounds)
    for (auto *guard : v.guards) if (guard) guard->setCondition(llvm::ConstantInt::getTrue(m.getContext()));
  if (fault == Reentry)
    rbp->setIncomingValue(rbp->getBasicBlockIndex(success), rbps[0]);
  if (fault == LiveOutFault)
    for (auto &block : *f) for (auto &op : block)
      if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&op)) {
        if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(s->getPointerOperand()))
          if (gep->getPointerOperand() == f->getArg(0) && llvm::isa<llvm::ConstantInt>(gep->getOperand(1))
              && llvm::cast<llvm::ConstantInt>(gep->getOperand(1))->equalsInt(16)) {
            if (auto *value = llvm::dyn_cast<llvm::BinaryOperator>(s->getValueOperand()))
              if (value->getOpcode() == llvm::Instruction::Xor) s->setOperand(0, value->getOperand(0));
          }
      }
  return {v, {sources[0], sources[1], sources[2], sources[3]}, first, second};
}

unsigned redirected(const Item &v) {
  unsigned n = 0;
  for (unsigned i = 0; i != 3; ++i)
    for (auto *succ : llvm::successors(v.sources[i]))
      if (succ != (i == 2 ? v.second : v.first) && succ->getName() != "early") ++n;
  return n;
}
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::filesystem::path out(argv[1]);
  for (unsigned n = 0; n != std::size(hub_context::Cases); ++n) {
    llvm::LLVMContext c; llvm::Module m("hub-independent", c); m.setDataLayout("e-p:64:64");
    auto v = hub_context::make(m, n);
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (std::string(hub_context::Cases[n]) + "-before.ll"));
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(m, {});
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (std::string(hub_context::Cases[n]) + "-after.ll"));
    llvm::outs() << "case=" << hub_context::Cases[n] << " available=" << stats.available
        << " changed=" << stats.changed << " forwarded=" << stats.loadsForwarded
        << " redirected=" << hub_context::redirected(v)
        << " unknown_retained=" << (v.sources[3]->getTerminator()->getSuccessor(0) == v.view.join)
        << " original_load=" << bool(v.view.targetLoad) << " invalid=0\n";
  }
  for (unsigned fault = hub_context::WrongPath; fault <= hub_context::LiveOutFault; ++fault) {
    llvm::LLVMContext c; llvm::Module m("hub-fault", c); m.setDataLayout("e-p:64:64");
    const auto which = fault == hub_context::Reentry ? 5 : fault == hub_context::Historical ? 13 : 0;
    hub_context::make(m, which, static_cast<hub_context::Fault>(fault));
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (std::string("control_") + hub_context::Faults[fault] + ".ll"));
  }
}
