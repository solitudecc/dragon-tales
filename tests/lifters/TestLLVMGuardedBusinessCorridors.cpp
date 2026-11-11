#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
namespace {
using namespace dragon::passes;
struct Fixture {
  dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
  llvm::Module &module=lifter.module();
  llvm::Function *function;
  std::string mode;
  Fixture(std::string kind="ordinary"):mode(std::move(kind)) {
    module.setDataLayout("e-p:64:64");auto &c=module.getContext();
    auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);
    function=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,i64,llvm::Type::getInt1Ty(c)},false),llvm::GlobalValue::ExternalLinkage,"view",module);
    auto block=[&](const char *s){return llvm::BasicBlock::Create(c,s,function);};
    auto *entry=block("entry"),*seed=block("seed"),*unknown=block("unknown"),*hub=block("hub"),*check1=block("check1"),*load1=block("load1"),*check2=block("check2"),*load2=block("load2"),*check3=block("check3"),*relay=block("relay"),*terminal=block("terminal"),*yes=block("yes"),*bad=block("bad");
    llvm::IRBuilder<llvm::NoFolder> ir(entry);
    auto *guest=ir.CreateLoad(ptr,function->getArg(1),"guest");guest->setAlignment(llvm::Align(1));
    auto store=[&](llvm::Value *v,llvm::Value *offset){auto *p=ir.CreateGEP(ir.getInt8Ty(),guest,offset);auto *s=ir.CreateStore(v,p);s->setAlignment(llvm::Align(1));return s;};
    ir.CreateCondBr(function->getArg(3),seed,unknown);
    ir.SetInsertPoint(seed);store(ir.getInt64(1),function->getArg(2));
    if(mode=="unrelated")store(ir.getInt64(2),ir.getInt64(64));
    if(mode=="duplicate-seed")ir.CreateCondBr(function->getArg(3),hub,hub);else ir.CreateBr(hub);
    ir.SetInsertPoint(unknown);store(function->getArg(2),function->getArg(2));ir.CreateBr(hub);
    ir.SetInsertPoint(hub);auto *frame=ir.CreatePHI(i64,mode=="loop"?3:2,"frame");frame->addIncoming(function->getArg(2),seed);frame->addIncoming(function->getArg(2),unknown);
    if(mode=="duplicate-seed")frame->addIncoming(function->getArg(2),seed);
    if(mode=="overwrite-state")store(ir.CreateAdd(ir.getInt64(2),ir.getInt64(0)),frame);
    if(mode=="overwrite-table")store(ir.getInt64(9),ir.getInt64(24));
    ir.CreateBr(check1);
    ir.SetInsertPoint(check1);ir.CreateCondBr(function->getArg(3),load1,bad);
    ir.SetInsertPoint(load1);auto *p=ir.CreateGEP(ir.getInt8Ty(),guest,frame);auto *state=ir.CreateLoad(i64,p,"state");state->setAlignment(llvm::Align(1));
    if(mode=="atomic")state->setAtomic(llvm::AtomicOrdering::Monotonic);
    auto *second=ir.CreateAdd(frame,ir.getInt64(8));store(state,second);
    if(mode=="loop"){ir.CreateCondBr(function->getArg(3),check2,hub);frame->addIncoming(frame,load1);}else ir.CreateBr(check2);
    ir.SetInsertPoint(check2);
    if(mode=="call")ir.CreateCall(module.getOrInsertFunction("unknown.effect",llvm::FunctionType::get(ir.getVoidTy(),{ptr},false)),{guest});
    ir.CreateCondBr(function->getArg(3),load2,bad);
    ir.SetInsertPoint(load2);auto *p2=ir.CreateGEP(ir.getInt8Ty(),guest,second);auto *again=ir.CreateLoad(i64,p2,"again");again->setAlignment(llvm::Align(1));
    auto *index=ir.CreateAdd(ir.CreateShl(again,ir.getInt64(3)),ir.getInt64(16),"table.index");ir.CreateBr(check3);
    ir.SetInsertPoint(check3);ir.CreateCondBr(function->getArg(3),relay,bad);
    ir.SetInsertPoint(relay);ir.CreateBr(terminal);
    ir.SetInsertPoint(terminal);auto *address=ir.CreateGEP(ir.getInt8Ty(),guest,index);auto *actual=ir.CreateLoad(i64,address,"actual");actual->setAlignment(llvm::Align(1));
    auto *sw=ir.CreateSwitch(actual,bad);sw->addCase(ir.getInt64(7),yes);sw->addCase(ir.getInt64(9),yes);
    ir.SetInsertPoint(yes);auto *out=ir.CreatePHI(i64,2);out->addIncoming(actual,terminal);out->addIncoming(actual,terminal);ir.CreateRet(out);
    ir.SetInsertPoint(bad);ir.CreateRet(ir.getInt64(-1));
    const std::string text="{\"view_symbol\":\"view\",\"memory\":[{\"offset\":\"0x18\",\"bytes\":\"07000000000000000900000000000000\"}]}";
    module.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(c,llvm::MDString::get(c,text)));
    REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));
  }
  std::string text(){std::string s;llvm::raw_string_ostream os(s);module.print(os,nullptr);return s;}
  Result run(bool enabled=true,Options extra={}) {
    PassPipeline pipeline;extra["business_corridors"]=std::int64_t(enabled);
    // The exact fixture also executes on the previous library. If that API
    // does not know the new option, run its unmodified default controller and
    // let the actual missing-corridor assertion fail; not a semantic mismatch.
    const bool accepted=pipeline.add("guarded-transitions",extra);
    INFO("business option accepted = "<<accepted);
    if(!accepted){extra.erase("business_corridors");REQUIRE(pipeline.add("guarded-transitions",extra));}
    auto report=pipeline.run(lifter);REQUIRE(report.runs.size()==1);
    REQUIRE_FALSE(llvm::verifyModule(module,&llvm::errs()));return report.runs.front().result;
  }
};
void checkGuard(Fixture &f,std::uint64_t hint,unsigned expected=1) {
  unsigned found=0;
  for(auto &b:*f.function) {
    auto *sw=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator());
    if(!sw || !sw->getMetadata("dragon.guarded-transitions.generated"))continue;
    ++found;CHECK(sw->getNumCases()==2);auto *actual=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());REQUIRE(actual);CHECK(actual->isSimple());
    auto *br=llvm::dyn_cast<llvm::BranchInst>(actual->getParent()->getTerminator());REQUIRE(br);REQUIRE(br->isConditional());CHECK(br->getSuccessor(1)==&b);
    auto *cmp=llvm::dyn_cast<llvm::ICmpInst>(br->getCondition());REQUIRE(cmp);CHECK(cmp->getPredicate()==llvm::CmpInst::ICMP_EQ);CHECK(cmp->getOperand(0)==actual);
    CHECK(llvm::cast<llvm::ConstantInt>(cmp->getOperand(1))->getZExtValue()==hint);
    auto it=sw->findCaseValue(llvm::cast<llvm::ConstantInt>(cmp->getOperand(1)));CHECK(it->getCaseSuccessor()==br->getSuccessor(0));
  }
  CHECK(found==expected);
}
}
TEST_CASE("Business corridors copy ordered state moves and retain same read fallback","[llvm][guarded-business]") {
  for(const char *mode:{"ordinary","loop","duplicate-seed"}) {
    Fixture f(mode);const auto before=f.text();auto disabled=f.run(false);CHECK_FALSE(disabled.changed);CHECK(f.text()==before);
    auto r=f.run();REQUIRE(r.changed);const unsigned expected=std::string(mode)=="duplicate-seed"?2:1;
    CHECK(r.counters.at("rewritten_edges")==expected);CHECK(r.counters.at("cloned_blocks")==9*expected);checkGuard(f,7,expected);
    const auto stable=f.text();auto again=f.run();CHECK_FALSE(again.changed);CHECK(f.text()==stable);
  }
}
TEST_CASE("Business corridor hints never substitute unrelated or overwritten state and table","[llvm][guarded-business]") {
  for(const char *mode:{"unrelated","overwrite-state","overwrite-table"}) {
    Fixture f(mode);auto r=f.run();REQUIRE(r.changed);checkGuard(f,std::string(mode)=="unrelated"?9:7);
    // Every original STORE and every copied STORE remains. The actual table
    // LOAD is checked above, not replaced with a hint, even for a wrong hint.
    unsigned original=0,copies=0;for(auto &b:*f.function)for(auto &i:b)if(llvm::isa<llvm::StoreInst>(i)) {
      if(b.getTerminator()->getMetadata("dragon.guarded-transitions.generated"))++copies;else ++original;
    }
    CHECK(original>=3);CHECK(copies>=1);
  }
}
TEST_CASE("Business corridors stop at effects and keep default behavior unchanged","[llvm][guarded-business]") {
  for(const char *mode:{"call","atomic"}) {Fixture f(mode);const auto before=f.text();auto r=f.run();CHECK_FALSE(r.changed);CHECK(f.text()==before);}
  PassPipeline p;CHECK_FALSE(p.add("guarded-transitions",{{"business_corridors",std::int64_t(2)}}));
}
TEST_CASE("Business corridor budgets reject transaction without changing input","[llvm][guarded-business]") {
  Fixture f;const auto before=f.text();auto r=f.run(true,{{"max_blocks",std::int64_t(1)}});
  CHECK_FALSE(r.changed);CHECK(r.status==Status::Incomplete);CHECK(r.counters.at("budget_exhausted")>0);CHECK(f.text()==before);
}

