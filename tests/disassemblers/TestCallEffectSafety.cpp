#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <algorithm>

using namespace dragon;
namespace {
std::vector<std::uint8_t> safetyFixture() {
  std::vector<std::uint8_t> code(0xe0,0xcc);
  const std::vector<std::uint8_t> caller{
    0xbd,0x40,0x10,0,0,0xba,0x50,0x10,0,0,0x83,0xff,0,
    0x0f,0x45,0xea,0xe8,0x6b,0,0,0,0xff,0xe5};
  std::copy(caller.begin(),caller.end(),code.begin());
  code[0x40]=code[0x50]=0xc3;
  return code;
}
analysis::FiniteValueResult safetyAnalyze(Architecture arch,
    const std::vector<std::uint8_t> &code,bool queryIP=false) {
  Graph graph{arch,{}};Disassembler dis{arch,{}};dis.disassemble(code,0x1000,graph);
  analysis::FiniteValueLimits limits;limits.proveLeafCalls=true;
  const analysis::FiniteValueQuery query{0x1000,x86::RIP};
  return analysis::finiteValues(graph,{0x1000},
      queryIP?std::span{&query,1}:std::span<const analysis::FiniteValueQuery>{},limits);
}
}

TEST_CASE("Call proofs reject explicit segment overrides before forwarding saved stack values",
          "[call-effect-safety][stack-calls]") {
  // With FS_BASE=0x1000, push BP writes [SP-word] while FS:[SP] reads a
  // different location. The callee returns normally but does not preserve BP.
  // Generic emitAddress currently omits segment bases without noteUnlifted;
  // this proof domain must reject those instructions rather than trust that IR.
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})
    for(auto segment:{0x26,0x2e,0x36,0x3e,0x64,0x65}) {
      CAPTURE(arch,segment);
      auto code=safetyFixture();std::vector<std::uint8_t> helper;
      if(arch==Architecture::X86_64)
        helper={0x55,std::uint8_t(segment),0x48,0x8b,0x2c,0x24,0x48,0x83,0xc4,8,0xc3};
      else helper={0x55,std::uint8_t(segment),0x8b,0x2c,0x24,0x83,0xc4,4,0xc3};
      std::copy(helper.begin(),helper.end(),code.begin()+0x80);
      auto result=safetyAnalyze(arch,code);REQUIRE(result.complete);
      CHECK(result.provenCalls==0);CHECK(result.rejectedCalls==1);
      REQUIRE(result.jumpTargets.contains(0x1000));
      CHECK(result.jumpTargets.at(0x1000).kind==analysis::ValueSetKind::Unknown);
    }
}

TEST_CASE("Call prefix guards do not mistake immediate or displacement bytes for prefixes",
          "[call-effect-safety][stack-calls]") {
  const std::vector<std::vector<std::uint8_t>> helpers{
    {0xb8,0x64,0x65,0,0,0xc3}, // mov eax,0x6564
    {0xc7,0x44,0x24,0x64,0x78,0x56,0x34,0x12,0xc3},
    {0xc7,0x44,0x24,0x65,0x78,0x56,0x34,0x12,0xc3}
  };
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(const auto &helper:helpers) {
    CAPTURE(arch,unsigned(helper[0]));auto code=safetyFixture();
    std::copy(helper.begin(),helper.end(),code.begin()+0x80);
    auto result=safetyAnalyze(arch,code);REQUIRE(result.complete);
    CHECK(result.provenCalls==1);CHECK(result.rejectedCalls==0);
    REQUIRE(result.jumpTargets.contains(0x1000));
    CHECK(result.jumpTargets.at(0x1000).values==std::vector<std::uint64_t>{0x1040,0x1050});
  }
}

TEST_CASE("Call result constants never resurrect a callee RIP after a machine return",
          "[call-effect-safety][stack-calls]") {
  for(bool nested:{false,true}) {
    CAPTURE(nested);auto code=safetyFixture();
    // The lifter's RIP setup is scratch state for this memory instruction,
    // not the architectural PC after returning to the caller.
    const std::vector<std::uint8_t> readRIP{0x48,0x8b,0x05,0,0,0,0,0xc3};
    if(nested) {
      const std::vector<std::uint8_t> parent{0xe8,0x3b,0,0,0,0xc3};
      std::copy(parent.begin(),parent.end(),code.begin()+0x80);
      std::copy(readRIP.begin(),readRIP.end(),code.begin()+0xc0);
    } else std::copy(readRIP.begin(),readRIP.end(),code.begin()+0x80);
    auto result=safetyAnalyze(Architecture::X86_64,code,true);REQUIRE(result.complete);
    CHECK(result.provenCalls==(nested?2:1));CHECK(result.rejectedCalls==0);
    REQUIRE(result.values.size()==1);
    CHECK(result.values[0].kind==analysis::ValueSetKind::Unknown);
    CHECK(result.values[0].values.empty());
  }
}
