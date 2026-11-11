#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <string>

using namespace dragon::llvmlifter;

namespace {
enum class Shape { Join, DuplicateJoin, DeadPred, SelfStable, SelfChanging,
                   BodyStable, BodyChanging, DuplicateLatch };

struct Fixture {
  llvm::LLVMContext context;
  llvm::Module module{"guest_join_edges", context};
  llvm::BasicBlock *left = nullptr, *right = nullptr, *header = nullptr;
  llvm::BasicBlock *body = nullptr, *dead = nullptr;
  llvm::WeakTrackingVH originalLoad;
  llvm::ReturnInst *result = nullptr;

  explicit Fixture(Shape shape) {
    module.setDataLayout("e-p:64:64");
    auto *ptr = llvm::PointerType::getUnqual(context);
    auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
        {ptr, ptr, llvm::Type::getInt1Ty(context),
         llvm::Type::getInt1Ty(context), llvm::Type::getInt32Ty(context)}, false);
    auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                           "join_edges", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    left = llvm::BasicBlock::Create(context, "left", function);
    if (shape != Shape::DuplicateLatch)
      right = llvm::BasicBlock::Create(context, "right", function);
    header = llvm::BasicBlock::Create(context, "header", function);
    const bool self = shape == Shape::SelfStable || shape == Shape::SelfChanging;
    const bool loop = self || shape == Shape::BodyStable ||
                      shape == Shape::BodyChanging || shape == Shape::DuplicateLatch;
    if (loop && !self) body = llvm::BasicBlock::Create(context, "body", function);
    auto *exit = loop ? llvm::BasicBlock::Create(context, "exit", function) : nullptr;
    if (shape == Shape::DeadPred) dead = llvm::BasicBlock::Create(context, "dead", function);
    llvm::IRBuilder<> builder(entry);
    auto *base = builder.CreateLoad(ptr, function->getArg(1), "guest.base");
    base->setAlignment(llvm::Align(1));
    auto *slot = builder.CreateGEP(builder.getInt8Ty(), base, builder.getInt64(16), "slot");
    if (right) builder.CreateCondBr(function->getArg(2), left, right);
    else builder.CreateBr(left);
    const auto store = [&](std::uint64_t value) {
      builder.CreateStore(builder.getInt64(value), slot)->setAlignment(llvm::Align(1));
    };
    builder.SetInsertPoint(left);
    store(1);
    if (shape == Shape::DuplicateJoin) {
      auto *dispatch = builder.CreateSwitch(function->getArg(4), header, 1);
      dispatch->addCase(builder.getInt32(0), header);
    } else builder.CreateBr(header);
    if (right) {
      builder.SetInsertPoint(right);
      store(2);
      builder.CreateBr(header);
    }
    if (dead) {
      builder.SetInsertPoint(dead);
      // This predecessor is unreachable but is still a real LLVM CFG edge.
      // Do not use the entry-defined pointer in this unreachable component.
      auto *deadBase = builder.CreateLoad(ptr, function->getArg(1));
      deadBase->setAlignment(llvm::Align(1));
      auto *deadSlot = builder.CreateGEP(builder.getInt8Ty(), deadBase, builder.getInt64(16));
      builder.CreateStore(builder.getInt64(3), deadSlot)->setAlignment(llvm::Align(1));
      builder.CreateBr(header);
    }
    builder.SetInsertPoint(header);
    auto *load = builder.CreateLoad(builder.getInt64Ty(), slot, "observed");
    load->setAlignment(llvm::Align(1));
    originalLoad = load;
    if (shape == Shape::SelfChanging) store(3);
    if (loop) builder.CreateCondBr(function->getArg(3), self ? header : body, exit);
    else result = builder.CreateRet(load);
    if (body) {
      builder.SetInsertPoint(body);
      if (shape == Shape::BodyChanging || shape == Shape::DuplicateLatch) store(2);
      if (shape == Shape::DuplicateLatch)
        builder.CreateCondBr(function->getArg(2), header, header);
      else builder.CreateBr(header);
    }
    if (exit) {
      builder.SetInsertPoint(exit);
      result = builder.CreateRet(load);
    }
  }
};

void verify(const llvm::Module &module) {
  std::string error;
  llvm::raw_string_ostream out(error);
  const bool invalid = llvm::verifyModule(module, &out);
  out.flush();
  INFO(error);
  REQUIRE_FALSE(invalid);
}

MemoryForwardStats run(Fixture &fixture, bool enablePhi = true) {
  verify(fixture.module);
  MemoryForwardOptions options;
  options.runMem2Reg = false;
  options.enablePhi = enablePhi;
  const auto stats = detail::runGuestMemorySSA(fixture.module, options);
  verify(fixture.module);
  return stats;
}

