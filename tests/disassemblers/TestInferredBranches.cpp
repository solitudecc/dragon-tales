#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/passes/FiniteDispatch.hpp>
#include <algorithm>
#include <array>
#include <vector>

namespace {
constexpr std::uint64_t entry = 0x1000, dispatch = 0x1014;
std::vector<std::uint8_t> code() {
  const std::vector<std::uint8_t> prefix{
      0xb9,0x60,0x10,0,0,0x48,0x89,0x0c,0x25,0x80,0x10,0,0,
      0x83,0x3e,1,0x77,0x2e,0x8b,0x06,0xff,0x24,0xc5,0x80,0x10,0,0};
  std::vector<std::uint8_t> bytes(128,0xcc);
  std::copy(prefix.begin(),prefix.end(),bytes.begin());
  for (auto pc : {0x1040,0x1050,0x1060}) bytes[pc-entry]=0xc3;
  return bytes;
}
dragon::Disassembler::AnalysisHints contract() {
  dragon::Disassembler::AnalysisHints hints;
  hints.entryPoints={entry};
  hints.readKnownMemory=[](const dragon::Disassembler::MemoryReadSite &,
                          std::uint64_t address,std::span<std::uint8_t> out) {
    const std::array<std::uint8_t,16> current{
        0x60,0x10,0,0,0,0,0,0,0x50,0x10,0,0,0,0,0,0};
    if (address<0x1080 || address-0x1080>current.size() ||
        out.size()>current.size()-(address-0x1080)) return false;
    std::copy_n(current.begin()+(address-0x1080),out.size(),out.begin());
    return true;
  };
  return hints;
}
}

