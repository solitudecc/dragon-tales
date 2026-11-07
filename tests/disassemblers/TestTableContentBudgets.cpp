#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/IndirectTargets.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/passes/FiniteDispatch.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
using namespace dragon::passes;

namespace {
constexpr std::uint64_t base=0x1000, table=0x3000;
const std::set<std::uint64_t> targets{0x1040,0x1050};
struct Fixture {
  std::vector<std::uint8_t> code;
  std::uint64_t dispatch;
};
Fixture fixture(bool call=false) {
  std::vector<std::uint8_t> prefix;
  if(call)prefix.insert(prefix.end(),{0x48,0xc7,0xc4,0x00,0xc8,0,0}); // MOV RSP,c800
  // Both entries are initialized by actual Guest stores, outside code bytes.
  for(unsigned i=0;i<2;++i) {
    prefix.insert(prefix.end(),{0x48,0xc7,0x04,0x25});
    for(unsigned b=0;b<4;++b)prefix.push_back(static_cast<std::uint8_t>((table+i*8)>>(8*b)));
    for(unsigned b=0;b<4;++b)prefix.push_back(static_cast<std::uint8_t>((0x1040+i*0x10)>>(8*b)));
  }
  if(call) {
    const auto displacement=std::uint32_t(0x1080-(base+prefix.size()+5));
    prefix.push_back(0xe8);
    for(unsigned b=0;b<4;++b)prefix.push_back(static_cast<std::uint8_t>(displacement>>(8*b)));
  }
  prefix.insert(prefix.end(),{0x83,0x3e,1}); // CMP dword [RSI],1
  const auto displacement=std::uint8_t(0x1040-(base+prefix.size()+2));
  prefix.insert(prefix.end(),{0x77,displacement,0x8b,0x06}); // JA default; MOV EAX,[RSI]
  const auto dispatch=base+prefix.size();
  prefix.insert(prefix.end(),{0xff,0x24,0xc5,0x00,0x30,0,0}); // JMP [RAX*8+3000]
  Fixture result{std::vector<std::uint8_t>(0x90,0xcc),dispatch};
  std::copy(prefix.begin(),prefix.end(),result.code.begin());
  for(auto address:{0x1040,0x1050,0x1080})result.code[address-base]=0xc3;
  return result;
}

Configuration configuration(DisassemblyStrategy strategy) {
  Configuration config;
  config.disassemblyStrategy=strategy;
#ifdef DRAGON_WITH_Z3
  config.useZ3ForPathDiscovery=false;
#endif
  return config;
}

enum class ProviderMode { Unavailable, Failed, Unknown };
class Provider final : public IndirectTargetResolver {
 public:
  explicit Provider(ProviderMode mode):mode(mode) {}
  unsigned calls=0;
  bool prepare(const Graph &,const std::set<std::uint64_t> &) override {return true;}
  std::optional<std::vector<std::uint64_t>> targets(std::uint64_t) const override {return std::nullopt;}
  std::optional<FiniteValueResult> memoryFacts(const Graph &,
      const std::set<std::uint64_t> &,std::span<const FiniteMemoryQuery> queries) override {
    ++calls;
    if(mode==ProviderMode::Unavailable)return std::nullopt;
    FiniteValueResult result;
    result.complete=mode!=ProviderMode::Failed;
    for(const auto &query:queries)result.values.push_back({ValueSetKind::Unknown,query.width,{}});
    return result;
  }
 private:
  ProviderMode mode;
};

void verifyStores(const Graph &graph) {
  unsigned count=0;
  const Function root{&graph,base};
  REQUIRE(root.isValid());
  for(const auto &item:root.blocks()) {
    const auto *ir=graph.getIGNILBlock(item.first);
    REQUIRE(ir);
    REQUIRE(ir->unlifted().empty());
    for(const auto &op:ir->ops())if(op.opcode==ignil::Opcode::STORE) {
      REQUIRE(op.srcs.size()==2);
      CHECK(ignil::widthOf(op.srcs[1])==64);
      ++count;
    }
  }
  CHECK(count==2);
}

Graph analyze(const Fixture &f,const Configuration &config,
              const Disassembler::AnalysisHints &extra={}) {
  Graph graph{Architecture::X86_64,config};
  Disassembler dis{Architecture::X86_64,config};
  auto hints=extra;
  hints.entryPoints={base};
  dis.disassemble(f.code,base,graph,hints);
  REQUIRE(graph.get(f.dispatch));
  return graph;
}

Result recover(Graph &graph,const Fixture &f,const Options &options={}) {
  PassPipeline pipeline;
  const std::array<std::uint64_t,1> entries{base};
  REQUIRE(pipeline.registerGraph("recover",FiniteDispatchPass{f.code,base,entries}));
  REQUIRE(pipeline.add("recover",options));
  auto report=pipeline.run(graph);
  REQUIRE(report.ok);
  REQUIRE(report.runs.size()==1);
  return report.runs.front().result;
}
} // namespace

