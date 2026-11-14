#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Support/ModRef.h>
#include <array>
#include <vector>

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

namespace {
void returnedConstant(llvm::Function &function, std::uint64_t expected) {
  unsigned returns = 0;
  for (auto &block : function)
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())) {
      auto *value = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue());
      REQUIRE(value); CHECK(value->getZExtValue() == expected); ++returns;
    }
  CHECK(returns > 0);
}

// This is the real algebraic form, with an actual historical integer PHI and
// an unrelated observable call. No history is enumerated or assumed constant.
llvm::Function *historicalFunnelPredicate(LLVMLifter &lifter, unsigned width,
                                        llvm::Intrinsic::ID id) {
  auto &context = lifter.module().getContext();
  auto *word = llvm::IntegerType::get(context, width);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word, word}, false),
      llvm::GlobalValue::ExternalLinkage, "historical_funnel", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *head = llvm::BasicBlock::Create(context, "head", function);
  auto *latch = llvm::BasicBlock::Create(context, "latch", function);
  auto *done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> b{entry};
  auto observer = lifter.module().getOrInsertFunction("observable_effect",
      llvm::FunctionType::get(b.getVoidTy(), {}, false));
  b.CreateCall(observer); b.CreateBr(head);
  b.SetInsertPoint(head);
  auto *history = b.CreatePHI(word, 2, "history");
  auto *iteration = b.CreatePHI(word, 2, "iteration");
  history->addIncoming(function->getArg(0), entry);
  iteration->addIncoming(llvm::ConstantInt::get(word, 0), entry);
  auto *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(&lifter.module(), id, {word});
  auto *observation = b.CreateCall(intrinsic,
      {history, history, function->getArg(1)}, "observation");
  auto *mixed = b.CreateXor(observation, history);
  auto *odd = b.CreateOr(mixed, llvm::ConstantInt::get(word, 1));
  auto *low = b.CreateAnd(b.CreateMul(odd, odd), llvm::ConstantInt::get(word, 7));
  auto *minus = b.CreateNSWAdd(low, llvm::ConstantInt::getAllOnesValue(word));
  auto *condition = b.CreateICmpEQ(minus, llvm::ConstantInt::get(word, 0));
  auto *selected = b.CreateSelect(condition, llvm::ConstantInt::get(word, 7),
                                 llvm::ConstantInt::get(word, 98));
  b.CreateBr(latch); b.SetInsertPoint(latch);
  auto *next = b.CreateAdd(history, llvm::ConstantInt::get(word, 1));
  auto *nextIteration = b.CreateAdd(iteration, llvm::ConstantInt::get(word, 1));
  history->addIncoming(next, latch); iteration->addIncoming(nextIteration, latch);
  b.CreateCondBr(b.CreateICmpULT(nextIteration, llvm::ConstantInt::get(word, 3)), head, done);
  b.SetInsertPoint(done); b.CreateRet(selected);
  return function;
}
}

TEST_CASE("LLVM predicate folding proves operand bounded nowrap additions and subtractions",
          "[llvm][predicate-fold][predicate-boundaries]") {
  for (unsigned width : {8u, 32u, 64u, 128u}) for (unsigned kind = 0; kind < 7; ++kind) {
    INFO("width " << width << " kind " << kind);
    LLVMLifter lifter{Architecture::X86_64}; auto &context = lifter.module().getContext();
    auto *word = llvm::IntegerType::get(context, width);
    auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word}, false),
        llvm::GlobalValue::ExternalLinkage, "safe_flags", lifter.module());
    llvm::IRBuilder<> b{llvm::BasicBlock::Create(context, "entry", function)};
    llvm::Value *left = b.CreateAnd(function->getArg(0), llvm::ConstantInt::get(word, 7));
    llvm::Value *right = llvm::ConstantInt::get(word, kind == 0 ? llvm::APInt::getAllOnes(width) : llvm::APInt(width, 1));
    const bool subtract = kind >= 3 && kind <= 5;
    if (kind == 4 || kind == 5) { right = left; left = llvm::ConstantInt::get(word, 7); }
    if (kind == 6) left = b.CreateZExt(b.CreateTrunc(function->getArg(0),
        llvm::IntegerType::get(context, width / 2)), word);
    auto *checked = b.Insert(subtract ? llvm::BinaryOperator::CreateSub(left, right)
                                     : llvm::BinaryOperator::CreateAdd(left, right), "checked");
    checked->setHasNoSignedWrap(kind == 0 || kind == 2 || kind == 3 || kind == 5 || kind == 6);
    checked->setHasNoUnsignedWrap(kind == 1 || kind == 2 || kind == 4 || kind == 5 || kind == 6);
    auto *plain = b.Insert(subtract ? llvm::BinaryOperator::CreateSub(left, right)
                                   : llvm::BinaryOperator::CreateAdd(left, right), "plain");
    auto *condition = b.CreateICmpEQ(checked, plain);
    b.CreateRet(b.CreateSelect(condition, llvm::ConstantInt::get(word, 7), llvm::ConstantInt::get(word, 98)));
    verify(lifter); const auto stats = lifter.runPredicateFolding({200, 64, 256});
    CHECK(stats.conditionsFolded == 1); CHECK(stats.unknownQueries == 0);
    returnedConstant(*function, 7); verify(lifter);
  }
}

