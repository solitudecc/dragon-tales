#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace {
constexpr std::uint64_t base=0x1000, guard=0x1040, dispatch=0x1054;
constexpr std::uint64_t body=0x1080, leaf=0x10c0, fallback=0x1100, table=0x3000;
using Bytes=std::vector<std::uint8_t>;

dragon::Configuration configuration(dragon::DisassemblyStrategy strategy) {
  dragon::Configuration result;
  result.disassemblyStrategy=strategy;
#ifdef DRAGON_WITH_Z3
  result.useZ3ForPathDiscovery=false;
#endif
  return result;
}

void word(Bytes &bytes,std::uint64_t address,std::uint32_t value) {
  REQUIRE(address>=base);
  REQUIRE(address-base+4<=bytes.size());
  for(unsigned i=0;i<4;++i)bytes[address-base+i]=static_cast<std::uint8_t>(value>>(8*i));
}
std::uint32_t branch(std::uint64_t from,std::uint64_t target) {
  const auto displacement=(static_cast<std::int64_t>(target)-static_cast<std::int64_t>(from))/4;
  return 0x14000000u|(static_cast<std::uint32_t>(displacement)&0x03ffffffu);
}
std::uint32_t movX(unsigned reg,std::uint16_t value) {
  return 0xd2800000u|(std::uint32_t(value)<<5)|reg;
}
Bytes fixture(bool alias,bool invalidTarget=false) {
  Bytes bytes(fallback-base+4);
  for(std::uint64_t address=base;address<base+bytes.size();address+=4)
    word(bytes,address,0xd4200000); // BRK padding is never executed.
  word(bytes,0x1000,movX(3,table));
  word(bytes,0x1004,movX(4,body));
  word(bytes,0x1008,0xf9000064); // STR X4,[X3]: table[0]=body
  word(bytes,0x100c,movX(4,invalidTarget?0x2000:leaf));
  word(bytes,0x1010,0xf9000464); // STR X4,[X3,#8]: table[1]=leaf
  word(bytes,0x1014,branch(0x1014,guard));
  // Refresh the table base on every visit; a loop must not turn the address
  // into an unrelated unresolved register before checking the content proof.
  word(bytes,0x1040,movX(3,table));
  word(bytes,0x1044,0xb9400020); // LDR W0,[X1]
  word(bytes,0x1048,0x7100041f); // CMP W0,#1
  word(bytes,0x104c,0x54000008u|(((fallback-0x104c)/4)<<5)); // B.HI fallback
  word(bytes,0x1050,0xf8605860); // LDR X0,[X3,W0,UXTW#3]
  word(bytes,dispatch,0xd61f0000); // BR X0
  // Reaching this body is initially known only through the recovered table.
  // The unknown value X2 either destroys entry zero or writes outside both
  // entries. The back-edge must participate in a new content fixed point.
  word(bytes,body,alias?0xf9000062:0xf9000862); // STR X2,[X3{,#16}]
  word(bytes,body+4,branch(body+4,guard));
  word(bytes,leaf,0xd65f03c0);
  word(bytes,fallback,0xd65f03c0);
  return bytes;
}

dragon::Disassembler::AnalysisHints hints(bool invalidTarget=false) {
  dragon::Disassembler::AnalysisHints result;
  result.entryPoints={base};
  // Initial bytes are included to reproduce the old reader's stale answer.
  // New recovery must establish current contents from the actual STRs.
  result.readMemory=[invalidTarget](std::uint64_t address,std::span<std::uint8_t> out) {
    const std::uint64_t values[]{body,invalidTarget?0x2000:leaf};
    if(address<table || address-table>16 || out.size()>16-(address-table))return false;
    for(std::size_t i=0;i<out.size();++i) {
      const auto offset=address-table+i;
      out[i]=static_cast<std::uint8_t>(values[offset/8]>>(8*(offset%8)));
    }
    return true;
  };
  return result;
}

void storesAreReal(const dragon::Graph &graph) {
  const auto *entry=graph.getIGNILBlock(base);
  REQUIRE(entry);
  REQUIRE(entry->unlifted().empty());
  for(auto address:{0x1008u,0x1010u}) {
    const auto store=std::find_if(entry->ops().begin(),entry->ops().end(),[&](const auto &op) {
      return op.address==address && op.opcode==dragon::ignil::Opcode::STORE;
    });
    REQUIRE(store!=entry->ops().end());
    REQUIRE(store->srcs.size()==2);
    CHECK(dragon::ignil::widthOf(store->srcs[1])==64);
  }
}

void linearNodesRemain(const dragon::Disassembler &dis,
                       const dragon::Configuration &config,
                       const Bytes &bytes,const dragon::Graph &actual) {
  // A separate linear analysis of the identical bytes supplies the original
  // nodes. Starting at the guard admits arbitrary table memory, bypassing the
  // entry's initializers; no image/trusted reader is supplied. Thus neither
  // the old image reader nor the new STORE proof can expand this baseline.
  dragon::Disassembler::AnalysisHints roots;
  roots.entryPoints={guard};
  dragon::Graph baseline{dragon::Architecture::ARM64,config};
  dis.disassemble(bytes,base,baseline,roots);
  REQUIRE(baseline.get(dispatch));
  REQUIRE(baseline.get(dispatch)->branches().empty());
  for(auto address:{body,leaf}) {
    CAPTURE(address);
    const auto *before=baseline.get(address);
    const auto *after=actual.get(address);
    REQUIRE(before);
    REQUIRE(after);
    CHECK(after->bytes()==before->bytes());
    CHECK(after->size()==before->size());
    CHECK(after->isBlockStart()==before->isBlockStart());
    CHECK(after->isFunctionStart()==before->isFunctionStart());
    CHECK(actual.isValidBlock(address)==baseline.isValidBlock(address));
    const auto *oldIR=baseline.getIGNILBlock(address);
    const auto *newIR=actual.getIGNILBlock(address);
    CHECK((newIR!=nullptr)==(oldIR!=nullptr));
    if(oldIR) {
      REQUIRE(newIR);
      CHECK(dragon::ignil::toString(*newIR)==dragon::ignil::toString(*oldIR));
    }
  }
}
} // namespace

