#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
using O=ignil::Opcode;

TEST_CASE("Self XOR snapshots can locate writes in callers and callees without equating unknown inputs",
          "[finite-values][call-memory-effects][call-snapshot-identity]") {
  for(bool wide:{false,true})for(bool inCaller:{false,true})for(bool same:{false,true})
    for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(wide,inCaller,same,strategy);
      constexpr std::uint64_t base=0x1000,helper=0x1080;
      std::vector<std::uint8_t> code(0xc0,0xcc);std::size_t pc=0;
      auto emit=[&](std::initializer_list<std::uint8_t> values){for(auto v:values)code.at(pc++)=v;};
      auto word=[&](std::uint32_t v){for(unsigned n=0;n<4;++n)emit({std::uint8_t(v>>(8*n))});};
      auto prefix=[&]{if(wide)emit({0x48});};
      auto inputs=[&] {
        // Different unknown LOAD events; equality of abstract Unknown bytes
        // must never be used as equality of these two actual register values.
        if(wide) {emit({0x44,0x8b,0x47,0x10,0x44,0x8b,0x4f,0x18});emit({0x45,0x31,std::uint8_t(same?0xc0:0xc8)});}
        else {emit({0x8b,0x4f,0x10,0x8b,0x57,0x18});emit({0x31,std::uint8_t(same?0xc9:0xd1)});}
      };
      prefix();emit({0x83,0xec,0x40,0xc7,0x44,0x24,8});word(1);
      prefix();emit({0x8d,0x7c,0x24,8});
      if(inCaller)inputs();
      emit({0xe8});word(std::uint32_t(helper-(base+pc+4)));
      const auto loadPC=base+pc;emit({0x8b,0x54,0x24,8});
      prefix();emit({0x83,0xc4,0x40,0xc3});
      pc=helper-base;if(!inCaller)inputs();
      if(wide)emit({0x42,0xc7,0x04,0x07});else emit({0xc7,0x04,0x0f});
      word(7);emit({0xc3});
      const auto arch=wide?Architecture::X86_64:Architecture::X86_32;
      Configuration config;config.disassemblyStrategy=strategy;
      Graph graph{arch,config};Disassembler disassembler{arch,config};
      Disassembler::AnalysisHints hints;hints.entryPoints={base};
      disassembler.disassemble(code,base,graph,hints);
      std::vector<FiniteValueQuery> queries;
      for(auto at:graph.validBlockAddresses())if(const auto *ir=graph.getIGNILBlock(at))
        for(const auto &op:ir->ops())if(op.address==loadPC && op.opcode==O::LOAD && op.dst)
          queries.push_back({at,*op.dst});
      REQUIRE(queries.size()==1);
      FiniteValueLimits limits;limits.proveLeafCalls=true;
      const auto result=finiteValues(graph,{base},queries,limits);
      REQUIRE(result.complete);REQUIRE(result.values.size()==1);
      if(same) {
        CHECK(result.values.front().kind==ValueSetKind::Finite);
        if(result.values.front().kind==ValueSetKind::Finite)
          CHECK(result.values.front().values==std::vector<std::uint64_t>{7});
      } else CHECK(result.values.front().kind==ValueSetKind::Unknown);
    }
}

TEST_CASE("Snapshot XOR requires a defined equal-width Temp and leaves legacy mode unchanged",
          "[finite-values][call-memory-effects][call-snapshot-identity]") {
  enum Scenario {Same,Distinct,Missing,MixedWidth};
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(auto scenario:{Same,Distinct,Missing,MixedWidth}) {
    CAPTURE(arch,scenario);Graph graph{arch,{}};ignil::Block block{0x1000};
    const ignil::Temp a{0,64},b{1,64},out{2,64};
    if(scenario!=Missing)block.append({O::UNDEF,a,{},0x1000});
    if(scenario==Distinct)block.append({O::UNDEF,b,{},0x1000});
    const ignil::Value right=scenario==Distinct?ignil::Value{b}:
        scenario==MixedWidth?ignil::Value{ignil::Temp{0,32}}:ignil::Value{a};
    block.append({O::XOR,out,{a,right},0x1000});block.append({O::RET,{}, {},0x1000});
    graph.addIGNILBlock(std::move(block));graph.addBlock(0x1000);
    const std::array queries{FiniteValueQuery{0x1000,out}};
    FiniteValueLimits limits;limits.proveLeafCalls=true;
    const auto result=finiteValues(graph,{0x1000},queries,limits);
    REQUIRE(result.complete);REQUIRE(result.values.size()==1);
    if(scenario==Same) {
      CHECK(result.values.front().kind==ValueSetKind::Finite);
      if(result.values.front().kind==ValueSetKind::Finite)
        CHECK(result.values.front().values==std::vector<std::uint64_t>{0});
    } else CHECK(result.values.front().kind==ValueSetKind::Unknown);
    const auto legacy=finiteValues(graph,{0x1000},queries);
    REQUIRE(legacy.complete);REQUIRE(legacy.values.size()==1);
    CHECK(legacy.values.front().kind==ValueSetKind::Unknown);
  }
}
