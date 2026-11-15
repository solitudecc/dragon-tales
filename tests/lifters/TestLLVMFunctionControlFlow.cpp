#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/DFAJumpThreading.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using dragon::Architecture;
using dragon::llvmlifter::ExecutionMode;
using dragon::llvmlifter::LLVMLifter;
using Builder = llvm::IRBuilder<llvm::NoFolder>;

std::string printed(llvm::Function &function) {
  std::string text; llvm::raw_string_ostream out(text); function.print(out); return text;
}
void verified(llvm::Module &module) {
  std::string error; llvm::raw_string_ostream out(error);
  const bool invalid = llvm::verifyModule(module, &out); INFO(error); REQUIRE_FALSE(invalid);
}
std::size_t instructions(llvm::Function &function) {
  std::size_t count = 0; for (auto &block : function) count += block.size(); return count;
}
std::string bodyless(llvm::Module &module, llvm::StringRef selected) {
  auto clone = llvm::CloneModule(module); auto *function = clone->getFunction(selected);
  REQUIRE(function); function->deleteBody();
  std::string text; llvm::raw_string_ostream out(text); clone->print(out, nullptr); return text;
}
struct Fixture {
  LLVMLifter lifter{Architecture::X86_64, "function-control-flow"};
  llvm::Function *target, *other, *caller, *external;
  llvm::GlobalVariable *global;
  llvm::NamedMDNode *metadata;
  explicit Fixture() {
    auto &module = lifter.module(); auto &context = module.getContext();
    module.setDataLayout("e-p:64:64");
    auto *i64 = llvm::Type::getInt64Ty(context); auto *ptr = llvm::PointerType::getUnqual(context);
    global = new llvm::GlobalVariable(module, i64, false,
        llvm::GlobalValue::ExternalLinkage, llvm::ConstantInt::get(i64, 7), "shared");
    global->setAlignment(llvm::Align(8));
    global->setMetadata("unit.global", llvm::MDNode::get(context, {llvm::MDString::get(context, "unchanged")}));
    metadata = module.getOrInsertNamedMetadata("unit.scope");
    metadata->addOperand(llvm::MDNode::get(context, {llvm::MDString::get(context, "stable")}));
    module.addModuleFlag(llvm::Module::Warning, "unit.function-control", 1);
    external = llvm::Function::Create(llvm::FunctionType::get(i64, {ptr}, false),
        llvm::GlobalValue::ExternalLinkage, "external", module);
    auto make = [&](const char *name) {
      auto *function = llvm::Function::Create(llvm::FunctionType::get(i64, {ptr, i64}, false),
          llvm::GlobalValue::ExternalLinkage, name, module);
      function->getArg(0)->setName("memory"); function->getArg(1)->setName("input");
      auto *entry = llvm::BasicBlock::Create(context, "entry", function);
      auto *good = llvm::BasicBlock::Create(context, "good", function);
      auto *bad = llvm::BasicBlock::Create(context, "bad", function);
      Builder builder(entry);
      auto *old = builder.CreateLoad(i64, function->getArg(0), "old.value"); old->setAlignment(llvm::Align(1));
      builder.CreateStore(function->getArg(1), global)->setAlignment(llvm::Align(8));
      builder.CreateCondBr(builder.getTrue(), good, bad);
      builder.SetInsertPoint(good);
      builder.CreateRet(builder.CreateAdd(function->getArg(1), builder.getInt64(0), "redundant"));
      builder.SetInsertPoint(bad); builder.CreateRet(old);
      return function;
    };
    target = make("target"); other = make("other");
    caller = llvm::Function::Create(target->getFunctionType(),
        llvm::GlobalValue::ExternalLinkage, "caller", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", caller); Builder builder(entry);
    auto *value = builder.CreateCall(target, {caller->getArg(0), caller->getArg(1)});
    builder.CreateRet(builder.CreateAdd(value, builder.getInt64(0), "caller.redundant"));
    verified(module);
  }
};
struct Identity {
  std::vector<llvm::Function *> functions;
  std::vector<std::vector<llvm::Argument *>> arguments;
  std::vector<llvm::BasicBlock *> blocks;
  std::vector<llvm::Instruction *> values;
  explicit Identity(llvm::Module &module) {
    for (auto &function : module) {
      functions.push_back(&function); std::vector<llvm::Argument *> args;
      for (auto &argument : function.args()) args.push_back(&argument);
      arguments.push_back(std::move(args));
      for (auto &block : function) { blocks.push_back(&block); for (auto &value : block) values.push_back(&value); }
    }
  }
  void checkFunctions(llvm::Module &module) const {
    REQUIRE(module.size() == functions.size()); std::size_t n = 0;
    for (auto &function : module) {
      CHECK(&function == functions[n]); REQUIRE(function.arg_size() == arguments[n].size());
      std::size_t a = 0; for (auto &argument : function.args()) CHECK(&argument == arguments[n][a++]); ++n;
    }
  }
  void checkAll(llvm::Module &module) const {
    checkFunctions(module); std::vector<llvm::BasicBlock *> currentBlocks;
    std::vector<llvm::Instruction *> currentValues;
    for (auto &function : module) for (auto &block : function) {
      currentBlocks.push_back(&block); for (auto &value : block) currentValues.push_back(&value);
    }
    CHECK(currentBlocks == blocks); CHECK(currentValues == values);
  }
};
// Independent spelling of the pre-existing recipe, not the product factory.
void referenceRecipe(llvm::Module &module, llvm::Function *selected) {
  llvm::LoopAnalysisManager loops; llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc; llvm::ModuleAnalysisManager modules; llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules); builder.registerCGSCCAnalyses(cgscc);
  builder.registerFunctionAnalyses(functions); builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  llvm::FunctionPassManager passes;
  passes.addPass(llvm::SCCPPass()); passes.addPass(llvm::DFAJumpThreadingPass());
  passes.addPass(llvm::JumpThreadingPass()); passes.addPass(llvm::SimplifyCFGPass());
  passes.addPass(llvm::InstCombinePass()); passes.addPass(llvm::DCEPass());
  if (selected) passes.run(*selected, functions);
  else {
    llvm::ModulePassManager all;
    all.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(passes))); all.run(module, modules);
  }
}
} // namespace

