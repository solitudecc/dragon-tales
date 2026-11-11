// Reuse the prior independent checked-memory fixture, never the unit builders.
#define DRAGON_CHECKED_EDGE_PRODUCER_LIBRARY
#include "guest_memory_checked_edges_native.cpp"

namespace {
struct OutsideView { View base; llvm::PHINode *outside; };
OutsideView outsideBuild(llvm::Module &module, bool fromLoad,
                         const std::string &name) {
  auto view = build(module, Plain);
  view.function->setName(name);
  llvm::BasicBlock *success = nullptr, *checked = nullptr, *done = nullptr;
  llvm::Value *entryLoad = nullptr;
  for (auto &block : *view.function) {
    if (block.getName() == "success.liveout") success = &block;
    if (block.getName() == "checked.load") checked = &block;
    if (block.getName() == "done") done = &block;
    for (auto &op : block) if (op.getName() == "old.reg16") entryLoad = &op;
  }
  auto *outside = llvm::PHINode::Create(llvm::Type::getInt64Ty(module.getContext()),
      1, "outside.success.value", &success->front());
  outside->addIncoming(fromLoad ? entryLoad : view.function->getArg(5), checked);
  auto *result = llvm::cast<llvm::PHINode>(&done->front());
  const auto index = result->getBasicBlockIndex(success);
  Builder b(success->getTerminator());
  auto *combined = b.CreateXor(result->getIncomingValue(index), outside, "outside.result");
  result->setIncomingValue(index, combined);
  return {view, outside};
}
}
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::filesystem::path out(argv[1]);
  for (const bool fromLoad : {false, true}) {
    const std::string name = fromLoad ? "outside_load" : "outside_arg";
    llvm::LLVMContext context;
    llvm::Module m("checked-edge-outside-phi", context); m.setDataLayout("e-p:64:64");
    auto view = outsideBuild(m, fromLoad, name);
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (name + "-before.ll"));
    const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(m, {65536, false});
    const bool invalid = llvm::verifyModule(m, &llvm::errs());
    dump(m, out / (name + "-after.ll"));
    llvm::outs() << "case=" << name << " forwarded=" << stats.loadsForwarded
        << " redirected=" << redirected(view.base)
        << " unknown_edges=" << unknownPreserved(view.base)
        << " invalid=" << invalid << '\n';
    if (invalid) return 3;
  }
  for (const bool fromLoad : {false, true}) {
    const std::string name = fromLoad ? "outside_load" : "outside_arg";
    llvm::LLVMContext context;
    llvm::Module m("checked-edge-outside-phi-fault", context); m.setDataLayout("e-p:64:64");
    auto view = outsideBuild(m, fromLoad, name);
    Builder b(view.base.function->getEntryBlock().getTerminator());
    auto *bad = b.CreateXor(view.outside->getIncomingValue(0), b.getInt64(1), "bad.outside.value");
    view.outside->setIncomingValue(0, bad);
    if (llvm::verifyModule(m, &llvm::errs())) return 3;
    dump(m, out / (name + "-control.ll"));
  }
}