TEST_CASE("Business corridor bounded batches resume unconsumed original successor slots","[llvm][guarded-business]") {
  Fixture f("duplicate-seed");
  auto first=f.run(true,{{"max_rewrites",std::int64_t(1)}});
  REQUIRE(first.changed);CHECK(first.counters.at("rewritten_edges")==1);CHECK(first.status==Status::Incomplete);checkGuard(f,7,1);
  auto second=f.run(true,{{"max_rewrites",std::int64_t(1)}});
  REQUIRE(second.changed);CHECK(second.counters.at("rewritten_edges")==1);checkGuard(f,7,2);
  const auto stable=f.text();auto third=f.run();CHECK_FALSE(third.changed);CHECK(f.text()==stable);
}

TEST_CASE("Business discovery charges repeated hint instruction scans before transactions","[llvm][guarded-business]") {
  Fixture f;auto *type=f.function->getFunctionType();f.function->eraseFromParent();
  auto &c=f.module.getContext();
  f.function=llvm::Function::Create(type,llvm::GlobalValue::ExternalLinkage,"view",f.module);
  auto *fn=f.function;auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,fn);};
  auto *entry=block("entry"),*constant=block("constant"),*seed=block("long.seed"),*hit=block("hit"),*miss=block("miss");
  llvm::IRBuilder<llvm::NoFolder> ir(entry);
  auto *guest=ir.CreateLoad(ir.getPtrTy(),fn->getArg(1));guest->setAlignment(llvm::Align(1));
  ir.CreateCondBr(fn->getArg(3),constant,seed);
  ir.SetInsertPoint(constant);auto *cp=ir.CreateGEP(ir.getInt8Ty(),guest,ir.getInt64(24));auto *cl=ir.CreateLoad(ir.getInt64Ty(),cp);cl->setAlignment(llvm::Align(1));
  auto *cs=ir.CreateSwitch(cl,miss);cs->addCase(ir.getInt64(7),hit);
  ir.SetInsertPoint(hit);ir.CreateRet(ir.getInt64(7));ir.SetInsertPoint(miss);ir.CreateRet(ir.getInt64(0));
  std::function<llvm::BasicBlock *(unsigned)> tree=[&](unsigned depth) {
    auto *b=block(depth?"split":"dynamic");
    if(depth) {
      auto *left=tree(depth-1),*right=tree(depth-1);ir.SetInsertPoint(b);ir.CreateCondBr(fn->getArg(3),left,right);
    } else {
      ir.SetInsertPoint(b);auto *sp=ir.CreateGEP(ir.getInt8Ty(),guest,fn->getArg(2));auto *state=ir.CreateLoad(ir.getInt64Ty(),sp);state->setAlignment(llvm::Align(1));
      auto *index=ir.CreateAdd(ir.CreateShl(state,ir.getInt64(3)),ir.getInt64(16));
      auto *tp=ir.CreateGEP(ir.getInt8Ty(),guest,index);auto *actual=ir.CreateLoad(ir.getInt64Ty(),tp);actual->setAlignment(llvm::Align(1));
      auto *sw=ir.CreateSwitch(actual,miss);sw->addCase(ir.getInt64(7),hit);
    }
    return b;
  };
  auto *root=tree(8);ir.SetInsertPoint(seed);llvm::Value *v=fn->getArg(2);
  for(unsigned n=0;n<4096;++n)v=ir.CreateAdd(v,ir.getInt64(n));
  ir.CreateBr(root);REQUIRE_FALSE(llvm::verifyModule(f.module,&llvm::errs()));
  const auto before=f.text();auto result=f.run();
  CHECK_FALSE(result.changed);CHECK(result.status==Status::Incomplete);
  CHECK(result.counters.at("rejected")==1);CHECK(result.counters.at("budget_exhausted")>0);
  CHECK(result.counters.at("candidate_edges")==1);CHECK(result.counters.at("unresolved_edges")==1);
  CHECK(result.counters.at("original_switches_retained")==1);
  CHECK(result.message=="business instruction scan budget exhausted");CHECK(f.text()==before);
}
