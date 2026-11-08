#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <llvm/Support/JSON.h>

#include <array>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
namespace {
using O=ignil::Opcode;
constexpr std::uint64_t cell=0x4000;
std::vector<std::uint8_t> little(std::uint64_t value,unsigned count=8) {
  std::vector<std::uint8_t> result;
  for(unsigned i=0;i<count;++i)result.push_back(std::uint8_t(value>>(8*i)));
  return result;
}
FiniteEntryState initial(std::uint64_t entry=0x1000,std::uint64_t value=7) {
  return {entry,{},{{cell,little(value)}}};
}
void add(Graph &graph,ignil::Block block) {
  const auto address=block.startAddress();
  graph.addIGNILBlock(std::move(block));graph.addBlock(address);
}
void load(ignil::Block &block,std::uint32_t id=0,ignil::Width width=64,
    ignil::Value address=ignil::Const{cell,64}) {
  block.append({O::LOAD,ignil::Temp{id,width},{address},block.startAddress()});
}
void store(ignil::Block &block,std::uint64_t at,std::uint64_t value,ignil::Width width=64) {
  block.append({O::STORE,{}, {ignil::Const{at,64},ignil::Const{value,width}},block.startAddress()});
}
void jump(ignil::Block &block,std::uint64_t at) {
  block.append({O::JMP,{}, {ignil::Const{at,64}},block.startAddress()});
}
void equal(const FiniteValues &value,std::vector<std::uint64_t> expected) {
  REQUIRE(value.kind==ValueSetKind::Finite);CHECK(value.values==expected);
}
void allUnknown(const FiniteInitialStateResult &result,std::size_t count) {
  REQUIRE_FALSE(result.analysis.complete);
  REQUIRE(result.analysis.values.size()==count);
  for(const auto &value:result.analysis.values) {
    CHECK(value.kind==ValueSetKind::Unknown);CHECK(value.values.empty());
  }
  for(const auto &[address,value]:result.analysis.jumpTargets) {
    CHECK(value.kind==ValueSetKind::Unknown);CHECK(value.values.empty());
  }
}
FiniteInitialStateResult memory(const Graph &graph,std::span<const FiniteEntryState> states,
    std::size_t before=0,const std::set<std::uint64_t> &entries={0x1000},
    const FiniteValueLimits &limits={}) {
  const FiniteMemoryQuery query{0x1000,before,cell,64};
  return finiteMemoryValuesWithInitialState(graph,entries,std::span{&query,1},states,limits);
}
} // namespace

TEST_CASE("Finite initial facts are conditional snapshots before the actual LOAD",
          "[finite-values][finite-initial-state]") {
  for(auto architecture:{Architecture::X86_32,Architecture::X86_64,Architecture::ARM64}) {
    CAPTURE(architecture);
    Graph graph{architecture,{}};
    ignil::Block block{0x1000};load(block);
    store(block,cell,9);load(block,1);
    block.append({O::JMP,{}, {ignil::Temp{0,64}},0x1000});
    add(graph,std::move(block));
    const auto before=ignil::toString(*graph.getIGNILBlock(0x1000));
    const std::array points{FiniteMemoryQuery{0x1000,0,cell,64},FiniteMemoryQuery{0x1000,2,cell,64}};
    const std::array values{FiniteValueQuery{0x1000,ignil::Temp{0,64}},FiniteValueQuery{0x1000,ignil::Temp{1,64}}};
    const auto old=finiteMemoryValues(graph,{0x1000},points);
    CHECK(old.values[0].kind==ValueSetKind::Unknown);equal(old.values[1],{9});
    const std::array states{initial()};
    const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},points,states);
    REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
    equal(result.analysis.values[0],{7});equal(result.analysis.values[1],{9});
    equal(result.analysis.jumpTargets.at(0x1000),{7});
    const auto captured=finiteValuesWithInitialState(graph,{0x1000},values,states);
    REQUIRE(captured.assumptionsValid);REQUIRE(captured.analysis.complete);
    equal(captured.analysis.values[0],{7});equal(captured.analysis.values[1],{9});
    CHECK(ignil::toString(*graph.getIGNILBlock(0x1000))==before);
    CHECK(finiteMemoryValues(graph,{0x1000},points).toJSON()==old.toJSON());
    const auto empty=finiteMemoryValuesWithInitialState(graph,{0x1000},points,{});
    REQUIRE(empty.assumptionsValid);CHECK(empty.assumptions.empty());
    CHECK(empty.analysis.toJSON()==old.toJSON());
  }
}

