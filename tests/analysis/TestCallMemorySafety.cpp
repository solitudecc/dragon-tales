#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <array>
#include <cstdint>
#include <vector>
#ifdef DRAGON_WITH_UNICORN
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#endif

using namespace dragon;
using namespace dragon::analysis;

namespace {
std::vector<std::uint8_t> numericRestore(bool wide,bool nested) {
  constexpr std::uint64_t base=0x1000,helper=0x1080,child=0x10a0,initialSP=0xc800;
  std::vector<std::uint8_t> code(0xc0,0xcc);std::size_t pc=0;
  auto byte=[&](std::uint8_t value){code.at(pc++)=value;};
  auto word=[&](std::uint32_t value){for(unsigned n=0;n<4;++n)byte(value>>(8*n));};
  auto call=[&](std::uint64_t target){byte(0xe8);word(std::uint32_t(target-(base+pc+4)));};
  call(helper);byte(0xc3);pc=helper-base;
  if(nested) {call(child);byte(0xc3);pc=child-base;}
  byte(0xbc);word(initialSP-(wide?8:4)*(nested?2:1));byte(0xc3);
  return code;
}
}

TEST_CASE("A numeric callee SP restoration preserves the caller coordinate after machine RET",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(bool nested:{false,true})
    for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(wide,nested,strategy);
      const auto arch=wide?Architecture::X86_64:Architecture::X86_32;
      Configuration config;config.disassemblyStrategy=strategy;
      Graph graph{arch,config};Disassembler disassembler{arch,config};
      Disassembler::AnalysisHints hints;hints.entryPoints={0x1000};
      disassembler.disassemble(numericRestore(wide,nested),0x1000,graph,hints);
      const auto sp=wide?x86::RSP:x86::ESP;
      std::vector<FiniteValueQuery> queries;
      for(auto at:graph.validBlockAddresses())if(const auto *ir=graph.getIGNILBlock(at))
        for(const auto &op:ir->ops())if(op.address==0x1005 && op.opcode==ignil::Opcode::RET)
          queries.push_back({at,sp});
      REQUIRE(queries.size()==1);
      const std::array initial{FiniteEntryState{0x1000,{{sp.offset,sp.width,0xc800}}, {}}};
      FiniteValueLimits limits;limits.proveLeafCalls=true;
      const auto result=finiteValuesWithInitialState(graph,{0x1000},queries,initial,limits);
      REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
      REQUIRE(result.analysis.values.size()==1);
      REQUIRE(result.analysis.values.front().kind==ValueSetKind::Finite);
      CHECK(result.analysis.values.front().values==std::vector<std::uint64_t>{0xc800});
    }
}

#ifdef DRAGON_WITH_UNICORN
TEST_CASE("Unicorn confirms caller SP after numeric direct and nested callee restoration",
          "[call-memory-effects][oracle]") {
  for(bool wide:{false,true})for(bool nested:{false,true}) {
    CAPTURE(wide,nested);const auto code=numericRestore(wide,nested);
    uc_engine *uc=nullptr;
    REQUIRE(uc_open(UC_ARCH_X86,wide?UC_MODE_64:UC_MODE_32,&uc)==UC_ERR_OK);
    struct Close {uc_engine *uc;~Close(){uc_close(uc);}} close{uc};
    REQUIRE(uc_mem_map(uc,0x1000,0x1000,UC_PROT_ALL)==UC_ERR_OK);
    REQUIRE(uc_mem_map(uc,0xc000,0x1000,UC_PROT_ALL)==UC_ERR_OK);
    REQUIRE(uc_mem_write(uc,0x1000,code.data(),code.size())==UC_ERR_OK);
    std::uint64_t sp=0xc800,pc=0;
    REQUIRE(uc_reg_write(uc,wide?UC_X86_REG_RSP:UC_X86_REG_ESP,&sp)==UC_ERR_OK);
    REQUIRE(uc_emu_start(uc,0x1000,0x1005,1000000,1000)==UC_ERR_OK);
    REQUIRE(uc_reg_read(uc,wide?UC_X86_REG_RSP:UC_X86_REG_ESP,&sp)==UC_ERR_OK);
    REQUIRE(uc_reg_read(uc,wide?UC_X86_REG_RIP:UC_X86_REG_EIP,&pc)==UC_ERR_OK);
    CHECK(sp==0xc800);CHECK(pc==0x1005);
    std::vector<std::uint8_t> after(code.size());
    REQUIRE(uc_mem_read(uc,0x1000,after.data(),after.size())==UC_ERR_OK);CHECK(after==code);
  }
}
#endif

