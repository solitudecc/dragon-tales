#include <catch2/catch_test_macros.hpp>
#include "../c_shim/dragon_c.h"

#include <array>
#include <string>

TEST_CASE("C Legacy refuses unlifted memory effects before creating LLVM IR",
          "[c-api][legacy-unlifted]") {
  for (auto arch : {DRAGON_ARCH_X86_32, DRAGON_ARCH_X86_64}) {
    auto *config = dragon_config_create(DRAGON_SYNTAX_INTEL, DRAGON_STRATEGY_LINEAR_SWEEP);
    REQUIRE(config);
    auto *dis = dragon_disasm_create(arch, config);
    dragon_config_destroy(config);
    REQUIRE(dis);
    // MOVBE [EBX/RBX], EAX is unsupported and has no register definitions.
    const std::array<uint8_t, 5> code{0x0f, 0x38, 0xf1, 0x03, 0xc3};
    std::array<char, 256> error{};
    auto *graph = dragon_disasm_analyze_with_hints(dis, code.data(), code.size(),
        0x1000, nullptr, error.data(), error.size());
    REQUIRE(graph);
    auto *function = dragon_graph_get_function(graph, 0x1000);
    REQUIRE(function);
    auto *lifter = dragon_llvm_lifter_create(arch, "refusal");
    REQUIRE(lifter);
    auto *before = dragon_llvm_lifter_ir(lifter);
    REQUIRE(before);
    const std::string snapshot{before};
    dragon_cstr_free(before);
    CHECK(dragon_llvm_lifter_lift(lifter, function, graph) == 0);
    auto *after = dragon_llvm_lifter_ir(lifter);
    REQUIRE(after);
    CHECK(std::string{after} == snapshot);
    dragon_cstr_free(after);
    dragon_llvm_lifter_destroy(lifter);
    dragon_function_destroy(function);
    dragon_graph_destroy(graph);
    dragon_disasm_destroy(dis);
  }
}

TEST_CASE("C LLVM static mode is an explicit opt in", "[c-api][static]") {
  auto *lifter=dragon_llvm_lifter_create(DRAGON_ARCH_X86_64,"static"); REQUIRE(lifter);
  CHECK(dragon_llvm_lifter_set_execution_mode(lifter,static_cast<dragon_llvm_execution_mode_t>(2))==1);
  CHECK(dragon_llvm_lifter_set_execution_mode(lifter,static_cast<dragon_llvm_execution_mode_t>(99))==0);
  dragon_llvm_lifter_destroy(lifter);
}

TEST_CASE("C analysis hints own entries and report invalid inputs", "[c-api][hints]") {
  auto *hints = dragon_analysis_hints_create();
  REQUIRE(hints);
  std::array<uint64_t, 2> entries{0x1000, 0x1002};
  REQUIRE(dragon_analysis_hints_set_entries(hints, entries.data(), entries.size()));
  entries[1] = 0;
  uint64_t entry = 0;
  CHECK(dragon_analysis_hints_entry_count(hints) == 2);
  CHECK(dragon_analysis_hints_entry(hints, 1, &entry) == 1);
  CHECK(entry == 0x1002);
  CHECK(dragon_analysis_hints_set_entries(hints, nullptr, 1) == 0);
  CHECK(dragon_analysis_hints_entry(hints, 2, &entry) == 0);
  CHECK(dragon_analysis_hints_entry(hints, 0, nullptr) == 0);
  CHECK(dragon_analysis_hints_set_memory_reader(nullptr, nullptr, nullptr) == 0);

  auto *config = dragon_config_create(DRAGON_SYNTAX_INTEL, DRAGON_STRATEGY_RECURSIVE_TRAVERSAL);
  REQUIRE(config);
  CHECK(dragon_config_calling_convention(config) == DRAGON_CC_DEFAULT);
  CHECK(dragon_config_set_calling_convention(config, DRAGON_CC_WIN64) == 1);
  CHECK(dragon_config_calling_convention(config) == DRAGON_CC_WIN64);
  CHECK(dragon_config_set_calling_convention(config, static_cast<dragon_calling_convention_t>(99)) == 0);
  CHECK(dragon_config_calling_convention(config) == DRAGON_CC_WIN64);
  auto *dis = dragon_disasm_create(DRAGON_ARCH_X86_64, config);
  dragon_config_destroy(config);
  REQUIRE(dis);
  const std::array<uint8_t, 3> code{0xc3, 0xcc, 0xc3};
  std::array<char, 64> error{};
  auto *graph = dragon_disasm_analyze_with_hints(dis, code.data(), code.size(),
                                                 0x1000, hints, error.data(), error.size());
  REQUIRE(graph);
  CHECK(error[0] == '\0');
  dragon_analysis_hints_destroy(hints);
  const auto *insn = dragon_graph_get_instruction(graph, 0x1002);
  REQUIRE(insn);
  CHECK(dragon_instruction_is_function_start(insn) == 1);
  dragon_graph_destroy(graph);
  CHECK(dragon_disasm_analyze_with_hints(nullptr, code.data(), code.size(), 0x1000,
                                          nullptr, error.data(), error.size()) == nullptr);
  CHECK(error[0] != '\0');
  std::array<char, 1> tiny{'x'};
  CHECK(dragon_disasm_analyze_with_hints(dis, nullptr, 0, 0, nullptr, tiny.data(), 1) == nullptr);
  CHECK(tiny[0] == '\0');
  dragon_disasm_destroy(dis);
  CHECK(dragon_analysis_hints_entry_count(nullptr) == 0);
  CHECK(dragon_analysis_hints_set_entries(nullptr, nullptr, 0) == 0);
  dragon_analysis_hints_destroy(nullptr);
}
