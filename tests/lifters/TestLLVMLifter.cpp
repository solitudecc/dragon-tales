#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>

#include <llvm/IR/Verifier.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

TEST_CASE("LLVM guest memory and register snapshots have byte alignment", "[llvm][alignment]") {
  using namespace dragon;
  ignil::Block block{0x1000};
  const ignil::Temp value{0,128};
  block.append({ignil::Opcode::LOAD,value,{ignil::Const{0x1003,64}},0x1000});
  block.append({ignil::Opcode::STORE,std::nullopt,{ignil::Const{0x1013,64},value},0x1000});
  // The x86 register snapshot also has byte-packed offsets (XMM0 = 143).
  block.append({ignil::Opcode::COPY,ignil::Reg{143,64,"XMM0"},{ignil::Reg{143,64,"XMM0"}},0x1000});
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  REQUIRE(lifter.liftBlock(block));
  unsigned external = 0;
  for (const auto &function : lifter.module())
    for (const auto &bb : function)
      for (const auto &instruction : bb) {
        const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
        const auto *pointer = load ? load->getPointerOperand() : store ? store->getPointerOperand() : nullptr;
        if (!pointer || llvm::isa<llvm::AllocaInst>(pointer)) continue;
        ++external;
        CHECK((load ? load->getAlign() : store->getAlign()) == llvm::Align(1));
      }
  CHECK(external == 4);
  CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static const dragon::Function *findFunction(
        const std::vector<dragon::Function> &fns, std::uint64_t addr) {
  auto it = std::find_if(fns.begin(), fns.end(),
                         [addr](const dragon::Function &f) {
                           return f.startAddress() == addr;
                         });
  return it == fns.end() ? nullptr : &*it;
}

// -----------------------------------------------------------------------
// Regression: a discovered function start with no blocks
// -----------------------------------------------------------------------

TEST_CASE("LLVMLifter - lift() refuses a function with no blocks",
          "[llvm][lifter]") {
  // call 0x0     e8 fb ef ff ff   (rel32 = -0x1005, target outside the range)
  // ret          c3
  //
  // Byte sequence verified with:
  //   objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x1000
  //
  // Call targets are trusted as function starts unconditionally, so 0x0
  // becomes a discovered function with zero blocks - the ordinary shape of
  // a PLT stub reached from a raw .text blob, not a contrived input. lift()
  // used to segfault on exactly this: buildBlockMap()'s "missing IGNIL
  // block" guard only fires from inside its per-block loop, so a blockless
  // function passed it vacuously and then read llfn->getEntryBlock() on a
  // function with no basic blocks. lift() now rejects any invalid Function
  // view up front.
  const std::vector<std::uint8_t> bytes = {0xE8, 0xFB, 0xEF, 0xFF, 0xFF, 0xC3};

  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::LINEAR_SWEEP;
  cfg.syntax = dragon::Syntax::INTEL;

  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g(dragon::Architecture::X86_64, cfg);
  dis.disassemble(bytes, 0x1000, g);

  // 0x0 is a discovered function start (call targets are trusted
  // unconditionally) but has no blocks, so its Function view reports
  // itself invalid - exactly what the constructor documents.
  //
  // Built directly rather than via Graph::functions(), which filters
  // invalid views out. That filter is why a C++ caller iterating
  // functions() never saw this; the C shim's dragon_graph_get_function()
  // has no such filter, so C and Python callers reach it directly.
  const dragon::Function blockless(&g, 0x0);
  REQUIRE(g.isValidFunction(0x0));
  REQUIRE(blockless.blocks().empty());
  REQUIRE_FALSE(blockless.isValid());

  dragon::llvmlifter::LLVMLifter llvml{dragon::Architecture::X86_64,
                                       "blockless"};

  // Refused, not crashed - and nothing is left behind in the module.
  REQUIRE_FALSE(llvml.lift(blockless, g));
  REQUIRE(llvml.ir().find("sub_0") == std::string::npos);

  // The real function beside it still lifts, on the same lifter instance.
  const auto funcs = g.functions();
  const dragon::Function *real = findFunction(funcs, 0x1000);
  REQUIRE(real != nullptr);
  REQUIRE(real->isValid());
  REQUIRE(llvml.lift(*real, g));
}

static std::optional<dragon::Function>
disassembleI386(const std::vector<std::uint8_t> &bytes, dragon::Graph &g) {
  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::LINEAR_SWEEP;
  dragon::Disassembler dis(dragon::Architecture::X86_32, cfg);
  dis.disassemble(bytes, 0x1000, g);
  const auto funcs = g.functions();
  if (const dragon::Function *fn = findFunction(funcs, 0x1000))
    return *fn;
  return std::nullopt;
}

TEST_CASE("LLVMLifter - i386 addresses are zero-extended", "[llvm][lifter]") {
  // mov eax, [ebx]; ret
  const std::vector<std::uint8_t> bytes = {0x8b, 0x03, 0xc3};
  dragon::Graph g(dragon::Architecture::X86_32, dragon::Configuration{});
  const auto fn = disassembleI386(bytes, g);
  REQUIRE(fn);

  dragon::llvmlifter::LLVMLifter llvml{dragon::Architecture::X86_32};
  REQUIRE(llvml.lift(*fn, g));
  REQUIRE_FALSE(llvm::verifyModule(llvml.module(), &llvm::errs()));
  // A sign-extended index would put [0x80000000] 2 GB below the memory base.
  const std::string ir = llvml.ir();
  REQUIRE(ir.find("zext i32") != std::string::npos);
  REQUIRE(ir.find("sext") == std::string::npos);
}

TEST_CASE("LLVMLifter - an indirect call is through a pointer",
          "[llvm][lifter]") {
  // call edx; ret
  const std::vector<std::uint8_t> bytes = {0xff, 0xd2, 0xc3};
  dragon::Graph g(dragon::Architecture::X86_32, dragon::Configuration{});
  const auto fn = disassembleI386(bytes, g);
  REQUIRE(fn);

  dragon::llvmlifter::LLVMLifter llvml{dragon::Architecture::X86_32};
  REQUIRE(llvml.lift(*fn, g));
  REQUIRE_FALSE(llvm::verifyModule(llvml.module(), &llvm::errs()));
}

TEST_CASE("LLVMLifter - IEEE double compares become fcmp",
          "[llvm][lifter][sse]") {
  // push rbp; mov rbp, rsp; comisd xmm0, xmm1; setb al; pop rbp; ret
  const std::vector<std::uint8_t> bytes = {0x55, 0x48, 0x89, 0xe5, 0x66, 0x0f,
                                           0x2f, 0xc1, 0x0f, 0x92, 0xc0, 0x5d,
                                           0xc3};
  dragon::Configuration cfg;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g(dragon::Architecture::X86_64, cfg);
  dis.disassemble(bytes, 0x1000, g);
  const dragon::Function fn{&g, 0x1000};
  REQUIRE(fn.isValid());
  dragon::llvmlifter::LLVMLifter llvml{dragon::Architecture::X86_64};
  REQUIRE(llvml.lift(fn, g));
  REQUIRE_FALSE(llvm::verifyModule(llvml.module(), &llvm::errs()));
  const std::string ir = llvml.ir();
  CHECK(ir.find("fcmp olt double") != std::string::npos);
  CHECK(ir.find("fcmp uno double") != std::string::npos);
  CHECK(ir.find("fcmp oeq double") != std::string::npos);
}

// -----------------------------------------------------------------------
// Invalid IR found by verifying every function of real binaries (opt
// -passes=verify over Lua/sqlite/zlib builds): the function's first block
// doubled as the LLVM entry block, and a block ending in a trap had no
// terminator.
// -----------------------------------------------------------------------

namespace {
std::string liftAndVerify(const std::vector<std::uint8_t> &bytes,
                          std::uint64_t entry,
                          std::vector<std::uint64_t> entries) {
  dragon::Configuration cfg;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g(dragon::Architecture::X86_64, cfg);
  dragon::Disassembler::AnalysisHints hints;
  hints.entryPoints = std::move(entries);
  dis.disassemble(bytes, 0x1000, g, hints);
  const dragon::Function fn{&g, entry};
  REQUIRE(fn.isValid());
  dragon::llvmlifter::LLVMLifter llvml{dragon::Architecture::X86_64};
  REQUIRE(llvml.lift(fn, g));
  std::string err;
  llvm::raw_string_ostream os(err);
  CHECK_FALSE(llvm::verifyModule(llvml.module(), &os));
  INFO(err);
  return llvml.ir();
}
} // namespace

TEST_CASE("LLVMLifter - a loop back to the function start is valid IR",
          "[llvm][lifter]") {
  // 0x1000: dec ecx ; jnz 0x1000 ; ret - the entry block is a branch target.
  liftAndVerify({0xff, 0xc9, 0x75, 0xfc, 0xc3}, 0x1000, {0x1000});
}

TEST_CASE("LLVMLifter - a block ending in a trap gets a terminator",
          "[llvm][lifter]") {
  // 0x1000: test edi, edi ; je 0x1005 ; ret ; 0x1005: ud2
  liftAndVerify({0x85, 0xff, 0x74, 0x01, 0xc3, 0x0f, 0x0b}, 0x1000, {0x1000});
}

TEST_CASE("LLVMLifter - execution starts at the entry, not the lowest block",
          "[llvm][lifter]") {
  // 0x1000: mov eax, 1 ; ret          <- a cold part below the entry
  // 0x1006: test edi, edi ; je 0x1000 ; xor eax, eax ; ret   <- entry
  const auto ir = liftAndVerify({0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, //
                                 0x85, 0xff, 0x74, 0xf6, 0x31, 0xc0, 0xc3},
                                0x1006, {0x1006});
  CHECK(ir.find("br label %bb_4102") != std::string::npos); // 0x1006
}