TEST_CASE("LLVM predicate folding proves the historical funnel odd square condition",
          "[llvm][predicate-fold][predicate-boundaries]") {
  for (unsigned width : {32u, 64u, 128u})
    for (auto id : {llvm::Intrinsic::fshl, llvm::Intrinsic::fshr}) {
      INFO("width " << width << " intrinsic " << id);
      LLVMLifter lifter{Architecture::X86_64};
      auto *function = historicalFunnelPredicate(lifter, width, id); verify(lifter);
      const auto stats = lifter.runPredicateFolding({200, 64, 256});
      CHECK(stats.conditionsFolded == 1); CHECK(stats.unknownQueries == 0);
      returnedConstant(*function, 7);
      unsigned effects = 0;
      for (auto &block : *function) for (auto &instruction : block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
          if (auto *callee = call->getCalledFunction(); callee && callee->getName() == "observable_effect") ++effects;
      CHECK(effects == 1); verify(lifter);
    }
}

TEST_CASE("LLVM predicate folding refuses unsafe or nested flagged operand ranges",
          "[llvm][predicate-fold][predicate-boundaries]") {
  for (unsigned width : {8u, 32u, 64u, 128u}) for (unsigned kind = 0; kind < 9; ++kind) {
    INFO("width " << width << " kind " << kind);
    LLVMLifter lifter{Architecture::X86_64}; auto &context = lifter.module().getContext();
    auto *word = llvm::IntegerType::get(context, width);
    auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word}, false),
        llvm::GlobalValue::ExternalLinkage, "unsafe_flags", lifter.module());
    llvm::IRBuilder<> b{llvm::BasicBlock::Create(context, "entry", function)};
    auto *x = function->getArg(0); auto *one = llvm::ConstantInt::get(word, 1);
    auto *low = b.CreateAnd(x, llvm::ConstantInt::get(word, 7));
    llvm::BinaryOperator *bad = nullptr;
    if (kind == 0) { bad = b.Insert(llvm::BinaryOperator::CreateAdd(x, one)); bad->setHasNoSignedWrap(); }
    if (kind == 1) { bad = b.Insert(llvm::BinaryOperator::CreateAdd(low, llvm::ConstantInt::getAllOnesValue(word))); bad->setHasNoUnsignedWrap(); }
    if (kind == 2) { bad = b.Insert(llvm::BinaryOperator::CreateSub(low, one)); bad->setHasNoUnsignedWrap(); }
    if (kind == 3) { bad = b.Insert(llvm::BinaryOperator::CreateMul(low, one)); bad->setHasNoSignedWrap(); }
    if (kind == 4) { bad = b.Insert(llvm::BinaryOperator::CreateShl(low, one)); bad->setHasNoUnsignedWrap(); }
    if (kind == 5) { bad = b.Insert(llvm::BinaryOperator::CreateSub(x, one)); bad->setHasNoSignedWrap(); }
    if (kind >= 6) {
      llvm::Value *operand = x;
      if (kind == 6) {
        auto *nested = b.Insert(llvm::BinaryOperator::CreateAdd(x, one)); nested->setHasNoSignedWrap(); operand = nested;
      } else operand = kind == 7 ? static_cast<llvm::Value *>(llvm::PoisonValue::get(word))
                                : static_cast<llvm::Value *>(llvm::UndefValue::get(word));
      // Raw AND prevents the builder from erasing the bad dependency first.
      auto *masked = b.Insert(llvm::BinaryOperator::CreateAnd(operand, llvm::ConstantInt::get(word, 0)));
      bad = b.Insert(llvm::BinaryOperator::CreateAdd(masked, llvm::ConstantInt::getAllOnesValue(word)));
      bad->setHasNoSignedWrap();
    }
    auto *condition = b.CreateICmpEQ(bad, llvm::ConstantInt::get(word, 0));
    b.CreateRet(b.CreateSelect(condition, llvm::ConstantInt::get(word, 7), llvm::ConstantInt::get(word, 98)));
    verify(lifter); const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding({200, 64, 256});
    CHECK(stats.conditionsFolded == 0); CHECK(stats.solverQueries == 0);
    CHECK(stats.unsupportedConditions == 1); CHECK(lifter.ir() == before); verify(lifter);
  }
}

