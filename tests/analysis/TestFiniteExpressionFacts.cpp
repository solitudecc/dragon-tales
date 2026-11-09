#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;

namespace {
using O = ignil::Opcode;
using W = ignil::Width;
constexpr std::uint64_t entry = 0x1000;

std::uint64_t mask(W width) {
  return width == 64 ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << width) - 1;
}

std::vector<std::uint64_t> subsets(std::uint64_t bits) {
  std::vector<std::uint64_t> result;
  for (auto value = bits;; value = (value - 1) & bits) {
    result.push_back(value);
    if (!value) break;
  }
  std::ranges::sort(result);
  return result;
}

void add(Graph &graph, ignil::Block block) {
  const auto address = block.startAddress();
  graph.addIGNILBlock(std::move(block));
  graph.addBlock(address);
}

FiniteValueResult evaluate(const Graph &graph, ignil::Value value,
                           FiniteValueLimits limits = {}) {
  const std::array queries{FiniteValueQuery{entry, std::move(value)}};
  return finiteValues(graph, {entry}, queries, limits);
}

void equal(const FiniteValueResult &result, std::vector<std::uint64_t> expected) {
  REQUIRE(result.complete);
  REQUIRE(result.values.size() == 1);
  CHECK(result.values[0].kind == ValueSetKind::Finite);
  CHECK(result.values[0].values == expected);
}

void unknown(const FiniteValueResult &result) {
  REQUIRE(result.complete);
  REQUIRE(result.values.size() == 1);
  CHECK(result.values[0].kind == ValueSetKind::Unknown);
  CHECK(result.values[0].values.empty());
}

void raised(ignil::Block &block, ignil::Value destination,
             ignil::Value source, std::uint64_t bits, W width,
             bool literalFirst = false) {
  const ignil::Value literal = ignil::Const{bits, width};
  if (literalFirst)
    block.append({O::OR, std::move(destination), {literal, std::move(source)}, entry});
  else
    block.append({O::OR, std::move(destination), {std::move(source), literal}, entry});
}
} // namespace

TEST_CASE("Finite OR minus its own snapshot bounds every integer width and literal order",
          "[finite-values][finite-expression-facts]") {
  for (unsigned bits = 1; bits <= 64; ++bits) {
    const W width = static_cast<W>(bits);
    const auto high = std::uint64_t{1} << (bits - 1);
    const std::array masks{std::uint64_t{0}, std::uint64_t{1}, high,
                          std::uint64_t{7} & mask(width),
                          (high | std::uint64_t{5}) & mask(width)};
    for (auto literal : masks) for (bool literalFirst : {false, true}) {
      CAPTURE(bits, literal, literalFirst);
      Graph graph{Architecture::X86_64, {}};
      ignil::Block block{entry};
      const ignil::Reg source{x86::RDI.offset, width, "input"};
      const ignil::Temp disjunction{0, width}, out{1, width};
      raised(block, disjunction, source, literal, width, literalFirst);
      block.append({O::SUB, out, {disjunction, source}, entry});
      add(graph, std::move(block));
      for (bool calls : {false, true}) {
        CAPTURE(calls);
        FiniteValueLimits limits;
        limits.proveLeafCalls = calls;
        equal(evaluate(graph, out, limits), subsets(literal));
      }
    }
  }
}

TEST_CASE("Finite expression snapshots survive equal width copies and register intermediates",
          "[finite-values][finite-expression-facts]") {
  for (W width : {W{8}, W{16}, W{32}, W{64}})
    for (bool registerMiddle : {false, true}) {
      CAPTURE(unsigned(width), registerMiddle);
      Graph graph{Architecture::X86_64, {}};
      ignil::Block block{entry};
      const ignil::Reg input{x86::RDI.offset, width, "input"};
      const ignil::Reg copied{x86::RDX.offset, width, "copied"};
      const ignil::Reg middle{x86::RSI.offset, width, "middle"};
      const ignil::Temp snapshot{0, width}, disjunction{1, width},
          forwarded{2, width}, out{3, width};
      block.append({O::COPY, snapshot, {input}, entry});
      block.append({O::COPY, copied, {snapshot}, entry});
      const ignil::Value carrier = registerMiddle ? ignil::Value{middle}
                                                   : ignil::Value{disjunction};
      raised(block, carrier, copied, 5, width);
      block.append({O::COPY, forwarded, {carrier}, entry});
      block.append({O::SUB, out, {forwarded, snapshot}, entry});
      add(graph, std::move(block));
      equal(evaluate(graph, out), {0, 1, 4, 5});
    }
}

