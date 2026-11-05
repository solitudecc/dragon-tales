#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Instructions.h>

using namespace dragon;
using namespace dragon::llvmlifter;

namespace {
llvm::Function *predicate(LLVMLifter &lifter, unsigned width, bool oddProduct,
                          bool unequal, bool select, bool twoLoads = false) {
  auto &module=lifter.module();auto &context=module.getContext();
  auto *type=llvm::IntegerType::get(context,width);
  llvm::Type *inputType=twoLoads ? static_cast<llvm::Type *>(llvm::PointerType::getUnqual(context)) : type;
  auto *function=llvm::Function::Create(
      llvm::FunctionType::get(type,{inputType},false),
      llvm::GlobalValue::ExternalLinkage,"predicate",module);
  llvm::IRBuilder<> builder{llvm::BasicBlock::Create(context,"entry",function)};
  auto *x=function->getArg(0);
  llvm::Value *left=x,*right=x;
  if (twoLoads) {
    left=builder.CreateLoad(type,x);
    // A real write makes address-based input merging unsound.
    builder.CreateStore(llvm::ConstantInt::get(type,1),x);
    right=builder.CreateLoad(type,x);
  } else if (!oddProduct) right=builder.CreateAdd(x,llvm::ConstantInt::get(type,1));
  llvm::Value *condition=nullptr;
  if (twoLoads) condition=builder.CreateICmpEQ(left,right);
  else {
    auto *product=builder.CreateMul(left,right);
    auto *low=builder.CreateAnd(product,llvm::ConstantInt::get(type,1));
    condition=builder.CreateICmp(unequal ? llvm::CmpInst::ICMP_NE : llvm::CmpInst::ICMP_EQ,
        low,llvm::ConstantInt::get(type,0));
  }
  if (select) builder.CreateRet(builder.CreateSelect(condition,
      llvm::ConstantInt::get(type,7),llvm::ConstantInt::get(type,98)));
  else {
    auto *yes=llvm::BasicBlock::Create(context,"yes",function);
    auto *no=llvm::BasicBlock::Create(context,"no",function);
    builder.CreateCondBr(condition,yes,no);
    builder.SetInsertPoint(yes);builder.CreateRet(llvm::ConstantInt::get(type,7));
    builder.SetInsertPoint(no);builder.CreateRet(llvm::ConstantInt::get(type,98));
  }
  return function;
}
unsigned conditionalConsumers(const llvm::Module &module) {
  unsigned count=0;
  for (const auto &function:module) for (const auto &block:function) for (const auto &instruction:block) {
    if (const auto *branch=llvm::dyn_cast<llvm::BranchInst>(&instruction);branch && branch->isConditional()) ++count;
    if (llvm::isa<llvm::SelectInst>(instruction)) ++count;
  }
  return count;
}
void verify(LLVMLifter &lifter) {
  std::string error;llvm::raw_string_ostream stream{error};
  const auto invalid=llvm::verifyModule(lifter.module(),&stream);
  INFO(error);REQUIRE_FALSE(invalid);
}
}

TEST_CASE("LLVM predicate folding availability follows the Z3 build", "[llvm][predicate-fold]") {
  LLVMLifter lifter{Architecture::X86_64};predicate(lifter,32,false,false,false);
  const auto before=lifter.ir();const auto stats=lifter.runPredicateFolding({200,64,256});
#ifdef DRAGON_WITH_Z3
  CHECK(stats.available);CHECK(stats.conditionsFolded==1);CHECK(stats.solverQueries>0);
  CHECK(conditionalConsumers(lifter.module())==0);
#else
  CHECK_FALSE(stats.available);CHECK(stats.conditionsFolded==0);CHECK(stats.solverQueries==0);
  CHECK(lifter.ir()==before);
#endif
  verify(lifter);
}

#ifdef DRAGON_WITH_Z3
TEST_CASE("LLVM predicate folding proves both branch polarities and selects", "[llvm][predicate-fold]") {
  for (auto width:{1u,8u,16u,32u,64u,128u}) for (bool unequal:{false,true}) for (bool select:{false,true}) {
    INFO("width " << width << " unequal " << unequal << " select " << select);
    LLVMLifter lifter{Architecture::X86_64};auto *function=predicate(lifter,width,false,unequal,select);
    REQUIRE(conditionalConsumers(lifter.module())==1);
    const auto stats=lifter.runPredicateFolding({200,64,256});
    CHECK(stats.available);CHECK(stats.conditionsFolded==1);CHECK(stats.unknownQueries==0);
    CHECK(conditionalConsumers(lifter.module())==0);verify(lifter);
    auto *result=llvm::dyn_cast<llvm::ReturnInst>(function->getEntryBlock().getTerminator());
    REQUIRE(result);auto *constant=llvm::dyn_cast<llvm::ConstantInt>(result->getReturnValue());
    REQUIRE(constant);CHECK(constant->getZExtValue()==((unequal ? 98u : 7u)&(width==1 ? 1u : 255u)));
  }
}

