#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#ifdef DRAGON_WITH_UNICORN
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#endif

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
namespace {
constexpr std::uint64_t base=0x1000,helper=0x1200,child=0x1300;
using O=ignil::Opcode;
struct Program {
  bool wide;
  std::vector<std::uint8_t> code=std::vector<std::uint8_t>(0x500,0xcc);
  std::uint64_t pc=base;
  std::vector<std::uint64_t> loads;
  explicit Program(bool wide):wide(wide) {}
  void emit(std::initializer_list<std::uint8_t> data) {
    for(auto byte:data)code.at(pc++-base)=byte;
  }
  void word(std::uint32_t value) {
    for(unsigned n=0;n<4;++n)emit({std::uint8_t(value>>(8*n))});
  }
  void rex() {if(wide)emit({0x48});}
  void adjust(bool subtract) {rex();emit({0x83,std::uint8_t(subtract?0xec:0xc4),0x40});}
  void pointer(std::uint8_t offset) {rex();emit({0x8d,0x7c,0x24,offset});}
  void immediate(std::uint32_t value) {emit({0xb8});word(value);}
  void initial(std::uint8_t offset,std::uint32_t value) {emit({0xc7,0x44,0x24,offset});word(value);}
  void call(std::uint64_t target) {emit({0xe8});word(std::uint32_t(target-(pc+4)));}
  void jump(std::uint64_t target) {emit({0xe9});word(std::uint32_t(target-(pc+4)));}
  void load(std::uint8_t offset) {loads.push_back(pc);emit({0x8b,0x54,0x24,offset});}
  void done() {adjust(false);emit({0xc3});}
  void storeHelper() {pc=helper;emit({0x89,0x07,0xc3});} // MOV [DI],EAX; RET
};
Program fixed(bool wide,std::uint32_t value=7) {
  Program p{wide};p.adjust(true);p.initial(8,1);p.pointer(8);p.immediate(value);
  p.call(helper);p.load(8);p.done();p.storeHelper();return p;
}
Graph graphFor(const Program &program,DisassemblyStrategy strategy) {
  const auto arch=program.wide?Architecture::X86_64:Architecture::X86_32;
  Configuration configuration;configuration.disassemblyStrategy=strategy;
  Graph graph{arch,configuration};Disassembler disassembler{arch,configuration};
  Disassembler::AnalysisHints hints;hints.entryPoints={base};
  disassembler.disassemble(program.code,base,graph,hints);
  return graph;
}
std::vector<FiniteValueQuery> queries(const Graph &graph,const Program &program) {
  std::vector<FiniteValueQuery> result;
  for(auto address:program.loads) {
    std::vector<FiniteValueQuery> found;
    for(auto at:graph.validBlockAddresses())if(const auto *ir=graph.getIGNILBlock(at))
      for(const auto &op:ir->ops())if(op.address==address && op.opcode==O::LOAD &&
          op.dst && ignil::widthOf(*op.dst)==32)found.push_back({at,*op.dst});
    REQUIRE(found.size()==1);result.push_back(found.front());
  }
  return result;
}
std::map<std::uint64_t,std::string> snapshot(const Graph &graph) {
  std::map<std::uint64_t,std::string> result;
  for(auto at:graph.validBlockAddresses())if(const auto *ir=graph.getIGNILBlock(at))
    result.emplace(at,ignil::toString(*ir));
  return result;
}
FiniteValueResult run(const Program &program,DisassemblyStrategy strategy,
    FiniteValueLimits limits={},std::span<const FiniteEntryState> initial={}) {
  auto graph=graphFor(program,strategy);const auto before=snapshot(graph);
  const auto points=queries(graph,program);
  FiniteValueResult result;
  if(initial.empty())result=finiteValues(graph,{base},points,limits);
  else {
    const auto conditional=finiteValuesWithInitialState(graph,{base},points,initial,limits);
    REQUIRE(conditional.assumptionsValid);result=conditional.analysis;
  }
  CHECK(snapshot(graph)==before);
  CHECK(result.operations<=limits.maxOperations);
  REQUIRE(result.values.size()==program.loads.size());
  return result;
}
FiniteValueLimits enabled() {FiniteValueLimits limits;limits.proveLeafCalls=true;return limits;}
void known(const FiniteValues &value,std::uint64_t expected) {
  REQUIRE(value.kind==ValueSetKind::Finite);CHECK(value.values==std::vector<std::uint64_t>{expected});
}
constexpr std::array strategies{DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL};
} // namespace

