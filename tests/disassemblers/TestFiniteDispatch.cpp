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

namespace {
// Near CALL writes [SP-pointerSize, SP); the table deliberately sits below it.
std::vector<std::uint8_t> leafFixture(Architecture arch,
    std::initializer_list<std::uint8_t> helper, bool returnSlot=false) {
  std::vector<std::uint8_t> code(0xb0,0xcc);
  auto put=[&](unsigned at,std::initializer_list<std::uint8_t> data) {
    std::copy(data.begin(),data.end(),code.begin()+at);
  };
  if(arch==Architecture::X86_64) {
    put(0,{0x48,0xc7,0x44,0x24,0xe0,0x40,0x10,0,0,
           0x48,0xc7,0x44,0x24,0xe8,0x50,0x10,0,0,
           0x89,0xf9,0x83,0xe1,1,0xe8,0x64,0,0,0,0xff,0x64,0xcc,0xe0});
    if(returnSlot) {code[4]=0xf8;code[13]=0xf0;code[31]=0xf8;put(18,{0xb9,0,0,0,0});}
  } else {
    put(0,{0xc7,0x44,0x24,0xf0,0x40,0x10,0,0,
           0xc7,0x44,0x24,0xf4,0x50,0x10,0,0,
           0x89,0xf9,0x83,0xe1,1,0xe8,0x66,0,0,0,0xff,0x64,0x8c,0xf0});
    if(returnSlot) {code[3]=0xfc;code[11]=0xf8;code[29]=0xfc;put(16,{0xb9,0,0,0,0});}
  }
  put(0x40,{0xb8,7,0,0,0,0x89,3,0xc3});
  put(0x50,{0xb8,9,0,0,0,0x89,3,0xc3});
  put(0x80,helper);
  return code;
}
}
TEST_CASE("Proven leaf calls retain disjoint stack tables without ABI assumptions","[leaf-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})
    for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      auto code=leafFixture(arch,{0x89,0xf8,0x31,0xf0,0x83,0xc0,7,0xc3});
      Configuration config;config.disassemblyStrategy=strategy;
      Disassembler dis{arch,config};Graph g{arch,config};dis.disassemble(code,0x1000,g);
      const auto site=arch==Architecture::X86_64?0x101c:0x101a;
      REQUIRE(g.get(site));REQUIRE(g.get(site)->branches().empty());
      PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
      REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));
      auto report=p.run(g);REQUIRE(report.ok);REQUIRE(report.changed);
      CHECK(g.get(site)->branches()==std::set<std::uint64_t>{0x1040,0x1050});
      CHECK(report.runs[0].result.status==Status::Success);
      CHECK(report.runs[0].result.counters.at("proven_calls")>=1);
      auto again=p.run(g);REQUIRE(again.ok);CHECK_FALSE(again.changed);
    }
}
TEST_CASE("Leaf call proofs reject unknown writes unknown flow and unsupported returns","[leaf-calls][finite-dispatch]") {
  const std::vector<std::vector<std::uint8_t>> helpers{
    {0x89,3,0xc3}, // arbitrary memory write
    {0x83,0xc4,8,0xc3}, // changed SP
    {0xc2,8,0}, // callee stack cleanup
    {0xff,0xe0}, // unresolved tail jump
    {0xe8,0x7b,0,0,0,0xc3}, // call outside the decoded graph
    {0x0f,0x0b}, // trap
    {0xc5,0xf8,0x77,0xc3} // unlifted vzeroupper
  };
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(const auto &helper:helpers) {
    auto code=leafFixture(arch,{});std::copy(helper.begin(),helper.end(),code.begin()+0x80);code[0xa0]=0xc3;
    Disassembler dis{arch,{}};Graph g{arch,{}};dis.disassemble(code,0x1000,g);
    const auto site=arch==Architecture::X86_64?0x101c:0x101a;
    const auto count=g.size();
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));
    auto report=p.run(g);REQUIRE(report.ok);CHECK_FALSE(report.changed);
    CHECK(report.runs[0].result.status==Status::Incomplete);
    CHECK(g.get(site)->branches().empty());CHECK(g.size()==count);
    CHECK(report.runs[0].result.counters.at("rejected_calls")>=1);
  }
}
TEST_CASE("Leaf calls invalidate their machine return slot and every written register byte","[leaf-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(bool returnSlot:{false,true}) {
    auto code=leafFixture(arch,returnSlot?std::initializer_list<std::uint8_t>{0xc3}:
        std::initializer_list<std::uint8_t>{0xb1,0xff,0xc3},returnSlot);
    Disassembler dis{arch,{}};Graph g{arch,{}};dis.disassemble(code,0x1000,g);
    const auto site=arch==Architecture::X86_64?0x101c:0x101a;
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));
    auto report=p.run(g);REQUIRE(report.ok);CHECK_FALSE(report.changed);
    CHECK(g.get(site)->branches().empty());
    CHECK(report.runs[0].result.counters.at("proven_calls")>=1);
  }
}
TEST_CASE("Finite leaf-call option is explicit and validates before mutation","[leaf-calls][finite-dispatch]") {
  auto code=leafFixture(Architecture::X86_64,{0xc3});
  Disassembler dis{Architecture::X86_64,{}};Graph g{Architecture::X86_64,{}};dis.disassemble(code,0x1000,g);
  PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
  REQUIRE(p.add("recover"));auto old=p.run(g);REQUIRE(old.ok);CHECK_FALSE(old.changed);
  CHECK(g.get(0x101c)->branches().empty());
  PassPipeline invalid;REQUIRE(invalid.registerGraph("recover",FiniteDispatchPass{code,0x1000},false,{},FiniteDispatchPass::validator()));
  CHECK_FALSE(invalid.add("recover",{{"prove_leaf_calls",std::int64_t{2}}}));
  REQUIRE(invalid.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));
  auto good=invalid.run(g);REQUIRE(good.ok);CHECK(good.changed);
}
TEST_CASE("Leaf-call analysis obeys global budgets and keeps every branch effect","[leaf-calls][finite-values]") {
  for(bool writes:{false,true}) {
    auto code=leafFixture(Architecture::X86_64,{0x83,0xff,0,0x74,1,0xc3,0x89,3,0xc3});
    if(!writes) {code[0x86]=0xb0;code[0x87]=7;} // AL write, no memory effect
    Disassembler dis{Architecture::X86_64,{}};Graph g{Architecture::X86_64,{}};dis.disassemble(code,0x1000,g);
    for(auto budget:{1,1000000}) {
      PassPipeline p;REQUIRE(p.add("finite-values",{{"prove_leaf_calls",std::int64_t{1}},
          {"max_operations",std::int64_t{budget}}}));
      REQUIRE(p.registerGraph("inspect",[&](auto &context,const Options &) {
        auto *facts=context.analyses.template get<analysis::FiniteValueResult>("finite-values");
        REQUIRE(facts);CHECK(facts->complete==(budget>1));
        REQUIRE(facts->jumpTargets.contains(0x1000));
        if(budget==1 || writes)CHECK(facts->jumpTargets.at(0x1000).kind==analysis::ValueSetKind::Unknown);
        else CHECK(facts->jumpTargets.at(0x1000).values==std::vector<std::uint64_t>{0x1040,0x1050});
        return Result{};
      },true,{"finite-values"}));
      REQUIRE(p.add("inspect"));auto r=p.run(g);REQUIRE(r.ok);CHECK_FALSE(r.changed);
    }
  }
}
TEST_CASE("Stack-call proofs preserve saved registers and disjoint caller tables","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})
    for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL})
      for(bool registers:{false,true}) {
        auto code=leafFixture(arch,{});const bool wide=arch==Architecture::X86_64;
        const std::vector<std::uint8_t> helper=wide?
          std::vector<std::uint8_t>{0x55,0x48,0x89,0xe5,0x53,0x48,0x83,0xec,0x20,
            0x89,0xf8,0x83,0xc0,7,0x89,0x45,0xe8,0x48,0x83,0xc4,0x20,0x5b,0xc9,0xc3}:
          std::vector<std::uint8_t>{0x55,0x89,0xe5,0x53,0x83,0xec,0x20,
            0x89,0xf8,0x83,0xc0,7,0x89,0x45,0xe8,0x83,0xc4,0x20,0x5b,0xc9,0xc3};
        std::copy(helper.begin(),helper.end(),code.begin()+0x80);
        std::uint64_t site=wide?0x101c:0x101a;
        if(registers) {
          const std::vector<std::uint8_t> root{0xbd,0x40,0x10,0,0,0xba,0x50,0x10,0,0,
            0x83,0xff,0,0x0f,0x45,0xea,0xe8,0x6b,0,0,0,0xff,0xe5};
          std::fill(code.begin(),code.begin()+0x40,0xcc);
          std::copy(root.begin(),root.end(),code.begin());site=0x1015;
        } else if(wide) {code[4]=0x10;code[13]=0x18;code[31]=0x10;}
        else {code[3]=8;code[11]=12;code[29]=8;}
        Configuration config;config.disassemblyStrategy=strategy;
        Disassembler dis{arch,config};Graph graph{arch,config};dis.disassemble(code,0x1000,graph);
        PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
        REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));
        auto result=p.run(graph);REQUIRE(result.ok);REQUIRE(result.changed);
        CHECK(graph.get(site)->branches()==std::set<std::uint64_t>{0x1040,0x1050});
        CHECK(result.runs[0].result.counters.at("proven_calls")>=1);
        CHECK(result.runs[0].result.status==Status::Success);
      }
}
TEST_CASE("Stack-call writes invalidate overlapping tables and corrupted saved registers","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(bool saved:{false,true}) {
    const bool wide=arch==Architecture::X86_64;
    auto code=leafFixture(arch,{});
    std::vector<std::uint8_t> helper=wide?std::vector<std::uint8_t>{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x20}:
        std::vector<std::uint8_t>{0x55,0x89,0xe5,0x83,0xec,0x20};
    helper.reserve(32);
    if(saved)helper.insert(helper.end(),{0x89,0x45,0}); // corrupt saved BP, keep return slot intact
    else helper.insert(helper.end(),{0x89,0x45,std::uint8_t(wide?0xf0:0xf8)}); // caller table
    helper.insert(helper.end(),{0xc9,0xc3});std::copy(helper.begin(),helper.end(),code.begin()+0x80);
    std::uint64_t site=wide?0x101c:0x101a;
    if(saved) {
      const std::vector<std::uint8_t> root{0xbd,0x40,0x10,0,0,0xba,0x50,0x10,0,0,
        0x83,0xff,0,0x0f,0x45,0xea,0xe8,0x6b,0,0,0,0xff,0xe5};
      std::fill(code.begin(),code.begin()+0x40,0xcc);std::copy(root.begin(),root.end(),code.begin());site=0x1015;
    }
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK_FALSE(result.changed);CHECK(graph.get(site)->branches().empty());
    CHECK(result.runs[0].result.counters.at("proven_calls")>=1);
  }
}
TEST_CASE("Stack-call proofs reject altered return bytes and disagreeing return stacks","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(unsigned kind=0;kind<4;++kind) {
    auto code=leafFixture(arch,{});std::vector<std::uint8_t> helper;
    if(kind==0)helper={0xc6,0x44,0x24,std::uint8_t(arch==Architecture::X86_64?7:3),0,0xc3};
    if(kind==1)helper={0x55,0x83,0xff,0,0x74,1,0x5d,0xc3}; // one exit leaks a push
    if(kind==2)helper={0xc7,0x44,0x24,0xff,0,0,0,0,0xc3}; // partial overlap across offset zero
    if(kind==3)helper={0x55,0x89,3,0x5d,0xc3}; // arbitrary write may corrupt the return
    std::copy(helper.begin(),helper.end(),code.begin()+0x80);
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK_FALSE(result.changed);CHECK(result.runs[0].result.counters.at("rejected_calls")>=1);
  }
}
TEST_CASE("Stack-call proofs never assume an ABI preserves unsaved registers","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(bool save:{false,true}) {
    auto code=leafFixture(arch,{});
    const std::vector<std::uint8_t> root{0xbb,0x40,0x10,0,0,0xba,0x50,0x10,0,0,
      0x83,0xff,0,0x0f,0x45,0xda,0xe8,0x6b,0,0,0,0xff,0xe3};
    std::fill(code.begin(),code.begin()+0x40,0xcc);std::copy(root.begin(),root.end(),code.begin());
    std::vector<std::uint8_t> helper;helper.reserve(16);
    if(save)helper.push_back(0x53);
    helper.insert(helper.end(),{0x89,0xfb}); // EBX is callee-saved in common ABIs; no guesses.
    if(save)helper.push_back(0x5b);
    helper.push_back(0xc3);std::copy(helper.begin(),helper.end(),code.begin()+0x80);
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK(result.changed==save);CHECK(result.runs[0].result.counters.at("proven_calls")>=1);
    if(save)CHECK(graph.get(0x1015)->branches()==std::set<std::uint64_t>{0x1040,0x1050});
    else CHECK(graph.get(0x1015)->branches().empty());
  }
}
TEST_CASE("Stack-call effects compose through internal calls without losing saved state","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(bool framed:{false,true}) {
    const bool wide=arch==Architecture::X86_64;auto code=leafFixture(arch,{});code.resize(0xe0,0xcc);
    const std::vector<std::uint8_t> root{0xbd,0x40,0x10,0,0,0xba,0x50,0x10,0,0,
      0x83,0xff,0,0x0f,0x45,0xea,0xe8,0x6b,0,0,0,0xff,0xe5};
    std::fill(code.begin(),code.begin()+0x40,0xcc);std::copy(root.begin(),root.end(),code.begin());
    std::vector<std::uint8_t> parent=wide?
      std::vector<std::uint8_t>{0x55,0x48,0x89,0xe5,0x48,0x83,0xec,0x10,0xe8,0x33,0,0,0,0xc9,0xc3}:
      std::vector<std::uint8_t>{0x55,0x89,0xe5,0x83,0xec,0x10,0xe8,0x35,0,0,0,0xc9,0xc3};
    if(!framed)parent={0xe8,0x3b,0,0,0,0xc3};
    const std::vector<std::uint8_t> child{0x89,0xf8,0x83,0xc0,7,0xc3};
    std::copy(parent.begin(),parent.end(),code.begin()+0x80);std::copy(child.begin(),child.end(),code.begin()+0xc0);
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK(result.changed);CHECK(graph.get(0x1015)->branches()==std::set<std::uint64_t>{0x1040,0x1050});
  }
}
TEST_CASE("Nested effects protect the outer return and bound recursive call trees","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(unsigned kind=0;kind<3;++kind) {
    const bool wide=arch==Architecture::X86_64;auto code=leafFixture(arch,{});code.resize(0xe0,0xcc);
    std::vector<std::uint8_t> parent{0xe8,0x3b,0,0,0,0xc3};
    if(kind==1)parent={0xe8,0xfb,0xff,0xff,0xff,0xc3}; // recursion, no finite summary
    std::copy(parent.begin(),parent.end(),code.begin()+0x80);
    const std::vector<std::uint8_t> child=kind==2?std::vector<std::uint8_t>{0x89,3,0xc3}:
      std::vector<std::uint8_t>{0xc7,0x44,0x24,std::uint8_t(wide?8:4),0,0,0,0,0xc3};
    std::copy(child.begin(),child.end(),code.begin()+0xc0);
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK_FALSE(result.changed);CHECK(graph.get(wide?0x101c:0x101a)->branches().empty());
    CHECK(result.runs[0].result.counters.at("rejected_calls")>=1);
  }
}
TEST_CASE("Call effects propagate proved constant results and caller stack writes","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(bool memory:{false,true}) {
    const bool wide=arch==Architecture::X86_64;auto code=leafFixture(arch,{});
    std::fill(code.begin(),code.begin()+0x40,0xcc);
    std::vector<std::uint8_t> root,helper;
    if(memory) {
      root=wide?std::vector<std::uint8_t>{0x48,0x83,0xec,0x10,0xe8,0x77,0,0,0,0xff,0x24,0x24}:
        std::vector<std::uint8_t>{0x83,0xec,0x10,0xe8,0x78,0,0,0,0xff,0x24,0x24};
      helper=wide?std::vector<std::uint8_t>{0xb8,0x40,0x10,0,0,0x48,0x89,0x44,0x24,8,0xc3}:
        std::vector<std::uint8_t>{0xc7,0x44,0x24,4,0x40,0x10,0,0,0xc3};
    } else {
      root={0xe8,0x7b,0,0,0,0xff,0xe0};helper={0xb8,0x40,0x10,0,0,0xc3};
    }
    std::copy(root.begin(),root.end(),code.begin());std::copy(helper.begin(),helper.end(),code.begin()+0x80);
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK(result.changed);CHECK(graph.get(memory?(wide?0x1009:0x1008):0x1005)->branches()==std::set<std::uint64_t>{0x1040});
  }
}
TEST_CASE("Call result proofs check every return and do not invent initial memory","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(bool memory:{false,true}) {
    const bool wide=arch==Architecture::X86_64;auto code=leafFixture(arch,{});
    std::fill(code.begin(),code.begin()+0x40,0xcc);
    std::vector<std::uint8_t> root,helper;
    if(memory) {
      root=wide?std::vector<std::uint8_t>{0x48,0x83,0xec,0x10,0xe8,0x77,0,0,0,0xff,0x24,0x24}:
        std::vector<std::uint8_t>{0x83,0xec,0x10,0xe8,0x78,0,0,0,0xff,0x24,0x24};
      helper=wide?
        std::vector<std::uint8_t>{0x83,0xff,0,0x74,11,0xb8,0x40,0x10,0,0,0x48,0x89,0x44,0x24,8,0xc3,0xc3}:
        std::vector<std::uint8_t>{0x83,0xff,0,0x74,9,0xc7,0x44,0x24,4,0x40,0x10,0,0,0xc3,0xc3};
    } else {
      root={0xe8,0x7b,0,0,0,0xff,0xe0};
      helper={0x83,0xff,0,0x74,6,0xb8,0x40,0x10,0,0,0xc3,0xb8,0x50,0x10,0,0,0xc3};
    }
    std::copy(root.begin(),root.end(),code.begin());std::copy(helper.begin(),helper.end(),code.begin()+0x80);
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK_FALSE(result.changed);CHECK(graph.get(memory?(wide?0x1009:0x1008):0x1005)->branches().empty());
    CHECK(result.runs[0].result.counters.at("proven_calls")>=1);
  }
}
TEST_CASE("Call-tree depth is bounded without accepting a partial summary","[stack-calls][finite-dispatch]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(unsigned depth:{4,5}) {
    auto code=leafFixture(arch,{});code.resize(0x201,0xcc);
    const std::vector<std::uint8_t> root{0xbd,0x40,0x10,0,0,0xba,0x50,0x10,0,0,
      0x83,0xff,0,0x0f,0x45,0xea,0xe8,0x6b,0,0,0,0xff,0xe5};
    std::fill(code.begin(),code.begin()+0x40,0xcc);std::copy(root.begin(),root.end(),code.begin());
    for(unsigned n=0;n<depth;++n) {
      const auto at=0x80+n*0x20;
      if(n+1==depth)code[at]=0xc3;
      else {const std::vector<std::uint8_t> call{0xe8,0x1b,0,0,0,0xc3};std::copy(call.begin(),call.end(),code.begin()+at);}
    }
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,0x1000,graph);
    PassPipeline p;REQUIRE(p.registerGraph("recover",FiniteDispatchPass{code,0x1000}));
    REQUIRE(p.add("recover",{{"prove_leaf_calls",std::int64_t{1}}}));auto result=p.run(graph);REQUIRE(result.ok);
    CHECK(result.changed==(depth==4));
    if(depth==4)CHECK(graph.get(0x1015)->branches()==std::set<std::uint64_t>{0x1040,0x1050});
    else {CHECK(graph.get(0x1015)->branches().empty());CHECK(result.runs[0].result.counters.at("rejected_calls")>=1);}
  }
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