void checkConstantIncoming(const llvm::PHINode &phi, llvm::BasicBlock *pred,
                           std::uint64_t value, unsigned expectedCount) {
  unsigned count = 0;
  for (unsigned index = 0; index < phi.getNumIncomingValues(); ++index) {
    if (phi.getIncomingBlock(index) != pred) continue;
    auto *constant = llvm::dyn_cast<llvm::ConstantInt>(phi.getIncomingValue(index));
    REQUIRE(constant);
    CHECK(constant->getZExtValue() == value);
    ++count;
  }
  CHECK(count == expectedCount);
}

void checkOriginalLoad(const Fixture &fixture) {
  auto *value = static_cast<llvm::Value *>(fixture.originalLoad);
  REQUIRE(value);
  CHECK(llvm::isa<llvm::LoadInst>(value));
  CHECK(value->getName() == "observed");
}
} // namespace

TEST_CASE("Guest joins do not leave incomplete loop header PHIs",
          "[llvm][guest-memory-ssa][guest-memory-join-edges]") {
  for (auto shape : {Shape::SelfStable, Shape::SelfChanging,
                     Shape::BodyStable, Shape::BodyChanging}) {
    DYNAMIC_SECTION("shape " << static_cast<int>(shape)) {
    Fixture fixture(shape);
    run(fixture);
    checkOriginalLoad(fixture);
    // Incoming preheader values disagree. No fabricated backedge value is
    // permitted merely to turn a partially constructed PHI into valid IR.
    for (const auto &instruction : *fixture.header)
      CHECK_FALSE(instruction.getName().starts_with("guest.mem.phi"));
    }
  }
}

TEST_CASE("Guest joins retain loads when a real predecessor was not analyzed",
          "[llvm][guest-memory-ssa][guest-memory-join-edges]") {
  Fixture fixture(Shape::DeadPred);
  run(fixture);
  checkOriginalLoad(fixture);
  CHECK(fixture.header->phis().empty());
}

TEST_CASE("Guest join PHIs retain duplicate predecessor edge slots",
          "[llvm][guest-memory-ssa][guest-memory-join-edges]") {
  Fixture fixture(Shape::DuplicateJoin);
  const auto stats = run(fixture);
  CHECK(stats.loadsForwarded == 1);
  auto *phi = llvm::dyn_cast<llvm::PHINode>(fixture.result->getReturnValue());
  REQUIRE(phi);
  CHECK(phi->getParent() == fixture.header);
  CHECK(phi->getNumIncomingValues() == 3);
  checkConstantIncoming(*phi, fixture.left, 1, 2);
  checkConstantIncoming(*phi, fixture.right, 2, 1);
}

TEST_CASE("Guest complete ordinary joins still forward distinct values",
          "[llvm][guest-memory-ssa][guest-memory-join-edges]") {
  Fixture fixture(Shape::Join);
  const auto stats = run(fixture);
  CHECK(stats.loadsForwarded == 1);
  auto *phi = llvm::dyn_cast<llvm::PHINode>(fixture.result->getReturnValue());
  REQUIRE(phi);
  CHECK(phi->getNumIncomingValues() == 2);
  checkConstantIncoming(*phi, fixture.left, 1, 1);
  checkConstantIncoming(*phi, fixture.right, 2, 1);
}

TEST_CASE("Guest unequal joins retain loads when PHIs are disabled",
          "[llvm][guest-memory-ssa][guest-memory-join-edges]") {
  for (auto shape : {Shape::Join, Shape::DuplicateJoin, Shape::SelfStable,
                     Shape::BodyChanging, Shape::DeadPred}) {
    DYNAMIC_SECTION("shape " << static_cast<int>(shape)) {
    Fixture fixture(shape);
    const auto stats = run(fixture, false);
    checkOriginalLoad(fixture);
    CHECK(stats.loadsForwarded == 0);
    CHECK(fixture.header->phis().empty());
    }
  }
}

TEST_CASE("Guest canonical loop PHIs retain duplicate latch edge slots",
          "[llvm][guest-memory-ssa][guest-memory-join-edges]") {
  Fixture fixture(Shape::DuplicateLatch);
  const auto stats = run(fixture);
  CHECK(stats.loadsForwarded == 1);
  auto *phi = llvm::dyn_cast<llvm::PHINode>(fixture.result->getReturnValue());
  REQUIRE(phi);
  CHECK(phi->getParent() == fixture.header);
  CHECK(phi->getNumIncomingValues() == 3);
  checkConstantIncoming(*phi, fixture.left, 1, 1);
  checkConstantIncoming(*phi, fixture.body, 2, 2);
}
