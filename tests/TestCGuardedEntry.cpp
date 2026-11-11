#include <catch2/catch_test_macros.hpp>
#include "../c_shim/dragon_c.h"
#include <array>
#include <limits>
#include <memory>
#include <string>

namespace {
using Owner = std::unique_ptr<dragon_llvm_lifter_t, decltype(&dragon_llvm_lifter_destroy)>;
std::string take(char *p) {
  REQUIRE(p);std::unique_ptr<char,decltype(&dragon_cstr_free)> value(p,dragon_cstr_free);
  return value.get();
}
Owner source() {
  const std::array<uint8_t,9> code{0x48,0x8b,0x04,0x25,0,0x40,0,0,0xc3};
  auto *config=dragon_config_create(DRAGON_SYNTAX_INTEL,DRAGON_STRATEGY_RECURSIVE_TRAVERSAL);
  auto *dis=dragon_disasm_create(DRAGON_ARCH_X86_64,config);dragon_config_destroy(config);REQUIRE(dis);
  std::array<char,256> error{};
  auto *graph=dragon_disasm_analyze_with_hints(dis,code.data(),code.size(),0x1000,nullptr,error.data(),error.size());
  dragon_disasm_destroy(dis);REQUIRE(graph);REQUIRE(error[0]=='\0');
  Owner lifter(dragon_llvm_lifter_create(DRAGON_ARCH_X86_64,"guarded-c"),dragon_llvm_lifter_destroy);REQUIRE(lifter);
  REQUIRE(dragon_llvm_lifter_set_execution_mode(lifter.get(),DRAGON_LLVM_STATIC_ANALYSIS));
  REQUIRE(dragon_llvm_lifter_set_static_entry_policy(lifter.get(),DRAGON_LLVM_ENTRY_FUNCTION_START));
  const auto *fn=dragon_graph_get_function(graph,0x1000);REQUIRE(fn);
  REQUIRE(dragon_llvm_lifter_lift(lifter.get(),fn,graph));dragon_graph_destroy(graph);
  return lifter;
}
}
TEST_CASE("C guarded entry owns its module and copied candidate bytes", "[c-api][guarded-entry]") {
  auto original=source();const auto before=take(dragon_llvm_lifter_ir(original.get()));
  CHECK(take(dragon_llvm_lifter_guarded_entry_manifest(original.get()))=="{}");
  std::array<uint8_t,8> bytes{0x44,0x33,0x22,0x11,0,0,0,0};
  dragon_finite_memory_fact_t cell{0x4000,bytes.data(),bytes.size()};
  dragon_finite_entry_state_t facts{0x1000,nullptr,0,&cell,1};
  char *error=nullptr;
  Owner view(dragon_llvm_lifter_create_guarded_entry_view(original.get(),&facts,"test","C binding fixture",&error),dragon_llvm_lifter_destroy);
  INFO((error?error:""));REQUIRE(view);CHECK(error==nullptr);if(error)dragon_cstr_free(error);
  CHECK(take(dragon_llvm_lifter_ir(original.get()))==before);
  const auto manifest=take(dragon_llvm_lifter_guarded_entry_manifest(view.get()));
  CHECK(manifest.find(".guarded.test")!=std::string::npos);
  CHECK(manifest.find("C binding fixture")!=std::string::npos);
  bytes.fill(0xff);original.reset();
  CHECK(take(dragon_llvm_lifter_guarded_entry_manifest(view.get()))==manifest);
  dragon_llvm_lifter_run_mem2reg(view.get());dragon_llvm_lifter_run_gvn(view.get());
  const auto ir=take(dragon_llvm_lifter_ir(view.get()));
  CHECK(ir.find(".guarded.test")!=std::string::npos);
  CHECK(ir.find("@dragon_static_entry_x86_64_1000(")!=std::string::npos);
}
TEST_CASE("C guarded entry rejects malformed buffers without mutating source", "[c-api][guarded-entry]") {
  auto original=source();const auto before=take(dragon_llvm_lifter_ir(original.get()));
  uint8_t byte=1;dragon_finite_memory_fact_t cell{0x4000,&byte,1};
  dragon_finite_register_fact_t reg{32,64,0xc7f8};
  dragon_finite_entry_state_t facts{0x1000,nullptr,0,&cell,1};
  auto reject=[&](const dragon_finite_entry_state_t *input,const char *id="test",const char *origin="C bad fixture") {
    char *error=nullptr;auto *view=dragon_llvm_lifter_create_guarded_entry_view(original.get(),input,id,origin,&error);
    CHECK(view==nullptr);if(view)dragon_llvm_lifter_destroy(view);
    REQUIRE(error);CHECK(std::string(error).size()>0);dragon_cstr_free(error);
    CHECK(take(dragon_llvm_lifter_ir(original.get()))==before);
  };
  reject(nullptr);reject(&facts,nullptr);reject(&facts,"test",nullptr);reject(&facts,"");
  reject(&facts,"bad space");reject(&facts,"test","");
  facts.memory_count=257;reject(&facts);facts.memory_count=1;
  facts.memory=nullptr;reject(&facts);facts.memory=&cell;
  cell.bytes=nullptr;reject(&facts);cell.bytes=&byte;
  cell.size=4097;reject(&facts);cell.size=0;reject(&facts);cell.size=1;
  cell.address=std::numeric_limits<uint64_t>::max();reject(&facts);cell.address=0x4000;
  facts.registers=&reg;facts.register_count=1;
  reg.width=1;reject(&facts);reg.width=8;reg.value=256;reject(&facts);
  reg.width=64;reg.value=0;reg.offset=std::numeric_limits<uint32_t>::max();reject(&facts);
  CHECK(dragon_llvm_lifter_create_guarded_entry_view(nullptr,&facts,"test","none",nullptr)==nullptr);
  CHECK(dragon_llvm_lifter_guarded_entry_manifest(nullptr)==nullptr);
}
