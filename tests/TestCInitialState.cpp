#include <catch2/catch_test_macros.hpp>
#include "../c_shim/dragon_c.h"

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Fixture {
  dragon_graph_t *graph = nullptr;
  explicit Fixture(const std::vector<uint8_t> &code) {
    auto *config=dragon_config_create(DRAGON_SYNTAX_INTEL,DRAGON_STRATEGY_RECURSIVE_TRAVERSAL);
    REQUIRE(config);
    auto *dis=dragon_disasm_create(DRAGON_ARCH_X86_64,config);
    dragon_config_destroy(config);REQUIRE(dis);
    std::array<char,256> error{};
    graph=dragon_disasm_analyze_with_hints(dis,code.data(),code.size(),0x1000,nullptr,error.data(),error.size());
    dragon_disasm_destroy(dis);REQUIRE(graph);REQUIRE(error[0]=='\0');
  }
  ~Fixture() {dragon_graph_destroy(graph);}
  Fixture(const Fixture &)=delete;
};
std::string owned(char *result) {
  REQUIRE(result);
  std::unique_ptr<char,decltype(&dragon_cstr_free)> text(result,dragon_cstr_free);
  return std::string{text.get()};
}
std::string query(const dragon_graph_t *graph,const dragon_finite_entry_state_t *states,
    size_t count,uint8_t width=32,uint32_t facts=262144) {
  const uint64_t root=0x1000;
  return owned(dragon_graph_finite_values_with_initial_state(graph,&root,1,root,0,width,
      64,4096,1000000,facts,0,states,count));
}
bool has(const std::string &text,const char *part) {return text.find(part)!=std::string::npos;}
} // namespace

TEST_CASE("C initial facts are owned conditional queries and leave the graph intact",
          "[c-api][finite-initial-state]") {
  // mov eax, dword ptr [0x4000]; ret
  Fixture fixture{{0x8b,0x04,0x25,0,0x40,0,0,0xc3}};
  const auto *ir=dragon_graph_get_ignil_block(fixture.graph,0x1000);REQUIRE(ir);
  const auto snapshot=owned(dragon_ignil_block_to_string(ir));
  const auto size=dragon_graph_size(fixture.graph);
  const auto baseline=owned(dragon_graph_finite_values(fixture.graph,nullptr,0,0x1000,0,32,
      64,4096,1000000,262144));
  CHECK(has(baseline,"\"kind\":2"));CHECK_FALSE(has(baseline,"conditional"));
  std::array<uint8_t,4> bytes{0x44,0x33,0x22,0x11};
  dragon_finite_memory_fact_t memory{0x4000,bytes.data(),bytes.size()};
  dragon_finite_entry_state_t state{0x1000,nullptr,0,&memory,1};
  const auto result=query(fixture.graph,&state,1);
  CHECK(has(result,"\"conditional\":true"));CHECK(has(result,"\"assumptions_valid\":true"));
  CHECK(has(result,"\"complete\":true"));CHECK(has(result,"\"values\":[\"0x11223344\"]"));
  CHECK(has(result,"\"bytes\":\"44332211\""));
  bytes.fill(0xff);
  CHECK(has(result,"44332211"));CHECK(has(query(fixture.graph,&state,1),"0xFFFFFFFF"));
  size_t load=std::numeric_limits<size_t>::max();
  dragon_ignil_block_each_op(ir,+[](uint64_t,dragon_ignil_opcode_t op,size_t index,void *user) {
    if(op==DRAGON_IGNIL_LOAD)*static_cast<size_t *>(user)=index;
  },&load);
  REQUIRE(load!=std::numeric_limits<size_t>::max());
  const std::array queries{dragon_finite_memory_query_t{0x1000,load,0x4000,32},
      dragon_finite_memory_query_t{0x1000,dragon_ignil_block_size(ir),0x4000,32}};
  const auto cells=owned(dragon_graph_finite_memory_values_with_initial_state(fixture.graph,
      nullptr,0,queries.data(),queries.size(),64,4096,1000000,262144,0,&state,1));
  CHECK(has(cells,"\"kind\":1"));CHECK(has(cells,"\"kind\":2"));
  CHECK(dragon_graph_size(fixture.graph)==size);
  CHECK(owned(dragon_ignil_block_to_string(ir))==snapshot);
  CHECK(owned(dragon_graph_finite_values(fixture.graph,nullptr,0,0x1000,0,32,
      64,4096,1000000,262144))==baseline);
}

