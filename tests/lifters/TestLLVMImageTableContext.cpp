#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#ifdef DRAGON_IMAGE_TABLE_CONTEXT_TESTING
#include "../../lib/LLVMImageTableContext.hpp"
#endif
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>

using namespace dragon::llvmlifter::detail;
namespace {
// This new private API cannot compile on the old product. These tests establish
// the new proof boundary; they are not evidence of an old execution defect.
struct Fixture {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  unsigned mode;
  explicit Fixture(unsigned mode = 0) : mode(mode) {
    std::string ir = R"IR(
target datalayout = "e-p:64:64"
declare void @ordinary(ptr, ptr, ptr, ptr)
declare void @unknown_call(ptr)
declare i32 @llvm.fshl.i32(i32, i32, i32)
define void @view(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {
entry:
  %slot = alloca i64
  %pcslot = alloca i64
  %regdesc = getelementptr i8, ptr %state, i64 24
  %regs = load ptr, ptr %regdesc, align 1
  %guest = load ptr, ptr %memory, align 1
  %spaddr = getelementptr i8, ptr %regs, i64 32
  %initialsp = load i64, ptr %spaddr, align 1
  store i64 %initialsp, ptr %slot
  %pcaddr = getelementptr i8, ptr %regs, i64 128
  %initialpc = load i64, ptr %pcaddr, align 1
  store i64 %initialpc, ptr %pcslot
  %pc = load i64, ptr %pcslot
  switch i64 %pc, label %unsupported [ i64 4096, label %bounds ]
bounds:
  %sizeaddr = getelementptr i8, ptr %memory, i64 8
  %size = load i64, ptr %sizeaddr, align 1
  %large = icmp uge i64 %size, 65536
  %nonnull = icmp ne ptr %guest, null
  %valid = and i1 %large, %nonnull
  br i1 %valid, label %spcheck, label %fallback
spcheck:
  %spok = icmp eq i64 %initialsp, 65536
  br i1 %spok, label %table0, label %fallback
table0:
  %t0p = getelementptr i8, ptr %guest, i64 8192
  %t0 = load i64, ptr %t0p, align 1
  %t0ok = icmp eq i64 %t0, 4352
  br i1 %t0ok, label %table1, label %fallback
table1:
  %t1p = getelementptr i8, ptr %guest, i64 8200
  %t1 = load i64, ptr %t1p, align 1
  %t1ok = icmp eq i64 %t1, 4608
  br i1 %t1ok, label %accepted, label %fallback
accepted:
  br label %prologue
prologue:
  %sp0 = load i64, ptr %slot
  %frame = sub i64 %sp0, 64
  store i64 %frame, ptr %slot
  %stateoff = add i64 %frame, 16
  %statep = getelementptr i8, ptr %guest, i64 %stateoff
  store i32 0, ptr %statep, align 1
  ; BASE_EFFECT
  br label %controller
controller:
  %sp1 = load i64, ptr %slot
  %off1 = add i64 %sp1, 16
  %statep1 = getelementptr i8, ptr %guest, i64 %off1
  %index = load i32, ptr %statep1, align 1
  %inrange = icmp ule i32 %index, 1
  br i1 %inrange, label %read, label %finished
read:
  %wide = zext i32 %index to i64
  %scaled = shl i64 %wide, 3
  %address = add i64 %scaled, 8192
  %targetp = getelementptr i8, ptr %guest, i64 %address
  %target = load i64, ptr %targetp, align 1
  switch i64 %target, label %unknown [
    i64 4096, label %prologue
    i64 4352, label %a
    i64 4608, label %b
  ]
a:
  %asp = load i64, ptr %slot
  %aoff = add i64 %asp, 16
  %ap = getelementptr i8, ptr %guest, i64 %aoff
  store i32 1, ptr %ap, align 1
  ; STEP_EFFECT
  br label %controller
b:
  %bsp = load i64, ptr %slot
  %boff = add i64 %bsp, 16
  %bp = getelementptr i8, ptr %guest, i64 %boff
  store i32 0, ptr %bp, align 1
  %choice = icmp eq ptr %runtime, null
  br i1 %choice, label %controller, label %finished
finished:
  ; TERMINAL_EFFECT
  ret void
unknown:
  store i64 %target, ptr %exit, align 1
  ret void
unsupported:
  ret void
fallback:
  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)
  ret void
}
)IR";
    auto replace = [&](const std::string &from, const std::string &to) {
      const auto at = ir.find(from); REQUIRE(at != std::string::npos);
      ir.replace(at, from.size(), to);
    };
    switch (mode) {
    case 1: replace("%t1, 4608", "%t1, 4609"); break;
    case 2: replace("%initialsp, 65536", "%initialsp, 65535"); break;
    case 3: replace("; BASE_EFFECT", "store i8 0, ptr %t0p, align 1"); break;
    case 4: replace("; STEP_EFFECT", "store i8 0, ptr %t1p, align 1"); break;
    case 5: replace("; STEP_EFFECT", "call void @unknown_call(ptr %guest)"); break;
    case 6: replace("store i32 1, ptr %ap", "store volatile i32 1, ptr %ap"); break;
    case 7: replace("store i32 1, ptr %ap, align 1", "store atomic i32 1, ptr %ap monotonic, align 4"); break;
    case 8: replace("%inrange = icmp ule i32 %index, 1", "%inrange = icmp ne ptr %runtime, null"); break;
    case 9: replace("; STEP_EFFECT", "%bad = getelementptr i8, ptr %guest, i64 8191\n  store i16 0, ptr %bad, align 1"); break;
    case 10: replace("; TERMINAL_EFFECT", "call void @unknown_call(ptr %guest)\n  store i64 0, ptr %t0p, align 1"); break;
    case 11: replace("; STEP_EFFECT", "%safe = getelementptr i8, ptr %guest, i64 8191\n  store i8 0, ptr %safe, align 1"); break;
    case 12: replace("; STEP_EFFECT", "%rotation = call i32 @llvm.fshl.i32(i32 %index, i32 %index, i32 13)"); break;
    case 13: replace("; STEP_EFFECT", "%unbounded = load i64, ptr %ap, align 1\n  %wrap = add i64 %asp, %unbounded\n  %wrap_p = getelementptr i8, ptr %guest, i64 %wrap\n  store i64 0, ptr %wrap_p, align 1"); break;
    case 14: replace("; STEP_EFFECT", "%badshift = shl i64 1, 64\n  %masked = and i64 %badshift, 7\n  %badaddr = add i64 %asp, %masked\n  %badptr = getelementptr i8, ptr %guest, i64 %badaddr\n  store i8 0, ptr %badptr, align 1"); break;
    case 15: replace("; STEP_EFFECT", "%external = getelementptr i8, ptr %state, i64 16\n  store i64 0, ptr %external, align 1"); break;
    case 16: replace("; STEP_EFFECT", "%changed_sp = sub i64 %asp, 8\n  store i64 %changed_sp, ptr %slot"); break;
    case 17:
      replace("br i1 %inrange, label %read, label %finished", "br i1 %inrange, label %joined, label %joined\njoined:\n  %joinedindex = phi i32 [ %index, %controller ], [ %index, %controller ]\n  br label %read");
      replace("%wide = zext i32 %index to i64", "%wide = zext i32 %joinedindex to i64");
      break;
    case 18:
      replace("  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)\n  ret void\n}",
              "  call void @ordinary(ptr %state, ptr %memory, ptr %runtime, ptr %exit)\n  ret void\ndead:\n  ret void\n}\n@held = global ptr blockaddress(@view, %dead)");
      break;
    case 19: ir += "\n@held = global ptr blockaddress(@view, %finished)\n"; break;
    case 20:
      replace("%t1, 4608", "%t1, 4352");
      replace("a:\n  %asp", "a:\n  %alive = phi i32 [ %index, %read ], [ 7, %b ]\n  %asp");
      replace("; STEP_EFFECT", "%livep = getelementptr i8, ptr %guest, i64 65496\n  store i32 %alive, ptr %livep, align 1");
      replace("br i1 %choice, label %controller, label %finished", "br i1 %choice, label %a, label %finished");
      break;
    }
    llvm::SMDiagnostic error;
    module = llvm::parseAssemblyString(ir, error, context);
    if (!module) { std::string message; llvm::raw_string_ostream out(message); error.print("fixture", out); INFO(out.str()); }
    REQUIRE(module);
    REQUIRE_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  }
  std::string ir() const { std::string result; llvm::raw_string_ostream out(result); module->print(out, nullptr); return out.str(); }
  ImageTableContextRequest request() const { return {"view", 8192, {4352, mode == 20 ? 4352u : 4608u}}; }
};
}