TEST_CASE("Reanalysis retracts inferred table edges without changing explicit evidence",
          "[table-contents][inferred-edges]") {
  for (auto strategy : {dragon::DisassemblyStrategy::LINEAR_SWEEP,
                         dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    dragon::Configuration config;config.disassemblyStrategy=strategy;
    dragon::Disassembler dis{dragon::Architecture::X86_64,config};
    dragon::Graph graph{dragon::Architecture::X86_64,config};
    auto hints=contract();const auto bytes=code();
    dis.disassemble(bytes,entry,graph,hints);
    auto *jump=graph.get(dispatch);REQUIRE(jump);
    REQUIRE(jump->branches()==std::set<std::uint64_t>{0x1050,0x1060});
    REQUIRE(jump->inferredBranches()==jump->branches());
    auto copy=graph.clone();
    REQUIRE(copy.get(dispatch)!=jump);
    CHECK(copy.get(dispatch)->inferredBranches()==jump->inferredBranches());

    // Promote one edge to explicit caller evidence. Only the other edge is
    // inference; withdrawing the content contract must preserve the former.
    jump->addBranch(0x1050);
    hints.readKnownMemory={};
    dis.disassemble(bytes,entry,graph,hints);
    CHECK(graph.get(dispatch)==jump);
    CHECK(jump->branches()==std::set<std::uint64_t>{0x1050});
    CHECK(jump->inferredBranches().empty());

    dis.disassemble(bytes,entry,copy,hints);
    CHECK(copy.get(dispatch)->branches().empty());
    CHECK(copy.get(dispatch)->inferredBranches().empty());
  }
}

TEST_CASE("Merging and cross region analysis invalidate old content proofs",
          "[table-contents][inferred-edges]") {
  dragon::Configuration config;
  dragon::Disassembler dis{dragon::Architecture::X86_64,config};
  dragon::Graph graph{dragon::Architecture::X86_64,config};
  const auto bytes=code();auto hints=contract();
  dis.disassemble(bytes,entry,graph,hints);
  REQUIRE(graph.get(dispatch));
  REQUIRE(graph.get(dispatch)->inferredBranches().size()==2);
  auto merged=graph.clone();
  dragon::Graph extra{dragon::Architecture::X86_64,config};
  dis.disassemble(std::array<std::uint8_t,1>{0xc3},0x2000,extra);
  merged.merge(std::move(extra));
  CHECK(merged.get(dispatch)->branches().empty());
  CHECK(graph.get(dispatch)->branches().size()==2);

  auto collision=graph.clone();
  // Same real instruction in another fragment has independent caller evidence.
  auto explicitFragment=graph.clone();
  explicitFragment.get(dispatch)->addBranch(0x1050);
  collision.merge(std::move(explicitFragment));
  CHECK(collision.get(dispatch)->branches()==std::set<std::uint64_t>{0x1050});
  CHECK(collision.get(dispatch)->inferredBranches().empty());

  dragon::Graph conditional{dragon::Architecture::X86_64,config};
  dis.disassemble(std::array<std::uint8_t,6>{0x85,0xc0,0x74,1,0xc3,0xc3},0x4000,conditional);
  REQUIRE(conditional.get(0x4002));
  REQUIRE(conditional.get(0x4002)->isConditional());
  // The decoder currently counts known branch targets (fallthrough is
  // implicit in BasicBlock), not both conditional successors.
  REQUIRE(conditional.get(0x4002)->edges()==1);
  conditional.get(0x4002)->setEdges(2); // explicit caller metadata
  auto sameConditional=conditional.clone();
  conditional.merge(std::move(sameConditional));
  CHECK(conditional.get(0x4002)->branches()==std::set<std::uint64_t>{0x4005});
  CHECK(conditional.get(0x4002)->edges()==2);

  // A new independent entry jumps past the STORE/guard into the old region.
  // The old region is outside this byte range, but its proof is stale too.
  hints.entryPoints={entry,0x2000};hints.readKnownMemory={};
  dis.disassemble(std::array<std::uint8_t,5>{0xe9,0x0d,0xf0,0xff,0xff},0x2000,graph,hints);
  REQUIRE(graph.get(0x2000));
  CHECK(graph.get(0x2000)->branches()==std::set<std::uint64_t>{0x1012});
  CHECK(graph.get(dispatch)->branches().empty());
}

TEST_CASE("Insufficient finite dispatch budgets withdraw old inference only",
          "[table-contents][inferred-edges]") {
  dragon::Configuration config;
  dragon::Disassembler dis{dragon::Architecture::X86_64,config};
  dragon::Graph graph{dragon::Architecture::X86_64,config};
  const auto bytes=code();const auto hints=contract();
  dis.disassemble(bytes,entry,graph,hints);
  REQUIRE(graph.get(dispatch));
  REQUIRE(graph.get(dispatch)->inferredBranches().size()==2);
  const auto instructions=graph.size();
  auto explicitCopy=graph.clone();
  explicitCopy.get(dispatch)->addBranch(0x1050);
  dragon::passes::AnalysisCache cache;
  dragon::passes::GraphContext context{graph,cache};
  dragon::passes::FiniteDispatchPass recovery{bytes,entry};
  const auto result=recovery(context,{{"max_operations",std::int64_t{0}}});
  CHECK(result.status==dragon::passes::Status::Incomplete);
  CHECK(result.changed);
  CHECK(result.counters.at("removed_edges")==2);
  CHECK(result.counters.at("new_edges")==0);
  CHECK(result.counters.at("new_instructions")==0);
  CHECK(result.counters.at("unresolved_jumps")==1);
  CHECK(graph.size()==instructions);
  CHECK(graph.get(dispatch)->branches().empty());
  dragon::passes::GraphContext other{explicitCopy,cache};
  const auto again=recovery(other,{{"max_rounds",std::int64_t{0}}});
  CHECK(again.status==dragon::passes::Status::Incomplete);
  CHECK(again.counters.at("removed_edges")==1);
  CHECK(explicitCopy.get(dispatch)->branches()==std::set<std::uint64_t>{0x1050});
}
