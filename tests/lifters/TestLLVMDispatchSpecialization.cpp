#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/CallEffectSummary.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>

using namespace dragon;
using namespace dragon::passes;
using namespace dragon::llvmlifter;

namespace {
unsigned switches(const llvm::Module &module) {
  unsigned count = 0;
  for (const auto &function : module)
    for (const auto &block : function)
      count += llvm::isa<llvm::SwitchInst>(block.getTerminator());
  return count;
}

void verify(const llvm::Module &module) {
  std::string error;
  llvm::raw_string_ostream stream{error};
  const bool invalid = llvm::verifyModule(module, &stream);
  stream.flush();
  INFO(error);
  CHECK_FALSE(invalid);
}
}

TEST_CASE("LLVM dispatcher specialization rewrites a uniquely constant PHI",
          "[llvm][dispatch-specialize]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "dispatcher", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i32, 1, "state");
  auto *switchInst = builder.CreateSwitch(state, fallback, 1);
  switchInst->addCase(llvm::ConstantInt::get(i32, 10), taken);
  state->addIncoming(llvm::ConstantInt::get(i32, 10), entry);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  auto *fallbackValue = builder.CreatePHI(i32, 1, "fallback.value");
  fallbackValue->addIncoming(llvm::ConstantInt::get(i32, 99), dispatch);
  builder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-specialize"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK(report.changed);
  CHECK(switches(lifter.module()) == 0);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("switches_rewritten") == 1);
  CHECK(report.runs[0].result.counters.at("unique_conditions") == 1);
  verify(lifter.module());
}

TEST_CASE("LLVM dispatcher specialization keeps differing PHI targets explicit",
          "[llvm][dispatch-specialize]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i1}, false),
      llvm::GlobalValue::ExternalLinkage, "unknown_dispatcher", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(right);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i32, 2, "state");
  auto *switchInst = builder.CreateSwitch(state, fallback, 1);
  switchInst->addCase(llvm::ConstantInt::get(i32, 10), taken);
  state->addIncoming(llvm::ConstantInt::get(i32, 10), left);
  state->addIncoming(llvm::ConstantInt::get(i32, 20), right);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-specialize"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK_FALSE(report.changed);
  CHECK(switches(lifter.module()) == 1);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("unresolved_switches") == 1);
  verify(lifter.module());
}

TEST_CASE("LLVM deflattening rewrites proven state edges",
          "[llvm][dispatch-deflatten]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "flat", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *caseLeft = llvm::BasicBlock::Create(context, "case.left", function);
  auto *caseRight = llvm::BasicBlock::Create(context, "case.right", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(right);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i32, 2, "state");
  auto *switchInst = builder.CreateSwitch(state, fallback, 2);
  switchInst->addCase(llvm::ConstantInt::get(i32, 10), caseLeft);
  switchInst->addCase(llvm::ConstantInt::get(i32, 20), caseRight);
  state->addIncoming(llvm::ConstantInt::get(i32, 10), left);
  state->addIncoming(llvm::ConstantInt::get(i32, 20), right);
  builder.SetInsertPoint(caseLeft);
  builder.CreateRetVoid();
  builder.SetInsertPoint(caseRight);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-deflatten"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK(report.changed);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("edges_rewritten") == 2);
  CHECK(left->getTerminator()->getSuccessor(0) == caseLeft);
  CHECK(right->getTerminator()->getSuccessor(0) == caseRight);
  verify(lifter.module());
}

