#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
namespace {
constexpr std::uint64_t base=0x1000,helper=0x1200,child=0x1300;
using O=ignil::Opcode;
struct Point {std::uint64_t pc;std::optional<ignil::Reg> reg;};
struct Program {
  bool wide;
  std::vector<std::uint8_t> code=std::vector<std::uint8_t>(0x500,0xcc);
  std::uint64_t pc=base;
  std::vector<Point> points;
  explicit Program(bool wide):wide(wide) {}
  void emit(std::initializer_list<std::uint8_t> data) {
    for(auto byte:data)code.at(pc++-base)=byte;
  }
  void word(std::uint32_t value) {for(unsigned n=0;n<4;++n)emit({std::uint8_t(value>>(8*n))});}
  void rex() {if(wide)emit({0x48});}
  void adjust(bool subtract) {rex();emit({0x83,std::uint8_t(subtract?0xec:0xc4),0x40});}
  void store(std::uint8_t offset,std::uint32_t value) {emit({0xc7,0x44,0x24,offset});word(value);}
  void pointer(std::uint8_t offset,bool source=false) {rex();emit({0x8d,std::uint8_t(source?0x74:0x7c),0x24,offset});}
  void immediate(std::uint8_t opcode,std::uint32_t value) {emit({opcode});word(value);}
  void call(std::uint64_t target) {emit({0xe8});word(std::uint32_t(target-(pc+4)));}
  void jump(std::uint64_t target) {emit({0xe9});word(std::uint32_t(target-(pc+4)));}
  void load(std::uint8_t offset) {points.push_back({pc,{}});emit({0x8b,0x54,0x24,offset});}
  void done() {adjust(false);emit({0xc3});}
  void result(ignil::Reg reg=x86::EAX) {points.push_back({pc,reg});emit({0xc3});}
};
Graph graphFor(const Program &p,DisassemblyStrategy strategy) {
  const auto arch=p.wide?Architecture::X86_64:Architecture::X86_32;
  Configuration config;config.disassemblyStrategy=strategy;
  Graph graph{arch,config};Disassembler disassembler{arch,config};
  Disassembler::AnalysisHints hints;hints.entryPoints={base};
  disassembler.disassemble(p.code,base,graph,hints);return graph;
}
FiniteValueResult run(const Program &p,DisassemblyStrategy strategy,FiniteValueLimits limits={}) {
  auto graph=graphFor(p,strategy);std::vector<FiniteValueQuery> queries;
  for(const auto &point:p.points) {
    std::vector<FiniteValueQuery> found;
    for(auto at:graph.validBlockAddresses())if(const auto *ir=graph.getIGNILBlock(at)) {
      bool owns=false;
      for(const auto &op:ir->ops())if(op.address==point.pc) {
        if(point.reg)owns=true;
        else if(op.opcode==O::LOAD && op.dst && ignil::widthOf(*op.dst)==32)found.push_back({at,*op.dst});
      }
      if(point.reg && owns)found.push_back({at,*point.reg});
    }
    REQUIRE(found.size()==1);queries.push_back(found.front());
  }
  const auto result=finiteValues(graph,{base},queries,limits);
  REQUIRE(result.values.size()==p.points.size());CHECK(result.operations<=limits.maxOperations);
  return result;
}
FiniteValueLimits enabled() {FiniteValueLimits limits;limits.proveLeafCalls=true;return limits;}
void known(const FiniteValues &v,std::uint64_t value) {
  REQUIRE(v.kind==ValueSetKind::Finite);CHECK(v.values==std::vector<std::uint64_t>{value});
}
constexpr std::array strategies{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL};

Program fill(bool wide,std::uint32_t count,std::uint8_t value,bool unknownCount=false) {
  Program p{wide};p.adjust(true);p.store(8,0xaabbccdd);p.pointer(8);
  p.immediate(0xb8,value);if(!unknownCount)p.immediate(0xb9,count);
  p.call(helper);p.load(8);p.done();
  p.pc=helper;p.emit({0x85,0xc9,0x74});const auto zero=p.pc;p.emit({0});
  const auto loop=p.pc;p.emit({0x88,0x07});p.rex();p.emit({0x83,0xc7,1,0x83,0xe9,1,0x75});
  p.emit({std::uint8_t(loop-(p.pc+1))});
  p.code.at(zero-base)=std::uint8_t(p.pc-(zero+1));p.emit({0xc3});return p;
}
Program copy(bool wide,std::uint32_t count,bool nested) {
  Program p{wide};p.adjust(true);p.store(8,0x00112233);p.store(16,0xaabbccdd);
  p.pointer(8);p.pointer(16,true);p.immediate(0xb9,count);p.call(helper);p.load(16);p.done();
  p.pc=helper;if(nested) {p.call(child);p.emit({0xc3});p.pc=child;}
  p.emit({0x85,0xc9,0x74});const auto zero=p.pc;p.emit({0});
  const auto loop=p.pc;p.emit({0x8a,0x07,0x88,0x06});
  p.rex();p.emit({0x83,0xc7,1});p.rex();p.emit({0x83,0xc6,1,0x83,0xe9,1,0x75});
  p.emit({std::uint8_t(loop-(p.pc+1))});
  p.code.at(zero-base)=std::uint8_t(p.pc-(zero+1));p.emit({0xc3});return p;
}
} // namespace