TEST_CASE("Finite snapshot facts do not equate independent values or changed registers",
          "[finite-values][finite-expression-facts]") {
  enum Scenario { OtherKnownRegister, OtherUnknownRegister, SeparateLoads,
                  SeparateUndefs, RegisterUndef, PartialWrite, ParentClearingWrite,
                  WideWrite, Call, RedefinedTemp, MissingTemp, MixedWidth };
  for (auto scenario : {OtherKnownRegister, OtherUnknownRegister, SeparateLoads,
                        SeparateUndefs, RegisterUndef, PartialWrite,
                        ParentClearingWrite, WideWrite, Call, RedefinedTemp,
                        MissingTemp, MixedWidth}) {
    CAPTURE(scenario);
    Graph graph{Architecture::X86_64, {}};
    ignil::Block block{entry};
    const ignil::Temp first{0, 64}, disjunction{1, 64}, second{2, 64}, out{3, 64};
    ignil::Value left = x86::RDI, right = x86::RDI;
    if (scenario == OtherKnownRegister) {
      block.append({O::COPY, x86::RSI, {ignil::Const{0, 64}}, entry});
      right = x86::RSI;
    } else if (scenario == OtherUnknownRegister) right = x86::RSI;
    else if (scenario == SeparateLoads) {
      // Equal addresses are not a proof that independent LOAD snapshots agree.
      block.append({O::LOAD, first, {ignil::Const{0x4000, 64}}, entry});
      left = first;
      right = second;
    } else if (scenario == SeparateUndefs || scenario == RedefinedTemp) {
      block.append({O::UNDEF, first, {}, entry});
      left = first;
      right = scenario == SeparateUndefs ? ignil::Value{second} : ignil::Value{first};
    } else if (scenario == MissingTemp) left = right = first;
    else if (scenario == MixedWidth) {
      block.append({O::COPY, first, {x86::RDI}, entry});
      left = first;
      right = ignil::Temp{0, 32};
    }
    raised(block, disjunction, left, 7, 64);
    switch (scenario) {
      case SeparateLoads:
        block.append({O::LOAD, second, {ignil::Const{0x4000, 64}}, entry});
        break;
      case SeparateUndefs:
        block.append({O::UNDEF, second, {}, entry});
        break;
      case RedefinedTemp:
        block.append({O::UNDEF, first, {}, entry});
        break;
      case RegisterUndef:
        block.append({O::UNDEF, x86::RDI, {}, entry});
        break;
      case PartialWrite:
        block.append({O::COPY, ignil::Reg{x86::RDI.offset, 8, "low byte"},
                      {ignil::Const{0, 8}}, entry});
        break;
      case ParentClearingWrite:
        block.append({O::COPY, x86::EDI, {ignil::Const{0, 32}}, entry});
        break;
      case WideWrite:
        block.append({O::UNDEF, ignil::Reg{x86::RSI.offset, 128, "wide overlap"}, {}, entry});
        break;
      case Call:
        block.append({O::CALL, {}, {ignil::Const{0x5000, 64}}, entry});
        break;
      default: break;
    }
    block.append({O::SUB, out, {disjunction, right}, entry});
    add(graph, std::move(block));
    for (bool calls : {false, true}) {
      CAPTURE(calls);
      FiniteValueLimits limits;
      limits.proveLeafCalls = calls;
      unknown(evaluate(graph, out, limits));
    }
  }
}