TEST_CASE("A failed or unknown table memory provider cannot fall back to default facts",
          "[table-contents][table-budget]") {
  for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    CAPTURE(strategy);
    const auto f=fixture();
    const auto config=configuration(strategy);
    auto automatic=analyze(f,config);
    verifyStores(automatic);
    REQUIRE(automatic.get(f.dispatch)->branches()==targets);
    for(auto mode:{ProviderMode::Failed,ProviderMode::Unknown,ProviderMode::Unavailable}) {
      CAPTURE(mode);
      const auto provider=std::make_shared<Provider>(mode);
      Disassembler::AnalysisHints hints;
      hints.indirectTargets=provider;
      auto graph=analyze(f,config,hints);
      verifyStores(graph);
      CHECK(provider->calls>0);
      if(mode==ProviderMode::Unavailable)CHECK(graph.get(f.dispatch)->branches()==targets);
      else CHECK(graph.get(f.dispatch)->branches().empty());
    }
  }
}

TEST_CASE("Trusted complete table assertions bypass providers but partial assertions do not",
          "[table-contents][table-budget]") {
  const auto f=fixture();
  const auto config=configuration(DisassemblyStrategy::LINEAR_SWEEP);
  for(bool complete:{false,true}) {
    CAPTURE(complete);
    const auto provider=std::make_shared<Provider>(ProviderMode::Failed);
    Disassembler::AnalysisHints hints;
    hints.indirectTargets=provider;
    hints.readKnownMemory=[complete,expectedPC=f.dispatch](const Disassembler::MemoryReadSite &site,
        std::uint64_t address,std::span<std::uint8_t> out) {
      CHECK(site.instructionAddress==expectedPC);
      if(out.size()!=8 || (address!=table && address!=table+8) ||
         (!complete && address==table+8))return false;
      const auto value=address==table?0x1040u:0x1050u;
      for(unsigned i=0;i<8;++i)out[i]=static_cast<std::uint8_t>(static_cast<std::uint64_t>(value)>>(i*8));
      return true;
    };
    auto graph=analyze(f,config,hints);
    if(complete) {
      CHECK(provider->calls==0);
      CHECK(graph.get(f.dispatch)->branches()==targets);
    } else {
      CHECK(provider->calls>0);
      CHECK(graph.get(f.dispatch)->branches().empty());
    }
  }
}

TEST_CASE("Finite dispatch memory queries share limits call proofs and per round caches",
          "[table-contents][table-budget]") {
  constexpr std::uint64_t budget=1000000;
  for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL})
    for(bool prove:{false,true}) {
      CAPTURE(strategy,prove);
      const auto f=fixture(true);
      auto graph=analyze(f,configuration(strategy));
      REQUIRE(graph.get(f.dispatch)->branches().empty());
      const auto report=recover(graph,f,{{"max_operations",std::int64_t(budget)},
                                         {"prove_leaf_calls",std::int64_t(prove)}});
      CHECK(report.counters.at("memory_analysis_operations")>0);
      CHECK(report.counters.at("memory_analysis_queries")>0);
      CHECK(report.counters.at("analysis_operations")>=report.counters.at("memory_analysis_operations"));
      CHECK(report.counters.at("max_round_analysis_operations")<=budget);
      CHECK(report.counters.at("memory_analysis_cached_facts_peak")<=262144);
      if(prove) {
        CHECK(report.status==Status::Success);
        CHECK(report.counters.at("proven_calls")>0);
        CHECK(graph.get(f.dispatch)->branches()==targets);
      } else {
        CHECK(report.status==Status::Incomplete);
        CHECK(graph.get(f.dispatch)->branches().empty());
        CHECK(report.counters.at("memory_analysis_cache_hits")>0);
      }
    }
}

TEST_CASE("Memory query exhaustion retracts old inference without publishing new targets",
          "[table-contents][table-budget]") {
  const auto f=fixture();
  const auto config=configuration(DisassemblyStrategy::LINEAR_SWEEP);
  auto full=analyze(f,config);
  const auto report=recover(full,f);
  REQUIRE(report.status==Status::Success);
  const auto peak=report.counters.at("max_round_analysis_operations");
  REQUIRE(peak>1);
  REQUIRE(report.counters.at("memory_analysis_operations")>0);
  auto limited=analyze(f,config);
  REQUIRE(limited.get(f.dispatch)->branches()==targets);
  const auto failure=recover(limited,f,{{"max_operations",std::int64_t(peak-1)}});
  CHECK(failure.status==Status::Incomplete);
  CHECK(failure.message=="finite dispatch analysis budget exhausted");
  CHECK(failure.counters.at("max_round_analysis_operations")<=peak-1);
  CHECK(failure.counters.at("memory_analysis_operations")>0);
  CHECK(failure.counters.at("removed_edges")==2);
  CHECK(failure.counters.at("new_edges")==0);
  CHECK(limited.get(f.dispatch)->branches().empty());
  CHECK(limited.get(f.dispatch)->inferredBranches().empty());
}