TEST_CASE("Image table context closes base and every loop step before rewriting", "[llvm][image-table-context]") {
  for (unsigned mode : {0u, 2u, 10u, 11u, 12u, 20u}) DYNAMIC_SECTION("accepted shape " << mode) {
    Fixture f(mode); auto *function = f.module->getFunction("view"); auto *argument = function->getArg(0);
    auto result = runImageTableCheckedContext(*f.module, f.request()); INFO(result.message);
    REQUIRE(result.closed); REQUIRE(result.changed); CHECK_FALSE(result.budgetExhausted);
    CHECK(result.guardedBytes == 16); CHECK(result.preservedStores >= 3); CHECK(result.constantValues > 0);
    CHECK(f.module->getFunction("view") == function); CHECK(function->getArg(0) == argument);
    CHECK(f.module->getFunction("ordinary")->isDeclaration());
    unsigned sourceReads = 0, stateSwitches = 0, fallbacks = 0, ordinaryCalls = 0;
    for (auto &block : *function) {
      if (block.getName().starts_with("image.table.fallback")) {
        ++fallbacks;
        auto *sw = llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator()); REQUIRE(sw);
        CHECK(sw->getNumCases() == 3); CHECK(sw->getDefaultDest()->getName() == "unknown");
        sourceReads += llvm::isa<llvm::LoadInst>(sw->getCondition());
      }
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());
          sw && sw->getCondition()->getType()->isIntegerTy(32)) {
        ++stateSwitches; CHECK(sw->getNumCases() == 2);
        CHECK(sw->getDefaultDest()->getName().starts_with("image.table.fallback"));
      }
      for (auto &i : block) if (auto *call = llvm::dyn_cast<llvm::CallBase>(&i))
        ordinaryCalls += call->getCalledFunction() == f.module->getFunction("ordinary");
    }
    CHECK(sourceReads == 1); CHECK(stateSwitches == 1); CHECK(fallbacks == 1); CHECK(ordinaryCalls == 1);
    REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    if (mode == 10) CHECK(f.ir().find("call void @unknown_call") != std::string::npos);
  }
}

