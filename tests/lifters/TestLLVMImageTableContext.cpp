#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#ifdef DRAGON_IMAGE_TABLE_CONTEXT_TESTING
#include "../../lib/LLVMImageTableContext.hpp"
#endif
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
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
    case 21:
      // No invented table target returns to the prologue. The private SP
      // expression may become dead after all Guest addresses are reassociated.
      replace("    i64 4096, label %prologue\n", "");
      break;
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

// An actual entry snapshot guard is useful beyond SP: an output pointer can
// write on every dispatcher iteration.  Only that exact loaded observation
// is constrained; another load from the same address is not a fact.
struct RegisterGuardFixture : Fixture {
  enum Kind { Exact, Missing, Partial, FreshGuard, FreshUse, NotEqual,
              Conflicting, GuestAliasWrite, AliasThenFreshUse, UnknownCall,
              UndefCheck, PoisonCheck };
  explicit RegisterGuardFixture(Kind kind = Exact, unsigned bits = 64) {
    auto *f = module->getFunction("view");
    auto named = [&](llvm::StringRef name) -> llvm::Value * {
      for (auto &b : *f) for (auto &i : b) if (i.getName() == name) return &i;
      return nullptr;
    };
    auto block = [&](llvm::StringRef name) -> llvm::BasicBlock * {
      for (auto &b : *f) if (b.getName() == name) return &b;
      return nullptr;
    };
    auto *regs = named("regs"); auto *guest = named("guest");
    REQUIRE(regs); REQUIRE(guest);
    auto *fallback = block("fallback"); auto *a = block("a");
    auto *spcheck = block("spcheck"); REQUIRE(fallback); REQUIRE(a); REQUIRE(spcheck);
    llvm::IRBuilder<> entry(f->getEntryBlock().getTerminator());
    auto *address = entry.CreateGEP(entry.getInt8Ty(), regs, entry.getInt64(48), "output.snapshot.address");
    auto *snapshot = entry.CreateAlignedLoad(entry.getIntNTy(bits), address, llvm::Align(1), "initial.output");
    if (kind != Missing) {
      auto *spbranch = llvm::cast<llvm::BranchInst>(spcheck->getTerminator());
      auto *next = spbranch->getSuccessor(0);
      auto *check = llvm::BasicBlock::Create(context, "output.check", f, next);
      spbranch->setSuccessor(0, check);
      llvm::IRBuilder<> guard(check);
      llvm::Value *actual = snapshot;
      if (kind == Partial) actual = guard.CreateTrunc(actual, guard.getInt32Ty());
      if (kind == FreshGuard) actual = guard.CreateAlignedLoad(guard.getInt64Ty(), address, llvm::Align(1), "another.output");
      if (kind == UndefCheck) actual = llvm::UndefValue::get(guard.getInt64Ty());
      if (kind == PoisonCheck) actual = llvm::PoisonValue::get(guard.getInt64Ty());
      auto *expected = llvm::ConstantInt::get(actual->getType(), 32768);
      auto *equal = kind == NotEqual ? guard.CreateICmpNE(actual, expected) : guard.CreateICmpEQ(actual, expected);
      if (kind == Conflicting) {
        auto *again = llvm::BasicBlock::Create(context, "output.conflict", f, next);
        guard.CreateCondBr(equal, again, fallback);
        llvm::IRBuilder<> other(again);
        other.CreateCondBr(other.CreateICmpEQ(snapshot, other.getInt64(36864)), next, fallback);
      } else guard.CreateCondBr(equal, next, fallback);
    }
    if (kind == GuestAliasWrite || kind == AliasThenFreshUse) {
      // Guest may physically overlap the register snapshot. This store must
      // not change the meaning of the already loaded SSA value, while any
      // later register load remains a separate observation.
      llvm::IRBuilder<> body(block("prologue")->getTerminator());
      auto *pointer = body.CreateGEP(body.getInt8Ty(), guest, body.getInt64(36864), "possible.register.alias");
      body.CreateAlignedStore(body.getInt64(8192), pointer, llvm::Align(1));
    }
    llvm::IRBuilder<> body(a->getTerminator());
    llvm::Value *output = bits == 64 ? static_cast<llvm::Value *>(snapshot) :
        body.CreateZExt(snapshot, body.getInt64Ty(), "output.wide");
    if (kind == FreshUse || kind == AliasThenFreshUse)
      output = body.CreateAlignedLoad(body.getInt64Ty(), address, llvm::Align(1), "future.output");
    if (kind == UnknownCall) body.CreateCall(module->getFunction("unknown_call"), {guest});
    auto *pointer = body.CreateGEP(body.getInt8Ty(), guest, output, "output.pointer");
    body.CreateAlignedStore(body.getInt32(123), pointer, llvm::Align(1));
    REQUIRE_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  }
};
}