TEST_CASE("LLVM predicate folding retains funnel poison call and effect barriers",
          "[llvm][predicate-fold][predicate-boundaries]") {
  for (unsigned width : {32u, 64u}) for (auto id : {llvm::Intrinsic::fshl, llvm::Intrinsic::fshr})
    for (const char *mode : {"poison0", "poison1", "poison2", "undef0", "undef1", "undef2",
                             "nested-poison", "nested-undef", "flagged-arg", "external",
                             "nested-external", "bundle", "convergent", "effects"}) {
      INFO("width " << width << " intrinsic " << id << " mode " << mode);
      LLVMLifter lifter{Architecture::X86_64}; auto &context = lifter.module().getContext();
      auto *word = llvm::IntegerType::get(context, width);
      auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word, word}, false),
          llvm::GlobalValue::ExternalLinkage, "funnel_barrier", lifter.module());
      llvm::IRBuilder<> b{llvm::BasicBlock::Create(context, "entry", function)};
      auto *x = function->getArg(0); std::array<llvm::Value *, 3> args{x, x, function->getArg(1)};
      const std::string kind = mode;
      for (unsigned i = 0; i < 3; ++i) {
        if (kind == "poison" + std::to_string(i)) args[i] = llvm::PoisonValue::get(word);
        if (kind == "undef" + std::to_string(i)) args[i] = llvm::UndefValue::get(word);
      }
      if (kind == "nested-poison" || kind == "nested-undef")
        args[0] = b.Insert(llvm::BinaryOperator::CreateAdd(x, kind == "nested-poison"
            ? static_cast<llvm::Value *>(llvm::PoisonValue::get(word)) : llvm::UndefValue::get(word)));
      if (kind == "flagged-arg") {
        auto *bad = b.Insert(llvm::BinaryOperator::CreateAdd(x, llvm::ConstantInt::get(word, 1)));
        bad->setHasNoSignedWrap(); args[0] = bad;
      }
      auto *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(&lifter.module(), id, {word});
      llvm::FunctionCallee callee = intrinsic;
      auto external = lifter.module().getOrInsertFunction("unknown_funnel",
          llvm::FunctionType::get(word, {word, word, word}, false));
      if (kind == "external") callee = external;
      if (kind == "nested-external") args[0] = b.CreateCall(external, {x, x, function->getArg(1)});
      if (kind == "effects") intrinsic->setMemoryEffects(llvm::MemoryEffects::unknown());
      std::vector<llvm::OperandBundleDef> bundles;
      if (kind == "bundle") bundles.emplace_back("test.observation", std::vector<llvm::Value *>{x});
      auto *observed = b.CreateCall(callee, args, bundles, "observed");
      if (kind == "convergent") observed->setConvergent();
      auto *odd = b.CreateOr(observed, llvm::ConstantInt::get(word, 1));
      auto *low = b.CreateAnd(b.CreateMul(odd, odd), llvm::ConstantInt::get(word, 7));
      auto *condition = b.CreateICmpEQ(low, llvm::ConstantInt::get(word, 1));
      b.CreateRet(b.CreateSelect(condition, llvm::ConstantInt::get(word, 7), llvm::ConstantInt::get(word, 98)));
      verify(lifter); const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding({200, 64, 256});
      CHECK(stats.conditionsFolded == 0); CHECK(stats.solverQueries == 0);
      CHECK(stats.unsupportedConditions == 1); CHECK(lifter.ir() == before); verify(lifter);
    }
}