TEST_CASE("Image table context refuses incomplete invariants without changing source", "[llvm][image-table-context]") {
  for (unsigned mode : {1u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 13u, 14u, 15u, 16u, 17u, 18u, 19u})
    DYNAMIC_SECTION("rejected shape " << mode) {
      Fixture f(mode); const auto before = f.ir(); auto *function = f.module->getFunction("view");
      auto *argument = function->getArg(0); auto result = runImageTableCheckedContext(*f.module, f.request());
      INFO(result.message); CHECK_FALSE(result.closed); CHECK_FALSE(result.changed);
      CHECK_FALSE(result.message.empty()); CHECK(f.ir() == before);
      CHECK(f.module->getFunction("view") == function); CHECK(function->getArg(0) == argument);
      REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    }
}

TEST_CASE("Image table context validates candidate bytes and shared work limits", "[llvm][image-table-context]") {
  for (unsigned mode = 0; mode != 5; ++mode) DYNAMIC_SECTION("request " << mode) {
    Fixture f; auto request = f.request();
    if (mode == 0) request.targets[1] = 4609;
    if (mode == 1) request.tableOffset = 8193;
    if (mode == 2) request.maxWork = 32;
    if (mode == 3) request.maxBlocks = 2;
    if (mode == 4) request.targets.push_back(0x1300);
    const auto before = f.ir(); auto result = runImageTableCheckedContext(*f.module, request);
    INFO(result.message); CHECK_FALSE(result.closed); CHECK_FALSE(result.changed); CHECK(f.ir() == before);
    if (mode == 2) CHECK(result.budgetExhausted);
  }
}

