#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#define main carried_original_main
#include "loop_carried_context_native.cpp"
#undef main

// Independent inputs derived from the unchanged earlier producer. The extra
// stores are executable IR, never metadata facts or assumed memory contents.
namespace store_context {
constexpr const char *Cases[] = {"guest", "regs", "overlap", "descriptor",
    "narrow16", "wide128", "history", "outer", "call", "volatile", "atomic",
    "fence", "budget"};
enum ExtraFault { Good, DropStore, ReverseStores, StaleRead, DuplicateStore,
                  ForceKnown, NoBounds, BadDefault, BadHistory, StaleSize };
constexpr const char *Faults[] = {"none", "drop_store", "reverse_stores",
    "stale_read", "duplicate_store", "unknown", "bounds", "default",
    "history", "stale_size"};
struct Item { View view; llvm::BasicBlock *outer; };

Item make(llvm::Module &m, unsigned which, ExtraFault fault = Good) {
  const std::string name = Cases[which];
  Mode mode = name == "history" ? Phi : name == "call" ? Call
      : name == "fence" ? Fence : Store;
  Fault baseFault = fault == NoBounds ? Bounds : fault == BadDefault ? Default
      : fault == BadHistory ? OldState : None;
  auto view = build(m, mode, baseFault);
  auto *f = view.fn;
  f->setName(fault == Good ? "store_" + name : std::string("control_") + Faults[fault]);
  auto *head = view.head;
  llvm::Value *state = nullptr, *iteration = nullptr;
  llvm::LoadInst *observed = nullptr;
  llvm::Value *data = nullptr;
  for (auto &op : f->getEntryBlock())
    if (op.getName() == "guest.base") data = &op;
  for (auto &op : *head) {
    if (op.getName() == "state") state = &op;
    if (op.getName() == "iteration") iteration = &op;
    if (op.getMetadata("carried.observe")) observed = llvm::cast<llvm::LoadInst>(&op);
  }
  if (!data || !state || !iteration || !observed) std::exit(4);
  if (name != "history")
    llvm::cast<llvm::PHINode>(state)->setIncomingValue(
        llvm::cast<llvm::PHINode>(state)->getBasicBlockIndex(view.entry[0]),
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(m.getContext()), 50));
  // Remove only the base fixture's head STORE. Body writes and all original
  // LOAD/freeze/table/unknown/default/reentry operations remain.
  for (auto it = head->begin(); it != head->end();) {
    auto &op = *it++;
    if (llvm::isa<llvm::StoreInst>(op)) op.eraseFromParent();
  }
  Builder b(observed);
  auto *regs = f->getArg(0), *desc = f->getArg(1), *x = f->getArg(3);
  auto *effect = name == "descriptor" ? byte(b, desc, 8)
      : (name == "regs" || name == "overlap") ? byte(b, regs, 96)
      : byte(b, data, 0x100);
  auto *counterAddress = byte(b, data, 0x180);
  auto *counter = read(b, b.getInt64Ty(), counterAddress, "store.execution.counter");
  write(b, b.CreateAdd(counter, b.getInt64(fault == DuplicateStore ? 2 : 1)), counterAddress);
  const unsigned width = name == "narrow16" ? 16 : name == "wide128" ? 128 : 64;
  auto *type = llvm::IntegerType::get(m.getContext(), width);
  auto *oldEffect = read(b, type, effect, "effect.before.store");
  llvm::Value *payload = b.CreateZExtOrTrunc(b.CreateXor(x, state), type);
  if (name == "descriptor")
    payload = b.CreateOr(b.CreateAnd(payload, b.getInt64(0x1ff)), b.getInt64(0x400));
  if (width == 128) {
    auto *upper = b.CreateShl(b.CreateZExt(b.CreateXor(x, b.getInt64(0xabcdef)), type),
                            llvm::ConstantInt::get(type, 64));
    payload = b.CreateOr(payload, upper);
  }
  auto emit = [&](llvm::Value *v, const char *tag) {
    auto *s = b.CreateStore(v, effect); s->setAlignment(llvm::Align(1)); mark(s, tag); return s;
  };
  llvm::LoadInst *oldSize = nullptr;
  if (fault == StaleSize) oldSize = read(b, b.getInt64Ty(), byte(b, desc, 8), "bad.cached.size");
  llvm::StoreInst *first = nullptr;
  if (fault != ReverseStores) first = emit(payload, "plain.store.first");
  llvm::Value *fresh = fault == StaleRead || fault == ReverseStores ? oldEffect
      : read(b, type, effect, "effect.after.store");
  auto *last = b.CreateAdd(fresh, llvm::ConstantInt::get(type, 1));
  if (fault != DropStore) emit(last, "plain.store.last");
  if (fault == ReverseStores) first = emit(payload, "plain.store.first");
  if (name == "volatile") first->setVolatile(true);
  if (name == "atomic") {
    first->setAtomic(llvm::AtomicOrdering::Release); first->setAlignment(llvm::Align(8));
  }
  if (fault == ForceKnown) {
    auto *phi = llvm::cast<llvm::PHINode>(state);
    for (unsigned i = 1; i != 4; ++i)
      phi->setIncomingValue(phi->getBasicBlockIndex(view.entry[i]), b.getInt64(50));
  }
  if (oldSize)
    for (auto &block : *f) for (auto &op : block)
      if (op.getName() == "logical.size") op.replaceAllUsesWith(oldSize);

