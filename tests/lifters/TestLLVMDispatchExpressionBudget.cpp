#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <string>

using namespace dragon;
using namespace dragon::llvmlifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum class ValueKind { Expression, Constant, Argument, Load, Poison, Flagged, UnknownWithDeepRight };
struct View { llvm::BasicBlock *entry, *taken; };
View fixture(llvm::Module &module, bool branch, ValueKind kind, const char *name="condition") {
  auto &context=module.getContext();auto *integer=llvm::IntegerType::get(context,branch?1:8);
  auto *pointer=llvm::PointerType::getUnqual(context);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context),{integer,pointer},false),
      llvm::GlobalValue::ExternalLinkage,name,module);
  auto *entry=llvm::BasicBlock::Create(context,"entry",f);
  auto *taken=llvm::BasicBlock::Create(context,"taken",f);
  auto *fallback=llvm::BasicBlock::Create(context,"fallback",f);
  Builder b(entry);llvm::Value *value=nullptr;
  auto constant=[&](unsigned n){return llvm::ConstantInt::get(integer,n);};
  switch(kind) {
    case ValueKind::Expression:
      value=branch?b.CreateXor(constant(1),constant(0)):b.CreateAdd(constant(1),constant(2));break;
    case ValueKind::Constant:value=constant(branch?1:3);break;
    case ValueKind::Argument:value=f->getArg(0);break;
    case ValueKind::Load:value=b.CreateLoad(integer,f->getArg(1),"memory.condition");break;
    case ValueKind::Poison:value=llvm::PoisonValue::get(integer);break;
    case ValueKind::Flagged:
      value=b.CreateAdd(constant(branch?1:255),constant(1));
      llvm::cast<llvm::BinaryOperator>(value)->setHasNoUnsignedWrap(true);break;
    case ValueKind::UnknownWithDeepRight:
      value=b.CreateAdd(f->getArg(0),b.CreateAdd(constant(0),constant(0)));break;
  }
  if(branch)b.CreateCondBr(value,taken,fallback);
  else {auto *s=b.CreateSwitch(value,fallback,1);s->addCase(constant(3),taken);}
  b.SetInsertPoint(taken);b.CreateRetVoid();b.SetInsertPoint(fallback);b.CreateRetVoid();
  REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return {entry,taken};
}
std::string text(llvm::Module &module) {
  std::string result;llvm::raw_string_ostream out(result);module.print(out,nullptr);return result;
}
void verify(llvm::Module &module) {REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));}
}

TEST_CASE("Dispatcher expression budget distinguishes exact branch and switch boundaries",
          "[llvm][dispatch-specialize][dispatch-expression-budget]") {
  for(bool branch:{false,true})for(unsigned cap:{2u,3u,4u})
    DYNAMIC_SECTION("branch "<<branch<<" cap "<<cap) {
      llvm::LLVMContext context;llvm::Module module("budget-boundary",context);
      auto view=fixture(module,branch,ValueKind::Expression);const auto before=text(module);
      DispatcherSpecializationOptions options;options.maxExpressionNodes=cap;
      const auto stats=runDispatcherSpecialization(module,options);verify(module);
      CHECK(stats.available);CHECK(stats.conditionsVisited==1);
      if(cap==2) {
        CHECK_FALSE(stats.complete);CHECK(stats.budgetExhausted==1);CHECK_FALSE(stats.changed);
        CHECK(stats.uniqueConditions==0);CHECK(text(module)==before);
        CHECK(stats.unresolvedSwitches==(branch?0:1));
      } else {
        CHECK(stats.complete);CHECK(stats.budgetExhausted==0);CHECK(stats.changed);
        CHECK(stats.uniqueConditions==1);CHECK(view.entry->getTerminator()->getNumSuccessors()==1);
        CHECK(view.entry->getTerminator()->getSuccessor(0)==view.taken);
      }
    }
}

TEST_CASE("Dispatcher unknown values are distinct from expression budget exhaustion",
          "[llvm][dispatch-specialize][dispatch-expression-budget]") {
  for(bool branch:{false,true})for(auto kind:{ValueKind::Argument,ValueKind::Load,ValueKind::Poison,ValueKind::Flagged})
    DYNAMIC_SECTION("branch "<<branch<<" kind "<<static_cast<int>(kind)) {
      llvm::LLVMContext context;llvm::Module module("unknown-control",context);
      fixture(module,branch,kind);const auto before=text(module);
      DispatcherSpecializationOptions options;options.maxExpressionNodes=3;
      const auto stats=runDispatcherSpecialization(module,options);verify(module);
      CHECK(stats.complete);CHECK(stats.budgetExhausted==0);CHECK_FALSE(stats.changed);
      CHECK(stats.uniqueConditions==0);CHECK(text(module)==before);
    }
}

TEST_CASE("Dispatcher budget accounting preserves eager unknown expression traversal",
          "[llvm][dispatch-specialize][dispatch-expression-budget]") {
  for(bool branch:{false,true})DYNAMIC_SECTION("branch "<<branch) {
    llvm::LLVMContext context;llvm::Module module("unknown-with-budget",context);
    fixture(module,branch,ValueKind::UnknownWithDeepRight);const auto before=text(module);
    DispatcherSpecializationOptions options;options.maxExpressionNodes=2;
    const auto stats=runDispatcherSpecialization(module,options);verify(module);
    CHECK_FALSE(stats.complete);CHECK(stats.budgetExhausted==1);
    CHECK_FALSE(stats.changed);CHECK(text(module)==before);
  }
}

TEST_CASE("Dispatcher expression exhaustion counts roots and continues later conditions",
          "[llvm][dispatch-specialize][dispatch-expression-budget]") {
  llvm::LLVMContext context;llvm::Module module("continue-after-budget",context);
  fixture(module,false,ValueKind::Expression,"switch_exhausted");
  fixture(module,true,ValueKind::Expression,"branch_exhausted");
  auto later=fixture(module,false,ValueKind::Constant,"later_proven");
  DispatcherSpecializationOptions options;options.maxExpressionNodes=2;
  const auto stats=runDispatcherSpecialization(module,options);verify(module);
  CHECK_FALSE(stats.complete);CHECK(stats.budgetExhausted==2);CHECK(stats.conditionsVisited==3);
  CHECK(stats.changed);CHECK(stats.uniqueConditions==1);CHECK(stats.switchesRewritten==1);
  CHECK(later.entry->getTerminator()->getNumSuccessors()==1);
  CHECK(later.entry->getTerminator()->getSuccessor(0)==later.taken);
}

TEST_CASE("Dispatcher expression budget reaches pipeline incomplete status",
          "[llvm][pipeline][dispatch-expression-budget]") {
  for(bool branch:{false,true})DYNAMIC_SECTION("branch "<<branch) {
    LLVMLifter lifter{Architecture::X86_64};fixture(lifter.module(),branch,ValueKind::Expression);
    const auto before=lifter.ir();passes::PassPipeline pipeline;
    REQUIRE(pipeline.add("dispatch-specialize",{{"max_expression_nodes",std::int64_t{2}}}));
    const auto report=pipeline.run(lifter);REQUIRE(report.ok);REQUIRE(report.runs.size()==1);
    CHECK(report.runs[0].result.status==passes::Status::Incomplete);
    CHECK(report.runs[0].result.counters.at("budget_exhausted")==1);
    CHECK_FALSE(report.runs[0].result.changed);CHECK(lifter.ir()==before);verify(lifter.module());
  }
}