TEST_CASE("Closed image table mode consumes only a complete joint proof", "[llvm][image-table-context][closed-image-table-context]") {
  for (unsigned mode : {0u, 2u, 10u, 11u, 12u, 20u}) DYNAMIC_SECTION("closed positive " << mode) {
    Fixture f(mode); auto *function = f.module->getFunction("view"); auto *argument = function->getArg(0);
    auto *ordinary = f.module->getFunction("ordinary");
    const auto result = runImageTableCheckedContext(*f.module, f.request(), true);
    INFO(result.message); REQUIRE(result.closed); REQUIRE(result.changed); CHECK_FALSE(result.budgetExhausted);
    CHECK(f.module->getFunction("view") == function); CHECK(function->getArg(0) == argument);
    CHECK(f.module->getFunction("ordinary") == ordinary); CHECK(ordinary->isDeclaration());
    unsigned stateSwitches = 0, oldTargetSwitches = 0, actualReads = 0, ordinaryCalls = 0, unreachable = 0;
    for (auto &block : *function) {
      CHECK_FALSE(block.getName().starts_with("image.table.fallback"));
      unreachable += llvm::isa<llvm::UnreachableInst>(block.getTerminator());
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator())) {
        oldTargetSwitches += sw->getCondition()->getType()->isIntegerTy(64) &&
                            llvm::isa<llvm::LoadInst>(sw->getCondition());
        if (sw->getCondition()->getType()->isIntegerTy(32)) {
          ++stateSwitches; CHECK(sw->getNumCases() == 2);
          CHECK(sw->getDefaultDest()->getName() == "unknown");
          auto *snapshot = llvm::dyn_cast<llvm::StoreInst>(&sw->getDefaultDest()->front()); REQUIRE(snapshot);
          auto *read = llvm::dyn_cast<llvm::LoadInst>(snapshot->getValueOperand()); REQUIRE(read);
          CHECK(read->isSimple()); CHECK(read->getType()->isIntegerTy(64));
          CHECK(read->getParent() == &block); CHECK(read->comesBefore(sw));
          ++actualReads;
        }
      }
      for (auto &i : block) if (auto *call = llvm::dyn_cast<llvm::CallBase>(&i))
        ordinaryCalls += call->getCalledFunction() == ordinary;
    }
    CHECK(stateSwitches == 1); CHECK(oldTargetSwitches == 1); // external PC entry switch only
    CHECK(actualReads == 1); CHECK(ordinaryCalls == 1); CHECK(unreachable == 0);
    REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
  }
  for (unsigned mode : {1u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 13u, 14u, 15u, 16u, 17u, 18u, 19u})
    DYNAMIC_SECTION("closed refusal " << mode) {
      Fixture f(mode); const auto before = f.ir(); auto *function = f.module->getFunction("view");
      auto *argument = function->getArg(0); const auto result = runImageTableCheckedContext(*f.module, f.request(), true);
      INFO(result.message); CHECK_FALSE(result.closed); CHECK_FALSE(result.changed);
      CHECK(f.ir() == before); CHECK(f.module->getFunction("view") == function); CHECK(function->getArg(0) == argument);
    }
}

TEST_CASE("Image table publication preserves module metadata and fastcc musttail identity", "[llvm][image-table-context][image-table-context-transaction]") {
  for (bool closed : {false, true}) for (bool fast : {false, true})
    DYNAMIC_SECTION("closed " << closed << " fastcc " << fast) {
      Fixture f;
      auto *function = f.module->getFunction("view");
      auto *argument = function->getArg(0);
      auto *ordinary = f.module->getFunction("ordinary");
      if (fast) {
        function->setCallingConv(llvm::CallingConv::Fast);
        function->addFnAttr(llvm::Attribute::NoUnwind);
        ordinary->setCallingConv(llvm::CallingConv::Fast);
        for (auto &block : *function) for (auto &instruction : block)
          if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
            if (call->getCalledFunction() != ordinary) continue;
            call->setCallingConv(llvm::CallingConv::Fast);
            call->setTailCallKind(llvm::CallInst::TCK_MustTail);
          }
      }
      auto *sentinel = f.module->getOrInsertNamedMetadata("sentinel");
      auto *operand = llvm::MDNode::get(f.context, {});
      sentinel->addOperand(operand);
      REQUIRE_FALSE(f.module->getNamedMetadata("llvm.dbg.cu"));
      REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
      auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
      INFO(result.message);
      CHECK(result.changed); CHECK(result.closed);
      CHECK(f.module->getFunction("view") == function);
      CHECK(function->getArg(0) == argument);
      CHECK(f.module->getFunction("ordinary") == ordinary);
      CHECK(f.module->getNamedMetadata("sentinel") == sentinel);
      REQUIRE(sentinel->getNumOperands() == 1);
      CHECK(sentinel->getOperand(0) == operand);
      CHECK_FALSE(f.module->getNamedMetadata("llvm.dbg.cu"));
      CHECK(function->getCallingConv() == (fast ? llvm::CallingConv::Fast : llvm::CallingConv::C));
      if (fast) CHECK(function->hasFnAttribute(llvm::Attribute::NoUnwind));
      unsigned calls = 0;
      for (auto &block : *function) for (auto &instruction : block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
          if (call->getCalledFunction() != ordinary) continue;
          ++calls;
          CHECK(call->isMustTailCall() == fast);
          CHECK(call->getCallingConv() == (fast ? llvm::CallingConv::Fast : llvm::CallingConv::C));
        }
      CHECK(calls == 1);
      REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    }
}