TEST_CASE("Image context consumes exactly guarded initial register snapshots", "[llvm][image-table-context][image-guarded-registers]") {
  for (bool closed : {false, true})
    for (auto kind : {RegisterGuardFixture::Exact, RegisterGuardFixture::GuestAliasWrite})
      DYNAMIC_SECTION("closed " << closed << " kind " << kind) {
        RegisterGuardFixture f(kind);
        auto *view = f.module->getFunction("view"); auto *argument = view->getArg(0);
        const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
        INFO(result.message);
        REQUIRE(result.closed); REQUIRE(result.changed); CHECK_FALSE(result.budgetExhausted);
        CHECK(f.module->getFunction("view") == view); CHECK(view->getArg(0) == argument);
        CHECK(result.guardedBytes == 16); CHECK(result.preservedStores >= 4);
        CHECK(f.ir().find("@ordinary") != std::string::npos);
        CHECK(f.ir().find("32768") != std::string::npos);
        if (kind == RegisterGuardFixture::GuestAliasWrite)
          CHECK(f.ir().find("36864") != std::string::npos);
        REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
      }
}

TEST_CASE("Image context never promotes unguarded or later register observations", "[llvm][image-table-context][image-guarded-registers]") {
  for (bool closed : {false, true})
    for (auto kind : {RegisterGuardFixture::Missing, RegisterGuardFixture::Partial,
         RegisterGuardFixture::FreshGuard, RegisterGuardFixture::FreshUse,
         RegisterGuardFixture::NotEqual, RegisterGuardFixture::Conflicting,
         RegisterGuardFixture::AliasThenFreshUse, RegisterGuardFixture::UnknownCall,
         RegisterGuardFixture::UndefCheck, RegisterGuardFixture::PoisonCheck})
      DYNAMIC_SECTION("closed " << closed << " kind " << kind) {
        RegisterGuardFixture f(kind); const auto before = f.ir();
        auto *view = f.module->getFunction("view"); auto *entry = &view->getEntryBlock();
        const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
        INFO(result.message); CHECK_FALSE(result.closed); CHECK_FALSE(result.changed);
        CHECK(f.ir() == before); CHECK(f.module->getFunction("view") == view);
        CHECK(&view->getEntryBlock() == entry);
        REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
      }
}

TEST_CASE("Image context preserves the width of complete guarded words", "[llvm][image-table-context][image-guarded-registers]") {
  for (bool closed : {false, true}) for (unsigned bits : {16u, 32u})
    DYNAMIC_SECTION("closed " << closed << " snapshot width " << bits) {
      RegisterGuardFixture f(RegisterGuardFixture::Exact, bits);
      const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
      INFO(result.message); REQUIRE(result.closed); REQUIRE(result.changed);
      CHECK_FALSE(result.budgetExhausted);
      REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    }
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


TEST_CASE("Image context retains private SP observations through normalization", "[llvm][image-table-context][image-sp-observations]") {
  for (bool closed : {false, true}) DYNAMIC_SECTION("closed " << closed) {
    Fixture f(21); auto *view = f.module->getFunction("view"); auto *argument = view->getArg(0);
    REQUIRE_FALSE(f.module->getFunction("llvm.fake.use"));
    const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
    INFO(result.message); REQUIRE(result.closed); REQUIRE(result.changed); CHECK_FALSE(result.budgetExhausted);
    CHECK(f.module->getFunction("view") == view); CHECK(view->getArg(0) == argument);
    CHECK_FALSE(f.module->getFunction("llvm.fake.use"));
    REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
  }
}

TEST_CASE("Image context preserves preexisting unused fake use declaration exactly", "[llvm][image-table-context][image-sp-observations]") {
  for (bool closed : {false, true}) DYNAMIC_SECTION("closed " << closed) {
    Fixture f(21);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(f.context), {}, true);
    auto *declaration = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                              "llvm.fake.use", *f.module);
    declaration->addFnAttr("fixture.existing.declaration", "keep-original");
    const auto attributes = declaration->getAttributes();
    const auto callingConvention = declaration->getCallingConv();
    REQUIRE(declaration->use_empty());
    REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
    INFO(result.message); REQUIRE(result.closed); REQUIRE(result.changed); CHECK_FALSE(result.budgetExhausted);
    CHECK(f.module->getFunction("llvm.fake.use") == declaration); CHECK(declaration->use_empty());
    CHECK(declaration->getAttributes() == attributes); CHECK(declaration->getCallingConv() == callingConvention);
    REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
  }
}