  llvm::BasicBlock *early = nullptr;
  if (name == "outer") {
    early = llvm::BasicBlock::Create(m.getContext(), "outer.failure", f);
    Builder e(early); auto *r = e.CreateXor(x, e.getInt64(0xabcd));
    write(e, r, regs); write(e, e.getInt64(0xe7), byte(e, data, 0x200));
    for (unsigned n = 0; n != 7; ++n) write(e, e.getInt64(0xf000 + n), byte(e, f->getArg(2), n * 8));
    e.CreateRet(r);
    auto *term = view.entry[0]->getTerminator(); Builder g(term);
    g.CreateCondBr(g.CreateICmpEQ(g.CreateAnd(x, g.getInt64(1)), g.getInt64(0)), head, early);
    term->eraseFromParent();
  }
  if (name == "budget") {
    // Many original known entries share this same effectful guard. Existing
    // module limits must leave some of them on the original path.
    auto *routing = llvm::cast<llvm::SwitchInst>(f->getEntryBlock().getTerminator());
    auto *statePhi = llvm::cast<llvm::PHINode>(state);
    for (unsigned n = 0; n != 3; ++n)
      statePhi->setIncomingValue(statePhi->getBasicBlockIndex(view.entry[n]), b.getInt64(n));
    for (unsigned n = 3; n != 20; ++n) {
      auto *seed = llvm::BasicBlock::Create(m.getContext(), "budget.known", f);
      Builder s(seed); s.CreateBr(head); routing->addCase(s.getInt32(n), seed);
      for (auto &phi : head->phis()) {
        llvm::Value *value = &phi == statePhi ? s.getInt64(n)
            : phi.getIncomingValueForBlock(view.entry[0]);
        phi.addIncoming(value, seed);
      }
    }
  }
  return {view, early};
}

unsigned sourceRedirects(const Item &item) {
  unsigned result = 0;
  for (auto *seed : item.view.entry)
    for (auto *succ : llvm::successors(seed))
      result += succ != item.view.head && succ != item.outer;
  result += item.view.body->getTerminator()->getSuccessor(0) != item.view.head;
  return result;
}
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const std::filesystem::path out(argv[1]);
  const bool explicitStores = std::string(argv[2]) == "stores";
  for (unsigned n = 0; n != std::size(store_context::Cases); ++n) {
    dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
    auto &m = lifter.module(); m.setDataLayout("e-p:64:64");
    auto item = store_context::make(m, n);
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (std::string(store_context::Cases[n]) + "-before.ll"));
    dragon::passes::PassPipeline pipeline;
    dragon::passes::Options options;
    if (explicitStores) options["allow_plain_stores"] = std::int64_t(1);
    const bool available = pipeline.add("dispatch-specialize", options);
    auto report = available ? pipeline.run(lifter) : dragon::passes::Report{};
    if (available && !report.ok) {llvm::errs() << report.error << '\n'; return 5;}
    if (!available) llvm::errs() << "case=" << store_context::Cases[n] << ' ' << pipeline.lastError() << '\n';
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (std::string(store_context::Cases[n]) + "-after.ll"));
    auto known = indices(m, item.view.fn->getName().str());
    unsigned count = 0;
    for (auto &block : *item.view.fn) for (auto &op : block) if (op.getMetadata("carried.table")) ++count;
    const auto rewritten = report.runs.empty() ? 0 : report.runs[0].result.counters["branches_rewritten"];
    const auto budget = report.runs.empty() ? 0 : report.runs[0].result.counters["budget_exhausted"];
    llvm::outs() << "case=" << store_context::Cases[n] << " available=" << available
        << " changed=" << report.changed << " redirected=" << store_context::sourceRedirects(item)
        << " index50=" << known.contains(0x590) << " index16=" << known.contains(0x480)
        << " table_loads=" << count << " branches=" << rewritten << " budget=" << budget
        << " unknown_retained=" << (item.view.entry[3]->getTerminator()->getSuccessor(0) == item.view.head)
        << " invalid=0\n";
  }
  for (unsigned fault = store_context::DropStore; fault <= store_context::StaleSize; ++fault) {
    llvm::LLVMContext c; llvm::Module m("store-context-fault", c); m.setDataLayout("e-p:64:64");
    store_context::make(m, fault == store_context::StaleSize ? 3 : 6,
                        static_cast<store_context::ExtraFault>(fault));
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (std::string("control_") + store_context::Faults[fault] + ".ll"));
  }
}
