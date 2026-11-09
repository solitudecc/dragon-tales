#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include "../../lib/LLVMLiftProvenance.hpp"
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Verifier.h>
#include <array>
#include <algorithm>

using namespace dragon;
using namespace dragon::llvmlifter;
namespace p = dragon::llvmlifter::detail;

namespace {
using Bytes = std::vector<std::uint8_t>;
const std::array architectures{Architecture::X86_32,Architecture::X86_64,Architecture::ARM64};
Graph graphFor(Architecture arch, const Bytes &bytes) {
  Graph graph{arch,{}};Disassembler dis{arch,{}};
  dis.disassemble(bytes,0x1000,graph);return graph;
}
Bytes ret(Architecture arch) { return arch == Architecture::ARM64 ? Bytes{0xc0,3,0x5f,0xd6} : Bytes{0xc3}; }
void configure(LLVMLifter &lifter) {
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
}
std::unique_ptr<p::PreparedRegion> prepare(const Graph &graph, std::span<const std::uint64_t> extra = {}) {
  auto prepared=p::PreparedRegion::prepare(Function{&graph,0x1000},graph,extra,{});
  REQUIRE(prepared);return prepared;
}
void checkExitFields(const p::ExitValues &fields, const llvm::BasicBlock &block) {
  const auto *function=block.getParent();
  std::map<std::int64_t,const llvm::Value *> stores;
  for (const auto &i : block) if (const auto *store=llvm::dyn_cast<llvm::StoreInst>(&i)) {
    std::int64_t offset=0;
    const auto *base=llvm::GetPointerBaseWithConstantOffset(store->getPointerOperand(),offset,
                                                           function->getParent()->getDataLayout());
    if (base == function->getArg(3)) stores[offset]=store->getValueOperand();
  }
  REQUIRE(stores.size() == 7);
  const auto constant=[&](std::size_t offset, std::uint64_t expected) {
    REQUIRE(stores.contains(offset));
    const auto *value=llvm::dyn_cast<llvm::ConstantInt>(stores.at(offset));REQUIRE(value);
    CHECK(value->getZExtValue() == expected);
  };
  constant(offsetof(ExecExitV2,reason),fields.reason);
  constant(offsetof(ExecExitV2,transfer_kind),fields.kind);
  constant(offsetof(ExecExitV2,detail),fields.detail);
  constant(offsetof(ExecExitV2,return_pc),fields.returnPC);
  constant(offsetof(ExecExitV2,next_pc_valid),fields.nextPCValid ? 1 : 0);
  CHECK(stores.at(offsetof(ExecExitV2,source_pc)) == fields.sourcePC);
  CHECK(stores.at(offsetof(ExecExitV2,next_pc)) == fields.nextPC);
}
void checkView(const p::LiftView &view, const p::PreparedRegion &prepared) {
  REQUIRE(view.active());CHECK(view.matches(prepared));
  const auto &function=view.function();
  CHECK_FALSE(llvm::verifyFunction(function));
  CHECK(view.blocks().size() == prepared.blocks().size());
  std::set<const llvm::Instruction *> terminators;
  for (const auto &edge : view.edges()) {
    CHECK(edge.source.instance == prepared.instance());
    REQUIRE(edge.terminator);CHECK(edge.terminator->isTerminator());
    CHECK(terminators.insert(edge.terminator).second);
  }
  for (const auto &exit : view.exits()) {
    REQUIRE(exit.instruction);CHECK(exit.instruction->getFunction() == &function);
    CHECK(terminators.insert(exit.instruction).second);
    if (exit.source.operation) CHECK(prepared.owns(*exit.source.operation));
    if (exit.fields) checkExitFields(*exit.fields,*exit.instruction->getParent());
  }
  CHECK(terminators.size() == function.size());
  for (const auto &bb : function) CHECK(terminators.contains(bb.getTerminator()));
  for (const auto &op : view.operations()) {
    CHECK(prepared.owns(op.origin));REQUIRE(op.begin);REQUIRE(op.end);
    CHECK(op.begin->getParent() == &function);CHECK(op.end->getParent() == &function);
  }
  for (const auto &operand : view.operands()) {
    CHECK(prepared.owns(operand.origin));REQUIRE(operand.value);
    const auto &op=prepared.graph().getIGNILBlock(operand.origin.block)->ops().at(operand.origin.opIndex);
    CHECK(operand.index < op.srcs.size());
  }
}
void checkAccess(const p::GuestAccessSite &site) {
  const auto &check=site.check;
  REQUIRE(check.branch);REQUIRE(check.address);REQUIRE(check.index);REQUIRE(check.memoryBase);
  REQUIRE(check.success);REQUIRE(check.failure);REQUIRE(site.instruction);REQUIRE(site.after());
  CHECK(check.branch->isConditional());
  CHECK(check.branch->getSuccessor(0) == check.success);
  CHECK(check.branch->getSuccessor(1) == check.failure);
  CHECK(site.instruction->getParent() == check.success);
  CHECK(site.after()->getParent() == check.success);
  CHECK(site.instruction->comesBefore(site.after()));
  const llvm::Value *pointer=nullptr;
  if (const auto *load=llvm::dyn_cast<llvm::LoadInst>(site.instruction)) {
    pointer=load->getPointerOperand();CHECK(site.value == load);CHECK(load->getAlign().value() == 1);
  } else {
    const auto *store=llvm::dyn_cast<llvm::StoreInst>(site.instruction);REQUIRE(store);
    pointer=store->getPointerOperand();CHECK(site.value == store->getValueOperand());
    CHECK(store->getAlign().value() == 1);
  }
  const auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);REQUIRE(gep);
  CHECK(gep->getPointerOperand() == check.memoryBase);CHECK(gep->getOperand(1) == check.index);
  CHECK(check.address->getType()->getIntegerBitWidth() == check.addressBits);
  CHECK((site.value->getType()->getIntegerBitWidth()+7)/8 == check.bytes);
}
}

