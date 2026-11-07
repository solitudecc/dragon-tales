#include <catch2/catch_test_macros.hpp>
#include "../c_shim/dragon_c.h"

#include <array>
#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

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
  CHECK(dragon_analysis_hints_set_known_memory_reader(nullptr, nullptr, nullptr) == 0);

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

TEST_CASE("C trusted contents report exact LOAD sites and require every entry",
          "[c-api][hints][table-contents]") {
  struct Request {uint64_t block, pc, address; size_t index, size;};
  struct Reader {
    std::array<uint8_t, 24> table{};
    std::vector<Request> requests;
    size_t available = 24;
    int result = 1;
  };
  const auto knownReader = +[](uint64_t block, size_t index, uint64_t pc,
      uint64_t address, uint8_t *out, size_t size, void *user) -> int {
    auto &reader = *static_cast<Reader *>(user);
    reader.requests.push_back({block, pc, address, index, size});
    if (address < 0x2000 || address - 0x2000 > reader.available ||
        size > reader.available - (address - 0x2000)) return 0;
    std::copy_n(reader.table.data() + (address - 0x2000), size, out);
    return reader.result;
  };
  auto branches = [](const dragon_graph_t *graph) {
    const auto *jump = dragon_graph_get_instruction(graph, 0x100b);
    REQUIRE(jump);
    std::set<uint64_t> result;
    dragon_instruction_each_branch(jump, +[](uint64_t address, void *user) {
      static_cast<std::set<uint64_t> *>(user)->insert(address);
    }, &result);
    return result;
  };
  for (auto strategy : {DRAGON_STRATEGY_LINEAR_SWEEP, DRAGON_STRATEGY_RECURSIVE_TRAVERSAL}) {
    auto *config = dragon_config_create(DRAGON_SYNTAX_INTEL, strategy);
    REQUIRE(config);
    auto *dis = dragon_disasm_create(DRAGON_ARCH_X86_64, config);
    dragon_config_destroy(config);
    REQUIRE(dis);
    std::vector<uint8_t> code(0x70, 0xcc);
    const std::array<uint8_t, 18> prefix{0x83,0xff,2,0x0f,0x87,0x57,0,0,0,
        0x89,0xff,0xff,0x24,0xfd,0,0x20,0,0};
    std::copy(prefix.begin(), prefix.end(), code.begin());
    for (auto offset : {0x30, 0x40, 0x50, 0x60}) code[offset] = 0xc3;
    Reader reader;
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned b = 0; b < 8; ++b)
        reader.table[i * 8 + b] = static_cast<uint8_t>((uint64_t{0x1030} + i * 16) >> (b * 8));
    auto *hints = dragon_analysis_hints_create();
    REQUIRE(hints);
    CHECK(dragon_analysis_hints_set_known_memory_reader(hints, nullptr, &reader) == 1);
    const auto initialReader = +[](uint64_t address, uint8_t *out, size_t size, void *user) -> int {
      const auto &table = static_cast<Reader *>(user)->table;
      if (address < 0x2000 || address - 0x2000 > table.size() ||
          size > table.size() - (address - 0x2000)) return 0;
      std::copy_n(table.data() + (address - 0x2000), size, out);
      return 1;
    };
    REQUIRE(dragon_analysis_hints_set_memory_reader(hints, initialReader, &reader));
    std::array<char, 256> error{};
    auto analyze = [&] {
      auto *graph = dragon_disasm_analyze_with_hints(dis, code.data(), code.size(),
          0x1000, hints, error.data(), error.size());
      REQUIRE(graph);
      CHECK(error[0] == '\0');
      return graph;
    };
    auto *unknown = analyze();
    CHECK(branches(unknown).empty());
    dragon_graph_destroy(unknown);
    REQUIRE(dragon_analysis_hints_set_known_memory_reader(hints, knownReader, &reader));
    auto *known = analyze();
    CHECK(branches(known) == std::set<uint64_t>{0x1030, 0x1040, 0x1050});
    REQUIRE_FALSE(reader.requests.empty());
    std::set<uint64_t> slots;
    const auto *ir = dragon_graph_get_ignil_block(known, 0x1009);
    REQUIRE(ir);
    using Operation = std::tuple<uint64_t, dragon_ignil_opcode_t, size_t>;
    std::vector<Operation> operations;
    dragon_ignil_block_each_op(ir, +[](uint64_t pc, dragon_ignil_opcode_t opcode,
        size_t index, void *user) {
      static_cast<std::vector<Operation> *>(user)->emplace_back(pc, opcode, index);
    }, &operations);
    for (const auto &request : reader.requests) {
      CHECK(request.block == 0x1009);
      CHECK(request.pc == 0x100b);
      CHECK(request.size == 8);
      CHECK(request.address >= 0x2000);
      CHECK(request.address + request.size <= 0x2018);
      CHECK(std::find(operations.begin(), operations.end(),
          Operation{request.pc, DRAGON_IGNIL_LOAD, request.index}) != operations.end());
      slots.insert(request.address);
    }
    CHECK(slots == std::set<uint64_t>{0x2000, 0x2008, 0x2010});
    dragon_graph_destroy(known);
    reader.available = 16; // One unproved entry invalidates the complete set.
    auto *partial = analyze();
    CHECK(branches(partial).empty());
    dragon_graph_destroy(partial);
    reader.available = 24;
    reader.result = 2; // Exactly 1, rather than any truthy int, is required.
    auto *bad = analyze();
    CHECK(branches(bad).empty());
    dragon_graph_destroy(bad);
    reader.result = -1; // The new protocol distinguishes errors from unknown.
    CHECK(dragon_disasm_analyze_with_hints(dis, code.data(), code.size(),
        0x1000, hints, error.data(), error.size()) == nullptr);
    CHECK(std::string{error.data()} == "known memory reader failed");
    REQUIRE(dragon_analysis_hints_set_known_memory_reader(hints, nullptr, nullptr));
    auto *cleared = analyze();
    CHECK(branches(cleared).empty());
    dragon_analysis_hints_destroy(hints);
    CHECK(branches(cleared).empty());
    dragon_graph_destroy(cleared);
    dragon_disasm_destroy(dis);
  }
}