TEST_CASE("Caller-proved pointers and scalar arguments instantiate fixed Guest stores",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies)
    for(auto value:{0u,1u,0x80000000u,0xffffffffu}) {
      CAPTURE(wide,strategy,value);const auto program=fixed(wide,value);
      const auto result=run(program,strategy,enabled());
      REQUIRE(result.complete);CHECK(result.provenCalls==1);CHECK(result.rejectedCalls==0);
      known(result.values.front(),value);
      const auto disabled=run(program,strategy);
      REQUIRE(disabled.complete);CHECK(disabled.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Caller context caches distinguish pointer and value at separate call sites",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);p.initial(8,1);p.initial(16,2);
    p.pointer(8);p.immediate(7);p.call(helper);
    p.pointer(16);p.immediate(9);p.call(helper);
    p.load(8);p.load(16);p.done();p.storeHelper();
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
    CHECK(result.provenCalls==2);CHECK(result.rejectedCalls==0);
    known(result.values[0],7);known(result.values[1],9);
  }
}

TEST_CASE("Nested call caches instantiate each child relative to its actual entry SP",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);p.initial(8,1);p.initial(16,2);
    p.pointer(8);p.call(helper);p.load(8);p.load(16);p.done();
    p.pc=helper;p.immediate(7);p.call(child);p.rex();p.emit({0x83,0xc7,8});
    p.immediate(9);p.call(child);p.emit({0xc3});
    p.pc=child;p.emit({0x89,0x07,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
    // The outer caller proves its helper; the two child call sites are also
    // analyzed from helper's independent arbitrary-input entry and reject.
    CHECK(result.provenCalls==1);CHECK(result.rejectedCalls==2);
    known(result.values[0],7);known(result.values[1],9);
  }
}

