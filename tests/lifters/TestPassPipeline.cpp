#include <catch2/catch_test_macros.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Instructions.h>
#include <stdexcept>
using namespace dragon;
using namespace dragon::passes;
using dragon::llvmlifter::LLVMLifter;
namespace {
llvm::Function *fixture(LLVMLifter &l) {
  auto &c=l.module().getContext();auto *t=llvm::Type::getInt32Ty(c);
  auto *f=llvm::Function::Create(llvm::FunctionType::get(t,{t},false),llvm::GlobalValue::ExternalLinkage,"test",l.module());
  llvm::IRBuilder<llvm::NoFolder> b(llvm::BasicBlock::Create(c,"entry",f));
  b.CreateRet(b.CreateAdd(f->getArg(0),llvm::ConstantInt::get(t,0)));return f;
}
}
TEST_CASE("Pass pipeline preserves the historical LLVM sweep", "[llvm][pipeline]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64,Architecture::ARM64}) {
    Configuration config;Disassembler d{arch,config};Graph g{arch,config};
    auto code=arch==Architecture::ARM64?std::vector<std::uint8_t>{0xe0,0,0x80,0x52,0xc0,3,0x5f,0xd6}:
        std::vector<std::uint8_t>{0xb8,7,0,0,0,0xc3};
    d.disassemble(code,0x1000,g);Function f{&g,0x1000};
    LLVMLifter manual{arch},managed{arch};
    REQUIRE(manual.setExecutionMode(dragon::llvmlifter::ExecutionMode::StaticAnalysis));
    REQUIRE(managed.setExecutionMode(dragon::llvmlifter::ExecutionMode::StaticAnalysis));
    REQUIRE(manual.lift(f,g));REQUIRE(managed.lift(f,g));
    manual.runMem2Reg();manual.runInstCombine();manual.runGVN();
    (void)manual.runMBASimplification();(void)manual.runPredicateFolding();manual.runDCE();
    PassPipeline p;for(const auto &name:PassPipeline::defaultLLVM()) REQUIRE(p.add(name));
    auto result=p.run(managed);REQUIRE(result.ok);CHECK(result.runs.size()==6);
    CHECK(manual.ir()==managed.ir());CHECK(result.iterations==1);
  }
}
TEST_CASE("Pass pipeline preflights stages dependencies and parameters", "[llvm][pipeline]") {
  LLVMLifter l{Architecture::X86_64};fixture(l);const auto before=l.ir();PassPipeline p;
  REQUIRE(p.add("instcombine"));REQUIRE(p.add("graph-audit"));
  auto r=p.run(l);CHECK_FALSE(r.ok);CHECK(r.runs.empty());CHECK(l.ir()==before);
  CHECK_FALSE(p.add("unknown"));CHECK_FALSE(p.add("mba",{{"max_queries",true}}));
  CHECK_FALSE(p.add("mba",{{"max_queries",std::int64_t{-1}}}));
  CHECK_FALSE(p.add("mba",{{"misspelled",std::int64_t{1}}}));
  REQUIRE(p.clear());
  REQUIRE(p.registerLLVM("dependent",[](auto &,const auto &){return Result{};},true,{"instcombine"}));
  REQUIRE(p.add("dependent"));r=p.run(l);CHECK_FALSE(r.ok);CHECK(r.runs.empty());CHECK(l.ir()==before);
  CHECK_FALSE(p.registerLLVM("dependent",[](auto &,const auto &){return Result{};}));
  CHECK_FALSE(p.run(l,{0,64,true}).ok);CHECK_FALSE(p.run(l,{65,64,true}).ok);
  CHECK_FALSE(p.run(l,{1,4097,true}).ok);
}
TEST_CASE("Pass pipeline has bounded iteration and actual LLVM change detection", "[llvm][pipeline]") {
  LLVMLifter l{Architecture::X86_64};fixture(l);PassPipeline p;REQUIRE(p.add("instcombine"));
  auto r=p.run(l,{5,64,true});REQUIRE(r.ok);CHECK(r.changed);CHECK(r.converged);
  CHECK_FALSE(r.budgetExhausted);CHECK(r.runs.size()==2);
  LLVMLifter limited{Architecture::X86_64};fixture(limited);
  r=p.run(limited,{5,1,true});REQUIRE(r.ok);CHECK(r.budgetExhausted);CHECK_FALSE(r.converged);CHECK(r.runs.size()==1);
  r=p.run(limited,{5,0,true});REQUIRE(r.ok);CHECK(r.budgetExhausted);CHECK(r.runs.empty());
  PassPipeline empty;r=empty.run(limited);CHECK(r.ok);CHECK(r.converged);CHECK(r.runs.empty());
}
TEST_CASE("Pass pipeline invalidates per-run analyses after mutating passes", "[llvm][pipeline]") {
  LLVMLifter l{Architecture::X86_64};auto *f=fixture(l);PassPipeline p;int observations=0;
  REQUIRE(p.registerLLVM("produce",[](auto &c,const auto &){c.analyses.put("fact",17);return Result{};},true));
  REQUIRE(p.registerLLVM("read",[&](auto &c,const auto &){auto *v=c.analyses.template get<int>("fact");REQUIRE(v);CHECK(*v==17);++observations;return Result{};},true));
  REQUIRE(p.registerLLVM("mutate",[&](auto &,const auto &){
    llvm::cast<llvm::ReturnInst>(f->getEntryBlock().getTerminator())->setOperand(0,f->getArg(0));return Result{};
  }));
  REQUIRE(p.registerLLVM("check",[](auto &c,const auto &){CHECK(c.analyses.template get<int>("fact")==nullptr);CHECK(c.analyses.revision()==1);return Result{};},true));
  for(auto name:{"produce","read","mutate","check"}) REQUIRE(p.add(name));
  auto r=p.run(l);REQUIRE(r.ok);CHECK(observations==1);CHECK(r.runs[2].result.changed);
  REQUIRE(p.clear());REQUIRE(p.add("check"));
  // New runs get a fresh cache and revision, not the prior target's facts.
  PassPipeline second;REQUIRE(second.registerLLVM("fresh",[](auto &c,const auto &){CHECK(c.analyses.revision()==0);CHECK(!c.analyses.template get<int>("fact"));return Result{};},true));
  REQUIRE(second.add("fresh"));CHECK(second.run(l).ok);
}
TEST_CASE("Pass pipeline stops on plugin errors and protects active registration", "[llvm][pipeline]") {
  LLVMLifter l{Architecture::X86_64};fixture(l);PassPipeline p;
  REQUIRE(p.registerLLVM("failure",[&](auto &,const auto &)->Result {
    CHECK_FALSE(p.clear());CHECK_FALSE(p.add("dce"));CHECK_FALSE(p.run(l).ok);
    throw std::runtime_error("deliberate plugin failure");
  }));
  REQUIRE(p.add("failure"));REQUIRE(p.add("dce"));auto r=p.run(l);
  CHECK_FALSE(r.ok);CHECK(r.runs.size()==1);CHECK(r.error.find("deliberate plugin failure")!=std::string::npos);
  REQUIRE(p.clear());REQUIRE(p.add("dce"));CHECK(p.run(l).ok);
}
TEST_CASE("Pass pipeline executes Graph plugins without crossing into LLVM", "[llvm][pipeline]") {
  Configuration config;Disassembler d{Architecture::X86_64,config};Graph g{Architecture::X86_64,config};
  d.disassemble(std::vector<std::uint8_t>{0xb8,7,0,0,0,0xc3},0x1000,g);
  PassPipeline p;
  REQUIRE(p.registerGraph("identity",[](auto &c,const auto &){
    auto *old=c.graph.getIGNILBlock(0x1000);REQUIRE(old);ignil::Block next{0x1000};
    next.append({ignil::Opcode::COPY,ignil::Reg{0,64,"RAX"},{ignil::Reg{0,64,"RAX"}},0x1000});
    for(const auto &op:old->ops()) next.append(op);
    for(auto address:old->unlifted()) next.noteUnlifted(address);
    *old=std::move(next);return Result{Status::Success,true,{},{}};
  }));
  const auto ops=g.getIGNILBlock(0x1000)->size();REQUIRE(p.add("identity"));REQUIRE(p.add("graph-audit"));
  auto r=p.run(g);REQUIRE(r.ok);CHECK(r.changed);CHECK(g.getIGNILBlock(0x1000)->size()==ops+1);
  CHECK(r.runs[1].result.counters.at("unlifted")==0);CHECK(r.runs[1].revision==1);
  REQUIRE(p.clear());REQUIRE(p.add("dce"));r=p.run(g);CHECK_FALSE(r.ok);CHECK(r.runs.empty());
}
TEST_CASE("Pass pipeline serializes typed configuration and unknown results", "[llvm][pipeline]") {
  auto options=parseOptionsJSON("{\"flag\":false,\"count\":0,\"text\":\"x\"}");
  CHECK(std::get<bool>(options.at("flag"))==false);CHECK(std::get<std::int64_t>(options.at("count"))==0);
  CHECK(parseOptionsJSON(optionsJSON(options))==options);
  CHECK_THROWS(parseOptionsJSON("[]"));CHECK_THROWS(parseOptionsJSON("{\"x\":[]}"));
  CHECK_THROWS(parseCountersJSON("{\"x\":true}"));CHECK_THROWS(parseNamesJSON("[0]"));
  LLVMLifter l{Architecture::X86_64};fixture(l);PassPipeline p;
  REQUIRE(p.add("mba",{{"max_queries",std::int64_t{0}}}));auto r=p.run(l);REQUIRE(r.ok);
#ifdef DRAGON_WITH_Z3
  CHECK(r.runs[0].result.status==Status::Success);
#else
  CHECK(r.runs[0].result.status==Status::Unavailable);
#endif
  CHECK(r.runs[0].result.counters.at("solver_queries")==0);
  CHECK(r.toJSON().find("solver_queries")!=std::string::npos);
}

TEST_CASE("Pass pipeline rejects invalid plugin output and false availability", "[llvm][pipeline]") {
  for(unsigned kind=0;kind<3;++kind) {
    LLVMLifter l{Architecture::X86_64};auto *f=fixture(l);PassPipeline p;
    REQUIRE(p.registerLLVM("bad",[&](auto &,const auto &) {
      auto *ret=llvm::cast<llvm::ReturnInst>(f->getEntryBlock().getTerminator());
      if(kind==2) ret->eraseFromParent();else ret->setOperand(0,f->getArg(0));
      return Result{kind==1?Status::Unavailable:Status::Success,false,{},{}};
    },kind==0));
    REQUIRE(p.add("bad"));REQUIRE(p.add("dce"));auto r=p.run(l);
    CHECK_FALSE(r.ok);CHECK(r.changed);CHECK(r.runs.size()==1);
    CHECK(r.runs[0].result.status==Status::Error);
    if(kind==2) CHECK(r.error.find("invalid output IR")!=std::string::npos);
  }
}