TEST_CASE("Finite initial memory obeys disjoint partial and unknown write effects",
          "[finite-values][finite-initial-state]") {
  enum Kind {Disjoint,PartialKnown,PartialUnknown,Alias};
  Kind kind=Disjoint;
  SECTION("a disjoint unknown value does not destroy initial bytes") {}
  SECTION("a known partial write replaces only its bytes") {kind=PartialKnown;}
  SECTION("an unknown partial write cannot recover a whole cell") {kind=PartialUnknown;}
  SECTION("an unknown pointer may alias every initial cell") {kind=Alias;}
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};
  if(kind==Disjoint)block.append({O::STORE,{}, {ignil::Const{cell+8,64},x86::RAX},0x1000});
  if(kind==PartialKnown)store(block,cell+2,0xcc,8);
  if(kind==PartialUnknown)block.append({O::STORE,{}, {ignil::Const{cell+2,64},x86::AL},0x1000});
  if(kind==Alias)block.append({O::STORE,{}, {x86::RDI,ignil::Const{9,64}},0x1000});
  load(block);load(block,1,8);add(graph,std::move(block));
  const std::array states{initial(0x1000,0x1122334455667788ULL)};
  const std::array points{FiniteMemoryQuery{0x1000,1,cell,64},FiniteMemoryQuery{0x1000,2,cell,8}};
  const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},points,states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  if(kind==Disjoint)equal(result.analysis.values[0],{0x1122334455667788ULL});
  else if(kind==PartialKnown)equal(result.analysis.values[0],{0x1122334455cc7788ULL});
  else CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
  if(kind==Alias)CHECK(result.analysis.values[1].kind==ValueSetKind::Unknown);
  else equal(result.analysis.values[1],{0x88});
}

TEST_CASE("Finite initial facts do not bypass CALL opaque or local control barriers",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};
  SECTION("ordinary CALL") {block.append({O::CALL,{}, {ignil::Const{0x5000,64}},0x1000});}
  SECTION("zero op unlifted marker") {block.noteUnlifted(0x1000);}
  SECTION("local control") {block.append({O::LABEL,{}, {},0x1000});}
  const auto before=block.ops().size();load(block);add(graph,std::move(block));
  const std::array states{initial()};
  const auto result=memory(graph,states,before);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
}

TEST_CASE("Finite initial concrete memory crosses only proved nonaliasing machine calls",
          "[finite-values][finite-initial-state]") {
  enum Scenario {UnknownSP,ConcreteSP,ReturnSlot,PartialSP,ProofDisabled};
  for(auto strategy:{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    for(auto scenario:{UnknownSP,ConcreteSP,ReturnSlot,PartialSP,ProofDisabled}) {
      CAPTURE(strategy,scenario);
      const std::uint64_t address=scenario==ReturnSlot?0xc7f8:cell;
      // CALL1030; MOV RAX,[absolute32]; RET. The actual helper is one C3.
      // Return-slot effects are absent from abstract IGNIL CALL but must be
      // included by a proof used for machine-level memory-value recovery.
      std::vector<std::uint8_t> code(0x31,0x90);
      const std::array<std::uint8_t,14> caller{0xe8,0x2b,0,0,0,0x48,0x8b,0x04,0x25,
        std::uint8_t(address),std::uint8_t(address>>8),std::uint8_t(address>>16),std::uint8_t(address>>24),0xc3};
      for(std::size_t i=0;i<caller.size();++i)code[i]=caller[i];
      code[0x30]=0xc3;
      Configuration configuration;configuration.disassemblyStrategy=strategy;
      Graph graph{Architecture::X86_64,configuration};
      Disassembler disassembler{Architecture::X86_64,configuration};
      disassembler.disassemble(code,0x1000,graph);
      REQUIRE(graph.get(0x1000));CHECK(graph.get(0x1000)->isCall());
      CHECK(graph.get(0x1000)->callees()==std::set<std::uint64_t>{0x1030});
      REQUIRE(graph.get(0x1030));CHECK(graph.get(0x1030)->bytes()==std::vector<std::uint8_t>{0xc3});
      std::vector<FiniteMemoryQuery> queries;
      for(auto block:graph.validBlockAddresses()) {
        const auto *ir=graph.getIGNILBlock(block);if(!ir)continue;
        for(std::size_t i=0;i<ir->ops().size();++i) {
          const auto &op=ir->ops()[i];
          if(op.address==0x1005 && op.opcode==O::LOAD && op.dst && ignil::widthOf(*op.dst)==64)
            queries.push_back({block,i,address,64});
        }
      }
      REQUIRE(queries.size()==1);
      std::array states{FiniteEntryState{0x1000,{},{{address,little(7)}}}};
      if(scenario!=UnknownSP)states[0].registers.push_back({x86::RSP.offset,ignil::Width(scenario==PartialSP?32:64),0xc800});
      FiniteValueLimits limits;limits.proveLeafCalls=scenario!=ProofDisabled;
      const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},queries,states,limits);
      REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);REQUIRE(result.analysis.values.size()==1);
      CHECK(result.analysis.provenCalls==(scenario==ProofDisabled?0:1));
      CHECK(result.analysis.rejectedCalls==0);
      if(scenario==ConcreteSP)equal(result.analysis.values[0],{7});
      else CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
      const auto old=finiteMemoryValues(graph,{0x1000},queries,limits);
      REQUIRE(old.complete);CHECK(old.values[0].kind==ValueSetKind::Unknown);
    }
  }
}