TEST_CASE("Image context never erases an original business fake use call", "[llvm][image-table-context][image-sp-observations]") {
  for (bool closed : {false, true}) DYNAMIC_SECTION("closed " << closed) {
    Fixture f(21);
    auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(f.context), {}, true);
    auto *declaration = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                              "llvm.fake.use", *f.module);
    declaration->addFnAttr("fixture.existing.declaration", "keep-original");
    auto *view = f.module->getFunction("view"); llvm::BasicBlock *step = nullptr; llvm::Value *index = nullptr;
    for (auto &block : *view) {
      if (block.getName() == "a") step = &block;
      for (auto &instruction : block) if (instruction.getName() == "index") index = &instruction;
    }
    REQUIRE(step); REQUIRE(index);
    auto *originalCall = llvm::IRBuilder<>(step->getTerminator()).CreateCall(declaration, {index});
    const auto before = f.ir(); const auto attributes = declaration->getAttributes();
    REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
    const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
    INFO(result.message); CHECK_FALSE(result.closed); CHECK_FALSE(result.changed); CHECK_FALSE(result.budgetExhausted);
    CHECK(f.ir() == before); CHECK(f.module->getFunction("llvm.fake.use") == declaration);
    CHECK(declaration->getAttributes() == attributes); CHECK(originalCall->getParent() == step);
    CHECK(originalCall->getCalledFunction() == declaration);
    REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
  }
}

