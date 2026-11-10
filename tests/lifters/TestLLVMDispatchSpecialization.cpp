#include <catch2/catch_test_macros.hpp>

#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>

using namespace dragon;
using namespace dragon::passes;

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