TEST_CASE("Finite initial facts merge every branch and every independent input",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block entry{0x1000};
  entry.append({O::CJMP,{}, {x86::ZF,ignil::Const{0x1010,64},ignil::Const{0x1020,64}},0x1000});
  add(graph,std::move(entry));
  ignil::Block a{0x1010};store(a,cell,9);jump(a,0x1030);add(graph,std::move(a));
  ignil::Block b{0x1020};jump(b,0x1030);add(graph,std::move(b));
  ignil::Block out{0x1030};load(out);add(graph,std::move(out));
  const FiniteMemoryQuery query{0x1030,0,cell,64};
  const std::array states{initial()};
  auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},std::span{&query,1},states);
  REQUIRE(result.analysis.complete);equal(result.analysis.values[0],{7,9});
  result=finiteMemoryValuesWithInitialState(graph,{0x1000,0x1020},std::span{&query,1},states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
  const std::array both{initial(),initial(0x1020,11)};
  result=finiteMemoryValuesWithInitialState(graph,{0x1000,0x1020},std::span{&query,1},both);
  REQUIRE(result.analysis.complete);equal(result.analysis.values[0],{7,9,11});
}

TEST_CASE("Finite initial assumptions never add a root or suppress a decoded callee",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block entry{0x1000};jump(entry,0x1010);add(graph,std::move(entry));
  ignil::Block out{0x1010};load(out);add(graph,std::move(out));
  const FiniteMemoryQuery query{0x1010,0,cell,64};
  const std::array root{initial()};
  const std::array unadmitted{initial(0x1010,9)};
  auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},std::span{&query,1},unadmitted);
  CHECK_FALSE(result.assumptionsValid);allUnknown(result,1);
  result=finiteMemoryValuesWithInitialState(graph,{0x1000},std::span{&query,1},root);
  REQUIRE(result.analysis.complete);equal(result.analysis.values[0],{7});
  Instruction call{0x2000,5,"call","0x1010",{0xe8,0x0b,0xf0,0xff,0xff},false,true,false,false,false,false};
  call.addCallee(0x1010);graph.addInstruction(std::move(call));
  result=finiteMemoryValuesWithInitialState(graph,{0x1000},std::span{&query,1},root);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
  const std::array both{initial(),initial(0x1010,9)};
  result=finiteMemoryValuesWithInitialState(graph,{0x1000},std::span{&query,1},both);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  equal(result.analysis.values[0],{7,9});
}