TEST_CASE("C image and finite dispatch routes retain explicit contents hints",
          "[c-api][hints][table-contents][hint-routing]") {
  // A real ELF: metadata entry 1000, an extra caller entry 1080, and a
  // separate allocated read-only table. Its section flag is not a proof.
  std::vector<uint8_t> code(0x90, 0xcc);
  const std::array<uint8_t, 18> prefix{0x83,0xff,2,0x0f,0x87,0x57,0,0,0,
      0x89,0xff,0xff,0x24,0xfd,0,0x20,0,0};
  std::copy(prefix.begin(), prefix.end(), code.begin());
  for (auto offset : {0x30, 0x40, 0x50, 0x60, 0x80}) code[offset] = 0xc3;
  auto put = [](std::vector<uint8_t> &out, size_t offset, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) out[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  };
  struct Reader {std::vector<uint8_t> table = std::vector<uint8_t>(24); int result = 1; size_t calls = 0;};
  Reader reader;
  for (unsigned i = 0; i < 3; ++i) put(reader.table, i * 8, 0x1030 + i * 16, 8);
  const std::string names{"\0.text\0.rodata\0.shstrtab\0", 25};
  const size_t tableOffset = 64 + code.size(), namesOffset = tableOffset + reader.table.size();
  const size_t sectionsOffset = (namesOffset + names.size() + 7) & ~size_t{7};
  std::vector<uint8_t> elf(sectionsOffset + 4 * 64);
  const std::array<uint8_t, 7> ident{0x7f, 'E', 'L', 'F', 2, 1, 1};
  std::copy(ident.begin(), ident.end(), elf.begin());
  put(elf, 16, 2, 2); put(elf, 18, 62, 2); put(elf, 20, 1, 4);
  put(elf, 24, 0x1000, 8); put(elf, 40, sectionsOffset, 8);
  put(elf, 52, 64, 2); put(elf, 58, 64, 2); put(elf, 60, 4, 2); put(elf, 62, 3, 2);
  std::copy(code.begin(), code.end(), elf.begin() + 64);
  std::copy(reader.table.begin(), reader.table.end(), elf.begin() + tableOffset);
  std::copy(names.begin(), names.end(), elf.begin() + namesOffset);
  auto section = [&](size_t index, uint32_t name, uint32_t type, uint64_t flags,
      uint64_t address, size_t offset, size_t size) {
    const size_t at = sectionsOffset + index * 64;
    put(elf, at, name, 4); put(elf, at + 4, type, 4); put(elf, at + 8, flags, 8);
    put(elf, at + 16, address, 8); put(elf, at + 24, offset, 8); put(elf, at + 32, size, 8);
    put(elf, at + 48, 1, 8);
  };
  section(1, 1, 1, 6, 0x1000, 64, code.size());
  section(2, 7, 1, 2, 0x2000, tableOffset, reader.table.size());
  section(3, 15, 3, 0, 0, namesOffset, names.size());
  const auto callback = +[](uint64_t, size_t, uint64_t, uint64_t address,
      uint8_t *out, size_t size, void *user) -> int {
    auto &reader = *static_cast<Reader *>(user);
    ++reader.calls;
    if (address < 0x2000 || address - 0x2000 > reader.table.size() ||
        size > reader.table.size() - (address - 0x2000)) return 0;
    std::copy_n(reader.table.begin() + (address - 0x2000), size, out);
    return reader.result;
  };
  auto branches = [](const dragon_graph_t *graph) {
    const auto *jump = dragon_graph_get_instruction(graph, 0x100b);
    REQUIRE(jump);
    std::set<uint64_t> result;
    dragon_instruction_each_branch(jump, +[](uint64_t address, void *user) {
      static_cast<std::set<uint64_t> *>(user)->insert(address);
    }, &result);
    return result;
  };
  CHECK(dragon_loader_disassemble_with_hints(nullptr, nullptr, nullptr) == nullptr);
  CHECK(dragon_pass_pipeline_register_finite_dispatch_with_hints(
      nullptr, "recover", code.data(), code.size(), 0x1000, nullptr, 0, nullptr) == 0);
  for (auto strategy : {DRAGON_STRATEGY_LINEAR_SWEEP, DRAGON_STRATEGY_RECURSIVE_TRAVERSAL}) {
    reader.result = 1;
    auto *config = dragon_config_create(DRAGON_SYNTAX_INTEL, strategy);
    REQUIRE(config);
    std::array<char, 256> error{};
    auto *image = dragon_load_bytes(elf.data(), elf.size(), error.data(), error.size());
    REQUIRE(image);
    auto *hints = dragon_analysis_hints_create();
    REQUIRE(hints);
    const uint64_t extraEntry = 0x1080, rootEntry = 0x1000;
    REQUIRE(dragon_analysis_hints_set_entries(hints, &extraEntry, 1));
    REQUIRE(dragon_analysis_hints_set_known_memory_reader(hints, callback, &reader));
    auto *unknown = dragon_loader_disassemble(image, config);
    REQUIRE(unknown);
    CHECK(branches(unknown).empty());
    dragon_graph_destroy(unknown);
    auto *known = dragon_loader_disassemble_with_hints(image, config, hints);
    REQUIRE(known);
    CHECK(branches(known) == std::set<uint64_t>{0x1030, 0x1040, 0x1050});
    for (auto address : {rootEntry, extraEntry}) {
      auto *function = dragon_graph_get_function(known, address);
      REQUIRE(function);
      CHECK(dragon_function_valid(function));
      dragon_function_destroy(function);
    }
    dragon_graph_destroy(known);
    reader.result = -1;
    CHECK(dragon_loader_disassemble_with_hints(image, config, hints) == nullptr);
    reader.result = 1;
    auto *pipeline = dragon_pass_pipeline_create(0);
    REQUIRE(pipeline);
    REQUIRE(dragon_pass_pipeline_register_finite_dispatch_with_hints(pipeline,
        "recover", code.data(), code.size(), rootEntry, &rootEntry, 1, hints));
    REQUIRE(dragon_pass_pipeline_add(pipeline, "recover", "{}"));
    size_t after = 0;
    REQUIRE(dragon_pass_pipeline_register(pipeline, "after", DRAGON_PASS_GRAPH,
        +[](void *, uint64_t, const char *, void *user, dragon_pass_result_t *out) {
          ++*static_cast<size_t *>(user);
          *out = {DRAGON_PASS_SUCCESS, 0, "", "{}"};
        }, &after, 1, "[]"));
    REQUIRE(dragon_pass_pipeline_add(pipeline, "after", "{}"));
    // Native registration owns its hints copy; only callback/userdata remain
    // borrowed until the pipeline is destroyed.
    REQUIRE(dragon_analysis_hints_set_known_memory_reader(hints, nullptr, nullptr));
    dragon_analysis_hints_destroy(hints);
    auto *dis = dragon_disasm_create(DRAGON_ARCH_X86_64, config);
    REQUIRE(dis);
    auto *target = dragon_disasm_analyze(dis, code.data(), code.size(), rootEntry);
    REQUIRE(target);
    CHECK(branches(target).empty());
    auto *report = dragon_pass_pipeline_run_graph(pipeline, target, 1, 64);
    REQUIRE(report);
    CHECK(std::string{report}.find("\"ok\":true") != std::string::npos);
    dragon_cstr_free(report);
    CHECK(branches(target) == std::set<uint64_t>{0x1030, 0x1040, 0x1050});
    CHECK(after == 1);
    auto *extra = dragon_graph_get_function(target, extraEntry);
    REQUIRE(extra);
    CHECK(dragon_function_valid(extra));
    dragon_function_destroy(extra);
    dragon_graph_destroy(target);
    reader.result = -1;
    after = 0;
    target = dragon_disasm_analyze(dis, code.data(), code.size(), rootEntry);
    REQUIRE(target);
    report = dragon_pass_pipeline_run_graph(pipeline, target, 1, 64);
    REQUIRE(report);
    CHECK(std::string{report}.find("\"ok\":false") != std::string::npos);
    CHECK(std::string{report}.find("known memory reader failed") != std::string::npos);
    CHECK(after == 0);
    dragon_cstr_free(report);
    dragon_graph_destroy(target);
    dragon_pass_pipeline_destroy(pipeline);
    dragon_disasm_destroy(dis);
    dragon_loader_destroy(image);
    dragon_config_destroy(config);
  }
}
