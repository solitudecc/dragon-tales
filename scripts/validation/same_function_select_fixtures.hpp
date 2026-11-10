#pragma once

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>

#include <string>
#include <vector>

namespace dragon::validation::select_fixture {

using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class Form { Phi, Memory };
enum class Condition {
  Freeze, NoUndef, Unknown, Undef, Poison, True, False, ComputedTrue, ComputedFalse
};
enum class Arm { Constant, Undef, Poison, FreezePoison };
enum class Gap { None, Compute, Load, ReadOnlyCall, CallThenStore, StoreThenCall };

struct Config {
  std::string name;
  Form form = Form::Phi;
  Condition condition = Condition::Freeze;
  Arm trueArm = Arm::Constant;
  Arm falseArm = Arm::Constant;
  Gap gap = Gap::None;
  bool defaultTrue = false;
  bool sameTarget = false;
  bool outerGuard = false;
  bool targetPhi = false;
  bool liveOut = false;
  bool deepTrueArm = false;
  bool freezeState = false;
  bool conditionalState = true;
  unsigned extraPureComputations = 0;
};

struct View {
  llvm::Function *function = nullptr;
  llvm::BasicBlock *predecessor = nullptr;
  llvm::BasicBlock *dispatch = nullptr;
  llvm::BasicBlock *trueTarget = nullptr;
  llvm::BasicBlock *falseTarget = nullptr;
  llvm::BasicBlock *guardExit = nullptr;
  llvm::Value *condition = nullptr;
  llvm::Instruction *gapInstruction = nullptr;
};

inline llvm::Value *armValue(Builder &builder, Arm arm, unsigned value) {
  switch (arm) {
    case Arm::Undef: return llvm::UndefValue::get(builder.getInt64Ty());
    case Arm::Poison: return llvm::PoisonValue::get(builder.getInt64Ty());
    case Arm::FreezePoison:
      return builder.CreateFreeze(llvm::PoisonValue::get(builder.getInt64Ty()));
    case Arm::Constant: return builder.getInt64(value);
  }
  return nullptr;
}

inline View build(llvm::Module &module, const Config &config) {
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *type = llvm::FunctionType::get(
      i64, {llvm::Type::getInt1Ty(context), llvm::Type::getInt1Ty(context), i64, ptr, ptr}, false);
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, config.name, module);
  auto *entry = llvm::BasicBlock::Create(context, "incoming", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *one = llvm::BasicBlock::Create(context, "one", function);
  auto *two = llvm::BasicBlock::Create(context, "two", function);
  auto *fallback = llvm::BasicBlock::Create(context, "default", function);
  auto *guardExit = config.outerGuard
      ? llvm::BasicBlock::Create(context, "guard.exit", function) : nullptr;
  Builder b(entry);
  llvm::Value *condition = function->getArg(0);
  switch (config.condition) {
    case Condition::Freeze: condition = b.CreateFreeze(condition, "stable.condition"); break;
    case Condition::NoUndef: function->addParamAttr(0, llvm::Attribute::NoUndef); break;
    case Condition::Unknown: break;
    case Condition::Undef: condition = llvm::UndefValue::get(b.getInt1Ty()); break;
    case Condition::Poison: condition = llvm::PoisonValue::get(b.getInt1Ty()); break;
    case Condition::True: condition = b.getTrue(); break;
    case Condition::False: condition = b.getFalse(); break;
    case Condition::ComputedTrue: condition = b.CreateXor(b.getTrue(), b.getFalse()); break;
    case Condition::ComputedFalse: condition = b.CreateXor(b.getTrue(), b.getTrue()); break;
  }
  auto *trueValue = armValue(b, config.trueArm, config.defaultTrue ? 3 : 1);
  auto *falseValue = armValue(b, config.falseArm, 2);
  if (config.deepTrueArm) trueValue = b.CreateAdd(trueValue, b.getInt64(0));
  llvm::Value *selected = b.getInt64(1);
  if (config.conditionalState)
    selected = b.CreateSelect(condition, trueValue, falseValue, "next.state");
  if (config.freezeState) selected = b.CreateFreeze(selected, "frozen.state");

  llvm::Function *writer = nullptr;
  if (config.gap == Gap::CallThenStore || config.gap == Gap::StoreThenCall) {
    writer = module.getFunction("select_unknown_writer");
    if (!writer)
      writer = llvm::Function::Create(
          llvm::FunctionType::get(b.getVoidTy(), {ptr, ptr, i64}, false),
          llvm::GlobalValue::ExternalLinkage, "select_unknown_writer", module);
  }
  auto callWriter = [&] {
    return b.CreateCall(writer, {function->getArg(3), function->getArg(4), function->getArg(2)});
  };
  llvm::Instruction *gapInstruction = nullptr;
  if (config.gap == Gap::CallThenStore) gapInstruction = callWriter();
  if (config.form == Form::Memory) b.CreateStore(selected, function->getArg(3));
  llvm::Value *business = nullptr;
  if (config.gap == Gap::Compute) {
    business = b.CreateAdd(function->getArg(2), b.getInt64(7), "kept.compute");
    for (unsigned n = 0; n < config.extraPureComputations; ++n)
      business = b.CreateAdd(business, b.getInt64(0), "kept.compute.extra");
    gapInstruction = llvm::cast<llvm::Instruction>(business);
  } else if (config.gap == Gap::Load) {
    business = b.CreateLoad(i64, function->getArg(4), "kept.load");
    gapInstruction = llvm::cast<llvm::Instruction>(business);
  } else if (config.gap == Gap::ReadOnlyCall) {
    auto *reader = module.getFunction("select_readonly_reader");
    if (!reader) {
      reader = llvm::Function::Create(
          llvm::FunctionType::get(i64, {ptr, i64}, false),
          llvm::GlobalValue::ExternalLinkage, "select_readonly_reader", module);
      reader->setOnlyReadsMemory();
    }
    business = b.CreateCall(reader, {function->getArg(3), function->getArg(2)}, "kept.readonly");
    gapInstruction = llvm::cast<llvm::Instruction>(business);
  } else if (config.gap == Gap::StoreThenCall) {
    gapInstruction = callWriter();
  }
  if (guardExit) {
    auto *guard = b.CreateFreeze(function->getArg(1));
    b.CreateCondBr(guard, dispatch, guardExit);
  } else {
    b.CreateBr(dispatch);
  }

  b.SetInsertPoint(dispatch);
  llvm::PHINode *payload = nullptr;
  if (config.liveOut) {
    payload = b.CreatePHI(i64, 1, "payload");
    payload->addIncoming(b.getInt64(7), entry);
  }
  llvm::Value *state;
  if (config.form == Form::Memory) state = b.CreateLoad(i64, function->getArg(3));
  else {
    auto *phi = b.CreatePHI(i64, 1, "state");
    phi->addIncoming(selected, entry);
    state = phi;
  }
  auto *sw = b.CreateSwitch(state, fallback, 2);
  sw->addCase(b.getInt64(1), one);
  sw->addCase(b.getInt64(2), config.sameTarget ? one : two);
  b.SetInsertPoint(one);
  if (config.targetPhi) {
    auto *phi = b.CreatePHI(i64, 1);
    phi->addIncoming(function->getArg(2), dispatch);
    b.CreateRet(phi);
  } else if (payload) b.CreateRet(payload);
  else if (config.gap == Gap::Load) b.CreateRet(b.CreateXor(business, function->getArg(2)));
  else if (business) b.CreateRet(business);
  else b.CreateRet(b.CreateAdd(function->getArg(2), b.getInt64(11)));
  b.SetInsertPoint(two);
  b.CreateRet(payload ? static_cast<llvm::Value *>(payload)
                      : b.CreateAdd(function->getArg(2), b.getInt64(22)));
  b.SetInsertPoint(fallback);
  b.CreateRet(payload ? static_cast<llvm::Value *>(payload)
                      : b.CreateAdd(function->getArg(2), b.getInt64(33)));
  if (guardExit) {
    b.SetInsertPoint(guardExit);
    b.CreateRet(b.CreateAdd(function->getArg(2), b.getInt64(44)));
  }
  return {function, entry, dispatch, config.defaultTrue ? fallback : one,
          config.sameTarget ? one : two, guardExit, condition, gapInstruction};
}

inline View buildLoop(llvm::Module &module) {
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *type = llvm::FunctionType::get(i64,
      {llvm::Type::getInt1Ty(context), llvm::Type::getInt1Ty(context), i64, ptr, ptr}, false);
  auto *f = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "select_loop", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", f);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", f);
  auto *body = llvm::BasicBlock::Create(context, "body", f);
  auto *done = llvm::BasicBlock::Create(context, "done", f);
  auto *fallback = llvm::BasicBlock::Create(context, "default", f);
  Builder b(entry);
  auto *counter = b.CreateGEP(b.getInt8Ty(), f->getArg(3), b.getInt64(8));
  auto *limit = b.CreateAnd(f->getArg(2), b.getInt64(3));
  b.CreateStore(b.getInt64(0), counter);
  b.CreateStore(b.getInt64(1), f->getArg(3));
  b.CreateBr(dispatch);
  b.SetInsertPoint(dispatch);
  auto *load = b.CreateLoad(i64, f->getArg(3));
  auto *sw = b.CreateSwitch(load, fallback, 2);
  sw->addCase(b.getInt64(1), body);
  sw->addCase(b.getInt64(2), done);
  b.SetInsertPoint(body);
  auto *count = b.CreateAdd(b.CreateLoad(i64, counter), b.getInt64(1));
  b.CreateStore(count, counter);
  auto *condition = b.CreateFreeze(b.CreateICmpULT(count, limit));
  auto *next = b.CreateSelect(condition, b.getInt64(1), b.getInt64(2));
  b.CreateStore(next, f->getArg(3));
  b.CreateBr(dispatch);
  b.SetInsertPoint(done);
  b.CreateRet(b.CreateLoad(i64, counter));
  b.SetInsertPoint(fallback);
  b.CreateRet(b.getInt64(0xbad));
  return {f, body, dispatch, body, done, nullptr, condition, nullptr};
}