TEST_CASE("Contextual Guest writes cannot overlap this or an ancestor return slot",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies)for(bool nested:{false,true})
    for(bool partial:{false,true}) {
      CAPTURE(wide,strategy,nested,partial);Program p{wide};p.adjust(true);p.initial(8,1);
      const auto word=wide?8:4;
      p.pointer(std::uint8_t(partial?-1:-word));p.immediate(7);p.call(helper);
      p.load(8);p.done();
      if(nested) {p.pc=helper;p.call(child);p.emit({0xc3});p.pc=child;p.emit({0x89,0x07,0xc3});}
      else p.storeHelper();
      const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
      // In the nested form helper's independent entry rejects its child too.
      CHECK(result.provenCalls==0);CHECK(result.rejectedCalls==(nested?2:1));
      CHECK(result.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Caller loop widening never reuses an earlier singleton context",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);p.initial(8,1);p.pointer(8);p.immediate(7);
    p.jump(0x1080);p.pc=0x1080;p.call(helper);p.rex();p.emit({0x83,0xc7,8});
    p.emit({0x83,0xc0,1,0x85,0xf6,0x0f,0x85});p.word(std::uint32_t(0x1080-(p.pc+4)));
    p.load(8);p.done();p.storeHelper();
    auto limits=enabled();limits.maxValues=2;
    const auto result=run(p,strategy,limits);REQUIRE(result.complete);
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Numeric SP context recognizes addresses without relabelling scalar arithmetic",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);p.initial(8,1);
    p.emit({0xbf});p.word(0xc7c8);p.call(helper);p.load(8);p.done();
    // Both ADD operands are literal pointer-width integers. Even with a numeric
    // entry SP, their sum is the scalar value 16 rather than a Stack identity.
    p.pc=helper;p.emit({0xbb,7,0,0,0,0xb9,9,0,0,0});p.rex();p.emit({0x01,0xcb,0x89,0x1f,0xc3});
    const auto sp=wide?x86::RSP:x86::ESP;
    const std::array initial{FiniteEntryState{base,{{sp.offset,sp.width,0xc800}}, {}}};
    const auto result=run(p,strategy,enabled(),initial);REQUIRE(result.complete);
    CHECK(result.provenCalls==1);known(result.values.front(),16);
    const auto unknown=run(p,strategy,enabled());REQUIRE(unknown.complete);
    CHECK(unknown.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Partial SP and pointer constraints never manufacture a concrete parent",
          "[finite-values][caller-context]") {
  enum Scenario {Full,PartialSP,SeedOnly32,UnknownSP};
  for(auto strategy:strategies)for(auto scenario:{Full,PartialSP,SeedOnly32,UnknownSP}) {
    CAPTURE(strategy,scenario);Program p{true};p.adjust(true);p.initial(8,1);p.immediate(7);
    p.call(helper);p.load(8);p.done();p.storeHelper();
    std::array initial{FiniteEntryState{base,{}, {}}};
    if(scenario!=UnknownSP)initial[0].registers.push_back({x86::RSP.offset,
        ignil::Width(scenario==PartialSP?32:64),0xc800});
    initial[0].registers.push_back({x86::RDI.offset,ignil::Width(scenario==SeedOnly32?32:64),0xc7c8});
    const auto result=run(p,strategy,enabled(),initial);REQUIRE(result.complete);
    if(scenario==Full) {known(result.values.front(),7);CHECK(result.provenCalls==1);}
    else {CHECK(result.values.front().kind==ValueSetKind::Unknown);CHECK(result.provenCalls==0);}
  }
}

TEST_CASE("Caller bindings do not seed independently admitted callee entry states",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);auto p=fixed(wide);p.pc=helper;p.emit({0x89,0x07});
    p.loads.push_back(p.pc);p.emit({0x8b,0x17,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
    known(result.values[0],7);CHECK(result.values[1].kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Numeric caller pointers cannot write decoded code under a fixed CFG proof",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies)for(bool ownBody:{false,true}) {
    CAPTURE(wide,strategy,ownBody);Program p{wide};p.adjust(true);p.initial(8,1);
    p.emit({0xbf});p.word(ownBody?helper:base);p.immediate(7);p.call(helper);p.load(8);p.done();p.storeHelper();
    // Keep both code and stack inside the bounded relative-write domain. The
    // refusal must come from decoded-byte overlap, not a distant-address cap.
    const auto sp=wide?x86::RSP:x86::ESP;
    const std::array initial{FiniteEntryState{base,{{sp.offset,sp.width,0xc800}}, {}}};
    const auto result=run(p,strategy,enabled(),initial);REQUIRE(result.complete);
    CHECK(result.provenCalls==0);CHECK(result.rejectedCalls==1);
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("The outer machine CALL push cannot overwrite decoded code even for a universal C3",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.call(helper);p.load(8);p.emit({0xc3});
    p.pc=helper;p.emit({0xc3});
    const auto sp=wide?x86::RSP:x86::ESP;
    const auto word=std::uint64_t(wide?8:4);
    const std::array initial{FiniteEntryState{base,{{sp.offset,sp.width,helper+word}},
        {{helper+word+8,{7,0,0,0}}}}};
    const auto result=run(p,strategy,enabled(),initial);REQUIRE(result.complete);
    CHECK(result.provenCalls==0);CHECK(result.rejectedCalls==1);
    CHECK(result.values.front().kind==ValueSetKind::Unknown);
    // Without a numeric stack anchor the unchanged universal C3 proof still
    // succeeds. It does not claim support for unknown-address self-modification.
    const auto generic=run(p,strategy,enabled());REQUIRE(generic.complete);
    CHECK(generic.provenCalls==1);CHECK(generic.rejectedCalls==0);
  }
}

TEST_CASE("Decoded code overlap checks include earlier overlapping long instructions",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.adjust(true);p.initial(8,1);
    p.emit({0xbf});p.word(0x1403);p.immediate(7);p.call(helper);p.load(8);p.done();p.storeHelper();
    // MOV EAX,0x9090c390 at1400 contains a real C3 at1402. A write at1403
    // misses that immediately preceding RET but still overwrites the MOV.
    p.pc=0x1400;p.emit({0xb8,0x90,0xc3,0x90,0x90,0xc3});
    auto graph=graphFor(p,strategy);
    const auto arch=wide?Architecture::X86_64:Architecture::X86_32;
    Disassembler disassembler{arch,{}};
    const auto longInstruction=disassembler.disassemble(std::span{p.code}.subspan(0x400,5),0x1400);
    const auto shortInstruction=disassembler.disassemble(std::span{p.code}.subspan(0x402,1),0x1402);
    REQUIRE(longInstruction.size()==1);REQUIRE(shortInstruction.size()==1);
    REQUIRE(longInstruction.front().size()==5);REQUIRE(shortInstruction.front().size()==1);
    REQUIRE(shortInstruction.front().isReturn());
    graph.addInstruction(longInstruction.front());graph.addInstruction(shortInstruction.front());
    const auto sp=wide?x86::RSP:x86::ESP;
    const std::array initial{FiniteEntryState{base,{{sp.offset,sp.width,0xc800}}, {}}};
    const auto points=queries(graph,p);
    const auto result=finiteValuesWithInitialState(graph,{base},points,initial,enabled());
    REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);
    CHECK(result.analysis.provenCalls==0);CHECK(result.analysis.rejectedCalls==1);
    REQUIRE(result.analysis.values.size()==1);
    CHECK(result.analysis.values.front().kind==ValueSetKind::Unknown);
  }
}

TEST_CASE("Owned caller contexts and proof work obey global fact and operation budgets",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);Program p{wide};p.initial(8,99);
    // Each caller state is tiny, but the same helper has sixteen distinct
    // argument contexts. A budget must include their owned cache keys/proofs,
    // not just reject a large caller prelude before reaching any CALL.
    for(unsigned value=0;value<16;++value) {
      p.pointer(8);p.immediate(value);p.call(helper);
    }
    p.load(8);p.emit({0xc3});p.storeHelper();
    auto generous=enabled();const auto accepted=run(p,strategy,generous);
    REQUIRE(accepted.complete);CHECK(accepted.provenCalls==16);known(accepted.values.front(),15);
    for(bool operations:{false,true}) {
      CAPTURE(operations);auto limits=enabled();
      if(operations)limits.maxOperations=std::uint32_t(accepted.operations/2);
      else limits.maxFacts=256;
      const auto stopped=run(p,strategy,limits);
      CHECK_FALSE(stopped.complete);CHECK_FALSE(stopped.message.empty());
      CHECK(stopped.values.front().kind==ValueSetKind::Unknown);
      for(const auto &[address,value]:stopped.jumpTargets)CHECK(value.kind==ValueSetKind::Unknown);
    }
  }
}