TEST_CASE("Lowering provenance identifies Guest accesses and actual success positions", "[llvm][provenance]") {
  for (auto arch : architectures) {
    CAPTURE(arch);
    const auto bytes=arch == Architecture::X86_32 ? Bytes{0x8b,3,0x89,0x43,4,0xc3} :
        arch == Architecture::X86_64 ? Bytes{0x48,0x8b,3,0x48,0x89,0x43,8,0xc3} :
        Bytes{0x20,0,0x40,0xf9,0x20,4,0,0xf9,0xc0,3,0x5f,0xd6};
    auto graph=graphFor(arch,bytes);auto prepared=prepare(graph);
    LLVMLifter lifter{arch};configure(lifter);
    std::optional<p::LiftView> saved;
    REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
      saved=view;checkView(view,*prepared);
      REQUIRE(view.accesses().size() == (arch == Architecture::ARM64 ? 2 : 3));
      CHECK(view.accesses()[0].role == p::AccessRole::Load);
      CHECK(view.accesses()[1].role == p::AccessRole::Store);
      if (arch != Architecture::ARM64) CHECK(view.accesses()[2].role == p::AccessRole::ReturnTarget);
      for (const auto &site : view.accesses()) {
        checkAccess(site);CHECK(site.ordinal == 0);
        CHECK(site.check.bytes == (arch == Architecture::X86_32 ? 4 : 8));
        CHECK(site.check.addressBits == (arch == Architecture::X86_32 ? 32 : 64));
        const auto failure=std::find_if(view.exits().begin(),view.exits().end(),[&](const auto &e) {
          return e.instruction->getParent() == site.check.failure;
        });
        REQUIRE(failure != view.exits().end());REQUIRE(failure->fields);
        CHECK(failure->fields->reason == DRAGON_EXIT_UNSUPPORTED);
        CHECK(failure->fields->detail == DRAGON_DETAIL_MEMORY);
        CHECK(failure->source.operation == site.origin);
      }
      std::size_t allMemoryOps=0;
      for (const auto &bb : view.function()) for (const auto &i : bb)
        allMemoryOps+=llvm::isa<llvm::LoadInst>(i) || llvm::isa<llvm::StoreInst>(i);
      CHECK(allMemoryOps > view.accesses().size()); // snapshots/allocas/Exit are excluded
    }));
    REQUIRE(saved);CHECK_FALSE(saved->active());
    CHECK_THROWS_AS(saved->accesses(),std::logic_error);
    lifter.runMem2Reg();lifter.runInstCombine();
    CHECK_THROWS_AS(saved->function(),std::logic_error);
  }
}