TEST_CASE("LLVM selected function cleanup leaves every other definition and module property intact",
          "[llvm][control-flow-function]") {
  Fixture fixture; auto &module = fixture.lifter.module(); Identity identity(module);
  const auto targetBefore = printed(*fixture.target), otherBefore = printed(*fixture.other);
  const auto callerBefore = printed(*fixture.caller), declarationBefore = printed(*fixture.external);
  const auto outside = bodyless(module, "target"); const auto count = instructions(*fixture.target);
  auto *initializer = fixture.global->getInitializer(); auto *namedOperand = fixture.metadata->getOperand(0);
  auto *globalMetadata = fixture.global->getMetadata("unit.global");
  // The selected view need not be a null-terminated C string or a whole string.
  const std::string nameWithSuffix = "target suffix";
  fixture.lifter.runControlFlowSimplification(std::string_view(nameWithSuffix.data(), 6));
  verified(module); identity.checkFunctions(module);
  CHECK(printed(*fixture.target) != targetBefore); CHECK(instructions(*fixture.target) < count);
  CHECK(printed(*fixture.other) == otherBefore); CHECK(printed(*fixture.caller) == callerBefore);
  CHECK(printed(*fixture.external) == declarationBefore); CHECK(bodyless(module, "target") == outside);
  CHECK(module.getGlobalVariable("shared") == fixture.global); CHECK(fixture.global->getInitializer() == initializer);
  CHECK(fixture.global->getMetadata("unit.global") == globalMetadata);
  CHECK(module.getNamedMetadata("unit.scope") == fixture.metadata); REQUIRE(fixture.metadata->getNumOperands() == 1);
  CHECK(fixture.metadata->getOperand(0) == namedOperand);
}

TEST_CASE("LLVM selected function cleanup rejects invalid names before all module mutation",
          "[llvm][control-flow-function]") {
  Fixture fixture; Identity identity(fixture.lifter.module()); const auto before = fixture.lifter.ir();
  const std::string embedded("target\0suffix", 13), trailing("target\0", 7), leading("\0target", 7);
  const std::vector<std::string_view> names{std::string_view{}, std::string_view("", 0),
      "missing", "external", "shared", " target", "target ", embedded, trailing, leading};
  std::size_t n = 0;
  for (auto name : names) DYNAMIC_SECTION("invalid name case " << n++) {
    CHECK_THROWS_AS(fixture.lifter.runControlFlowSimplification(name), std::invalid_argument);
    CHECK(fixture.lifter.ir() == before); identity.checkAll(fixture.lifter.module()); verified(fixture.lifter.module());
  }
}

TEST_CASE("LLVM selected and global cleanup retain the exact historical six pass recipe",
          "[llvm][control-flow-function]") {
  Fixture selected, selectedReference, global, globalReference;
  const auto otherBefore = printed(*global.other), callerBefore = printed(*global.caller);
  selected.lifter.runControlFlowSimplification("target");
  referenceRecipe(selectedReference.lifter.module(), selectedReference.target);
  CHECK(selected.lifter.ir() == selectedReference.lifter.ir());
  global.lifter.runControlFlowSimplification(); referenceRecipe(globalReference.lifter.module(), nullptr);
  CHECK(global.lifter.ir() == globalReference.lifter.ir());
  CHECK(printed(*global.target) == printed(*selected.target));
  CHECK(printed(*global.other) != otherBefore); CHECK(printed(*global.caller) != callerBefore);
  CHECK(printed(*selected.other) == otherBefore); CHECK(printed(*selected.caller) == callerBefore);
  verified(selected.lifter.module()); verified(selectedReference.lifter.module());
  verified(global.lifter.module()); verified(globalReference.lifter.module());
}

TEST_CASE("LLVM execution mode is readable without mutation and follows accepted setter changes",
          "[llvm][control-flow-function]") {
  LLVMLifter lifter{Architecture::X86_64}; const auto &readOnly = lifter;
  CHECK(readOnly.executionMode() == ExecutionMode::Legacy);
  for (auto mode : {ExecutionMode::StatefulV2, ExecutionMode::StaticAnalysis, ExecutionMode::Legacy}) {
    REQUIRE(lifter.setExecutionMode(mode)); const auto before = lifter.ir();
    CHECK(readOnly.executionMode() == mode); CHECK(lifter.ir() == before);
  }
  CHECK_FALSE(lifter.setExecutionMode(static_cast<ExecutionMode>(99)));
  CHECK(readOnly.executionMode() == ExecutionMode::Legacy);
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(lifter.module().getContext()), false),
      llvm::GlobalValue::ExternalLinkage, "already_present", lifter.module());
  const auto before = lifter.ir(); CHECK_FALSE(lifter.setExecutionMode(ExecutionMode::Legacy));
  CHECK(readOnly.executionMode() == ExecutionMode::StaticAnalysis); CHECK(lifter.ir() == before);
}