TEST_CASE("LLVM deflattening rewrites finite states with one destination",
          "[llvm][dispatch-deflatten]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i1}, false),
      llvm::GlobalValue::ExternalLinkage, "flat_finite", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *incoming = llvm::BasicBlock::Create(context, "incoming", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(0), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(incoming);
  builder.SetInsertPoint(right);
  builder.CreateBr(incoming);
  builder.SetInsertPoint(incoming);
  auto *finite = builder.CreatePHI(i32, 2, "finite.state");
  finite->addIncoming(llvm::ConstantInt::get(i32, 10), left);
  finite->addIncoming(llvm::ConstantInt::get(i32, 11), right);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i32, 1, "state");
  state->addIncoming(finite, incoming);
  auto *switchInst = builder.CreateSwitch(state, fallback, 2);
  switchInst->addCase(llvm::ConstantInt::get(i32, 10), taken);
  switchInst->addCase(llvm::ConstantInt::get(i32, 11), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-deflatten"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK(report.changed);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("edges_rewritten") == 1);
  CHECK(incoming->getTerminator()->getSuccessor(0) == taken);
  verify(lifter.module());
}

TEST_CASE("LLVM deflattening rewrites a local memory state selector",
          "[llvm][dispatch-deflatten]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "flat_memory_state", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  auto *stateSlot = builder.CreateAlloca(i32, nullptr, "state.slot");
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);
  builder.SetInsertPoint(left);
  builder.CreateStore(llvm::ConstantInt::get(i32, 10), stateSlot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(right);
  builder.CreateStore(llvm::ConstantInt::get(i32, 11), stateSlot);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreateLoad(i32, stateSlot, "state");
  auto *switchInst = builder.CreateSwitch(state, fallback, 2);
  switchInst->addCase(llvm::ConstantInt::get(i32, 10), taken);
  switchInst->addCase(llvm::ConstantInt::get(i32, 11), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-deflatten"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK(report.changed);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("edges_rewritten") == 2);
  CHECK(left->getTerminator()->getSuccessor(0) == taken);
  CHECK(right->getTerminator()->getSuccessor(0) == taken);
  verify(lifter.module());
}

TEST_CASE("LLVM deflattening consumes a complete cross-call GuestState summary",
          "[llvm][dispatch-deflatten]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *calleeType = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                             {ptr}, false);
  auto *callee = llvm::Function::Create(
      calleeType, llvm::GlobalValue::InternalLinkage, "callee_state_dispatch",
      lifter.module());
  auto *caller = llvm::Function::Create(
      calleeType, llvm::GlobalValue::InternalLinkage, "caller_state_dispatch",
      lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", callee);
  auto *taken = llvm::BasicBlock::Create(context, "taken", callee);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", callee);
  llvm::IRBuilder<> builder(entry);
  auto *slot = builder.CreateGEP(builder.getInt8Ty(), callee->getArg(0),
                                 builder.getInt64(128));
  auto *state = builder.CreateLoad(i64, slot, "state");
  auto *dispatch = builder.CreateSwitch(state, fallback, 1);
  dispatch->addCase(llvm::ConstantInt::get(i64, 42), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();

  auto *callerEntry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  auto *call = callerBuilder.CreateCall(callee, {caller->getArg(0)});
  CallEffectSummary summary;
  summary.version = 2;
  summary.proven = true;
  summary.memory = CallMemoryEffectKind::KnownRanges;
  const CallMemoryRange range{CallMemoryRange::Base::GuestState, 128, 8};
  summary.writes.push_back(range);
  summary.memoryConstants.push_back({range, 42, true});
  REQUIRE(attachCallEffectSummary(*call, summary));
  callerBuilder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-deflatten"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK(report.changed);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("cross_call_dispatchers_rewritten") == 1);
  CHECK(report.runs[0].result.counters.at("cross_call_summaries_consumed") == 1);
  CHECK(llvm::isa<llvm::BranchInst>(entry->getTerminator()));
  CHECK(entry->getTerminator()->getSuccessor(0) == taken);
  CHECK(switches(lifter.module()) == 0);
  verify(lifter.module());
}

TEST_CASE("LLVM deflattening keeps incomplete cross-call state explicit",
          "[llvm][dispatch-deflatten]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr}, false);
  auto *callee = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "callee_incomplete_dispatch",
      lifter.module());
  auto *caller = llvm::Function::Create(
      type, llvm::GlobalValue::InternalLinkage, "caller_incomplete_dispatch",
      lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", callee);
  auto *taken = llvm::BasicBlock::Create(context, "taken", callee);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", callee);
  llvm::IRBuilder<> builder(entry);
  auto *slot = builder.CreateGEP(builder.getInt8Ty(), callee->getArg(0),
                                 builder.getInt64(128));
  auto *state = builder.CreateLoad(i64, slot, "state");
  auto *dispatch = builder.CreateSwitch(state, fallback, 1);
  dispatch->addCase(llvm::ConstantInt::get(i64, 42), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();
  auto *callerEntry = llvm::BasicBlock::Create(context, "entry", caller);
  llvm::IRBuilder<> callerBuilder(callerEntry);
  auto *call = callerBuilder.CreateCall(callee, {caller->getArg(0)});
  CallEffectSummary summary;
  summary.version = 2;
  summary.proven = true;
  summary.memory = CallMemoryEffectKind::KnownRanges;
  const CallMemoryRange range{CallMemoryRange::Base::GuestState, 128, 8};
  summary.writes.push_back(range);
  summary.memoryConstants.push_back({range, 42, false});
  REQUIRE(attachCallEffectSummary(*call, summary));
  callerBuilder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-deflatten"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK_FALSE(report.changed);
  CHECK(report.runs[0].result.counters.at("unresolved_dispatchers") == 1);
  CHECK(llvm::isa<llvm::SwitchInst>(entry->getTerminator()));
  CHECK(switches(lifter.module()) == 1);
  verify(lifter.module());
}

TEST_CASE("LLVM deflattening keeps unknown state paths explicit",
          "[llvm][dispatch-deflatten]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i32}, false),
      llvm::GlobalValue::ExternalLinkage, "flat_unknown", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(llvm::ConstantInt::getTrue(context), left, right);
  builder.SetInsertPoint(left);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(right);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i32, 2, "state");
  auto *switchInst = builder.CreateSwitch(state, fallback, 1);
  switchInst->addCase(llvm::ConstantInt::get(i32, 10), taken);
  state->addIncoming(llvm::ConstantInt::get(i32, 10), left);
  state->addIncoming(function->getArg(0), right);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-deflatten"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK_FALSE(report.changed);
  CHECK(report.runs[0].result.counters.at("unresolved_dispatchers") == 1);
  CHECK(left->getTerminator()->getSuccessor(0) == dispatch);
  CHECK(right->getTerminator()->getSuccessor(0) == dispatch);
  verify(lifter.module());
}

TEST_CASE("LLVM dispatcher specialization rejects flagged poison arithmetic",
          "[llvm][dispatch-specialize]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "poison_dispatcher", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = llvm::BinaryOperator::CreateAdd(
      llvm::ConstantInt::get(i8, 255), llvm::ConstantInt::get(i8, 1), "state",
      dispatch);
  state->setHasNoUnsignedWrap();
  auto *switchInst = builder.CreateSwitch(state, fallback, 1);
  switchInst->addCase(llvm::ConstantInt::get(i8, 0), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-specialize"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK_FALSE(report.changed);
  CHECK(switches(lifter.module()) == 1);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("unresolved_switches") == 1);
  verify(lifter.module());
}

TEST_CASE("LLVM dispatcher specialization rejects poison shifts and selects",
          "[llvm][dispatch-specialize]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i1 = llvm::Type::getInt1Ty(context);

  auto *shiftFunction = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "overshift_dispatcher",
      lifter.module());
  auto *shiftEntry = llvm::BasicBlock::Create(context, "entry", shiftFunction);
  auto *shiftDispatch = llvm::BasicBlock::Create(context, "dispatch", shiftFunction);
  auto *shiftTaken = llvm::BasicBlock::Create(context, "taken", shiftFunction);
  auto *shiftFallback = llvm::BasicBlock::Create(context, "fallback", shiftFunction);
  llvm::IRBuilder<> shiftBuilder(shiftEntry);
  shiftBuilder.CreateBr(shiftDispatch);
  shiftBuilder.SetInsertPoint(shiftDispatch);
  auto *overshift = llvm::BinaryOperator::CreateLShr(
      llvm::ConstantInt::get(i8, 1), llvm::ConstantInt::get(i8, 8), "state",
      shiftDispatch);
  auto *shiftSwitch = shiftBuilder.CreateSwitch(overshift, shiftFallback, 1);
  shiftSwitch->addCase(llvm::ConstantInt::get(i8, 0), shiftTaken);
  shiftBuilder.SetInsertPoint(shiftTaken);
  shiftBuilder.CreateRetVoid();
  shiftBuilder.SetInsertPoint(shiftFallback);
  shiftBuilder.CreateRetVoid();

  auto *selectFunction = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i1}, false),
      llvm::GlobalValue::ExternalLinkage, "poison_select_dispatcher",
      lifter.module());
  auto *selectEntry = llvm::BasicBlock::Create(context, "entry", selectFunction);
  auto *selectDispatch = llvm::BasicBlock::Create(context, "dispatch", selectFunction);
  auto *selectTaken = llvm::BasicBlock::Create(context, "taken", selectFunction);
  auto *selectFallback = llvm::BasicBlock::Create(context, "fallback", selectFunction);
  llvm::IRBuilder<> selectBuilder(selectEntry);
  selectBuilder.CreateBr(selectDispatch);
  selectBuilder.SetInsertPoint(selectDispatch);
  auto *sameArms = llvm::SelectInst::Create(
      selectFunction->getArg(0), llvm::ConstantInt::get(i8, 7),
      llvm::ConstantInt::get(i8, 7), "state", selectDispatch);
  auto *selectSwitch = selectBuilder.CreateSwitch(sameArms, selectFallback, 1);
  selectSwitch->addCase(llvm::ConstantInt::get(i8, 7), selectTaken);
  selectBuilder.SetInsertPoint(selectTaken);
  selectBuilder.CreateRetVoid();
  selectBuilder.SetInsertPoint(selectFallback);
  selectBuilder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-specialize"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK_FALSE(report.changed);
  CHECK(switches(lifter.module()) == 2);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("unresolved_switches") == 2);
  verify(lifter.module());
}

