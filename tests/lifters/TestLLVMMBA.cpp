#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>

using namespace dragon;
using namespace dragon::llvmlifter;
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
llvm::Value *encoded(Builder &b, llvm::Value *x, llvm::Value *y, unsigned kind = 0) {
  auto *type=x->getType();auto *two=llvm::ConstantInt::get(type,2);
  if (kind==0) return b.CreateAdd(b.CreateXor(x,y),b.CreateMul(b.CreateAnd(x,y),two));
  if (kind==1) return b.CreateSub(b.CreateAdd(x,y),b.CreateMul(b.CreateAnd(x,y),two));
  if (kind==2) return b.CreateSub(b.CreateAdd(x,y),b.CreateAnd(x,y));
  if (kind==3) return b.CreateSub(b.CreateAdd(x,y),b.CreateOr(x,y));
  if (kind==4) return b.CreateSub(b.CreateOr(x,y),b.CreateAnd(x,y));
  return b.CreateAdd(b.CreateAdd(x,b.CreateXor(y,llvm::ConstantInt::getAllOnesValue(type))),llvm::ConstantInt::get(type,1));
}
llvm::Function *fixture(LLVMLifter &lifter, unsigned width=32, unsigned kind=0) {
  auto &c=lifter.module().getContext();auto *type=llvm::IntegerType::get(c,width);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(type,{type,type},false),
      llvm::GlobalValue::ExternalLinkage,"mba",lifter.module());
  Builder b{llvm::BasicBlock::Create(c,"entry",f)};
  b.CreateRet(encoded(b,f->getArg(0),f->getArg(1),kind));return f;
}
void verify(LLVMLifter &lifter) {
  std::string error;llvm::raw_string_ostream out{error};
  const auto invalid=llvm::verifyModule(lifter.module(),&out);INFO(error);REQUIRE_FALSE(invalid);
}
llvm::Value *returned(llvm::Function *f) {
  return llvm::cast<llvm::ReturnInst>(f->getEntryBlock().getTerminator())->getReturnValue();
}
}

TEST_CASE("LLVM MBA availability follows the Z3 build", "[llvm][mba]") {
  LLVMLifter l{Architecture::X86_64};auto *f=fixture(l);const auto before=l.ir();
  const auto s=l.runMBASimplification();
#ifdef DRAGON_WITH_Z3
  CHECK(s.available);CHECK(s.expressionsSimplified==1);CHECK(s.solverQueries==1);
  CHECK(s.unknownQueries==0);CHECK(f->getInstructionCount()==2);
#else
  CHECK_FALSE(s.available);CHECK(s.expressionsSimplified==0);CHECK(s.solverQueries==0);
  CHECK(l.ir()==before);
#endif
  verify(l);
}

#ifdef DRAGON_WITH_Z3
TEST_CASE("LLVM MBA reconstructs modular sums differences and Boolean operations", "[llvm][mba]") {
  // The A64/x64 oracle uses XOR/AND/OR/ADD/SUB/LSL, including all-one/sign-bit inputs.
  const unsigned opcodes[]={llvm::Instruction::Add,llvm::Instruction::Xor,llvm::Instruction::Or,
      llvm::Instruction::And,llvm::Instruction::Xor,llvm::Instruction::Sub};
  for (unsigned width:{1u,8u,16u,32u,64u,128u}) for (unsigned kind=0;kind<6;++kind) {
    INFO("width " << width << " kind " << kind);
    LLVMLifter l{Architecture::X86_64};auto *f=fixture(l,width,kind);
    const auto s=l.runMBASimplification({5000,64,256,3,8000000});
    CHECK(s.expressionsSimplified>=1);CHECK(s.rejectedCandidates==0);CHECK(s.unknownQueries==0);
    verify(l);REQUIRE(f->getInstructionCount()==2);
    auto *op=llvm::dyn_cast<llvm::BinaryOperator>(returned(f));REQUIRE(op);
    if (width!=1) CHECK(op->getOpcode()==opcodes[kind]);
  }
}

TEST_CASE("LLVM MBA synthesizes a three-input truth function", "[llvm][mba]") {
  LLVMLifter l{Architecture::X86_64};auto &c=l.module().getContext();auto *t=llvm::Type::getInt8Ty(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(t,{t,t,t},false),llvm::GlobalValue::ExternalLinkage,"three",l.module());
  Builder b{llvm::BasicBlock::Create(c,"entry",f)};auto *x=f->getArg(0),*y=f->getArg(1),*z=f->getArg(2);
  auto *two=llvm::ConstantInt::get(t,2),*four=llvm::ConstantInt::get(t,4);
  auto *v=b.CreateAdd(b.CreateAdd(x,y),z);
  for (auto *pair:{b.CreateAnd(x,y),b.CreateAnd(x,z),b.CreateAnd(y,z)}) v=b.CreateSub(v,b.CreateMul(pair,two));
  b.CreateRet(b.CreateAdd(v,b.CreateMul(b.CreateAnd(b.CreateAnd(x,y),z),four)));
  const auto s=l.runMBASimplification({5000,64,256,3,8000000});
  CHECK(s.expressionsSimplified>0);CHECK(s.rejectedCandidates==0);CHECK(s.unknownQueries==0);
  CHECK(f->getInstructionCount()==3);verify(l);
}