TEST_CASE("LLVM predicate folding keeps funnel Value identity freeze and original budgets",
          "[llvm][predicate-fold][predicate-boundaries]") {
  for (bool distinct : {false, true}) for (bool frozen : {false, true}) {
    INFO("distinct " << distinct << " frozen " << frozen);
    LLVMLifter lifter{Architecture::X86_64}; auto &context = lifter.module().getContext();
    auto *word = llvm::Type::getInt32Ty(context);
    auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word, word}, false),
        llvm::GlobalValue::ExternalLinkage, "funnel_identity", lifter.module());
    llvm::IRBuilder<> b{llvm::BasicBlock::Create(context, "entry", function)};
    llvm::Value *x = function->getArg(0);
    if (frozen) x = b.Insert(new llvm::FreezeInst(llvm::PoisonValue::get(word)), "fixed");
    auto *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(&lifter.module(), llvm::Intrinsic::fshl, {word});
    auto *left = b.CreateCall(intrinsic, {x, x, function->getArg(1)}, "first");
    llvm::Value *right = distinct ? b.CreateCall(intrinsic, {x, x, function->getArg(1)}, "second") : left;
    auto *one = llvm::ConstantInt::get(word, 1);
    auto *low = b.CreateAnd(b.CreateMul(b.CreateOr(left, one), b.CreateOr(right, one)), llvm::ConstantInt::get(word, 7));
    auto *condition = b.CreateICmpEQ(low, one);
    b.CreateRet(b.CreateSelect(condition, llvm::ConstantInt::get(word, 7), llvm::ConstantInt::get(word, 98)));
    verify(lifter); const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding({200, 64, 256});
    CHECK(stats.conditionsFolded == (distinct ? 0 : 1)); CHECK(stats.unknownQueries == 0);
    if (distinct) CHECK(lifter.ir() == before); else returnedConstant(*function, 7);
    verify(lifter);
  }
  for (auto options : {PredicateFoldOptions{0, 64, 256}, PredicateFoldOptions{200, 0, 256},
                       PredicateFoldOptions{200, 64, 1}}) {
    LLVMLifter lifter{Architecture::X86_64}; historicalFunnelPredicate(lifter, 32, llvm::Intrinsic::fshl);
    const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding(options);
    CHECK(stats.conditionsFolded == 0); CHECK(stats.solverQueries == 0);
    CHECK(stats.budgetSkipped > 0); CHECK(lifter.ir() == before); verify(lifter);
  }
}
#endif

TEST_CASE("LLVM predicate funnel observation availability follows the existing contract",
          "[llvm][predicate-fold][predicate-boundaries]") {
  LLVMLifter lifter{Architecture::X86_64}; auto &context = lifter.module().getContext();
  auto *word = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word}, false),
      llvm::GlobalValue::ExternalLinkage, "funnel_available", lifter.module());
  llvm::IRBuilder<> b{llvm::BasicBlock::Create(context, "entry", function)};
  auto *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(&lifter.module(), llvm::Intrinsic::fshr, {word});
  auto *x = b.CreateCall(intrinsic, {function->getArg(0), function->getArg(0), function->getArg(0)});
  auto *odd = b.CreateOr(x, llvm::ConstantInt::get(word, 1));
  auto *low = b.CreateAnd(b.CreateMul(odd, odd), llvm::ConstantInt::get(word, 7));
  auto *minus = b.CreateNSWAdd(low, llvm::ConstantInt::getAllOnesValue(word));
  auto *condition = b.CreateICmpEQ(minus, llvm::ConstantInt::get(word, 0));
  b.CreateRet(b.CreateSelect(condition, llvm::ConstantInt::get(word, 7), llvm::ConstantInt::get(word, 98)));
  const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding({200, 64, 256});
