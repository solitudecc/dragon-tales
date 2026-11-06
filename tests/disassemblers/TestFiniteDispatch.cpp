#include <catch2/catch_test_macros.hpp>
#include <dragon/passes/FiniteDispatch.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/analysis/FiniteValues.hpp>
using namespace dragon;
using namespace dragon::passes;
namespace {
std::vector<std::uint8_t> fixture(bool unknown=false) {
  std::vector<std::uint8_t> code(0x31,0x90);
  auto put=[&](unsigned at,std::initializer_list<std::uint8_t> data) {
    std::copy(data.begin(),data.end(),code.begin()+at);
  };
  put(0,{0xb8,0x10,0x10,0,0,0xff,0xe0});
  put(0x10,{0xb8,0x20,0x10,0,0,0xeb,0xee});
  if(unknown)put(0x20,{0x89,0xf8,0xeb,0xe1});
  else put(0x20,{0xb8,0x30,0x10,0,0,0xeb,0xde});
  code[0x30]=0xc3;
  return code;
}
}
TEST_CASE("Finite dispatch grows candidates and decodes every reachable target","[finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})
    for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      Configuration config;config.disassemblyStrategy=strategy;
      Graph g{arch,config};Disassembler dis{arch,config};auto code=fixture();
      dis.disassemble(code,0x1000,g);
      REQUIRE(g.get(0x1005));CHECK(g.get(0x1005)->branches().empty());
      const auto originalSize=g.size();
      const std::uint64_t entry=0x1000;
      PassPipeline pipeline;
      REQUIRE(pipeline.registerGraph("finite-dispatch",FiniteDispatchPass{code,0x1000,std::span{&entry,1}},
          false,{},FiniteDispatchPass::validator()));
      REQUIRE(pipeline.add("finite-dispatch"));
      auto r=pipeline.run(g);REQUIRE(r.ok);REQUIRE(r.changed);
      CHECK((g.get(0x1005)->branches()==std::set<std::uint64_t>{0x1010,0x1020,0x1030}));
      CHECK(r.runs[0].result.counters.at("rounds")>=4);
      CHECK(r.runs[0].result.counters.at("new_edges")==3);
      CHECK(r.runs[0].result.counters.at("unresolved_jumps")==0);
      for(auto at:{0x1010,0x1020,0x1030}) {
        REQUIRE(g.get(at));REQUIRE(g.isValidBlock(at));REQUIRE(g.getIGNILBlock(at));
      }
      if(strategy==DisassemblyStrategy::RECURSIVE_TRAVERSAL)CHECK(g.size()>originalSize);
      auto again=pipeline.run(g);REQUIRE(again.ok);CHECK_FALSE(again.changed);
    }
}
TEST_CASE("Finite dispatch discards unknown hypotheses and budget-limited expansion","[finite-dispatch]") {
  for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL})
    for(bool unknown:{false,true}) {
      auto code=fixture(unknown);Configuration config;config.disassemblyStrategy=strategy;
      Graph g{Architecture::X86_64,config};Disassembler dis{Architecture::X86_64,config};
      dis.disassemble(code,0x1000,g);
      const auto originalSize=g.size();
      const auto originalBlocks=g.validBlockAddresses();
      const std::uint64_t entry=0x1000;
      PassPipeline p;REQUIRE(p.registerGraph("resolve",FiniteDispatchPass{code,0x1000,std::span{&entry,1}}));
      REQUIRE(p.add("resolve",unknown?Options{}:Options{{"max_rounds",std::int64_t{1}}}));
      auto r=p.run(g);REQUIRE(r.ok);CHECK_FALSE(r.changed);
      CHECK(r.runs[0].result.status==Status::Incomplete);
      CHECK(r.runs[0].result.counters.at("unresolved_jumps")>=1);
      CHECK(g.size()==originalSize);CHECK(g.validBlockAddresses()==originalBlocks);
      CHECK(g.get(0x1005)->branches().empty());
    }
}
TEST_CASE("Finite analysis pass caches complete results and graph clone is independent","[finite-values][pipeline]") {
  Configuration config;Graph g{Architecture::X86_64,config};Disassembler dis{Architecture::X86_64,config};
  auto code=fixture();dis.disassemble(code,0x1000,g);
  auto cloned=g.clone();
  cloned.get(0x1005)->addBranch(0x1010);
  CHECK(g.get(0x1005)->branches().empty());CHECK(cloned.get(0x1005)->branches().size()==1);
  PassPipeline p;REQUIRE(p.add("finite-values"));
  REQUIRE(p.registerGraph("observe",[](auto &context,const Options &) {
    auto *facts=context.analyses.template get<analysis::FiniteValueResult>("finite-values");
    REQUIRE(facts);CHECK(facts->complete);CHECK(context.analyses.revision()==0);
    return Result{};
  },true,{"finite-values"}));
  REQUIRE(p.add("observe"));auto r=p.run(g);REQUIRE(r.ok);CHECK_FALSE(r.changed);
}
TEST_CASE("Finite dispatch rechecks existing edges after a different dispatcher expands","[finite-dispatch]") {
  std::vector<std::uint8_t> code(0x74,0x90);
  auto put=[&](unsigned at,std::initializer_list<std::uint8_t> data) {
    std::copy(data.begin(),data.end(),code.begin()+at);
  };
  put(0,{0xb9,0x50,0x10,0,0,0x83,0xff,0,0x75,0x26,0xeb,4});
  put(0x10,{0xff,0xe1});
  put(0x30,{0xb8,0x60,0x10,0,0,0xff,0xe0});
  put(0x50,{0xb8,7,0,0,0,0xc3});
  put(0x60,{0xb8,0x70,0x10,0,0,0xeb,0xce});
  put(0x70,{0x89,0xf9,0xeb,0x9c});
  for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    Configuration config;config.disassemblyStrategy=strategy;
    Disassembler dis{Architecture::X86_64,config};Graph g{Architecture::X86_64,config};
    dis.disassemble(code,0x1000,g);
    REQUIRE(g.get(0x1010));REQUIRE(g.get(0x1035));
    REQUIRE(g.get(0x1010)->branches()==std::set<std::uint64_t>{0x1050});
    REQUIRE(g.get(0x1035)->branches().empty());
    const auto oldSize=g.size();
    const std::uint64_t entry=0x1000;
    PassPipeline p;REQUIRE(p.registerGraph("resolve",FiniteDispatchPass{code,0x1000,std::span{&entry,1}}));
    REQUIRE(p.add("resolve"));
    const auto result=p.run(g);REQUIRE(result.ok);
    CHECK_FALSE(result.changed);
    CHECK(result.runs[0].result.status==Status::Incomplete);
    CHECK(result.runs[0].result.counters.at("rounds")>=3);
    CHECK(g.size()==oldSize);
    CHECK(g.get(0x1010)->branches()==std::set<std::uint64_t>{0x1050});
    CHECK(g.get(0x1035)->branches().empty());
  }
}
TEST_CASE("Finite dispatch admits independent inputs at newly decoded direct callees","[finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64}) {
    std::vector<std::uint8_t> code(0x211,0xcc);
    auto put=[&](unsigned at,std::initializer_list<std::uint8_t> data) {
      std::copy(data.begin(),data.end(),code.begin()+at);
    };
    // Neither CMOV alternative is a unique constant for the old slicer.
    put(0,{0x85,0xff,0xb8,0x00,0x11,0,0,0xba,0x20,0x11,0,0,0x0f,0x44,0xc2,0xff,0xe0});
    put(0x100,{0xe8,0x7b,0,0,0}); // call 1180 with arbitrary memory
    if(arch==Architecture::X86_64) {
      put(0x105,{0x48,0xc7,0x04,0x25,0x00,0x30,0,0,0x00,0x12,0,0,0xeb,0x6d});
      put(0x180,{0xff,0x24,0x25,0x00,0x30,0,0});
    } else {
      put(0x105,{0xc7,0x05,0x00,0x30,0,0,0x00,0x12,0,0,0xeb,0x6f});
      put(0x180,{0xff,0x25,0x00,0x30,0,0});
    }
    code[0x120]=code[0x200]=code[0x210]=0xc3;
    Configuration config;config.disassemblyStrategy=DisassemblyStrategy::RECURSIVE_TRAVERSAL;
    Disassembler dis{arch,config};Graph g{arch,config};dis.disassemble(code,0x1000,g);
    REQUIRE(g.get(0x100f));REQUIRE(g.get(0x100f)->branches().empty());
    REQUIRE_FALSE(g.get(0x1180));
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));REQUIRE(p.add("recover"));
    auto result=p.run(g);REQUIRE(result.ok);REQUIRE(result.changed);
    CHECK(g.get(0x100f)->branches()==std::set<std::uint64_t>{0x1100,0x1120});
    REQUIRE(g.get(0x1180));
    CHECK(g.get(0x1180)->branches().empty());
    CHECK(result.runs[0].result.status==Status::Incomplete);
  }
}