TEST_CASE("Lowering origins retain duplicate constants and same PC vector events", "[llvm][provenance]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64}) {
    auto graph=graphFor(arch,{0xf3,0x0f,0x6f,3,0xf3,0x0f,0x7f,0x43,0x10,0xc3});
    auto prepared=prepare(graph);LLVMLifter lifter{arch};configure(lifter);
    REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
      checkView(view,*prepared);REQUIRE(view.accesses().size() == 5);
      const auto sites=view.accesses();
      CHECK(sites[0].origin.machinePC == sites[1].origin.machinePC);
      CHECK_FALSE(sites[0].origin == sites[1].origin);
      CHECK(sites[2].origin.machinePC == sites[3].origin.machinePC);
      CHECK_FALSE(sites[2].origin == sites[3].origin);
      CHECK(sites[0].check.branch != sites[1].check.branch);
      for (const auto &site : sites) checkAccess(site);
    }));
  }
  auto graph=graphFor(Architecture::X86_64,{0xc3});
  ignil::Block block{0x1000};
  block.append({ignil::Opcode::COPY,ignil::Temp{1,64},{ignil::Const{7,64}},0x1000});
  block.append({ignil::Opcode::COPY,ignil::Temp{2,64},{ignil::Const{7,64}},0x1000});
  block.append({ignil::Opcode::RET,std::nullopt,{},0x1000});
  *graph.getIGNILBlock(0x1000)=block;
  auto prepared=prepare(graph), other=prepare(graph);
  LLVMLifter lifter{Architecture::X86_64};configure(lifter);
  REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
    checkView(view,*prepared);CHECK_FALSE(view.matches(*other));
    REQUIRE(view.operations().size() == 3);
    CHECK(view.operations()[0].result == view.operations()[1].result);
    CHECK_FALSE(view.operations()[0].origin == view.operations()[1].origin);
    CHECK(view.operations()[0].origin.opIndex == 0);CHECK(view.operations()[1].origin.opIndex == 1);
  }));
}

TEST_CASE("Lowering captures call request normal propagation and implicit stack accesses", "[llvm][provenance]") {
  for (auto arch : architectures) for (bool word : {false,true}) {
    if (word && arch != Architecture::X86_32) continue;
    const auto bytes=arch == Architecture::ARM64 ? Bytes{0xc0,3,0x3f,0xd6,0xc0,3,0x5f,0xd6} :
        word ? Bytes{0x66,0xff,0xd4,0x66,0xc2,4,0} : Bytes{0xff,0xd4,0xc2,8,0};
    auto graph=graphFor(arch,bytes);auto prepared=prepare(graph);
    LLVMLifter lifter{arch};configure(lifter);
    REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
      checkView(view,*prepared);REQUIRE(view.calls().size() == 1);
      const auto &call=view.calls()[0];
      REQUIRE(call.instruction);CHECK(call.instruction->getFunction() == &view.function());
      CHECK(call.origin.machinePC == 0x1000);
      CHECK(call.returnPC == (arch == Architecture::ARM64 ? 0x1004 : word ? 0x1003 : 0x1002));
      CHECK(call.request.reason == DRAGON_EXIT_TRANSFER);CHECK(call.request.kind == DRAGON_TRANSFER_CALL);
      CHECK(call.request.returnPC == call.returnPC);
      checkExitFields(call.request,*call.instruction->getParent());
      CHECK(call.validateExit->getSuccessor(0) == call.invalid);
      CHECK(call.validateExit->getSuccessor(1) == call.validateReturn->getParent());
      CHECK(call.validateReturn->getSuccessor(0) == call.normal);
      CHECK(call.validateReturn->getSuccessor(1) == call.propagate);
      CHECK(std::count_if(view.exits().begin(),view.exits().end(),[&](const auto &exit) {
        return !exit.fields && exit.instruction->getParent() == call.propagate;
      }) == 1);
      CHECK(std::count_if(view.exits().begin(),view.exits().end(),[&](const auto &exit) {
        return exit.fields && exit.fields->detail == DRAGON_DETAIL_INVALID_EXIT &&
               exit.instruction->getParent() == call.invalid;
      }) == 1);
      CHECK(std::none_of(view.exits().begin(),view.exits().end(),[](const auto &exit) {
        return exit.fields && exit.fields->reason == DRAGON_EXIT_TRANSFER && exit.fields->kind == DRAGON_TRANSFER_CALL;
      }));
      REQUIRE(view.accesses().size() == (arch == Architecture::ARM64 ? 0 : 2));
      if (arch != Architecture::ARM64) {
        const auto &push=view.accesses()[0], &pop=view.accesses()[1];
        CHECK(push.role == p::AccessRole::CallReturnSlot);CHECK(pop.role == p::AccessRole::ReturnTarget);
        CHECK(push.check.bytes == (word ? 2 : arch == Architecture::X86_32 ? 4 : 8));
        CHECK(pop.check.bytes == push.check.bytes);checkAccess(push);checkAccess(pop);
        const auto *target=llvm::dyn_cast<llvm::Instruction>(call.target);REQUIRE(target);
        CHECK(target->getParent() == push.check.branch->getParent());
        CHECK(target->comesBefore(push.check.branch)); // CALL SP snapshots before SP changes
        const auto *stored=llvm::dyn_cast<llvm::ConstantInt>(push.value);REQUIRE(stored);
        CHECK(stored->getZExtValue() == call.returnPC);
      }
    }));
  }
}