TEST_CASE("Universal stack effects reject actual decoded code writes at a numeric call site",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,
                                               DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    CAPTURE(wide,strategy);
    constexpr std::uint64_t base=0x1000,helper=0x1200,slot=0x8000;
    std::vector<std::uint8_t> code(0x240,0xcc);
    std::size_t pc=0;
    auto byte=[&](std::uint8_t value){code.at(pc++)=value;};
    auto word=[&](std::uint32_t value){for(unsigned n=0;n<4;++n)byte(value>>(8*n));};
    byte(0xb8);word(0x55); // MOV EAX,0x55
    byte(0xbc);word(slot+(wide?8:4)); // A numeric SP; CALL push is outside code.
    byte(0xe8);word(std::uint32_t(helper-(base+pc+4)));
    const auto resultPC=base+pc;byte(0xc3);
    pc=helper-base;
    // MOV dword [SP-0x6e00],7; RET. The universal summary knows the
    // relative write, but only this call site locates it in the helper code.
    for(auto value:std::array<std::uint8_t,3>{0xc7,0x84,0x24})byte(value);
    word(std::uint32_t(helper-slot));word(7);byte(0xc3);
    const auto arch=wide?Architecture::X86_64:Architecture::X86_32;
    Configuration config;config.disassemblyStrategy=strategy;
    Graph graph{arch,config};Disassembler disassembler{arch,config};
    Disassembler::AnalysisHints hints;hints.entryPoints={base};
    disassembler.disassemble(code,base,graph,hints);
    std::vector<FiniteValueQuery> queries;
    for(auto at:graph.validBlockAddresses())if(const auto *ir=graph.getIGNILBlock(at))
      for(const auto &op:ir->ops())if(op.address==resultPC && op.opcode==ignil::Opcode::RET)
        queries.push_back({at,x86::EAX});
    REQUIRE(queries.size()==1);
    FiniteValueLimits limits;limits.proveLeafCalls=true;
    const auto result=finiteValues(graph,{base},queries,limits);
    REQUIRE(result.complete);REQUIRE(result.values.size()==1);
    CHECK(result.provenCalls==0);CHECK(result.rejectedCalls==1);
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("A call proof cache shares the fact budget with live block temporaries",
          "[finite-values][call-memory-effects][call-memory-budget]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64}) {
    CAPTURE(arch);
    // The extra COPYs define dead SSA values only; architectural state and the
    // real E8/C3 pair are unchanged. A terminal CALL still retains those temps.
    const std::array<std::uint8_t,7> code{0xe8,1,0,0,0,0xc3,0xc3};
    Configuration config;config.disassemblyStrategy=DisassemblyStrategy::RECURSIVE_TRAVERSAL;
    Graph graph{arch,config};Disassembler disassembler{arch,config};
    Disassembler::AnalysisHints hints;hints.entryPoints={0x1000};
    disassembler.disassemble(code,0x1000,graph,hints);
    ignil::Block block{0x1000};
    for(unsigned n=0;n<23;++n)
      block.append({ignil::Opcode::COPY,ignil::Temp{n,64},{ignil::Const{n,64}},0x1000});
    block.append({ignil::Opcode::CALL,{}, {ignil::Const{0x1006,64}},0x1000});
    block.append({ignil::Opcode::RET,{}, {},0x1005});
    auto *existing=graph.getIGNILBlock(0x1000);
    REQUIRE(existing!=nullptr);
    *existing=std::move(block);
    REQUIRE(existing->ops().size()==25);
    REQUIRE(existing->ops()[23].opcode==ignil::Opcode::CALL);
    // No query replay: the budget must already include the live temporaries
    // on the initial transfer, even when this terminal block has no successor.
    const std::span<const FiniteValueQuery> queries;
    FiniteValueLimits limits;limits.proveLeafCalls=true;
    const auto accepted=finiteValues(graph,{0x1000},queries,limits);
    REQUIRE(accepted.complete);CHECK(accepted.provenCalls==1);
    limits.maxFacts=24;
    const auto stopped=finiteValues(graph,{0x1000},queries,limits);
    CHECK_FALSE(stopped.complete);CHECK_FALSE(stopped.message.empty());
    CHECK(stopped.values.empty());CHECK(stopped.operations<=limits.maxOperations);
  }
}