TEST_CASE("Finite initial facts enter once and are not reset on a loop backedge",
          "[finite-values][finite-initial-state]") {
  bool alias=false;
  SECTION("later iterations write a different constant") {}
  SECTION("a later iteration may overwrite initial memory through an alias") {alias=true;}
  Graph graph{Architecture::X86_64,{}};
  ignil::Block head{0x1000};load(head);
  head.append({O::CJMP,{}, {x86::ZF,ignil::Const{0x1010,64},ignil::Const{0x1020,64}},0x1000});
  add(graph,std::move(head));
  ignil::Block body{0x1010};
  if(alias)body.append({O::STORE,{}, {x86::RDI,ignil::Const{9,64}},0x1010});
  else store(body,cell,9);
  jump(body,0x1000);add(graph,std::move(body));
  ignil::Block done{0x1020};done.append({O::RET,{}, {},0x1020});add(graph,std::move(done));
  const std::array states{initial()};
  const auto result=memory(graph,states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  if(alias)CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
  else equal(result.analysis.values[0],{7,9});
}

TEST_CASE("Finite initial facts use the existing default function and rootless loop entries",
          "[finite-values][finite-initial-state]") {
  bool loop=false;
  SECTION("default valid function input") {}
  SECTION("rootless cyclic graph still admits external input") {loop=true;}
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};load(block);
  if(loop) {store(block,cell,9);jump(block,0x1000);}
  else block.append({O::RET,{}, {},0x1000});
  add(graph,std::move(block));
  if(!loop)graph.addFunction(0x1000);
  const std::array states{initial()};
  const auto result=memory(graph,states,0,{});
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  if(loop)equal(result.analysis.values[0],{7,9});
  else equal(result.analysis.values[0],{7});
  CHECK(finiteMemoryValues(graph,{},std::array{FiniteMemoryQuery{0x1000,0,cell,64}}).values[0].kind==ValueSetKind::Unknown);
}

TEST_CASE("Finite initial memory keeps invalid and unreachable query meanings separate",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};load(block);add(graph,std::move(block));
  ignil::Block dead{0x2000};load(dead);add(graph,std::move(dead));
  const std::array states{initial()};
  const std::array queries{FiniteMemoryQuery{0x1000,0,cell,64},FiniteMemoryQuery{0x1000,0,cell,32},
    FiniteMemoryQuery{0x1000,1,cell,64},FiniteMemoryQuery{0x2000,0,cell,64},FiniteMemoryQuery{0x2000,1,cell,64}};
  const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},queries,states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  equal(result.analysis.values[0],{7});
  CHECK(result.analysis.values[1].kind==ValueSetKind::Unknown);
  CHECK(result.analysis.values[2].kind==ValueSetKind::Unknown);
  CHECK(result.analysis.values[3].kind==ValueSetKind::Unreachable);
  CHECK(result.analysis.values[4].kind==ValueSetKind::Unknown);
}

