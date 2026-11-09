#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Graph.hpp>

using namespace dragon;
using namespace dragon::analysis;

TEST_CASE("Finite terminal STORE accounts for split memory facts without a query",
          "[finite-values][finite-write-footprints][finite-store-budget]") {
  using O=ignil::Opcode;
  for(auto architecture:{Architecture::Unknown,Architecture::X86_64,Architecture::ARM64})
    for(bool temporary:{false,true}) {
      CAPTURE(architecture,temporary);
      Graph graph{architecture,{}};ignil::Block block{0x1000};
      block.append({O::STORE,{}, {ignil::Const{0x4000,64},
          ignil::Const{0x1122334455667788ULL,64}},0x1000});
      if(temporary)block.append({O::COPY,ignil::Temp{0,64},
          {ignil::Reg{0x3000,64,{}}},0x1000});
      block.append({O::STORE,{}, {ignil::Const{0x4004,64},
          ignil::Reg{0x3010,8,{}}},0x1000});
      graph.addIGNILBlock(std::move(block));graph.addBlock(0x1000);

      // The last STORE retains seven scalar byte facts. There is no later
      // destination, successor or report query that can hide a missed check.
      FiniteValueLimits limited;limited.maxFacts=4;
      const auto stopped=finiteValues(graph,{0x1000},{},limited);
      CHECK_FALSE(stopped.complete);CHECK_FALSE(stopped.message.empty());
      CHECK(stopped.values.empty());CHECK(stopped.jumpTargets.empty());
      limited.maxFacts=16;
      const auto complete=finiteValues(graph,{0x1000},{},limited);
      CHECK(complete.complete);CHECK(complete.message.empty());
    }
}