TEST_CASE("LLVM deflattening rejects poison shifts",
          "[llvm][dispatch-deflatten]") {
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  auto &context = lifter.module().getContext();
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "poison_flat", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *incoming = llvm::BasicBlock::Create(context, "incoming", function);
  auto *dispatch = llvm::BasicBlock::Create(context, "dispatch", function);
  auto *taken = llvm::BasicBlock::Create(context, "taken", function);
  auto *fallback = llvm::BasicBlock::Create(context, "fallback", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateBr(incoming);
  builder.SetInsertPoint(incoming);
  auto *overshift = llvm::BinaryOperator::CreateLShr(
      llvm::ConstantInt::get(i8, 1), llvm::ConstantInt::get(i8, 8), "state.in",
      incoming);
  builder.CreateBr(dispatch);
  builder.SetInsertPoint(dispatch);
  auto *state = builder.CreatePHI(i8, 1, "state");
  state->addIncoming(overshift, incoming);
  auto *switchInst = builder.CreateSwitch(state, fallback, 1);
  switchInst->addCase(llvm::ConstantInt::get(i8, 0), taken);
  builder.SetInsertPoint(taken);
  builder.CreateRetVoid();
  builder.SetInsertPoint(fallback);
  builder.CreateRetVoid();
  verify(lifter.module());

  PassPipeline pipeline;
  REQUIRE(pipeline.add("dispatch-deflatten"));
  const auto report = pipeline.run(lifter);
  REQUIRE(report.ok);
  CHECK_FALSE(report.changed);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs[0].result.counters.at("unresolved_dispatchers") == 1);
  CHECK(incoming->getTerminator()->getSuccessor(0) == dispatch);
  verify(lifter.module());
}