TEST_CASE("Call memory facts refine a universal LOAD result without inventing writes",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies)for(auto value:{0u,1u,0x80000000u,0xffffffffu}) {
    CAPTURE(wide,strategy,value);Program p{wide};p.adjust(true);p.store(8,value);p.pointer(8);p.call(helper);
    p.adjust(false);p.result();p.pc=helper;p.emit({0x8b,0x07,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);known(result.values.front(),value);
    const auto disabled=run(p,strategy);REQUIRE(disabled.complete);
    CHECK(disabled.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Call cache identity includes changed input memory with identical registers",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);p.store(8,7);p.pointer(8);
    p.immediate(0xb8,0);p.call(helper);p.points.push_back({p.pc,x86::EAX});p.jump(0x1080);
    p.pc=0x1080;p.store(8,9);p.immediate(0xb8,0);p.call(helper);p.adjust(false);p.result();
    p.pc=helper;p.emit({0x8b,0x07,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
    known(result.values[0],7);known(result.values[1],9);
  }
}

TEST_CASE("A complete SP-relative pointer stored in memory remains an address inside a helper",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);p.store(32,1);p.pointer(32,true);
    p.rex();p.emit({0x89,0x74,0x24,8});p.pointer(8);p.call(helper);p.load(32);p.done();
    p.pc=helper;p.rex();p.emit({0x8b,0x37,0xc7,0x06,7,0,0,0,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);known(result.values.front(),7);
  }
}

TEST_CASE("Exact ordinary CFG fill loops prove zero one and multiple real iterations",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies)for(auto count:{0u,1u,3u,4u})
    for(auto value:{std::uint8_t{0},std::uint8_t{0x80},std::uint8_t{0xff}}) {
      CAPTURE(wide,strategy,count,unsigned(value));const auto p=fill(wide,count,value);
      const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
      auto expected=std::uint32_t{0xaabbccdd};
      for(unsigned n=0;n<count;++n)expected=(expected&~(std::uint32_t{255}<<(8*n)))|(std::uint32_t(value)<<(8*n));
      known(result.values.front(),expected);
  }
}

TEST_CASE("Caller memory propagates through nested exact byte copy loops",
          "[finite-values][call-memory-effects]") {
  const std::array expected{0xaabbccddu,0xaabbcc33u,0xaabb2233u,0xaa112233u,0x00112233u};
  for(bool wide:{false,true})for(auto strategy:strategies)for(bool nested:{false,true})
    for(unsigned count=0;count<expected.size();++count) {
      CAPTURE(wide,strategy,nested,count);const auto p=copy(wide,count,nested);
      const auto result=run(p,strategy,enabled());REQUIRE(result.complete);known(result.values.front(),expected[count]);
  }
}

TEST_CASE("Unknown or excessive loop counts never publish a sampled prefix",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies)for(bool unknown:{false,true}) {
    CAPTURE(wide,strategy,unknown);const auto p=fill(wide,0xffffffff,0x55,unknown);
    const auto result=run(p,strategy,enabled());
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Entry memory remains unknown after partial initialization or an aliasing STORE",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies)for(bool alias:{false,true}) {
    CAPTURE(wide,strategy,alias);Program p{wide};p.adjust(true);
    if(alias) {p.store(8,7);p.emit({0xc7,0x03,9,0,0,0});}
    else p.emit({0x66,0xc7,0x44,0x24,8,7,0});
    p.pointer(8);p.call(helper);p.adjust(false);p.result();p.pc=helper;p.emit({0x8b,0x07,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("A fresh machine CALL return slot cannot import the caller previous bytes",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);
    p.store(std::uint8_t(-(wide?8:4)),7);p.call(helper);p.adjust(false);p.result();
    p.pc=helper;p.emit({0x8b,0x04,0x24,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Loop proof operation exhaustion keeps all externally reported facts unknown",
          "[finite-values][call-memory-effects]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);const auto p=copy(wide,4,true);
    auto limits=enabled();limits.maxOperations=16;
    const auto result=run(p,strategy,limits);CHECK_FALSE(result.complete);
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
    CHECK_FALSE(result.message.empty());
  }
}