inline std::vector<Config> nativeConfigs() {
  std::vector<Config> result;
  for (const auto form : {Form::Phi, Form::Memory}) {
    const auto suffix = form == Form::Phi ? "_phi" : "_load";
    for (const auto condition : {Condition::Freeze, Condition::NoUndef}) {
      Config c{std::string(condition == Condition::Freeze ? "select_freeze" : "select_noundef") + suffix};
      c.form = form;
      c.condition = condition;
      result.push_back(c);
    }
    for (const auto prefix : {"select_default", "select_same", "select_guard"}) {
      Config c{std::string(prefix) + suffix};
      c.form = form;
      c.defaultTrue = std::string(prefix) == "select_default";
      c.sameTarget = std::string(prefix) == "select_same";
      c.outerGuard = std::string(prefix) == "select_guard";
      result.push_back(c);
    }
  }
  for (const auto gap : {Gap::Compute, Gap::Load, Gap::ReadOnlyCall,
                         Gap::CallThenStore, Gap::StoreThenCall}) {
    const char *name = gap == Gap::Compute ? "gap_compute" : gap == Gap::Load ? "gap_load"
        : gap == Gap::ReadOnlyCall ? "gap_readonly"
        : gap == Gap::CallThenStore ? "call_then_store" : "store_then_call";
    Config c{name};
    c.form = Form::Memory;
    c.gap = gap;
    c.conditionalState = gap == Gap::CallThenStore || gap == Gap::StoreThenCall;
    result.push_back(c);
  }
  return result;
}
} // namespace dragon::validation::select_fixture