TEST_CASE("Lowering maps actual coverage switches and retained default exits", "[llvm][provenance]") {
  for (auto arch : architectures) {
    auto graph=graphFor(arch,arch == Architecture::ARM64 ? Bytes{0,0,0x1f,0xd6} : Bytes{0xff,0xe0});
    Disassembler dis{arch,{}};dis.disassemble(ret(arch),0x2000,graph);
    const std::uint64_t extra=0x2000;auto prepared=prepare(graph,std::span{&extra,1});
    REQUIRE(graph.get(0x1000)->branches().empty());
    LLVMLifter lifter{arch};configure(lifter);
    REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
      checkView(view,*prepared);
      const auto edge=std::find_if(view.edges().begin(),view.edges().end(),[](const auto &edge) {
        return edge.role == p::EdgeRole::Jump;
      });
      REQUIRE(edge != view.edges().end());
      const auto *sw=llvm::dyn_cast<llvm::SwitchInst>(edge->terminator);REQUIRE(sw);
      std::set<std::uint64_t> cases;
      for (const auto &c : sw->cases()) cases.insert(c.getCaseValue()->getZExtValue());
      CHECK((cases == std::set<std::uint64_t>{0x1000,0x2000}));
      CHECK(sw->getSuccessor(0) == sw->getDefaultDest());
      const auto exit=std::find_if(view.exits().begin(),view.exits().end(),[&](const auto &exit) {
        return exit.instruction->getParent() == sw->getDefaultDest();
      });
      REQUIRE(exit != view.exits().end());REQUIRE(exit->fields);
      CHECK(exit->fields->reason == DRAGON_EXIT_TRANSFER);
      CHECK(exit->fields->kind == DRAGON_TRANSFER_BRANCH);
      const auto entry=std::find_if(view.edges().begin(),view.edges().end(),[](const auto &e) {return e.role == p::EdgeRole::Entry;});
      REQUIRE(entry != view.edges().end());CHECK_FALSE(entry->source.operation);
      CHECK(entry->source.kind == p::SourceKind::Entry);
      CHECK(llvm::cast<llvm::SwitchInst>(entry->terminator)->getNumCases() == 1);
    }));
  }
}

TEST_CASE("Lowering boundaries and unsupported prefixes never invent operation identities", "[llvm][provenance]") {
  for (bool prefix : {false,true}) {
    auto graph=graphFor(Architecture::X86_64,prefix ? Bytes{0xb8,7,0,0,0,0x0f,0x0b} : Bytes{0x0f,0x0b});
    auto prepared=prepare(graph);LLVMLifter lifter{Architecture::X86_64};configure(lifter);
    REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
      checkView(view,*prepared);CHECK(view.accesses().empty());
      const auto marker=std::find_if(view.exits().begin(),view.exits().end(),[](const auto &exit) {
        return exit.source.kind == p::SourceKind::UnsupportedInstruction;
      });
      REQUIRE(marker != view.exits().end());CHECK_FALSE(marker->source.operation);
      CHECK(marker->source.machinePC == (prefix ? 0x1005 : 0x1000));
      REQUIRE(marker->fields);CHECK(marker->fields->detail == DRAGON_DETAIL_INSTRUCTION);
      CHECK(view.operations().empty() == !prefix);
      for (std::size_t i=0;i<view.operations().size();++i) CHECK(view.operations()[i].origin.opIndex == i);
    }));
  }
  auto graph=graphFor(Architecture::X86_64,{0x90}); // stream ends without a machine transfer
  auto prepared=prepare(graph);LLVMLifter lifter{Architecture::X86_64};configure(lifter);
  REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
    CHECK(std::any_of(view.exits().begin(),view.exits().end(),[](const auto &e) {
      return e.source.kind == p::SourceKind::StreamEnd && !e.source.operation && e.source.machinePC == 0x1001;
    }));
  }));
}

