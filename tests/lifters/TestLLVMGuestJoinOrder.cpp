#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <functional>
#include <string>
#include <utility>

using namespace dragon::llvmlifter;

namespace {
struct JoinFixture {
  llvm::LLVMContext context;
  llvm::Module module{"guest-join-order", context};
  llvm::BasicBlock *left = nullptr;
  llvm::BasicBlock *right = nullptr;
  llvm::StoreInst *result = nullptr;

  JoinFixture(bool reverseHostOrder, bool storeOnRight) {
    module.setDataLayout("e-p:64:64");
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *i64 = llvm::Type::getInt64Ty(context);
    auto *i1 = llvm::Type::getInt1Ty(context);
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                {ptr, ptr, i64, i1}, false),
        llvm::GlobalValue::ExternalLinkage, "join_order", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *one = llvm::BasicBlock::Create(context);
    auto *two = llvm::BasicBlock::Create(context);
    if (std::less<llvm::BasicBlock *>{}(two, one)) std::swap(one, two);
    // Choose roles after allocation. Both layouts have identical named IR,
    // but the left block's host pointer sorts before/after the right block.
    left = reverseHostOrder ? two : one;
    right = reverseHostOrder ? one : two;
    left->setName("left");
    right->setName("right");
    left->insertInto(function);
    right->insertInto(function);
    auto *join = llvm::BasicBlock::Create(context, "join", function);
    llvm::IRBuilder<llvm::NoFolder> ir(entry);
    auto *base = ir.CreateLoad(ptr, function->getArg(1), "guest.base");
    base->setAlignment(llvm::Align(1));
    auto *slot = ir.CreateGEP(ir.getInt8Ty(), base, function->getArg(2), "slot");
    ir.CreateCondBr(function->getArg(3), left, right);
    ir.SetInsertPoint(left);
    ir.CreateStore(ir.getInt64(10), slot)->setAlignment(llvm::Align(1));
    ir.CreateBr(join);
    ir.SetInsertPoint(right);
    if (storeOnRight)
      ir.CreateStore(ir.getInt64(20), slot)->setAlignment(llvm::Align(1));
    ir.CreateBr(join);
    ir.SetInsertPoint(join);
    auto *load = ir.CreateLoad(i64, slot, "observed");
    load->setAlignment(llvm::Align(1));
    result = ir.CreateStore(load, function->getArg(0));
    result->setAlignment(llvm::Align(1));
    ir.CreateRetVoid();
  }

  std::string text() const {
    std::string result;
    llvm::raw_string_ostream output(result);
    module.print(output, nullptr);
    return result;
  }
};
} // namespace

TEST_CASE("Guest join rejection uses IR predecessor order",
          "[llvm][guest-memory-ssa][guest-join-order]") {
  JoinFixture normal(false, false), reversed(true, false);
  REQUIRE(normal.text() == reversed.text());
  MemoryForwardOptions options;
  options.runMem2Reg = false;
  options.enablePhi = false; // Keep checked copies out of this join test.
  for (auto *fixture : {&normal, &reversed}) {
    REQUIRE_FALSE(llvm::verifyModule(fixture->module, &llvm::errs()));
    const auto stats = detail::runGuestMemorySSA(fixture->module, options);
    CHECK(stats.available);
    CHECK(stats.complete);
    CHECK(stats.joinBarriers == 1);
    CHECK(stats.loadsForwarded == 0);
    CHECK(llvm::isa<llvm::LoadInst>(fixture->result->getValueOperand()));
    REQUIRE_FALSE(llvm::verifyModule(fixture->module, &llvm::errs()));
  }
  CHECK(normal.text() == reversed.text());
}

TEST_CASE("Guest join PHI operands ignore host allocation order",
          "[llvm][guest-memory-ssa][guest-join-order]") {
  JoinFixture normal(false, true), reversed(true, true);
  REQUIRE(normal.text() == reversed.text());
  MemoryForwardOptions options;
  options.runMem2Reg = false;
  for (auto *fixture : {&normal, &reversed}) {
    REQUIRE_FALSE(llvm::verifyModule(fixture->module, &llvm::errs()));
    const auto stats = detail::runGuestMemorySSA(fixture->module, options);
    CHECK(stats.available);
    CHECK(stats.complete);
    CHECK(stats.joinBarriers == 0);
    CHECK(stats.phisInserted == 1);
    CHECK(stats.loadsForwarded == 1);
    auto *phi = llvm::dyn_cast<llvm::PHINode>(fixture->result->getValueOperand());
    REQUIRE(phi);
    REQUIRE(phi->getNumIncomingValues() == 2);
    CHECK(phi->getIncomingBlock(0) == fixture->left);
    CHECK(phi->getIncomingBlock(1) == fixture->right);
    CHECK(phi->getIncomingValue(0) == llvm::ConstantInt::get(phi->getType(), 10));
    CHECK(phi->getIncomingValue(1) == llvm::ConstantInt::get(phi->getType(), 20));
    REQUIRE_FALSE(llvm::verifyModule(fixture->module, &llvm::errs()));
  }
  CHECK(normal.text() == reversed.text());
}