TEST_CASE("Finite copied Temp snapshots remain identities across stores and call barriers",
          "[finite-values][finite-expression-facts]") {
  for (bool call : {false, true}) for (bool redefineCopy : {false, true}) {
    CAPTURE(call, redefineCopy);
    Graph graph{Architecture::X86_64, {}};
    ignil::Block block{entry};
    const ignil::Temp snapshot{0, 64}, disjunction{1, 64}, copy{2, 64}, out{3, 64};
    block.append({O::LOAD, snapshot, {ignil::Const{0x4000, 64}}, entry});
    raised(block, disjunction, snapshot, 5, 64);
    if (call) block.append({O::CALL, {}, {ignil::Const{0x5000, 64}}, entry});
    else block.append({O::STORE, {}, {x86::RDI, ignil::Const{0, 64}}, entry});
    // Reusing an id is safe only if its actual reaching definition copies the
    // same captured snapshot. A new UNDEF definition is covered above.
    block.append({O::COPY, copy, {snapshot}, entry});
    if (redefineCopy) block.append({O::COPY, snapshot, {copy}, entry});
    block.append({O::SUB, out, {disjunction, snapshot}, entry});
    add(graph, std::move(block));
    equal(evaluate(graph, out), {0, 1, 4, 5});
  }
}

TEST_CASE("Finite expression register snapshots obey A64 vector parent clearing",
          "[finite-values][finite-expression-facts]") {
  Graph graph{Architecture::ARM64, {}};
  ignil::Block block{entry};
  const ignil::Reg upper{arm::V0.offset + 8, 64, "V0 high half"};
  const ignil::Temp disjunction{0, 64}, out{1, 64};
  raised(block, disjunction, upper, 7, 64);
  // S0 clears all of V0, including the captured high-half register span.
  block.append({O::COPY, arm::S0, {ignil::Const{0, 32}}, entry});
  block.append({O::SUB, out, {disjunction, upper}, entry});
  add(graph, std::move(block));
  unknown(evaluate(graph, out));
}

TEST_CASE("Finite expression facts respect literal capacity and never publish partial budget results",
          "[finite-values][finite-expression-facts]") {
  Graph graph{Architecture::X86_64, {}};
  ignil::Block block{entry};
  const ignil::Temp disjunction{0, 64}, out{1, 64};
  raised(block, disjunction, x86::RDI, 0x7f, 64);
  block.append({O::SUB, out, {disjunction, x86::RDI}, entry});
  block.append({O::JMP, {}, {out}, entry});
  add(graph, std::move(block));
  unknown(evaluate(graph, out)); // 128 subsets cannot be truncated to 64.
  FiniteValueLimits limits;
  limits.maxValues = 128;
  const auto full = evaluate(graph, out, limits);
  equal(full, subsets(0x7f));
  CHECK(full.jumpTargets.at(entry) == full.values[0]);
  REQUIRE(full.operations > 0);
  for (bool factLimit : {false, true}) {
    CAPTURE(factLimit);
    auto limited = limits;
    if (factLimit) limited.maxFacts = 0;
    else limited.maxOperations = static_cast<std::uint32_t>(full.operations - 1);
    const auto result = evaluate(graph, out, limited);
    CHECK_FALSE(result.complete);
    REQUIRE(result.values.size() == 1);
    CHECK(result.values[0].kind == ValueSetKind::Unknown);
    CHECK(result.values[0].values.empty());
    CHECK(result.jumpTargets.at(entry).kind == ValueSetKind::Unknown);
    CHECK(result.jumpTargets.at(entry).values.empty());
  }
}

TEST_CASE("Finite expression facts do not cross opaque or block local control regions",
          "[finite-values][finite-expression-facts]") {
  for (bool opaque : {false, true}) {
    CAPTURE(opaque);
    Graph graph{Architecture::X86_64, {}};
    ignil::Block block{entry};
    const ignil::Temp disjunction{0, 64}, out{1, 64};
    raised(block, disjunction, x86::RDI, 7, 64);
    if (opaque) block.noteUnlifted(entry + 1);
    else block.append({O::LABEL, {}, {}, entry});
    block.append({O::SUB, out, {disjunction, x86::RDI}, entry});
    add(graph, std::move(block));
    unknown(evaluate(graph, out));
  }
}