TEST_CASE("Lowering local control uses original op indices and captures every real edge", "[llvm][provenance]") {
  auto graph=graphFor(Architecture::X86_64,{0xc3});
  ignil::Block block{0x1000};
  block.append({ignil::Opcode::COPY,ignil::Temp{7,1},{ignil::Const{1,1}},0x1000});
  block.append({ignil::Opcode::LABEL,std::nullopt,{},0x1000,1});
  block.append({ignil::Opcode::LOCALCJMP,std::nullopt,{ignil::Temp{7,1}},0x1000,2});
  block.append({ignil::Opcode::LOCALJMP,std::nullopt,{},0x1000,1});
  block.append({ignil::Opcode::LABEL,std::nullopt,{},0x1000,2});
  block.append({ignil::Opcode::RET,std::nullopt,{},0x1000});
  *graph.getIGNILBlock(0x1000)=block;
  auto prepared=prepare(graph);LLVMLifter lifter{Architecture::X86_64};configure(lifter);
  REQUIRE(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
    checkView(view,*prepared);REQUIRE(view.operations().size() == 6);
    for (std::size_t i=0;i<6;++i) CHECK(view.operations()[i].origin.opIndex == i);
    CHECK(std::count_if(view.edges().begin(),view.edges().end(),[](const auto &e) {
      return e.role == p::EdgeRole::LocalJump || e.role == p::EdgeRole::LocalConditional;
    }) == 2);
  }));
}

TEST_CASE("Lowering failed bodies budgets and exceptional consumers expire without callbacks", "[llvm][provenance]") {
  auto graph=graphFor(Architecture::X86_64,{0xc3});auto prepared=prepare(graph);
  for (auto limit : {0u,1u,1048577u}) {
    LLVMLifter lifter{Architecture::X86_64};configure(lifter);unsigned calls=0;
    CHECK_FALSE(p::RegionLowering::run(lifter,*prepared,[&](const auto &) {++calls;},p::CaptureLimits{limit}));
    CHECK(calls == 0);CHECK(lifter.module().empty());
  }
  std::optional<p::LiftView> saved;
  {
    LLVMLifter lifter{Architecture::X86_64};configure(lifter);
    CHECK_THROWS_AS(p::RegionLowering::run(lifter,*prepared,[&](const p::LiftView &view) {
      saved=view;throw std::runtime_error("consumer control");
    }),std::runtime_error);
    REQUIRE(saved);CHECK_FALSE(saved->active());CHECK_THROWS_AS(saved->edges(),std::logic_error);
    CHECK_FALSE(lifter.module().empty()); // completed body; failed consumer target is discarded below
  }
  CHECK_THROWS_AS(saved->function(),std::logic_error);
  ignil::Block bad{0x1000};
  bad.append({ignil::Opcode::COPY,ignil::Temp{1,64},{ignil::Temp{999,64}},0x1000});
  bad.append({ignil::Opcode::RET,std::nullopt,{},0x1000});
  *graph.getIGNILBlock(0x1000)=bad;
  auto broken=prepare(graph);LLVMLifter lifter{Architecture::X86_64};configure(lifter);unsigned calls=0;
  CHECK_FALSE(p::RegionLowering::run(lifter,*broken,[&](const auto &) {++calls;}));
  CHECK(calls == 0);CHECK(lifter.module().empty());
  CHECK(p::RegionLowering::run(lifter,*prepared)); // complete, fresh transaction after failure
  CHECK_THROWS_AS(saved->calls(),std::logic_error);
}

TEST_CASE("Lowering read-only callbacks preserve static region IR and entry restrictions", "[llvm][provenance]") {
  for (auto arch : architectures) {
    auto graph=graphFor(arch,ret(arch));auto prepared=prepare(graph);
    LLVMLifter observed{arch}, discarded{arch};configure(observed);configure(discarded);
    unsigned calls=0;
    REQUIRE(p::RegionLowering::run(observed,*prepared,[&](const auto &view) {++calls;checkView(view,*prepared);}));
    REQUIRE(discarded.liftStaticRegion(Function{&graph,0x1000},graph,{}));
    CHECK(calls == 1);CHECK(observed.ir() == discarded.ir());
    for (auto mode : {ExecutionMode::Legacy,ExecutionMode::StatefulV2}) {
      LLVMLifter invalid{arch};REQUIRE(invalid.setExecutionMode(mode));
      CHECK_FALSE(p::RegionLowering::run(invalid,*prepared));CHECK(invalid.module().empty());
    }
    LLVMLifter resumable{arch};REQUIRE(resumable.setExecutionMode(ExecutionMode::StaticAnalysis));
    CHECK_FALSE(p::RegionLowering::run(resumable,*prepared));CHECK(resumable.module().empty());
  }
}