TEST_CASE("ARM table recovery revalidates content after a newly decoded back edge",
          "[arm64][analysis][table-closure]") {
  for(auto strategy:{dragon::DisassemblyStrategy::LINEAR_SWEEP,
                     dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL})for(bool alias:{false,true}) {
    CAPTURE(strategy,alias);
    const auto config=configuration(strategy);
    dragon::Disassembler dis{dragon::Architecture::ARM64,config};
    dragon::Graph graph{dragon::Architecture::ARM64,config};
    const auto bytes=fixture(alias);
    // The new arm really is a fully lifted STORE, rather than an unsupported
    // instruction or missing decode which could conceal the incoming path.
    const auto instructions=dis.disassemble(std::span(bytes).subspan(body-base,8),body);
    REQUIRE(instructions.size()==2);
    const auto lifted=dis.liftTrace(instructions);
    REQUIRE(lifted.unlifted().empty());
    CHECK(std::any_of(lifted.ops().begin(),lifted.ops().end(),[](const auto &op) {
      return op.opcode==dragon::ignil::Opcode::STORE;
    }));
    const auto extra=hints();
    dis.disassemble(bytes,base,graph,extra);
    storesAreReal(graph);
    const auto *jump=graph.get(dispatch);
    REQUIRE(jump);
    const auto expected=alias?std::set<std::uint64_t>{}:std::set<std::uint64_t>{body,leaf};
    CHECK(jump->branches()==expected);
    REQUIRE(graph.get(0x104c));
    CHECK(graph.get(0x104c)->branches()==std::set<std::uint64_t>{fallback});
    if(alias) {
      if(strategy==dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL) {
        CHECK_FALSE(graph.isValidBlock(body));
        CHECK_FALSE(graph.isValidBlock(leaf));
        CHECK(graph.getIGNILBlock(body)==nullptr);
        CHECK(graph.getIGNILBlock(leaf)==nullptr);
        CHECK(graph.get(body)==nullptr);
        CHECK(graph.get(leaf)==nullptr);
      } else linearNodesRemain(dis,config,bytes,graph);
    } else {
      REQUIRE(graph.get(body));
      REQUIRE(graph.get(leaf));
      CHECK(graph.isValidBlock(body));
      CHECK(graph.isValidBlock(leaf));
      const auto *borrowed=graph.get(base);
      const auto *borrowedIR=graph.getIGNILBlock(base);
      dis.disassemble(bytes,base,graph,extra);
      CHECK(graph.get(base)==borrowed);
      CHECK(graph.getIGNILBlock(base)==borrowedIR);
      CHECK(graph.get(dispatch)->branches()==expected);
    }
  }
}

TEST_CASE("ARM table target validation rejects the entire partially invalid set",
          "[arm64][analysis][table-closure]") {
  for(auto strategy:{dragon::DisassemblyStrategy::LINEAR_SWEEP,
                     dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
    CAPTURE(strategy);
    const auto config=configuration(strategy);
    dragon::Disassembler dis{dragon::Architecture::ARM64,config};
    dragon::Graph graph{dragon::Architecture::ARM64,config};
    const auto bytes=fixture(false,true);
    const auto extra=hints(true);
    dis.disassemble(bytes,base,graph,extra);
    storesAreReal(graph);
    REQUIRE(graph.get(dispatch));
    CHECK(graph.get(dispatch)->branches().empty());
    if(strategy==dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL) {
      CHECK_FALSE(graph.isValidBlock(body));
      CHECK(graph.getIGNILBlock(body)==nullptr);
      CHECK(graph.get(body)==nullptr);
    } else linearNodesRemain(dis,config,bytes,graph);
  }
}

TEST_CASE("ARM recovery preserves explicit branches and borrowed instructions",
          "[arm64][analysis][table-closure]") {
  const auto config=configuration(dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL);
  dragon::Disassembler dis{dragon::Architecture::ARM64,config};
  dragon::Graph graph{dragon::Architecture::ARM64,config};
  const auto bytes=fixture(true);
  const auto extra=hints();
  dis.disassemble(bytes,base,graph,extra);
  auto *jump=graph.get(dispatch);
  REQUIRE(jump);
  jump->addBranch(fallback); // An explicit trusted caller edge is not inference.
  jump->setEdges(jump->branches().size());
  const auto *borrowed=graph.get(base);
  dis.disassemble(bytes,base,graph,extra);
  CHECK(graph.get(base)==borrowed);
  CHECK(graph.get(dispatch)==jump);
  CHECK(jump->branches()==std::set<std::uint64_t>{fallback});
  CHECK(graph.get(0x104c)->branches()==std::set<std::uint64_t>{fallback});
}