TEST_CASE("LLVM predicate folding keeps varying inputs and independent reads", "[llvm][predicate-fold]") {
  for (bool loads:{false,true}) {
    LLVMLifter lifter{Architecture::X86_64};predicate(lifter,32,true,false,false,loads);
    const auto before=lifter.ir();const auto stats=lifter.runPredicateFolding({200,64,256});
    CHECK(stats.conditionsFolded==0);CHECK(stats.solverQueries==2);CHECK(stats.unknownQueries==0);
    CHECK(lifter.ir()==before);verify(lifter);
  }
}

TEST_CASE("LLVM predicate folding respects expression and query budgets", "[llvm][predicate-fold]") {
  for (auto options:{PredicateFoldOptions{0,64,256},PredicateFoldOptions{200,0,256},
                      PredicateFoldOptions{200,64,1}}) {
    LLVMLifter lifter{Architecture::X86_64};predicate(lifter,64,false,false,false);
    const auto before=lifter.ir();const auto stats=lifter.runPredicateFolding(options);
    CHECK(stats.conditionsFolded==0);CHECK(stats.solverQueries==0);CHECK(stats.budgetSkipped>0);
    CHECK(lifter.ir()==before);verify(lifter);
  }
  LLVMLifter lifter{Architecture::X86_64};predicate(lifter,64,true,false,false);
  const auto before=lifter.ir();const auto stats=lifter.runPredicateFolding({200,1,256});
  CHECK(stats.solverQueries==1);CHECK(stats.budgetSkipped==1);CHECK(stats.conditionsFolded==0);
  CHECK(lifter.ir()==before);verify(lifter);
}

TEST_CASE("LLVM predicate folding rejects unsupported and explicit unknown expressions", "[llvm][predicate-fold]") {
  for (unsigned kind=0;kind<6;++kind) {
    INFO("kind " << kind);
    LLVMLifter lifter{Architecture::X86_64};auto &module=lifter.module();auto &context=module.getContext();
    auto *type=llvm::Type::getInt32Ty(context);
    auto *function=llvm::Function::Create(llvm::FunctionType::get(type,{type,type},false),
        llvm::GlobalValue::ExternalLinkage,"unsupported",module);
    auto *entry=llvm::BasicBlock::Create(context,"entry",function);
    llvm::IRBuilder<> builder{entry};llvm::Value *value=nullptr;
    if (kind==0) value=builder.CreateUDiv(function->getArg(0),function->getArg(1));
    if (kind==1) value=builder.CreateShl(function->getArg(0),function->getArg(1));
    if (kind==2) value=builder.CreateShl(function->getArg(0),llvm::ConstantInt::get(type,32));
    if (kind==3) value=builder.CreateNSWMul(function->getArg(0),function->getArg(1));
    if (kind==4) value=llvm::UndefValue::get(type);
    if (kind==5) value=llvm::PoisonValue::get(type);
    // Raw instructions avoid IRBuilder folding the negative control itself.
    auto *condition=new llvm::ICmpInst(entry,llvm::CmpInst::ICMP_EQ,value,llvm::ConstantInt::get(type,0));
    auto *selection=llvm::SelectInst::Create(condition,llvm::ConstantInt::get(type,7),
        llvm::ConstantInt::get(type,99),"",entry);
    builder.CreateRet(selection);
    const auto before=lifter.ir();const auto stats=lifter.runPredicateFolding({200,64,256});
    CHECK(stats.conditionsFolded==0);CHECK(stats.solverQueries==0);CHECK(stats.unsupportedConditions==1);
    CHECK(lifter.ir()==before);verify(lifter);
  }
}

TEST_CASE("LLVM predicate folding leaves solver unknowns intact", "[llvm][predicate-fold]") {
  LLVMLifter lifter{Architecture::X86_64};predicate(lifter,64,false,false,false);
  const auto before=lifter.ir();const auto stats=lifter.runPredicateFolding({200,64,256,1});
  CHECK(stats.solverQueries==2);CHECK(stats.unknownQueries==2);CHECK(stats.conditionsFolded==0);
  CHECK(lifter.ir()==before);verify(lifter);
}