TEST_CASE("Universal successes remain usable without constructing caller bindings",
          "[finite-values][caller-context]") {
  for(bool wide:{false,true})for(auto strategy:strategies) {
    CAPTURE(wide,strategy);auto p=fixed(wide);p.pc=helper;
    // This known SP-relative STORE already has a universal proof. Its result
    // must not depend on being able to specialize arbitrary caller registers.
    p.emit({0xc7,0x44,0x24,std::uint8_t((wide?8:4)+8),7,0,0,0,0xc3});
    const auto result=run(p,strategy,enabled());REQUIRE(result.complete);
    CHECK(result.provenCalls==1);CHECK(result.rejectedCalls==0);known(result.values.front(),7);
  }
}

#ifdef DRAGON_WITH_UNICORN
TEST_CASE("Independent Unicorn executes caller pointer fixtures with their expected loads",
          "[caller-context][oracle]") {
  // This validates actual E8/C3 execution and the observable scalar LOAD, not
  // Dragon LLVM execution or a complete-register differential comparison.
  for(bool wide:{false,true})for(auto value:{0u,1u,0x80000000u,0xffffffffu}) {
    CAPTURE(wide,value);const auto p=fixed(wide,value);
    uc_engine *uc=nullptr;
    REQUIRE(uc_open(UC_ARCH_X86,wide?UC_MODE_64:UC_MODE_32,&uc)==UC_ERR_OK);
    struct Close {uc_engine *uc;~Close(){uc_close(uc);}} close{uc};
    REQUIRE(uc_mem_map(uc,base,0x1000,UC_PROT_ALL)==UC_ERR_OK);
    REQUIRE(uc_mem_map(uc,0x8000,0x4000,UC_PROT_ALL)==UC_ERR_OK);
    REQUIRE(uc_mem_write(uc,base,p.code.data(),p.code.size())==UC_ERR_OK);
    std::uint64_t sp=0xb800,sentinel=0x1f00;
    REQUIRE(uc_reg_write(uc,wide?UC_X86_REG_RSP:UC_X86_REG_ESP,&sp)==UC_ERR_OK);
    REQUIRE(uc_mem_write(uc,sp,&sentinel,wide?8:4)==UC_ERR_OK);
    REQUIRE(uc_emu_start(uc,base,sentinel,1000000,10000)==UC_ERR_OK);
    std::uint64_t pc=0,finalSP=0;std::uint32_t loaded=0,memory=0;
    REQUIRE(uc_reg_read(uc,wide?UC_X86_REG_RIP:UC_X86_REG_EIP,&pc)==UC_ERR_OK);
    REQUIRE(uc_reg_read(uc,wide?UC_X86_REG_RSP:UC_X86_REG_ESP,&finalSP)==UC_ERR_OK);
    REQUIRE(uc_reg_read(uc,UC_X86_REG_EDX,&loaded)==UC_ERR_OK);
    REQUIRE(uc_mem_read(uc,sp-0x40+8,&memory,sizeof(memory))==UC_ERR_OK);
    CHECK(pc==sentinel);CHECK(finalSP==sp+(wide?8:4));CHECK(loaded==value);CHECK(memory==value);
    std::vector<std::uint8_t> after(p.code.size());
    REQUIRE(uc_mem_read(uc,base,after.data(),after.size())==UC_ERR_OK);CHECK(after==p.code);
  }
}
#endif