// Draft only: append to TestLLVMImageTableContext.cpp after its existing
// Fixture/RegisterGuardFixture definitions. No new API, CMake registration,
// source fixture or expected binary is changed by this standalone draft.
namespace {
struct SeededCellFixture : RegisterGuardFixture {
  enum Kind { Kept, OldSnapshotAlias, DuplicateBackedge, TerminalEffects,
              MissingSeed, PartialSeed, SeedOnlyOnBackedge, BypassSeed, ReadBeforeSeed, ChangedCell,
              ClobberRestore, PartialOverlap, UnknownCall, FreshHostReg, OtherBaseWrite,
              VolatileRead, AtomicRead, WrappedStore };
  explicit SeededCellFixture(Kind kind = Kept)
      : RegisterGuardFixture(kind == OldSnapshotAlias ? GuestAliasWrite : Exact) {
    auto *f = module->getFunction("view");
    auto named = [&](llvm::StringRef name) -> llvm::Value * {
      for (auto &bb : *f) for (auto &i : bb)
        if (i.getName() == name) return &i;
      return nullptr;
    };
    auto block = [&](llvm::StringRef name) -> llvm::BasicBlock * {
      for (auto &bb : *f) if (bb.getName() == name) return &bb;
      return nullptr;
    };
    auto *prologue = block("prologue"); auto *a = block("a");
    auto *b = block("b"); auto *controller = block("controller");
    auto *guest = named("guest"); auto *frame = named("frame");
    auto *snapshot = named("initial.output");
    auto *output = llvm::cast<llvm::GetElementPtrInst>(named("output.pointer"));
    REQUIRE(prologue); REQUIRE(a); REQUIRE(b); REQUIRE(controller);
    REQUIRE(guest); REQUIRE(frame); REQUIRE(snapshot); REQUIRE(output);
    // Equivalent to Fixture mode21 without modifying either old fixture:
    // this valid source has no invented target edge back to its prologue.
    auto *dispatch = llvm::cast<llvm::SwitchInst>(block("read")->getTerminator());
    auto bad = dispatch->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 4096));
    REQUIRE(bad != dispatch->case_end());
    dispatch->removeCase(bad);
    llvm::IRBuilder<> prefix(prologue->getTerminator());
    llvm::Value *cellOffset = kind == WrappedStore ? static_cast<llvm::Value *>(prefix.getInt64(0)) :
        prefix.CreateAdd(frame, prefix.getInt64(32), "seed.offset");
    auto *cell = prefix.CreateGEP(prefix.getInt8Ty(), guest, cellOffset, "seed.cell");
    llvm::Value *seedValue = snapshot;
    if (kind == FreshHostReg) {
      // Do not infer this new host observation from initial.output's guard.
      // Guest and the byte-packed host registers have no noalias promise.
      auto *alias = prefix.CreateGEP(prefix.getInt8Ty(), guest, prefix.getInt64(36864));
      prefix.CreateAlignedStore(prefix.getInt64(8192), alias, llvm::Align(1));
      seedValue = prefix.CreateAlignedLoad(prefix.getInt64Ty(),
          named("output.snapshot.address"), llvm::Align(1), "fresh.host.output");
    }
    llvm::LoadInst *old = nullptr;
    if (kind == ReadBeforeSeed)
      old = prefix.CreateAlignedLoad(prefix.getInt64Ty(), cell, llvm::Align(1), "old.before.seed");
    auto *seed = prefix.CreateAlignedStore(seedValue, cell, llvm::Align(1));
    llvm::IRBuilder<> body(output);
    auto *reload = body.CreateAlignedLoad(body.getInt64Ty(), cell, llvm::Align(1), "seed.reload");
    output->setOperand(1, old ? static_cast<llvm::Value *>(old) : reload);
    if (kind == VolatileRead) reload->setVolatile(true);
    if (kind == AtomicRead) {
      reload->setAtomic(llvm::AtomicOrdering::Monotonic);
      reload->setAlignment(llvm::Align(8));
    }
    if (kind == MissingSeed) seed->eraseFromParent();
    if (kind == PartialSeed) {
      llvm::IRBuilder<> partial(seed);
      partial.CreateAlignedStore(partial.CreateTrunc(seedValue, partial.getInt32Ty()), cell, llvm::Align(1));
      seed->eraseFromParent();
    }
    if (kind == SeedOnlyOnBackedge) {
      seed->eraseFromParent();
      llvm::IRBuilder<> back(b->getTerminator());
      back.CreateAlignedStore(seedValue, cell, llvm::Align(1));
    }
    if (kind == BypassSeed) {
      seed->eraseFromParent();
      auto *term = prologue->getTerminator();
      auto *seedBlock = llvm::BasicBlock::Create(context, "maybe.seed", f, controller);
      llvm::IRBuilder<> writer(seedBlock);
      writer.CreateAlignedStore(seedValue, cell, llvm::Align(1));
      writer.CreateBr(controller);
      llvm::IRBuilder<> choose(term);
      choose.CreateCondBr(choose.CreateICmpEQ(f->getArg(2),
          llvm::ConstantPointerNull::get(choose.getPtrTy())), seedBlock, controller);
      term->eraseFromParent();
    }
    if (kind == DuplicateBackedge) {
      auto *branch = llvm::cast<llvm::BranchInst>(b->getTerminator());
      REQUIRE(branch->isConditional());
      branch->setSuccessor(0, controller);
      branch->setSuccessor(1, controller);
    }
    if (kind == ChangedCell || kind == ClobberRestore || kind == PartialOverlap || kind == WrappedStore) {
      llvm::IRBuilder<> back(b->getTerminator());
      if (kind == ChangedCell || kind == ClobberRestore) {
        back.CreateAlignedStore(back.getInt64(8192), cell, llvm::Align(1));
        if (kind == ClobberRestore)
          back.CreateAlignedStore(seedValue, cell, llvm::Align(1));
      }
      else if (kind == PartialOverlap) {
        // 0x8000 -> 0x2000: an actual table-aliasing new pointer, not an
        // innocuous write of a byte that was already zero.
        auto *overlap = back.CreateGEP(back.getInt8Ty(), cell, back.getInt64(1));
        back.CreateAlignedStore(back.getInt8(32), overlap, llvm::Align(1));
      } else {
        auto *wrapped = back.CreateGEP(back.getInt8Ty(), guest, back.getInt64(-4));
        back.CreateAlignedStore(back.getInt64(8192ULL << 32), wrapped, llvm::Align(1));
      }
    }
    if (kind == UnknownCall || kind == OtherBaseWrite) {
      llvm::IRBuilder<> back(b->getTerminator());
      if (kind == UnknownCall)
        back.CreateCall(module->getFunction("unknown_call"), {guest});
      else {
        auto *other = back.CreateGEP(back.getInt8Ty(), f->getArg(0), back.getInt64(16));
        back.CreateAlignedStore(back.getInt64(0), other, llvm::Align(1));
      }
    }
    if (kind == TerminalEffects) {
      llvm::IRBuilder<> terminal(block("finished")->getTerminator());
      terminal.CreateCall(module->getFunction("unknown_call"), {guest});
      // The call may have changed the cell. No seeded fact is available for
      // this terminal observation, even though it is at the same address.
      auto *fresh = terminal.CreateAlignedLoad(terminal.getInt64Ty(), cell,
          llvm::Align(1), "terminal.seed.reload");
      auto *freshPointer = terminal.CreateGEP(terminal.getInt8Ty(), guest, fresh,
          "terminal.output.pointer");
      terminal.CreateAlignedStore(terminal.getInt32(77), freshPointer, llvm::Align(1));
      terminal.CreateAlignedStore(terminal.getInt64(0), named("t0p"), llvm::Align(1));
    }
    REQUIRE_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  }
};
}