TEST_CASE("Finite initial register constraints do not perform 32 bit parent clearing",
          "[finite-values][finite-initial-state]") {
  for(auto architecture:{Architecture::X86_64,Architecture::ARM64}) {
    CAPTURE(architecture);
    Graph graph{architecture,{}};
    ignil::Block block{0x1000};block.append({O::RET,{}, {},0x1000});add(graph,std::move(block));
    const std::array queries{FiniteValueQuery{0x1000,ignil::Reg{0,32,{}}},
                            FiniteValueQuery{0x1000,ignil::Reg{0,64,{}}},
                            FiniteValueQuery{0x1000,ignil::Reg{4,32,{}}}};
    std::array states{FiniteEntryState{0x1000,{{0,32,0xaabbccdd}}, {}}};
    auto result=finiteValuesWithInitialState(graph,{0x1000},queries,states);
    REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
    equal(result.analysis.values[0],{0xaabbccdd});
    CHECK(result.analysis.values[1].kind==ValueSetKind::Unknown);
    CHECK(result.analysis.values[2].kind==ValueSetKind::Unknown);
    states[0].registers.push_back({4,32,0x11223344});
    result=finiteValuesWithInitialState(graph,{0x1000},queries,states);
    REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
    equal(result.analysis.values[1],{0x11223344aabbccddULL});equal(result.analysis.values[2],{0x11223344});
    const auto old=finiteValues(graph,{0x1000},queries);
    for(const auto &value:old.values)CHECK(value.kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Finite conditional register queries reject wrap after the default SP fact is clobbered",
          "[finite-values][finite-initial-state][finite-register-range]") {
  constexpr std::uint32_t high=0x1ffffff8U;
  constexpr std::uint32_t boundary=UINT32_MAX-8;
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};block.append({O::UNDEF,x86::RSP,{},0x1000});
  block.append({O::JMP,{}, {ignil::Reg{UINT32_MAX-6,64,{}}},0x1000});add(graph,std::move(block));
  const std::array states{FiniteEntryState{0x1000,
    {{high,64,0x1122334455667788ULL},{boundary,64,0x8877665544332211ULL}}, {}}};
  const std::array queries{
    FiniteValueQuery{0x1000,ignil::Reg{high,64,{}}},FiniteValueQuery{0x1000,ignil::Reg{boundary,64,{}}},
    FiniteValueQuery{0x1000,ignil::Reg{UINT32_MAX-1,8,{}}},FiniteValueQuery{0x1000,ignil::Reg{UINT32_MAX,8,{}}},
    FiniteValueQuery{0x1000,ignil::Reg{UINT32_MAX,16,{}}},FiniteValueQuery{0x1000,ignil::Reg{UINT32_MAX-2,32,{}}},
    FiniteValueQuery{0x1000,ignil::Reg{UINT32_MAX-6,64,{}}}};
  const auto result=finiteValuesWithInitialState(graph,{0x1000},queries,states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);REQUIRE(result.analysis.values.size()==queries.size());
  equal(result.analysis.values[0],{0x1122334455667788ULL});
  equal(result.analysis.values[1],{0x8877665544332211ULL});equal(result.analysis.values[2],{0x88});
  for(std::size_t i=3;i<queries.size();++i) {
    CAPTURE(i);CHECK(result.analysis.values[i].kind==ValueSetKind::Unknown);CHECK(result.analysis.values[i].values.empty());
  }
  CHECK(result.analysis.jumpTargets.at(0x1000).kind==ValueSetKind::Unknown);
}

TEST_CASE("Finite initial register facts cannot survive overflowing writes as stale constants",
          "[finite-values][finite-initial-state][finite-register-write-range]") {
  enum Scenario {Fallback32,Span32,Span64,WideOverflow,ZeroWidthInvalidIR,
                 Last64,LastFallback32,HighPartial32,FixedParentUpper32,ValidWideUndef,SeedOnly32};
  for(auto scenario:{Fallback32,Span32,Span64,WideOverflow,ZeroWidthInvalidIR,
                    Last64,LastFallback32,HighPartial32,FixedParentUpper32,ValidWideUndef,SeedOnly32}) {
    CAPTURE(scenario);
    const std::uint32_t at=scenario==HighPartial32?UINT32_MAX-16:
        scenario==FixedParentUpper32?0:scenario==ValidWideUndef?0x1000:UINT32_MAX-8;
    constexpr std::uint64_t unrelated=0xcafef00d98765432ULL;
    std::array states{FiniteEntryState{0x1000,{{at,64,0x1122334455667788ULL},{x86::RCX.offset,64,unrelated}}, {}}};
    if(scenario==SeedOnly32)states[0].registers[0]={at+4,32,0xaabbccdd};
    Graph graph{Architecture::X86_64,{}};ignil::Block block{0x1000};
    if(scenario==Fallback32)block.append({O::COPY,ignil::Reg{UINT32_MAX-4,32,{}},{ignil::Const{0xaabbccdd,32}},0x1000});
    if(scenario==Span32)block.append({O::COPY,ignil::Reg{UINT32_MAX-2,32,{}},{ignil::Const{0xaabbccdd,32}},0x1000});
    if(scenario==Span64)block.append({O::COPY,ignil::Reg{UINT32_MAX-6,64,{}},{ignil::Const{0xaabbccddeeff0011ULL,64}},0x1000});
    if(scenario==WideOverflow)block.append({O::UNDEF,ignil::Reg{at,128,{}},{},0x1000});
    // This malformed-IR control does not stand for an actual machine opcode.
    if(scenario==ZeroWidthInvalidIR)block.append({O::UNDEF,ignil::Reg{at,0,{}},{},0x1000});
    if(scenario==Last64)block.append({O::COPY,ignil::Reg{at,64,{}},{ignil::Const{0xaabbccdd01020304ULL,64}},0x1000});
    if(scenario==LastFallback32)block.append({O::COPY,ignil::Reg{at,32,{}},{ignil::Const{0xaabbccdd,32}},0x1000});
    if(scenario==HighPartial32 || scenario==FixedParentUpper32)
      block.append({O::COPY,ignil::Reg{at+4,32,{}},{ignil::Const{0xaabbccdd,32}},0x1000});
    if(scenario==ValidWideUndef)block.append({O::UNDEF,ignil::Reg{at,128,{}},{},0x1000});
    block.append({O::JMP,{}, {ignil::Reg{at,64,{}}},0x1000});add(graph,std::move(block));
    const std::array queries{FiniteValueQuery{0x1000,ignil::Reg{at,64,{}}},
      FiniteValueQuery{0x1000,ignil::Reg{at+4,32,{}}},FiniteValueQuery{0x1000,x86::RCX}};
    const auto result=finiteValuesWithInitialState(graph,{0x1000},queries,states);
    REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);REQUIRE(result.analysis.values.size()==queries.size());
    if(scenario<=ZeroWidthInvalidIR) {
      for(const auto &value:result.analysis.values) {CHECK(value.kind==ValueSetKind::Unknown);CHECK(value.values.empty());}
      CHECK(result.analysis.jumpTargets.at(0x1000).kind==ValueSetKind::Unknown);
    } else {
      equal(result.analysis.values[2],{unrelated});
      if(scenario==ValidWideUndef || scenario==SeedOnly32) {
        CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
        CHECK(result.analysis.jumpTargets.at(0x1000).kind==ValueSetKind::Unknown);
        if(scenario==SeedOnly32)equal(result.analysis.values[1],{0xaabbccdd});
        else CHECK(result.analysis.values[1].kind==ValueSetKind::Unknown);
      } else {
        const auto expected=scenario==Last64?0xaabbccdd01020304ULL:
            scenario==LastFallback32?0x00000000aabbccddULL:0xaabbccdd55667788ULL;
        equal(result.analysis.values[0],{expected});equal(result.analysis.values[1],{expected>>32});
        equal(result.analysis.jumpTargets.at(0x1000),{expected});
      }
    }
  }
}