#ifdef DRAGON_WITH_Z3
  CHECK(stats.available); CHECK(stats.conditionsFolded == 1); CHECK(stats.unknownQueries == 0);
  CHECK(lifter.ir() != before);
#else
  CHECK_FALSE(stats.available); CHECK(stats.conditionsFolded == 0); CHECK(stats.solverQueries == 0);
  CHECK(lifter.ir() == before);
#endif
  verify(lifter);
}

#ifdef DRAGON_WITH_Z3
namespace {
struct ScheduledPredicateFixture {
  llvm::Function *function;
  std::vector<llvm::WeakTrackingVH> targets;
};
ScheduledPredicateFixture scheduledPredicates(LLVMLifter &lifter, unsigned varying,
    bool reverse, bool sharedVarying = false, unsigned targetCopies = 1) {
  auto &context = lifter.module().getContext();
  auto *word = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word}, false),
      llvm::GlobalValue::ExternalLinkage, "scheduled_predicates", lifter.module());
  llvm::IRBuilder<> b{llvm::BasicBlock::Create(context, "entry", function)};
  auto *x = function->getArg(0);
  auto *product = b.CreateMul(x, b.CreateAdd(x, b.getInt32(1)));
  auto *invariant = b.CreateICmpEQ(b.CreateAnd(product, b.getInt32(1)), b.getInt32(0));
  llvm::Value *sum = b.getInt32(0), *shared = nullptr;
  ScheduledPredicateFixture fixture{function, {}};
  auto addTargets = [&] {
    for (unsigned i = 0; i < targetCopies; ++i) {
      auto *selected = b.CreateSelect(invariant, b.getInt32(7), b.getInt32(98), "invariant.selection");
      fixture.targets.emplace_back(selected); sum = b.CreateAdd(sum, selected);
    }
  };
  if (reverse) addTargets();
  for (unsigned i = 0; i < varying; ++i) {
    auto *condition = sharedVarying && shared ? shared : b.CreateICmpULT(x, b.getInt32(i + 1));
    if (sharedVarying) shared = condition;
    sum = b.CreateAdd(sum, b.CreateSelect(condition, b.getInt32(11), b.getInt32(22)));
  }
  if (!reverse) addTargets();
  b.CreateRet(sum); return fixture;
}
void checkScheduledTargets(const ScheduledPredicateFixture &fixture) {
  for (const auto &target : fixture.targets) {
    auto *constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(static_cast<llvm::Value *>(target));
    CHECK(constant != nullptr);
    if (constant) CHECK(constant->getZExtValue() == 7);
  }
}
}

TEST_CASE("LLVM predicate scheduling protects invariant proofs from early varying conditions",
          "[llvm][predicate-fold][predicate-scheduling]") {
  for (bool reverse : {false, true}) {
    INFO("reverse " << reverse);
    LLVMLifter lifter{Architecture::X86_64};
    auto fixture = scheduledPredicates(lifter, 2, reverse); verify(lifter);
    const auto stats = lifter.runPredicateFolding({200, 2, 128});
    CHECK(stats.conditionsVisited == 3); CHECK(stats.conditionsFolded == 1);
    CHECK(stats.solverQueries == 2); CHECK(stats.budgetSkipped == 2);
    CHECK(stats.unknownQueries == 0); checkScheduledTargets(fixture); verify(lifter);
  }
}

TEST_CASE("LLVM predicate scheduling shares both deferred and proven consumers",
          "[llvm][predicate-fold][predicate-scheduling]") {
  for (bool reverse : {false, true}) {
    INFO("reverse " << reverse);
    LLVMLifter lifter{Architecture::X86_64};
    auto fixture = scheduledPredicates(lifter, 2, reverse, true, 2); verify(lifter);
    const auto stats = lifter.runPredicateFolding({200, 2, 128});
    CHECK(stats.conditionsVisited == 4); CHECK(stats.conditionsFolded == 2);
    CHECK(stats.solverQueries == 2); CHECK(stats.budgetSkipped == 2);
    CHECK(stats.unknownQueries == 0); checkScheduledTargets(fixture); verify(lifter);
  }
}