TEST_CASE("LLVM predicate folding reuses proofs for shared consumers", "[llvm][predicate-fold]") {
  LLVMLifter lifter{Architecture::X86_64};auto *function=predicate(lifter,32,false,false,false);
  auto *branch=llvm::cast<llvm::BranchInst>(function->getEntryBlock().getTerminator());
  llvm::IRBuilder<> builder{branch};auto *type=llvm::Type::getInt32Ty(lifter.module().getContext());
  auto *selection=builder.CreateSelect(branch->getCondition(),llvm::ConstantInt::get(type,7),llvm::ConstantInt::get(type,98));
  llvm::cast<llvm::ReturnInst>(branch->getSuccessor(0)->getTerminator())->setOperand(0,selection);
  const auto stats=lifter.runPredicateFolding({200,64,256});
  CHECK(stats.conditionsFolded==2);CHECK(stats.solverQueries==2);
  CHECK(conditionalConsumers(lifter.module())==0);verify(lifter);
}

TEST_CASE("LLVM predicate folding preserves signed casts ranges and fixed shifts", "[llvm][predicate-fold]") {
  // Signed byte edges are -128..127; unsigned edges are 0..255. The
  // A64 SXT[B]/UXT[B]/LSL/ASR oracle exercises both negative and positive inputs.
  for (unsigned kind=0;kind<14;++kind) {
    INFO("kind " << kind);
    LLVMLifter lifter{Architecture::X86_64};auto &context=lifter.module().getContext();
    auto *byte=llvm::Type::getInt8Ty(context),*wide=llvm::Type::getInt64Ty(context);
    auto *function=llvm::Function::Create(llvm::FunctionType::get(wide,{kind>=12 ? wide : byte},false),
        llvm::GlobalValue::ExternalLinkage,"range",lifter.module());
    llvm::IRBuilder<> builder{llvm::BasicBlock::Create(context,"entry",function)};
    llvm::Value *value=kind<6 ? builder.CreateSExt(function->getArg(0),wide) :
        kind==12 ? builder.CreateZExt(builder.CreateTrunc(function->getArg(0),byte),wide) :
        builder.CreateZExt(function->getArg(0),wide);
    llvm::Value *condition=nullptr;bool expected=true,variable=false;
    if (kind<6) {
      const llvm::CmpInst::Predicate predicates[]={llvm::CmpInst::ICMP_SGT,llvm::CmpInst::ICMP_SLE,
          llvm::CmpInst::ICMP_SLT,llvm::CmpInst::ICMP_SGE,llvm::CmpInst::ICMP_SLT,llvm::CmpInst::ICMP_SGE};
      const std::int64_t bounds[]={127,127,-128,-128,0,0};
      condition=builder.CreateICmp(predicates[kind],value,llvm::ConstantInt::get(wide,std::uint64_t(bounds[kind])));
      expected=kind==1 || kind==3;variable=kind>=4;
    } else if (kind<10 || kind==12) {
      const llvm::CmpInst::Predicate predicates[]={llvm::CmpInst::ICMP_UGT,llvm::CmpInst::ICMP_ULE,
          llvm::CmpInst::ICMP_ULT,llvm::CmpInst::ICMP_UGE};
      const unsigned index=kind==12 ? 2 : kind-6;
      condition=builder.CreateICmp(predicates[index],value,llvm::ConstantInt::get(wide,index<2 ? 255 : 256));
      expected=index==1 || index==2;
    } else if (kind==13) {
      auto *extended=builder.CreateZExt(value,llvm::IntegerType::get(context,128));
      auto *bound=llvm::ConstantInt::get(context,llvm::APInt::getOneBitSet(128,127));
      condition=builder.CreateICmpULT(extended,bound);
    } else {
      if (kind==11) value=builder.CreateSExt(function->getArg(0),wide);
      auto *shifted=builder.CreateShl(value,llvm::ConstantInt::get(wide,24));
      auto *restored=kind==11 ? builder.CreateAShr(shifted,llvm::ConstantInt::get(wide,24)) :
          builder.CreateLShr(shifted,llvm::ConstantInt::get(wide,24));
      condition=builder.CreateICmpEQ(restored,value);
    }
    builder.CreateRet(builder.CreateSelect(condition,llvm::ConstantInt::get(wide,7),llvm::ConstantInt::get(wide,98)));
    const auto stats=lifter.runPredicateFolding({200,64,256});
    CHECK(stats.conditionsFolded==(variable ? 0 : 1));CHECK(stats.unknownQueries==0);verify(lifter);
    if (!variable) {
      auto *result=llvm::cast<llvm::ReturnInst>(function->getEntryBlock().getTerminator());
      auto *constant=llvm::dyn_cast<llvm::ConstantInt>(result->getReturnValue());
      REQUIRE(constant);CHECK(constant->getZExtValue()==(expected ? 7 : 98));
    }
  }
}