TEST_CASE("Finite partial SP constraints discard the whole symbolic pointer identity",
          "[finite-values][finite-initial-state]") {
  for(auto architecture:{Architecture::X86_32,Architecture::X86_64,Architecture::ARM64}) {
    CAPTURE(architecture);
    const auto sp=architecture==Architecture::X86_32?x86::ESP:architecture==Architecture::X86_64?x86::RSP:arm::SP;
    const auto half=ignil::Width(sp.width/2);
    Graph graph{architecture,{}};
    ignil::Block block{0x1000};
    block.append({O::STORE,{}, {sp,ignil::Const{7,64}},0x1000});
    load(block,0,64,sp);block.append({O::JMP,{}, {ignil::Temp{0,64}},0x1000});
    add(graph,std::move(block));
    const std::array queries{FiniteValueQuery{0x1000,ignil::Temp{0,64}},FiniteValueQuery{0x1000,sp}};
    const auto old=finiteValues(graph,{0x1000},queries);
    equal(old.values[0],{7});CHECK(old.values[1].kind==ValueSetKind::Unknown);
    std::array states{FiniteEntryState{0x1000,{{sp.offset,half,0xc800}}, {}}};
    auto result=finiteValuesWithInitialState(graph,{0x1000},queries,states);
    REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
    CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
    CHECK(result.analysis.values[1].kind==ValueSetKind::Unknown);
    states[0].registers.push_back({sp.offset+unsigned(half/8),half,0});
    result=finiteValuesWithInitialState(graph,{0x1000},queries,states);
    REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
    equal(result.analysis.values[0],{7});equal(result.analysis.values[1],{0xc800});
  }
}

TEST_CASE("Finite initial assumptions validate structure before retaining owned data",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};load(block);block.append({O::JMP,{}, {ignil::Temp{0,64}},0x1000});add(graph,std::move(block));
  ignil::Block other{0x2000};load(other);add(graph,std::move(other));
  const std::array queries{FiniteMemoryQuery{0x1000,0,cell,64},FiniteMemoryQuery{0x2000,0,cell,64}};
  std::vector<FiniteEntryState> states{initial()};
  SECTION("duplicate entry") {states.push_back(initial());}
  SECTION("valid block outside the admitted roots") {states[0].entry=0x2000;}
  SECTION("nonexistent entry") {states[0].entry=0x9999;}
  SECTION("zero register width") {states[0].registers.push_back({0,0,0});}
  SECTION("non byte register width") {states[0].registers.push_back({0,7,0});}
  SECTION("overwide register") {states[0].registers.push_back({0,128,0});}
  SECTION("out of width register value") {states[0].registers.push_back({0,8,256});}
  SECTION("register range overflow") {states[0].registers.push_back({UINT32_MAX,8,0});}
  SECTION("overlapping subregisters") {states[0].registers={{0,32,0},{1,8,0}};}
  SECTION("identical register constraints still overlap") {states[0].registers={{0,32,7},{0,32,7}};}
  SECTION("empty memory range") {states[0].memory={{cell,{}}};}
  SECTION("memory range wraps") {states[0].memory={{UINT64_MAX,{1,2}}};}
  SECTION("overlapping memory ranges") {states[0].memory.push_back({cell+7,{0,0}});}
  SECTION("identical memory facts still overlap") {states[0].memory.push_back(states[0].memory.front());}
  const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},queries,states);
  CHECK_FALSE(result.assumptionsValid);CHECK(result.assumptions.empty());
  allUnknown(result,queries.size());
  CHECK(result.analysis.message.find("invalid finite initial-state assumptions")!=std::string::npos);
}

