#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

using namespace dragon::llvmlifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
struct View {
  llvm::BasicBlock *pointerStore, *legacyStore, *hub, *join, *loaded;
  llvm::LoadInst *load;
};
View addFixture(llvm::Module &module, const char *name, bool pointer, bool legacy) {
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *f = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, i64, i64, llvm::Type::getInt1Ty(context)}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  for (auto &arg : f->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto block = [&](const char *n) { return llvm::BasicBlock::Create(context, n, f); };
  auto *entry = block("entry"), *unknown = block("unknown");
  auto *a = pointer ? block("anchor.a") : nullptr;
  auto *b = pointer ? block("anchor.b") : nullptr;
  auto *source = pointer ? block("pointer.store") : nullptr;
  auto *known = legacy ? block("legacy.store") : nullptr;
  auto *other = legacy ? block("legacy.unknown") : nullptr;
  auto *hub = legacy ? block("legacy.hub") : nullptr;
  auto *join = block("checked.join"), *loaded = block("checked.load");
  auto *done = block("done"), *failure = block("failure");
  Builder ir(entry);
  auto *base = ir.CreateLoad(ptr, f->getArg(1), "guest.base"); base->setAlignment(llvm::Align(1));
  auto *route = ir.CreateSwitch(f->getArg(3), unknown);
  llvm::Value *pointerFrame = nullptr, *hubFrame = nullptr;
  if (pointer) {
    route->addCase(ir.getInt64(0), a); route->addCase(ir.getInt64(1), b);
    ir.SetInsertPoint(a);
    auto *pa = ir.CreateGEP(ir.getInt8Ty(), base, ir.CreateAdd(f->getArg(2), ir.getInt64(16)));
    ir.CreateBr(source);
    ir.SetInsertPoint(b);
    auto *fb = ir.CreateAdd(f->getArg(2), ir.getInt64(8));
    auto *pb = ir.CreateGEP(ir.getInt8Ty(), base, ir.CreateAdd(fb, ir.getInt64(16)));
    ir.CreateBr(source);
    ir.SetInsertPoint(source);
    auto *frame = ir.CreatePHI(i64, 2, "pointer.frame");
    frame->addIncoming(f->getArg(2), a); frame->addIncoming(fb, b);
    auto *address = ir.CreatePHI(ptr, 2, "pointer.address");
    address->addIncoming(pa, a); address->addIncoming(pb, b);
    ir.CreateStore(ir.getInt64(14), address)->setAlignment(llvm::Align(1));
    ir.CreateBr(join); pointerFrame = frame;
  }
  if (legacy) {
    route->addCase(ir.getInt64(2), known); route->addCase(ir.getInt64(3), other);
    ir.SetInsertPoint(known);
    auto *fk = ir.CreateAdd(f->getArg(2), ir.getInt64(32));
    auto *address = ir.CreateGEP(ir.getInt8Ty(), base, ir.CreateAdd(fk, ir.getInt64(16)));
    ir.CreateStore(ir.getInt64(34), address)->setAlignment(llvm::Align(1)); ir.CreateBr(hub);
    ir.SetInsertPoint(other);
    auto *fo = ir.CreateAdd(f->getArg(2), ir.getInt64(48)); ir.CreateBr(hub);
    ir.SetInsertPoint(hub);
    auto *frame = ir.CreatePHI(i64, 2, "hub.frame");
    frame->addIncoming(fk, known); frame->addIncoming(fo, other); ir.CreateBr(join); hubFrame = frame;
  }
  ir.SetInsertPoint(unknown); ir.CreateBr(join);
  ir.SetInsertPoint(join);
  auto *frame = ir.CreatePHI(i64, 3, "selected.frame");
  frame->addIncoming(f->getArg(2), unknown);
  if (pointer) frame->addIncoming(pointerFrame, source);
  if (legacy) frame->addIncoming(hubFrame, hub);
  auto *index = ir.CreateAdd(frame, ir.getInt64(16));
  ir.CreateCondBr(f->getArg(4), loaded, failure);
  ir.SetInsertPoint(loaded);
  auto *address = ir.CreateGEP(ir.getInt8Ty(), base, index);
  auto *load = ir.CreateLoad(i64, address, "actual.load"); load->setAlignment(llvm::Align(1));
  ir.CreateBr(done);
  ir.SetInsertPoint(done); ir.CreateRet(load);
  ir.SetInsertPoint(failure); ir.CreateRet(ir.getInt64(0));
  return {source, known, hub, join, loaded, load};
}
}

TEST_CASE("Guest pointer PHI searches preserve legacy hub choice for the same LOAD",
          "[llvm][guest-memory-ssa][guest-pointer-phi-priority]") {
  llvm::LLVMContext context; llvm::Module module("same-load-priority", context);
  module.setDataLayout("e-p:64:64");
  auto view = addFixture(module, "competing_paths", true, true);
  REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
  MemoryForwardOptions options; options.runMem2Reg = false;
  const auto stats = detail::runGuestMemorySSA(module, options);
  CHECK(stats.loadsForwarded == 1);
  CHECK(view.pointerStore->getTerminator()->getSuccessor(0) == view.join);
  CHECK(view.legacyStore->getTerminator()->getSuccessor(0) != view.hub);
  CHECK(view.load->getParent() == view.loaded);
  REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
}

TEST_CASE("Guest pointer PHI searches preserve later module legacy hub budget",
          "[llvm][guest-memory-ssa][guest-pointer-phi-priority]") {
  llvm::LLVMContext context; llvm::Module module("module-priority", context);
  module.setDataLayout("e-p:64:64");
  auto earlier = addFixture(module, "earlier_pointer", true, false);
  auto later = addFixture(module, "later_legacy_hub", false, true);
  REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
  MemoryForwardOptions options; options.runMem2Reg = false; options.maxRewrites = 1;
  const auto stats = detail::runGuestMemorySSA(module, options);
  CHECK(stats.loadsForwarded == 1);
  CHECK_FALSE(stats.complete);
  CHECK(earlier.pointerStore->getTerminator()->getSuccessor(0) == earlier.join);
  CHECK(later.legacyStore->getTerminator()->getSuccessor(0) != later.hub);
  CHECK(earlier.load->getParent() == earlier.loaded);
  CHECK(later.load->getParent() == later.loaded);
  REQUIRE_FALSE(llvm::verifyModule(module, &llvm::errs()));
}
