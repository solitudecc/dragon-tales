#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include <array>
#include <cstring>
using namespace dragon;
using namespace dragon::llvmlifter;

TEST_CASE("LLVM V2 selects a distinct uniform entry ABI", "[llvm][dispatch-v2]") {
  Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
  Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
  dis.disassemble(std::vector<std::uint8_t>{0xe9,0xfb,0x1f,0,0},0x1000,graph);
  Function root{&graph,0x1000};REQUIRE(root.isValid());
  LLVMLifter lifter{Architecture::X86_64};
  CHECK_FALSE(lifter.setExecutionMode(static_cast<ExecutionMode>(99)));
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StatefulV2));
  REQUIRE(lifter.lift(root,graph));
  auto *entry=lifter.module().getFunction("dragon_v2_x86_64_1000");REQUIRE(entry);
  CHECK(entry->arg_size()==4);CHECK(entry->getReturnType()->isVoidTy());
  CHECK_FALSE(lifter.module().getFunction("sub_1000"));
  CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  CHECK(lifter.ir().find("unreachable")==std::string::npos);
  CHECK_FALSE(lifter.setExecutionMode(ExecutionMode::Legacy));
  CHECK_FALSE(lifter.lift(root,graph)); // no LLVM auto-renaming of duplicate entries
  ignil::Block trace{0x2000};trace.append({ignil::Opcode::RET,std::nullopt,{},0x2000});
  CHECK_FALSE(lifter.liftBlock(trace)); // no unapproved V2 trace protocol
}

TEST_CASE("LLVM V2 runtime distinguishes zero target missing model and budget", "[llvm][dispatch-v2]") {
  std::array<std::uint8_t,512> regs{};std::array<std::uint8_t,64> data{};
  GuestStateV2 state{2,static_cast<std::uint64_t>(Architecture::X86_64),regs.size(),regs.data()};
  GuestMemoryV2 memory{data.data(),data.size()};ExecContextV2 context{nullptr,0,2,nullptr};ExecExitV2 exit{};
  executeV2(state,memory,context,exit,0xffff);
  CHECK(exit.reason==DRAGON_EXIT_NEED_TARGET);CHECK(exit.next_pc==0);CHECK(exit.next_pc_valid==1);
  auto model=+[](GuestStateV2 *,GuestMemoryV2 *m,ExecContextV2 *,ExecExitV2 *e) {
    ++m->data[0];*e={DRAGON_EXIT_TRANSFER,DRAGON_TRANSFER_BRANCH,0,0,1,0,0};
  };
  TargetEntryV2 entry{0,model};context={&entry,1,3,nullptr};
  executeV2(state,memory,context,exit,0xffff);
  CHECK(exit.reason==DRAGON_EXIT_BUDGET);CHECK(context.budget==0);CHECK(data[0]==3);
  state.regs_size=8;context.budget=10;
  executeV2(state,memory,context,exit,0xffff);
  CHECK(exit.reason==DRAGON_EXIT_UNSUPPORTED);CHECK(exit.detail==DRAGON_DETAIL_INVALID_STATE);
}

TEST_CASE("LLVM V2 runtime rejects invalid model exits", "[llvm][dispatch-v2]") {
  std::array<std::uint8_t,512> regs{};std::array<std::uint8_t,8> data{};
  GuestStateV2 state{2,2,regs.size(),regs.data()};GuestMemoryV2 memory{data.data(),data.size()};
  auto model=+[](GuestStateV2 *,GuestMemoryV2 *,ExecContextV2 *,ExecExitV2 *e) {
    *e={DRAGON_EXIT_TRANSFER,DRAGON_TRANSFER_BRANCH,0,123,1,0,0};
  };
  TargetEntryV2 entry{0,model};ExecContextV2 context{&entry,1,3,nullptr};ExecExitV2 exit{};
  executeV2(state,memory,context,exit,0xffff);
  CHECK(exit.reason==DRAGON_EXIT_UNSUPPORTED);CHECK(exit.detail==DRAGON_DETAIL_INVALID_EXIT);
  CHECK(exit.next_pc_valid==1);CHECK(exit.next_pc==0);
  CHECK(exit.source_pc==0);CHECK(exit.transfer_kind==DRAGON_TRANSFER_NONE);
}