TEST_CASE("Finite initial regions may be adjacent and may end at the final memory byte",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};load(block);load(block,1,8,ignil::Const{UINT64_MAX,64});add(graph,std::move(block));
  const std::array queries{FiniteMemoryQuery{0x1000,0,cell,64},FiniteMemoryQuery{0x1000,1,UINT64_MAX,8}};
  const std::array states{FiniteEntryState{0x1000,{},{{cell,{1,2,3,4}},{cell+4,{5,6,7,8}},{UINT64_MAX,{0xaa}}}}};
  const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},queries,states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  equal(result.analysis.values[0],{0x0807060504030201ULL});equal(result.analysis.values[1],{0xaa});
}

TEST_CASE("Finite initial memory reassembles unequal ranges and internal chunks without bridging gaps",
          "[finite-values][finite-initial-state]") {
  bool gap=false;
  std::vector<FiniteMemoryFact> facts;
  SECTION("unequal adjacent facts can be supplied in reverse address order") {
    facts={{cell+3,{4,5,6,7,8,9}},{cell,{1,2,3}}};
  }
  SECTION("a nine byte fact crosses an internal eight byte chunk") {
    facts={{cell,{1,2,3,4,5,6,7,8,9}}};
  }
  SECTION("an uninitialized byte between facts is not silently zero filled") {
    gap=true;facts={{cell+4,{5,6,7,8,9}},{cell,{1,2,3}}};
  }
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};load(block);load(block,1,64,ignil::Const{cell+1,64});load(block,2,8);
  add(graph,std::move(block));
  const std::array states{FiniteEntryState{0x1000,{},facts}};
  const std::array queries{FiniteMemoryQuery{0x1000,0,cell,64},FiniteMemoryQuery{0x1000,1,cell+1,64},FiniteMemoryQuery{0x1000,2,cell,8}};
  const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},queries,states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
  if(gap) {
    CHECK(result.analysis.values[0].kind==ValueSetKind::Unknown);
    CHECK(result.analysis.values[1].kind==ValueSetKind::Unknown);
  } else {
    equal(result.analysis.values[0],{0x0807060504030201ULL});
    equal(result.analysis.values[1],{0x0908070605040302ULL});
  }
  equal(result.analysis.values[2],{1});
}

TEST_CASE("Finite initial addresses wrap at the arithmetic width before the LOAD",
          "[finite-values][finite-initial-state]") {
  for(auto architecture:{Architecture::X86_32,Architecture::X86_64}) {
    CAPTURE(architecture);
    for(auto width:{ignil::Width(32),ignil::Width(64)}) {
      CAPTURE(width);
      // IGNIL explicitly distinguishes effective-address arithmetic from the
      // memory access width. This is not a claim that a multi-byte access
      // wraps its individual bytes at a 32-bit address boundary.
      Graph graph{architecture,{}};
      ignil::Block block{0x1000};
      block.append({O::ADD,ignil::Temp{0,width},{ignil::Reg{0,width,{}},ignil::Const{4,width}},0x1000});
      load(block,1,32,ignil::Temp{0,width});add(graph,std::move(block));
      const std::array states{FiniteEntryState{0x1000,{{0,64,0xfffffffcULL}},
        {{0,{0x44,0x33,0x22,0x11}},{0x100000000ULL,{0xdd,0xcc,0xbb,0xaa}}}}};
      const std::array queries{FiniteValueQuery{0x1000,ignil::Temp{0,width}},FiniteValueQuery{0x1000,ignil::Temp{1,32}}};
      const auto result=finiteValuesWithInitialState(graph,{0x1000},queries,states);
      REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
      equal(result.analysis.values[0],{width==32?0ULL:0x100000000ULL});
      equal(result.analysis.values[1],{width==32?0x11223344ULL:0xaabbccddULL});
      const auto old=finiteValues(graph,{0x1000},queries);
      CHECK(old.values[0].kind==ValueSetKind::Unknown);CHECK(old.values[1].kind==ValueSetKind::Unknown);
    }
  }
}