TEST_CASE("C initial register constraints do not imply architectural writes",
          "[c-api][finite-initial-state]") {
  Fixture fixture{{0x90,0xc3}};
  std::array facts{dragon_finite_register_fact_t{0,32,0x89abcdef},
      dragon_finite_register_fact_t{4,32,0x12345678}};
  dragon_finite_entry_state_t state{0x1000,facts.data(),1,nullptr,0};
  CHECK(has(query(fixture.graph,&state,1,64),"\"kind\":2"));
  state.register_count=2;
  CHECK(has(query(fixture.graph,&state,1,64),"0x1234567889ABCDEF"));
  facts[1].offset=0;
  const auto invalid=query(fixture.graph,&state,1,64);
  CHECK(has(invalid,"\"assumptions_valid\":false"));CHECK(has(invalid,"\"complete\":false"));
  CHECK(has(invalid,"\"kind\":2"));
  Fixture write{{0xb8,0xef,0xcd,0xab,0x89,0xc3}};
  state.register_count=1;
  CHECK(has(query(write.graph,&state,1,64),"\"values\":[\"0x89ABCDEF\"]"));
}

TEST_CASE("C initial state validates pointer counts structure and budgets",
          "[c-api][finite-initial-state]") {
  Fixture fixture{{0x90,0xc3}};
  auto call=[&](const uint64_t *entries,size_t count,const dragon_finite_entry_state_t *states,
      size_t stateCount) {
    return dragon_graph_finite_values_with_initial_state(fixture.graph,entries,count,
        0x1000,0,64,64,4096,1000000,262144,0,states,stateCount);
  };
  CHECK(call(nullptr,1,nullptr,0)==nullptr);
  const uint64_t root=0x1000;
  CHECK(call(&root,262145,nullptr,0)==nullptr);
  CHECK(call(nullptr,0,nullptr,1)==nullptr);
  dragon_finite_entry_state_t state{0x1000,nullptr,1,nullptr,0};
  CHECK(call(nullptr,0,&state,1)==nullptr);
  state.register_count=0;state.memory_count=1;
  CHECK(call(nullptr,0,&state,1)==nullptr);
  const std::array<uint8_t,2> bytes{1,2};
  dragon_finite_memory_fact_t memory{UINT64_MAX,bytes.data(),bytes.size()};state.memory=&memory;
  CHECK(has(query(fixture.graph,&state,1),"\"assumptions_valid\":false"));
  memory={0x4000,nullptr,1};CHECK(call(nullptr,0,&state,1)==nullptr);
  memory={0x4000,bytes.data(),262145};CHECK(call(nullptr,0,&state,1)==nullptr);
  state={0x1000,nullptr,0,nullptr,0};
  const std::array duplicate{state,state};
  CHECK(has(query(fixture.graph,duplicate.data(),2),"\"assumptions_valid\":false"));
  state.entry=0x1001;
  CHECK(has(query(fixture.graph,&state,1),"\"assumptions_valid\":false"));
  state.entry=0x1000;
  CHECK(has(query(fixture.graph,&state,1,64,0),"\"complete\":false"));
  CHECK(dragon_graph_finite_memory_values_with_initial_state(fixture.graph,nullptr,0,
      nullptr,1,64,4096,1000000,262144,0,nullptr,0)==nullptr);
  CHECK(dragon_graph_finite_values_with_initial_state(nullptr,nullptr,0,
      0,0,0,64,4096,1000000,262144,0,nullptr,0)==nullptr);
}