TEST_CASE("Image context proves an entry seeded Guest cell through a store-free loop",
          "[llvm][image-table-context][image-seeded-cell]") {
  for (bool closed : {false, true})
    for (auto kind : {SeededCellFixture::Kept, SeededCellFixture::OldSnapshotAlias,
                      SeededCellFixture::DuplicateBackedge, SeededCellFixture::TerminalEffects})
      DYNAMIC_SECTION("closed " << closed << " kind " << kind) {
        SeededCellFixture f(kind);
        auto *view = f.module->getFunction("view"); auto *argument = view->getArg(0);
        const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
        INFO(result.message);
        REQUIRE(result.closed); REQUIRE(result.changed); CHECK_FALSE(result.budgetExhausted);
        CHECK(f.module->getFunction("view") == view); CHECK(view->getArg(0) == argument);
        // Keep real source operations, exits and ordinary guard-miss fallback.
        CHECK(f.ir().find("@ordinary") != std::string::npos);
        CHECK(f.ir().find("store i32 123") != std::string::npos);
        llvm::LoadInst *keptReload = nullptr;
        llvm::LoadInst *stateReload = nullptr;
        for (auto &bb : *view) for (auto &instruction : bb) {
          if (instruction.getName() == "seed.reload")
            keptReload = llvm::dyn_cast<llvm::LoadInst>(&instruction);
          if (instruction.getName() == "index")
            stateReload = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        }
        CHECK(keptReload != nullptr); // actual Guest LOAD is not erased.
        REQUIRE(stateReload);
        CHECK_FALSE(stateReload->use_empty()); // a failed state-cell hypothesis cannot leak.
        if (kind == SeededCellFixture::TerminalEffects) {
          CHECK(f.ir().find("call void @unknown_call") != std::string::npos);
          llvm::LoadInst *terminalReload = nullptr;
          llvm::GetElementPtrInst *terminalPointer = nullptr;
          for (auto &bb : *view) for (auto &instruction : bb) {
            if (instruction.getName() == "terminal.seed.reload")
              terminalReload = llvm::dyn_cast<llvm::LoadInst>(&instruction);
            if (instruction.getName() == "terminal.output.pointer")
              terminalPointer = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction);
          }
          REQUIRE(terminalReload); REQUIRE(terminalPointer);
          CHECK(terminalPointer->getOperand(1) == terminalReload);
        }
        REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
      }
}

TEST_CASE("Image seeded facts never cross missing definitions or changing memory",
          "[llvm][image-table-context][image-seeded-cell]") {
  for (bool closed : {false, true})
    for (auto kind : {SeededCellFixture::MissingSeed, SeededCellFixture::PartialSeed,
                      SeededCellFixture::SeedOnlyOnBackedge, SeededCellFixture::BypassSeed,
                      SeededCellFixture::ReadBeforeSeed, SeededCellFixture::ChangedCell, SeededCellFixture::ClobberRestore,
                      SeededCellFixture::PartialOverlap, SeededCellFixture::UnknownCall,
                      SeededCellFixture::FreshHostReg, SeededCellFixture::OtherBaseWrite,
                      SeededCellFixture::VolatileRead, SeededCellFixture::AtomicRead,
                      SeededCellFixture::WrappedStore})
      DYNAMIC_SECTION("closed " << closed << " kind " << kind) {
        SeededCellFixture f(kind); const auto before = f.ir();
        auto *view = f.module->getFunction("view"); auto *entry = &view->getEntryBlock();
        const auto result = runImageTableCheckedContext(*f.module, f.request(), closed);
        INFO(result.message); CHECK_FALSE(result.closed); CHECK_FALSE(result.changed);
        CHECK(f.ir() == before); CHECK(f.module->getFunction("view") == view);
        CHECK(&view->getEntryBlock() == entry);
        REQUIRE_FALSE(llvm::verifyModule(*f.module, &llvm::errs()));
      }
}