TEST_CASE("LLVM predicate folding proves the bitwise subtraction identity", "[llvm][predicate-fold]") {
  // A64 ORR/AND/SUB/EOR confirms (x|y)-(x&y) == (x^y), including carries.
  // This wide identity needs more proof work than the production default;
  // exercise the explicit budget without weakening the expected rewrite.
  for (unsigned width:{1u,8u,16u,32u,64u,128u}) {
    INFO("width " << width);
    LLVMLifter lifter{Architecture::X86_64};auto &context=lifter.module().getContext();
    auto *type=llvm::IntegerType::get(context,width);
    auto *function=llvm::Function::Create(llvm::FunctionType::get(type,{type,type},false),
        llvm::GlobalValue::ExternalLinkage,"bitwise",lifter.module());
    llvm::IRBuilder<> builder{llvm::BasicBlock::Create(context,"entry",function)};
    auto *x=function->getArg(0),*y=function->getArg(1);
    auto *difference=builder.CreateSub(builder.CreateOr(x,y),builder.CreateAnd(x,y));
    auto *condition=builder.CreateICmpEQ(difference,builder.CreateXor(x,y));
    builder.CreateRet(builder.CreateSelect(condition,llvm::ConstantInt::get(type,7),llvm::ConstantInt::get(type,98)));
    const auto stats=lifter.runPredicateFolding({5000,64,256,2000000});
    CHECK(stats.conditionsFolded==1);CHECK(stats.unknownQueries==0);verify(lifter);
    auto *result=llvm::cast<llvm::ReturnInst>(function->getEntryBlock().getTerminator());
    auto *constant=llvm::dyn_cast<llvm::ConstantInt>(result->getReturnValue());
    REQUIRE(constant);CHECK(constant->getZExtValue()==(width==1 ? 1 : 7));
  }
}

TEST_CASE("LLVM predicate folding keeps the selected arm and independent inputs", "[llvm][predicate-fold]") {
  for (bool variable:{false,true}) {
    INFO("variable " << variable);
    LLVMLifter lifter{Architecture::X86_64};auto &context=lifter.module().getContext();
    auto *type=llvm::Type::getInt64Ty(context);
    auto *function=llvm::Function::Create(llvm::FunctionType::get(type,
        {llvm::Type::getInt1Ty(context),type,type},false),llvm::GlobalValue::ExternalLinkage,
        "select_input",lifter.module());
    auto *entry=llvm::BasicBlock::Create(context,"entry",function);llvm::IRBuilder<> builder{entry};
    auto *input=function->getArg(0);
    auto *selection=llvm::SelectInst::Create(input,
        variable ? static_cast<llvm::Value *>(function->getArg(1)) : llvm::ConstantInt::get(type,1),
        variable ? static_cast<llvm::Value *>(function->getArg(2)) : llvm::ConstantInt::get(type,0),"",entry);
    llvm::Value *condition=nullptr;
    if (variable) condition=builder.CreateICmpEQ(selection,function->getArg(1));
    else condition=builder.CreateICmpEQ(builder.CreateICmpEQ(selection,llvm::ConstantInt::get(type,1)),input);
    builder.CreateRet(builder.CreateSelect(condition,llvm::ConstantInt::get(type,7),llvm::ConstantInt::get(type,98)));
    const auto before=lifter.ir();const auto stats=lifter.runPredicateFolding({200,64,256});
    CHECK(stats.conditionsFolded==(variable ? 0 : 1));CHECK(stats.unknownQueries==0);verify(lifter);
    if (variable) CHECK(lifter.ir()==before);
    else {
      auto *result=llvm::cast<llvm::ReturnInst>(function->getEntryBlock().getTerminator());
      auto *constant=llvm::dyn_cast<llvm::ConstantInt>(result->getReturnValue());
      REQUIRE(constant);CHECK(constant->getZExtValue()==7);
    }
  }
}
#endif