TEST_CASE("LLVM predicate scheduling still executes legacy single condition queries",
          "[llvm][predicate-fold][predicate-scheduling]") {
  for (unsigned queries : {0u, 1u, 2u}) for (bool loads : {false, true}) {
    INFO("queries " << queries << " loads " << loads);
    LLVMLifter lifter{Architecture::X86_64}; predicate(lifter, 32, true, false, false, loads);
    const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding({200, queries, 128});
    CHECK(stats.conditionsFolded == 0); CHECK(stats.solverQueries == queries);
    CHECK(stats.budgetSkipped == (queries < 2 ? 1 : 0));
    CHECK(stats.unknownQueries == 0); CHECK(lifter.ir() == before); verify(lifter);
  }
}

TEST_CASE("LLVM predicate scheduling falls back to SMT when extra sampling work runs out",
          "[llvm][predicate-fold][predicate-scheduling]") {
  LLVMLifter lifter{Architecture::X86_64}; scheduledPredicates(lifter, 3, false, false, 0);
  const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding({200, 4, 4});
  // The separate sampling allowance is4*4 units. A3-node/1-atom formula
  // reserves5 units per numeric assignment. One dual witness costs10; the
  // next attempt reserves5 and cannot reserve another. Legacy SMT handles
  // that condition and its successor; the first deferred consumer is skipped.
  CHECK(stats.conditionsVisited == 3); CHECK(stats.conditionsFolded == 0);
  CHECK(stats.solverQueries == 4); CHECK(stats.budgetSkipped == 1);
  CHECK(stats.unknownQueries == 0); CHECK(lifter.ir() == before); verify(lifter);
}

TEST_CASE("LLVM predicate scheduling preserves zero and low original budgets",
          "[llvm][predicate-fold][predicate-scheduling]") {
  for (auto options : {PredicateFoldOptions{0, 2, 128}, PredicateFoldOptions{200, 0, 128},
                       PredicateFoldOptions{200, 2, 1}}) {
    LLVMLifter lifter{Architecture::X86_64}; scheduledPredicates(lifter, 2, false);
    const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding(options);
    CHECK(stats.conditionsFolded == 0); CHECK(stats.solverQueries == 0);
    CHECK(stats.budgetSkipped == 3); CHECK(lifter.ir() == before); verify(lifter);
  }
}

TEST_CASE("LLVM predicate scheduling never samples unsupported poison or call expressions",
          "[llvm][predicate-fold][predicate-scheduling]") {
  for (unsigned kind = 0; kind < 4; ++kind) {
    INFO("kind " << kind);
    LLVMLifter lifter{Architecture::X86_64}; auto &context = lifter.module().getContext();
    auto *word = llvm::Type::getInt32Ty(context);
    auto *function = llvm::Function::Create(llvm::FunctionType::get(word, {word}, false),
        llvm::GlobalValue::ExternalLinkage, "unsupported_schedule", lifter.module());
    llvm::IRBuilder<> b{llvm::BasicBlock::Create(context, "entry", function)};
    llvm::Value *value = nullptr;
    if (kind == 0) value = llvm::PoisonValue::get(word);
    if (kind == 1) value = llvm::UndefValue::get(word);
    if (kind == 2) value = b.Insert(llvm::BinaryOperator::CreateAdd(function->getArg(0), llvm::PoisonValue::get(word)));
    if (kind == 3) {
      auto call = lifter.module().getOrInsertFunction("unknown_scheduling_input",
          llvm::FunctionType::get(word, {word}, false));
      value = b.CreateCall(call, {function->getArg(0)});
    }
    auto *condition = b.Insert(new llvm::ICmpInst(llvm::CmpInst::ICMP_EQ, value, b.getInt32(0)));
    b.CreateRet(b.CreateSelect(condition, b.getInt32(7), b.getInt32(98)));
    verify(lifter); const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding({200, 2, 128});
    CHECK(stats.conditionsFolded == 0); CHECK(stats.solverQueries == 0);
    CHECK(stats.unsupportedConditions == 1); CHECK(lifter.ir() == before); verify(lifter);
  }
}
#endif