TEST_CASE("Finite initial facts and owned assumptions obey initialization and analysis budgets",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};load(block);block.append({O::JMP,{}, {ignil::Temp{0,64}},0x1000});add(graph,std::move(block));
  const FiniteMemoryQuery query{0x1000,0,cell,64};
  const std::array states{initial()};
  const auto full=finiteMemoryValuesWithInitialState(graph,{0x1000},std::span{&query,1},states);
  REQUIRE(full.assumptionsValid);REQUIRE(full.analysis.complete);REQUIRE(full.analysis.operations>1);
  SECTION("initialization work is part of the operation budget") {
    FiniteValueLimits limits;limits.maxOperations=1;
    const auto result=memory(graph,states,0,{0x1000},limits);
    CHECK_FALSE(result.assumptionsValid);CHECK(result.assumptions.empty());allUnknown(result,1);
    CHECK(result.analysis.message.find("initialization budget")!=std::string::npos);
    CHECK(result.analysis.operations<=limits.maxOperations);
  }
  SECTION("owned initialization is part of the fact budget") {
    FiniteValueLimits limits;limits.maxFacts=1;
    const auto result=memory(graph,states,0,{0x1000},limits);
    CHECK_FALSE(result.assumptionsValid);CHECK(result.assumptions.empty());allUnknown(result,1);
  }
  SECTION("later exhaustion retains bounded structurally valid assumptions") {
    FiniteValueLimits limits;limits.maxOperations=std::uint32_t(full.analysis.operations-1);
    const auto result=memory(graph,states,0,{0x1000},limits);
    CHECK(result.assumptionsValid);REQUIRE(result.assumptions.size()==1);allUnknown(result,1);
    CHECK(result.analysis.operations<=limits.maxOperations);
  }
  SECTION("invalid engine limits cannot publish seeded constants") {
    FiniteValueLimits limits;limits.maxValues=0;
    const auto result=memory(graph,states,0,{0x1000},limits);
    CHECK(result.assumptionsValid);allUnknown(result,1);
  }
  SECTION("hard initialization cap applies even with larger user budgets") {
    FiniteValueLimits limits;limits.maxFacts=1000000;limits.maxOperations=1000000;
    const std::array large{FiniteEntryState{0x1000,{},{{cell,std::vector<std::uint8_t>(262145,0)}}}};
    const auto result=memory(graph,large,0,{0x1000},limits);
    CHECK_FALSE(result.assumptionsValid);CHECK(result.assumptions.empty());allUnknown(result,1);
    CHECK(result.analysis.message.find("initialization budget")!=std::string::npos);
  }
}

TEST_CASE("Finite initial JSON owns and labels exact assumptions without changing legacy JSON",
          "[finite-values][finite-initial-state]") {
  Graph graph{Architecture::X86_64,{}};
  ignil::Block block{0x1000};load(block,0,8);add(graph,std::move(block));
  const FiniteMemoryQuery query{0x1000,0,cell,8};
  std::array states{FiniteEntryState{0x1000,{{0,32,0x1234}},{{cell,{0,1,0x7f,0x80,0xff}}}}};
  const auto result=finiteMemoryValuesWithInitialState(graph,{0x1000},std::span{&query,1},states);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);equal(result.analysis.values[0],{0});
  const auto serialized=result.toJSON();
  states[0].registers[0].value=99;states[0].memory[0].bytes[0]=0xff;
  CHECK(result.toJSON()==serialized);CHECK(result.assumptions[0].registers[0].value==0x1234);
  CHECK(result.assumptions[0].memory[0].bytes[0]==0);
  auto parsed=llvm::json::parse(serialized);REQUIRE(parsed);
  const auto *object=parsed->getAsObject();REQUIRE(object);
  CHECK(object->getBoolean("conditional")==true);CHECK(object->getBoolean("assumptions_valid")==true);
  const auto *entries=object->getArray("entry_states");REQUIRE(entries);REQUIRE(entries->size()==1);
  const auto *entry=(*entries)[0].getAsObject();REQUIRE(entry);CHECK(entry->getString("entry")=="0x1000");
  const auto *memory=entry->getArray("memory");REQUIRE(memory);REQUIRE(memory->size()==1);
  CHECK((*memory)[0].getAsObject()->getString("bytes")=="00017f80ff");
  const auto legacy=finiteMemoryValues(graph,{0x1000},std::span{&query,1});
  CHECK(legacy.values[0].kind==ValueSetKind::Unknown);
  const auto legacyJSON=legacy.toJSON();
  CHECK(legacyJSON.find("conditional")==std::string::npos);CHECK(legacyJSON.find("entry_states")==std::string::npos);
}