TEST_CASE("LLVM MBA distinguishes memory events across stores", "[llvm][mba]") {
  LLVMLifter l{Architecture::X86_64};auto &c=l.module().getContext();auto *t=llvm::Type::getInt32Ty(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(t,{llvm::PointerType::getUnqual(c)},false),
      llvm::GlobalValue::ExternalLinkage,"loads",l.module());
  Builder b{llvm::BasicBlock::Create(c,"entry",f)};
  auto *x=b.CreateLoad(t,f->getArg(0));b.CreateStore(llvm::ConstantInt::get(t,1),f->getArg(0));
  b.CreateRet(encoded(b,x,b.CreateLoad(t,f->getArg(0))));
  const auto s=l.runMBASimplification({5000,64,256,3,8000000});
  CHECK(s.expressionsSimplified==1);auto *ret=llvm::dyn_cast<llvm::BinaryOperator>(returned(f));
  REQUIRE(ret);CHECK(ret->getOpcode()==llvm::Instruction::Add);
  auto *one=llvm::dyn_cast<llvm::ConstantInt>(ret->getOperand(1));REQUIRE(one);CHECK(one->isOne());verify(l);
}

TEST_CASE("LLVM MBA preserves shared intermediates and observable writes", "[llvm][mba]") {
  LLVMLifter l{Architecture::X86_64};auto &c=l.module().getContext();auto *t=llvm::Type::getInt32Ty(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(t,{t,t,llvm::PointerType::getUnqual(c)},false),
      llvm::GlobalValue::ExternalLinkage,"shared",l.module());
  Builder b{llvm::BasicBlock::Create(c,"entry",f)};auto *x=f->getArg(0),*y=f->getArg(1),*p=f->getArg(2);
  auto *left=b.CreateXor(x,y),*right=b.CreateMul(b.CreateAnd(x,y),llvm::ConstantInt::get(t,2));
  b.CreateStore(left,p)->setVolatile(true);
  b.CreateStore(right,b.CreateGEP(t,p,llvm::ConstantInt::get(t,1)))->setVolatile(true);
  b.CreateRet(b.CreateAdd(left,right));const auto count=f->getInstructionCount();
  const auto s=l.runMBASimplification({5000,64,256,3,8000000});
  CHECK(s.expressionsSimplified==1);CHECK(f->getInstructionCount()==count);
  auto *op=llvm::dyn_cast<llvm::BinaryOperator>(returned(f));REQUIRE(op);CHECK(op->getOpcode()==llvm::Instruction::Add);
  CHECK(op->getOperand(0)==x);CHECK(op->getOperand(1)==y);verify(l);
}

TEST_CASE("LLVM MBA retains nonlinear masks and unsupported semantics", "[llvm][mba]") {
  for (unsigned kind=0;kind<7;++kind) {
    INFO("kind " << kind);LLVMLifter l{Architecture::X86_64};auto &c=l.module().getContext();auto *t=llvm::Type::getInt32Ty(c);
    auto *f=llvm::Function::Create(llvm::FunctionType::get(t,{t,t},false),llvm::GlobalValue::ExternalLinkage,"unknown",l.module());
    Builder b{llvm::BasicBlock::Create(c,"entry",f)};llvm::Value *v=f->getArg(0);auto *y=f->getArg(1);
    if (kind==0) v=b.CreateSub(b.CreateMul(v,v),v);
    if (kind==1) v=b.CreateAdd(b.CreateAnd(v,llvm::ConstantInt::get(t,128)),b.CreateMul(b.CreateAnd(v,y),llvm::ConstantInt::get(t,2)));
    if (kind==2) v=encoded(b,llvm::UndefValue::get(t),y);
    if (kind==3) v=encoded(b,llvm::PoisonValue::get(t),y);
    if (kind==4) v=encoded(b,b.CreateNSWAdd(v,y),y);
    if (kind==5) v=encoded(b,b.CreateShl(v,y),y);
    if (kind==6) v=encoded(b,b.CreateUDiv(v,y),y);
    b.CreateRet(v);const auto before=l.ir();const auto s=l.runMBASimplification({5000,64,256,3,8000000});
    CHECK(s.expressionsSimplified==0);CHECK(l.ir()==before);verify(l);
  }
}

TEST_CASE("LLVM MBA respects disabled and exhausted proof budgets", "[llvm][mba]") {
  for (const auto options:{MBASimplifyOptions{0,16,128,3},MBASimplifyOptions{100,0,128,3},
      MBASimplifyOptions{100,16,1,3},MBASimplifyOptions{100,16,128,1},MBASimplifyOptions{100,16,128,0}}) {
    LLVMLifter l{Architecture::X86_64};fixture(l);const auto before=l.ir();const auto s=l.runMBASimplification(options);
    CHECK(s.expressionsSimplified==0);CHECK(s.solverQueries==0);CHECK(l.ir()==before);verify(l);
  }
  LLVMLifter l{Architecture::X86_64};fixture(l);fixture(l);const auto s=l.runMBASimplification({5000,1,256,3,8000000});
  CHECK(s.solverQueries==1);CHECK(s.expressionsSimplified==1);CHECK(s.budgetSkipped>0);verify(l);
}

TEST_CASE("LLVM MBA requires a completed proof before changing IR", "[llvm][mba]") {
  LLVMLifter l{Architecture::X86_64};fixture(l,64);const auto before=l.ir();
  const auto s=l.runMBASimplification({1000,16,256,3,1});
  CHECK(s.solverQueries==1);CHECK(s.unknownQueries==1);CHECK(s.expressionsSimplified==0);
  CHECK(l.ir()==before);verify(l);
}
#endif
