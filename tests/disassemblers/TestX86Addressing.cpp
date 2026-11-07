#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace dragon;
namespace {
std::vector<std::uint8_t> addressBytes(std::string_view text) {
  auto digit=[](char c)->unsigned {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    FAIL("invalid hex fixture");return 0;
  };
  REQUIRE(text.size()%2==0);
  std::vector<std::uint8_t> bytes;
  for(std::size_t i=0;i<text.size();i+=2) bytes.push_back((digit(text[i])<<4)|digit(text[i+1]));
  return bytes;
}
analysis::FiniteValueResult addressValues(Architecture arch,const std::vector<std::uint8_t> &code,
    std::uint64_t pc,DisassemblyStrategy strategy,ignil::Reg queryReg) {
  Configuration config;config.disassemblyStrategy=strategy;
  Graph graph{arch,config};Disassembler dis{arch,config};dis.disassemble(code,pc,graph);
  REQUIRE(graph.getIGNILBlock(pc));REQUIRE(graph.getIGNILBlock(pc)->unlifted().empty());
  const analysis::FiniteValueQuery query{pc,queryReg};
  return analysis::finiteValues(graph,{pc},std::span{&query,1});
}
void constantResult(const analysis::FiniteValueResult &result,std::uint64_t expected) {
  REQUIRE(result.complete);REQUIRE(result.values.size()==1);
  CHECK(result.values[0].kind==analysis::ValueSetKind::Finite);
  CHECK(result.values[0].values==std::vector<std::uint64_t>{expected});
}
}

TEST_CASE("Effective address wrapping keeps distinct Guest stores distinct",
          "[x86-address][finite-values]") {
  // The x64 case is the frozen independent Unicorn counterexample: the
  // addr32 STORE writes address4 while [RDX] remains0x1060. The i386 variant
  // independently distinguishes addr16 address4 from address0x10004.
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})
    for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(arch,strategy);
      auto prefix=addressBytes(arch==Architecture::X86_64 ?
        "48ba0400000001000000c70260100000b8fcffffff67c74008401000008b02ffe0" :
        "bb04000100c70360100000befcffcdab67c74408401000008b03ffe0");
      std::vector<std::uint8_t> code(128,0xcc);std::copy(prefix.begin(),prefix.end(),code.begin());
      code[0x40]=code[0x60]=0xc3;
      const auto result=addressValues(arch,code,0x1000,strategy,arch==Architecture::X86_64?x86::RAX:x86::EAX);
      constantResult(result,0x1060);
      REQUIRE(result.jumpTargets.contains(0x1000));
      CHECK(result.jumpTargets.at(0x1000).kind==analysis::ValueSetKind::Finite);
      CHECK(result.jumpTargets.at(0x1000).values==std::vector<std::uint64_t>{0x1060});
    }
}

TEST_CASE("LEA address size comes from the complete machine instruction",
          "[x86-address][finite-values]") {
  struct Case {Architecture arch;std::uint64_t pc;const char *hex;std::uint64_t result;};
  const Case cases[]={
    {Architecture::X86_64,0x1000,"67488d0425f0ffffffc3",0xfffffff0},
    {Architecture::X86_64,0x1000,"488d0425f0ffffffc3",0xfffffffffffffff0ull},
    {Architecture::X86_64,0x1000,"678d0425f0ffffffc3",0xfffffff0},
    {Architecture::X86_64,0x1000,"6467488d0425f0ffffffc3",0xfffffff0},
    {Architecture::X86_64,0x100002000ull,"67488d05f0ffffffc3",0x1ff8},
    {Architecture::X86_64,0x100002000ull,"488d05f0ffffffc3",0x100001ff7ull},
    {Architecture::X86_64,0xfffffffc,"67488d0508000000c3",0xc},
    {Architecture::X86_32,0x1000,"678d06f0ffc3",0xfff0},
    {Architecture::X86_32,0x1000,"bbfcffcdabbe08003412678d00c3",4},
    // 0x67 in a displacement is an operand byte, not a prefix.
    {Architecture::X86_64,0x1000,"48bb0400000001000000488d4367c3",0x10000006bull},
  };
  for(const auto &test:cases)for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    CAPTURE(test.hex,test.pc,strategy);
    constantResult(addressValues(test.arch,addressBytes(test.hex),test.pc,strategy,
        test.arch==Architecture::X86_64?x86::RAX:x86::EAX),test.result);
  }
}

TEST_CASE("Address size changes preserve segmented memory rejection",
          "[x86-address][x86-segments][ignil]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})
    for(auto segment:{0x64,0x65})for(bool store:{false,true}) {
      auto code=addressBytes(arch==Architecture::X86_64 ? (store?"6748894308":"67488b4308") :
                                                         (store?"67894008":"678b4008"));
      code.insert(code.begin(),segment);Disassembler dis{arch,{}};
      const auto listing=dis.disassemble(code,0x1000);REQUIRE(listing.size()==1);
      const auto block=dis.liftTrace(listing);
      CHECK(std::find(block.unlifted().begin(),block.unlifted().end(),0x1000)!=block.unlifted().end());
      CHECK(std::none_of(block.ops().begin(),block.ops().end(),[](const auto &op) {
        return op.opcode==ignil::Opcode::LOAD || op.opcode==ignil::Opcode::STORE;
      }));
    }
}
