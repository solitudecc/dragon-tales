#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/IndirectTargets.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>

#ifdef DRAGON_WITH_UNICORN
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#endif

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {
constexpr std::uint64_t base = 0x1000, table = 0x3000;
constexpr std::uint64_t body = 0x1080, aliasPath = 0x10a0;
constexpr std::uint64_t leaf = 0x10c0, changed = 0x10d0, fallback = 0x1070;

void word(std::vector<std::uint8_t> &code, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    code.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void store(std::vector<std::uint8_t> &code, bool wide,
           std::uint32_t address, std::uint32_t value) {
  if (wide) code.insert(code.end(), {0x48,0xc7,0x04,0x25});
  else code.insert(code.end(), {0xc7,0x05});
  word(code,address);
  word(code,value);
}

struct Fixture {
  std::vector<std::uint8_t> code;
  std::uint64_t dispatch;
};

Fixture fixture(bool wide) {
  std::vector<std::uint8_t> prefix;
  store(prefix,wide,table,body);
  store(prefix,wide,table + (wide ? 8 : 4),leaf);
  const auto guard = base + prefix.size();
  // Use one register index, independent of the table's aliasing STORE. Two
  // separate index LOADs would also test memory-origin correlation at the
  // loop join, obscuring whether the decode limit was handled correctly.
  prefix.insert(prefix.end(), {0x89,0xf8,0x83,0xf8,1}); // MOV EAX,EDI; CMP EAX,1
  prefix.insert(prefix.end(), {0x77,
      static_cast<std::uint8_t>(fallback - (base + prefix.size() + 2))});
  const auto dispatch = base + prefix.size();
  prefix.insert(prefix.end(), {0xff,0x24,static_cast<std::uint8_t>(wide ? 0xc5 : 0x85)});
  word(prefix,table); // JMP [RAX/EAX * entrySize + table]
  Fixture result{std::vector<std::uint8_t>(0xf0,0xcc),dispatch};
  std::copy(prefix.begin(),prefix.end(),result.code.begin());

  // Four first-block instructions: three zero-op NOPs, then a direct JMP.
  // A limit of three fails inside that block; four leaves its real alias
  // path pending. Silently retaining either prefix must not certify a table.
  std::vector<std::uint8_t> first{0x90,0x90,0x90,0xe9};
  word(first,static_cast<std::uint32_t>(aliasPath - (body + first.size() + 4)));
  std::copy(first.begin(),first.end(),result.code.begin() + (body - base));

  std::vector<std::uint8_t> alias;
  store(alias,wide,table,changed);
  alias.push_back(0xe9);
  word(alias,static_cast<std::uint32_t>(guard - (aliasPath + alias.size() + 4)));
  std::copy(alias.begin(),alias.end(),result.code.begin() + (aliasPath - base));
  for (auto address : {fallback,leaf,changed}) result.code[address - base] = 0xc3;
  return result;
}

struct Snapshot {
  std::set<std::uint64_t> instructions,blocks,functions;
  std::map<std::uint64_t,std::string> ignil;
  bool operator==(const Snapshot &) const = default;
};

Snapshot snapshot(const dragon::Graph &graph) {
  Snapshot result;
  for (const auto &[address,instruction] : graph.listing()) result.instructions.insert(address);
  result.blocks = graph.validBlockAddresses();
  result.functions = graph.validFunctionAddresses();
  for (auto address : result.blocks)
    if (const auto *ir = graph.getIGNILBlock(address))
      result.ignil.emplace(address,dragon::ignil::toString(*ir));
  return result;
}

class DecodeLimit final : public dragon::analysis::IndirectTargetResolver {
 public:
  explicit DecodeLimit(std::uint32_t limit) : limit(limit) {}
  unsigned prepares = 0;
  std::vector<std::string> stoppedMessages;
  Snapshot initial;

  bool prepare(const dragon::Graph &graph,const std::set<std::uint64_t> &) override {
    if (++prepares == 1) initial = snapshot(graph);
    return true;
  }
  std::optional<std::vector<std::uint64_t>> targets(std::uint64_t) const override {
    return std::nullopt; // All targets below require real Guest STORE facts.
  }
  // Deliberately no override keyword: this test can also compile against the
  // pre-cap SDK, whose resolver ignores this new lower decode limit.
  virtual std::uint32_t maxNewInstructions() const { return limit; }
  void stopped(std::string_view message) override { stoppedMessages.emplace_back(message); }

 private:
  std::uint32_t limit;
};

dragon::Configuration configuration() {
  dragon::Configuration config;
  config.disassemblyStrategy = dragon::DisassemblyStrategy::RECURSIVE_TRAVERSAL;
#ifdef DRAGON_WITH_Z3
  config.useZ3ForPathDiscovery = false;
#endif
  return config;
}
} // namespace

TEST_CASE("Incomplete x86 case decoding withdraws the entire speculative closure",
          "[table-contents][decode-closure]") {
  for (bool wide : {false,true}) for (std::uint32_t limit : {0u,3u,4u}) {
    CAPTURE(wide,limit);
    const auto f = fixture(wide);
    const auto architecture = wide ? dragon::Architecture::X86_64 : dragon::Architecture::X86_32;
    const auto config = configuration();
    dragon::Disassembler dis{architecture,config};
    dragon::Graph graph{architecture,config};
    auto provider = std::make_shared<DecodeLimit>(limit);
    dragon::Disassembler::AnalysisHints hints;
    hints.entryPoints = {base};
    hints.indirectTargets = provider;
    dis.disassemble(f.code,base,graph,hints);

    REQUIRE(graph.get(f.dispatch));
    REQUIRE(provider->prepares > 0);
    CHECK(provider->prepares == 1);
    CHECK(provider->stoppedMessages == std::vector<std::string>{
        "indirect recovery decoding budget exhausted"});
    CHECK(graph.get(f.dispatch)->branches().empty());
    CHECK(graph.get(f.dispatch)->inferredBranches().empty());
    CHECK(snapshot(graph) == provider->initial);
    for (auto address : {body,aliasPath,leaf,changed}) {
      CHECK(graph.get(address) == nullptr);
      CHECK_FALSE(graph.isValidBlock(address));
      CHECK(graph.getIGNILBlock(address) == nullptr);
    }
  }
}

TEST_CASE("Complete x86 case decoding revalidates real alias writes before publishing",
          "[table-contents][decode-closure]") {
  for (bool wide : {false,true}) {
    CAPTURE(wide);
    const auto f = fixture(wide);
    const auto architecture = wide ? dragon::Architecture::X86_64 : dragon::Architecture::X86_32;
    const auto config = configuration();
    dragon::Disassembler dis{architecture,config};
    dragon::Graph graph{architecture,config};
    auto provider = std::make_shared<DecodeLimit>(16);
    dragon::Disassembler::AnalysisHints hints;
    hints.entryPoints = {base};
    hints.indirectTargets = provider;
    dis.disassemble(f.code,base,graph,hints);

    REQUIRE(graph.get(f.dispatch));
    CHECK(provider->prepares >= 2);
    CHECK(provider->stoppedMessages.empty());
    CHECK(graph.get(f.dispatch)->branches() == std::set<std::uint64_t>{body,leaf,changed});
    CHECK(graph.get(f.dispatch)->inferredBranches() == std::set<std::uint64_t>{body,leaf,changed});
    REQUIRE(graph.get(aliasPath));
    CHECK(graph.get(aliasPath)->bytes() == (wide
        ? std::vector<std::uint8_t>{0x48,0xc7,0x04,0x25,0x00,0x30,0,0,0xd0,0x10,0,0}
        : std::vector<std::uint8_t>{0xc7,0x05,0x00,0x30,0,0,0xd0,0x10,0,0}));
    bool aliasStore = false;
    for (auto address : graph.validBlockAddresses()) {
      const auto *ir = graph.getIGNILBlock(address);
      if (!ir) continue;
      for (const auto &op : ir->ops())
        if (op.address == aliasPath && op.opcode == dragon::ignil::Opcode::STORE) {
          aliasStore = true;
          REQUIRE(op.srcs.size() == 2);
          CHECK(dragon::ignil::widthOf(op.srcs[1]) == (wide ? 64 : 32));
        }
    }
    CHECK(aliasStore);
  }
}

#ifdef DRAGON_WITH_UNICORN
namespace {
struct CloseUnicorn {
  void operator()(uc_engine *engine) const { uc_close(engine); }
};
struct MachineWrite {
  std::uint64_t address;
  unsigned size;
  std::uint64_t value;
  bool operator==(const MachineWrite &) const = default;
};
struct MachineTrace {
  std::vector<std::uint64_t> instructions;
  std::vector<MachineWrite> writes;
};
} // namespace

TEST_CASE("Unicorn confirms x86 decode closure fixtures execute their real alias path",
          "[table-contents][decode-closure][unicorn]") {
  constexpr std::uint64_t stack = 0x8800, sentinel = 0xf000;
  constexpr std::size_t memorySize = 0x10000;
  for (bool wide : {false,true})
    for (std::uint32_t index : {0u,1u,2u,0x80000000u,0xffffffffu}) {
      CAPTURE(wide,index);
      const auto f = fixture(wide);
      const unsigned pointerSize = wide ? 8 : 4;
      uc_engine *raw = nullptr;
      REQUIRE(uc_open(UC_ARCH_X86,wide ? UC_MODE_64 : UC_MODE_32,&raw) == UC_ERR_OK);
      std::unique_ptr<uc_engine,CloseUnicorn> uc{raw};
      REQUIRE(uc_mem_map(raw,base,memorySize,UC_PROT_ALL) == UC_ERR_OK);

      // Seed all 64 KiB independently of Dragon. The table starts as arbitrary
      // bytes; only the fixture's actual Guest STOREs initialize its entries.
      std::vector<std::uint8_t> memory(memorySize);
      for (std::size_t i = 0; i < memory.size(); ++i)
        memory[i] = static_cast<std::uint8_t>(37 * i + 11);
      std::copy(f.code.begin(),f.code.end(),memory.begin());
      auto put = [&](std::uint64_t address,std::uint64_t value,unsigned size) {
        for (unsigned i = 0; i < size; ++i)
          memory[address - base + i] = static_cast<std::uint8_t>(value >> (8 * i));
      };
      put(stack,sentinel,pointerSize);
      REQUIRE(uc_mem_write(raw,base,memory.data(),memory.size()) == UC_ERR_OK);
      const std::uint64_t initialSP = stack, inputIndex = index, flags = 0x202;
      REQUIRE(uc_reg_write(raw,wide ? UC_X86_REG_RSP : UC_X86_REG_ESP,&initialSP) == UC_ERR_OK);
      REQUIRE(uc_reg_write(raw,wide ? UC_X86_REG_RDI : UC_X86_REG_EDI,&inputIndex) == UC_ERR_OK);
      REQUIRE(uc_reg_write(raw,UC_X86_REG_EFLAGS,&flags) == UC_ERR_OK);

      MachineTrace trace;
      uc_hook codeHook{},writeHook{};
      auto code = +[](uc_engine *,std::uint64_t address,std::uint32_t,void *context) {
        static_cast<MachineTrace *>(context)->instructions.push_back(address);
      };
      auto write = +[](uc_engine *,uc_mem_type,std::uint64_t address,int size,
                       std::int64_t value,void *context) {
        static_cast<MachineTrace *>(context)->writes.push_back(
            {address,static_cast<unsigned>(size),static_cast<std::uint64_t>(value)});
      };
      REQUIRE(uc_hook_add(raw,&codeHook,UC_HOOK_CODE,reinterpret_cast<void *>(code),
                          &trace,1,0) == UC_ERR_OK);
      REQUIRE(uc_hook_add(raw,&writeHook,UC_HOOK_MEM_WRITE,reinterpret_cast<void *>(write),
                          &trace,1,0) == UC_ERR_OK);
      REQUIRE(uc_emu_start(raw,base,sentinel,0,100) == UC_ERR_OK);

      std::uint64_t pc = 0, sp = 0;
      REQUIRE(uc_reg_read(raw,wide ? UC_X86_REG_RIP : UC_X86_REG_EIP,&pc) == UC_ERR_OK);
      REQUIRE(uc_reg_read(raw,wide ? UC_X86_REG_RSP : UC_X86_REG_ESP,&sp) == UC_ERR_OK);
      CHECK(pc == sentinel); // The real RET executed and consumed its return slot.
      CHECK(sp == stack + pointerSize);
      REQUIRE_FALSE(trace.instructions.empty());
      CHECK(trace.instructions.back() == (index == 0 ? changed : index == 1 ? leaf : fallback));

      std::vector<std::uint64_t> indirectTargets;
      for (std::size_t i = 0; i < trace.instructions.size(); ++i)
        if (trace.instructions[i] == f.dispatch) {
          REQUIRE(i + 1 < trace.instructions.size());
          indirectTargets.push_back(trace.instructions[i + 1]);
        }
      const auto expectedTargets = index == 0 ? std::vector<std::uint64_t>{body,changed}
          : index == 1 ? std::vector<std::uint64_t>{leaf} : std::vector<std::uint64_t>{};
      CHECK(indirectTargets == expectedTargets);
      CHECK(std::count(trace.instructions.begin(),trace.instructions.end(),aliasPath) == (index == 0 ? 1 : 0));
      CHECK(std::count(trace.instructions.begin(),trace.instructions.end(),body) == (index == 0 ? 1 : 0));

      std::vector<MachineWrite> expectedWrites{
          {table,pointerSize,body},{table + pointerSize,pointerSize,leaf}};
      if (index == 0) expectedWrites.push_back({table,pointerSize,changed});
      CHECK(trace.writes == expectedWrites);
      put(table,index == 0 ? changed : body,pointerSize);
      put(table + pointerSize,leaf,pointerSize);
      std::vector<std::uint8_t> actual(memory.size());
      REQUIRE(uc_mem_read(raw,base,actual.data(),actual.size()) == UC_ERR_OK);
      CHECK(actual == memory); // Every byte, including stack and unused data.
      CHECK(std::equal(f.code.begin(),f.code.end(),actual.begin()));
    }
}
#endif
