#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <array>
#include <cstring>
#include <mutex>
using namespace dragon;
using namespace dragon::llvmlifter;

TEST_CASE("LLVM V2 switch default preserves concrete targets after optimization", "[llvm][dispatch-v2][dispatch-execution]") {
  static std::once_flag init;std::call_once(init,[]{llvm::InitializeNativeTarget();llvm::InitializeNativeTargetAsmPrinter();});
  for (bool optimized:{false,true}) {
    Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
    Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
    const std::vector<std::uint8_t> code{0xff,0xe0,0xc3,0xc3};
    for (auto instruction:dis.disassemble(code,0x1000)) graph.addInstruction(std::move(instruction));
    graph.addBlock(0x1000);graph.addFunction(0x1000);
    ignil::Block dispatch{0x1000};dispatch.append({ignil::Opcode::JMP,std::nullopt,{ignil::Reg{0,64,"RAX"}},0x1000});graph.addIGNILBlock(std::move(dispatch));
    auto *jump=graph.get(0x1000);REQUIRE(jump);jump->addBranch(0x1002);jump->addBranch(0x1003);
    for (auto address:{0x1002u,0x1003u}) {
      if (!graph.get(address)) graph.addInstruction(Instruction(address,1,"ret","",{0xc3},true,false,false,false,false,false));

      graph.addBlock(address);ignil::Block block{address};block.append({ignil::Opcode::RET,std::nullopt,{},address});graph.addIGNILBlock(std::move(block));
    }
    Function root{&graph,0x1000};REQUIRE(root.isValid());REQUIRE(root.blocks().contains(0x1002));REQUIRE(root.blocks().contains(0x1003));LLVMLifter lifter{Architecture::X86_64};
    REQUIRE(lifter.setExecutionMode(ExecutionMode::StatefulV2));REQUIRE(lifter.lift(root,graph));
    if (optimized) {lifter.runMem2Reg();lifter.runInstCombine();lifter.runGVN();lifter.runDCE();}
    auto context=std::make_unique<llvm::LLVMContext>();llvm::SMDiagnostic diagnostic;
    auto module=llvm::parseIR(llvm::MemoryBufferRef(lifter.ir(),"dispatch"),diagnostic,*context);REQUIRE(module);
    auto jitResult=llvm::orc::LLJITBuilder().create();REQUIRE(bool(jitResult));auto jit=std::move(*jitResult);
    auto error=jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module),std::move(context)));REQUIRE_FALSE(bool(error));
    auto symbol=jit->lookup("dragon_v2_x86_64_1000");REQUIRE(bool(symbol));auto function=symbol->toPtr<TargetV2>();
    for (std::uint64_t target:{0x1002u,0x1003u,0x3000u,0u}) {
      INFO("optimized="<<optimized<<" target="<<target);
      std::array<std::uint8_t,512> regs{};std::array<std::uint8_t,0x10000> memory{};
      std::copy(code.begin(),code.end(),memory.begin()+0x1000);
      std::uint64_t sp=0x8000,sentinel=0xffff;std::memcpy(memory.data()+sp,&sentinel,8);std::memcpy(regs.data()+32,&sp,8);
      std::uint64_t start=0x1000;std::memcpy(regs.data(),&target,8);std::memcpy(regs.data()+128,&start,8);
      GuestStateV2 state{2,2,regs.size(),regs.data()};GuestMemoryV2 mem{memory.data(),memory.size()};
      TargetEntryV2 entry{start,function};ExecContextV2 runtime{&entry,1,20,nullptr};ExecExitV2 exit{};
      executeV2(state,mem,runtime,exit,0xffff);
      uc_engine *uc=nullptr;REQUIRE(uc_open(UC_ARCH_X86,UC_MODE_64,&uc)==UC_ERR_OK);
      REQUIRE(uc_mem_map(uc,0,memory.size(),UC_PROT_ALL)==UC_ERR_OK);
      REQUIRE(uc_mem_write(uc,0,memory.data(),memory.size())==UC_ERR_OK);
      REQUIRE(uc_reg_write(uc,UC_X86_REG_RAX,&target)==UC_ERR_OK);
      REQUIRE(uc_reg_write(uc,UC_X86_REG_RSP,&sp)==UC_ERR_OK);
      uc_hook hook{};
      auto stop=+[](uc_engine *u,std::uint64_t pc,std::uint32_t,void *opaque) {auto t=*static_cast<std::uint64_t*>(opaque);if(pc==t && t!=0x1002 && t!=0x1003)uc_emu_stop(u);};
      REQUIRE(uc_hook_add(uc,&hook,UC_HOOK_CODE,reinterpret_cast<void*>(stop),&target,1,0)==UC_ERR_OK);
      REQUIRE(uc_emu_start(uc,start,0xffff,0,10)==UC_ERR_OK);
      std::uint64_t expectedPC=0;REQUIRE(uc_reg_read(uc,UC_X86_REG_RIP,&expectedPC)==UC_ERR_OK);uc_close(uc);
      std::uint64_t actualPC=0;std::memcpy(&actualPC,regs.data()+128,8);
      CHECK(actualPC==expectedPC);CHECK(exit.next_pc==expectedPC);CHECK(exit.next_pc_valid==1);
      CHECK(exit.reason==(target==0x1002 || target==0x1003 ? DRAGON_EXIT_RETURN : DRAGON_EXIT_NEED_TARGET));
      CHECK(exit.source_pc==(target==0x1002 || target==0x1003 ? target : 0x1000));
      CHECK(exit.transfer_kind==(target==0x1002 || target==0x1003 ? DRAGON_TRANSFER_RETURN : DRAGON_TRANSFER_BRANCH));
      CHECK(runtime.budget>0);
    }
  }
}