TEST_CASE("Image table context refuses escaping local and foreign block identities exactly", "[llvm][image-table-context][image-table-context-transaction]") {
  for (bool closed : {false, true}) for (unsigned mode = 0; mode != 6; ++mode)
    DYNAMIC_SECTION("closed " << closed << " identity " << mode) {
      Fixture f;
      auto *function = f.module->getFunction("view");
      auto *argument = function->getArg(0);
      auto *entry = &function->getEntryBlock();
      auto *instruction = &entry->front();
      llvm::TrackingMDRef retained;
      if (mode < 3) {
        llvm::Value *value = mode == 0 ? static_cast<llvm::Value *>(argument) :
                            mode == 1 ? static_cast<llvm::Value *>(entry) : instruction;
        retained.reset(llvm::ValueAsMetadata::get(value));
        REQUIRE(value->isUsedByMetadata());
      } else {
        auto *other = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(f.context), false),
                                             llvm::GlobalValue::ExternalLinkage, "foreign", *f.module);
        auto *head = llvm::BasicBlock::Create(f.context, "head", other);
        auto *target = llvm::BasicBlock::Create(f.context, "target", other);
        llvm::BranchInst::Create(target, head);
        llvm::ReturnInst::Create(f.context, target);
        auto *address = llvm::BlockAddress::get(other, target);
        if (mode == 3) {
          auto *type = llvm::Type::getInt64Ty(f.context);
          auto *wrapped = llvm::ConstantExpr::getAdd(llvm::ConstantExpr::getPtrToInt(address, type),
                                                    llvm::ConstantInt::get(type, 1));
          llvm::BinaryOperator::CreateAdd(wrapped, llvm::ConstantInt::get(type, 0), "foreign.address", entry->getTerminator());
        } else {
          auto *inner = llvm::MDNode::get(f.context, llvm::ValueAsMetadata::get(address));
          auto *outer = llvm::MDNode::get(f.context, inner);
          if (mode == 4) instruction->setMetadata("capture", outer);
          else function->setMetadata("capture", outer);
        }
      }
      REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
      const auto before = f.ir();
      auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
      INFO(result.message);
      REQUIRE_FALSE(result.changed); CHECK_FALSE(result.closed);
      CHECK(f.ir() == before);
      CHECK(f.module->getFunction("view") == function);
      CHECK(function->getArg(0) == argument);
      CHECK(&function->getEntryBlock() == entry);
      CHECK(&entry->front() == instruction);
      REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    }
}

#ifdef DRAGON_IMAGE_TABLE_CONTEXT_TESTING
TEST_CASE("Image table context restores metadata and old body on controlled commit failures", "[llvm][image-table-context][image-table-context-transaction]") {
  for (bool closed : {false, true}) for (unsigned stage : {1u, 2u})
    DYNAMIC_SECTION("closed " << closed << " failure stage " << stage) {
      Fixture f;
      auto *function = f.module->getFunction("view");
      auto *argument = function->getArg(0);
      auto *entry = &function->getEntryBlock();
      auto *instruction = &entry->front();
      auto *sentinel = f.module->getOrInsertNamedMetadata("sentinel");
      auto *operand = llvm::MDNode::get(f.context, {});
      sentinel->addOperand(operand);
      REQUIRE_FALSE(f.module->getNamedMetadata("llvm.dbg.cu"));
      const auto before = f.ir();
      auto result = runImageTableContextCommitFailureForTest(*f.module, f.request(), closed, stage);
      INFO(result.message);
      CHECK_FALSE(result.changed); CHECK_FALSE(result.closed);
      CHECK(f.ir() == before);
      CHECK(f.module->getFunction("view") == function);
      CHECK(function->getArg(0) == argument);
      CHECK(&function->getEntryBlock() == entry);
      CHECK(&entry->front() == instruction);
      CHECK(f.module->getNamedMetadata("sentinel") == sentinel);
      REQUIRE(sentinel->getNumOperands() == 1);
      CHECK(sentinel->getOperand(0) == operand);
      CHECK_FALSE(f.module->getNamedMetadata("llvm.dbg.cu"));
      REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    }
}
#endif
