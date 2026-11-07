#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t entry = 0x1000;

Bytes returns(Architecture arch) {
  return arch == Architecture::ARM64 ? Bytes{0xc0,0x03,0x5f,0xd6} : Bytes{0xc3};
}

Bytes withReturn(Architecture arch, Bytes bytes) {
  const auto ret = returns(arch);
  bytes.insert(bytes.end(),ret.begin(),ret.end());
  return bytes;
}

Graph decode(Architecture arch, const Bytes &bytes, std::uint64_t address = entry,
             DisassemblyStrategy strategy = DisassemblyStrategy::LINEAR_SWEEP) {
  Configuration config;
  config.disassemblyStrategy = strategy;
  Graph graph{arch,config};
  Disassembler dis{arch,config};
  dis.disassemble(bytes,address,graph);
  return graph;
}

bool marked(const ignil::Block &block, std::uint64_t pc) {
  return std::find(block.unlifted().begin(),block.unlifted().end(),pc) != block.unlifted().end();
}

void verify(LLVMLifter &lifter) {
  std::string error;
  llvm::raw_string_ostream out{error};
  const bool invalid = llvm::verifyModule(lifter.module(),&out);
  INFO(error);
  CHECK_FALSE(invalid);
}

// This is a real decoded MOVBE store, deliberately split before the RET.
// Its unsupported effect has no register result and thus no IGNIL operation.
Graph markerOnlyBlock(Architecture arch) {
  Disassembler dis{arch,{}};
  const auto instructions = dis.disassemble(Bytes{0x0f,0x38,0xf1,0x03,0xc3},entry);
  REQUIRE(instructions.size() == 2);
  REQUIRE(instructions.front().size() == 4);
  Graph graph{arch,{}};
  for (const auto &instruction : instructions) graph.addInstruction(instruction);
  graph.addBlock(entry);
  graph.addBlock(entry + 4);
  graph.addFunction(entry);
  graph.addIGNILBlock(dis.liftTrace(std::span{instructions}.first(1)));
  graph.addIGNILBlock(dis.liftTrace(std::span{instructions}.last(1)));
  const auto *first = graph.getIGNILBlock(entry);
  REQUIRE(first);
  REQUIRE(first->ops().empty());
  REQUIRE(marked(*first,entry));
  return graph;
}

std::map<std::size_t,std::uint64_t> exitConstants(
    const llvm::BasicBlock &block, const llvm::Function &function,
    const llvm::Module &module) {
  std::map<std::size_t,std::uint64_t> result;
  for (const auto &instruction : block) {
    const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    if (!store) continue;
    const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    if (!constant || constant->getBitWidth() > 64) continue;
    std::int64_t offset = 0;
    const auto *base = llvm::GetPointerBaseWithConstantOffset(
        store->getPointerOperand(),offset,module.getDataLayout());
    if (base == function.getArg(3) && offset >= 0)
      result[static_cast<std::size_t>(offset)] = constant->getZExtValue();
  }
  return result;
}

void checkUnsupported(LLVMLifter &lifter, Architecture arch, ExecutionMode mode,
                      std::uint64_t pc) {
  const auto name = std::string(mode == ExecutionMode::StatefulV2 ? "dragon_v2_" : "dragon_static_") +
                    ArchitectureName(arch) + "_1000";
  const auto *function = lifter.module().getFunction(name);
  REQUIRE(function);
  REQUIRE(function->arg_size() == 4);
  unsigned found = 0;
  for (const auto &block : *function) {
    const auto fields = exitConstants(block,*function,lifter.module());
    const auto reason = fields.find(offsetof(ExecExitV2,reason));
    const auto source = fields.find(offsetof(ExecExitV2,source_pc));
    if (reason == fields.end() || source == fields.end() ||
        reason->second != DRAGON_EXIT_UNSUPPORTED || source->second != pc) continue;
    ++found;
    CHECK(fields.at(offsetof(ExecExitV2,transfer_kind)) == DRAGON_TRANSFER_NONE);
    CHECK(fields.at(offsetof(ExecExitV2,next_pc)) == pc);
    CHECK(fields.at(offsetof(ExecExitV2,next_pc_valid)) == 1);
    CHECK(fields.at(offsetof(ExecExitV2,return_pc)) == 0);
    CHECK(fields.at(offsetof(ExecExitV2,detail)) == DRAGON_DETAIL_INSTRUCTION);
    CHECK(llvm::isa<llvm::ReturnInst>(block.getTerminator()));
  }
  CHECK(found == 1);
}
} // namespace

TEST_CASE("Legacy whole functions reject actual unlifted instruction effects",
          "[llvm][legacy-unlifted]") {
  struct Case { Architecture arch; Bytes instruction; };
  const std::vector<Case> cases{
    {Architecture::X86_64,{0x0f,0x38,0xf1,0x03}}, // MOVBE [RBX], EAX: no defined register
    {Architecture::X86_32,{0x0f,0x38,0xf1,0x03}}, // MOVBE [EBX], EAX
    {Architecture::X86_64,{0x64,0x48,0x8b,0x03}}, // FS scalar load
    {Architecture::X86_32,{0x65,0x89,0x03}},      // GS scalar store
    {Architecture::X86_64,{0x64,0x0f,0x10,0x03}}, // FS MOVUPS XMM0, [RBX]
    {Architecture::X86_32,{0x65,0x0f,0x11,0x03}}, // GS MOVUPS [EBX], XMM0
    {Architecture::ARM64,{0x20,0x14,0xc0,0xda}}   // CLS X0, X1
  };
  for (const auto &test : cases)
    for (auto strategy : {DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(test.arch,test.instruction,strategy);
      auto graph = decode(test.arch,withReturn(test.arch,test.instruction),entry,strategy);
      const Function root{&graph,entry};
      REQUIRE(root.isValid());
      const auto *block = graph.getIGNILBlock(entry);
      REQUIRE(block);
      REQUIRE(marked(*block,entry));
      LLVMLifter lifter{test.arch};
      const auto before = lifter.ir();
      CHECK_FALSE(lifter.lift(root,graph));
      CHECK(lifter.ir() == before);
      CHECK(lifter.module().empty());
    }
}

TEST_CASE("Legacy scans nonentry blocks for unlifted effects",
          "[llvm][legacy-unlifted]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64,Architecture::ARM64}) {
    const auto code = arch == Architecture::ARM64
        ? Bytes{0x40,0,0,0x34,0xc0,0x03,0x5f,0xd6,0x20,0x14,0xc0,0xda,0xc0,0x03,0x5f,0xd6}
        : Bytes{0x85,0xff,0x74,0x01,0xc3,0x0f,0x38,0xf1,0x03,0xc3};
    const auto badPC = arch == Architecture::ARM64 ? entry + 8 : entry + 5;
    for (auto strategy : {DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(arch,strategy);
      auto graph = decode(arch,code,entry,strategy);
      const Function root{&graph,entry};
      REQUIRE(root.isValid());
      REQUIRE(root.blocks().size() == 3);
      REQUIRE(graph.getIGNILBlock(entry));
      REQUIRE(graph.getIGNILBlock(entry)->unlifted().empty());
      REQUIRE(graph.getIGNILBlock(badPC));
      REQUIRE(marked(*graph.getIGNILBlock(badPC),badPC));
      LLVMLifter lifter{arch};
      CHECK_FALSE(lifter.lift(root,graph));
      CHECK(lifter.module().empty());
    }
  }
}

TEST_CASE("Legacy refuses marker only empty IR blocks",
          "[llvm][legacy-unlifted]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64}) {
    CAPTURE(arch);
    auto graph = markerOnlyBlock(arch);
    const Function root{&graph,entry};
    REQUIRE(root.isValid());
    REQUIRE(root.blocks().size() == 2);
    LLVMLifter lifter{arch};
    CHECK_FALSE(lifter.lift(root,graph));
    CHECK(lifter.module().empty());
  }
}

TEST_CASE("Legacy refusal preserves existing definitions and remains reusable",
          "[llvm][legacy-unlifted]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64,Architecture::ARM64}) {
    CAPTURE(arch);
    const auto good = arch == Architecture::ARM64 ? withReturn(arch,{0x60,0,0x80,0xd2})
                                                 : withReturn(arch,{0xb8,7,0,0,0});
    const auto bad = arch == Architecture::ARM64 ? withReturn(arch,{0x20,0x14,0xc0,0xda})
                                                : withReturn(arch,{0x0f,0x38,0xf1,0x03});
    auto first = decode(arch,good);
    auto rejected = decode(arch,bad,0x2000);
    auto collision = decode(arch,bad);
    auto later = decode(arch,good,0x3000);
    LLVMLifter lifter{arch};
    REQUIRE(lifter.lift(Function{&first,entry},first));
    auto *original = lifter.module().getFunction("sub_1000");
    REQUIRE(original);
    REQUIRE_FALSE(original->isDeclaration());
    const auto before = lifter.ir();
    CHECK_FALSE(lifter.lift(Function{&rejected,0x2000},rejected));
    CHECK(lifter.ir() == before);
    CHECK(lifter.module().getFunction("sub_1000") == original);
    CHECK_FALSE(lifter.lift(Function{&collision,entry},collision));
    CHECK(lifter.ir() == before);
    CHECK(lifter.module().getFunction("sub_1000") == original);
    CHECK_FALSE(original->isDeclaration());
    CHECK(lifter.module().getFunction("sub_2000") == nullptr);
    REQUIRE(lifter.lift(Function{&later,0x3000},later));
    REQUIRE(lifter.module().getFunction("sub_3000"));
    verify(lifter);
  }
}

TEST_CASE("Legacy accepts supported functions fences and defined unknown results",
          "[llvm][legacy-unlifted]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64}) {
    for (const auto &body : std::vector<Bytes>{
        {0xb8,7,0,0,0}, {0xf3,0x90}, {0x0f,0xae,0xf0},
        {0x0f,0xae,0xe8}, {0x0f,0xae,0xf8}, {0x0f,0xbc,0xc1}}) {
      CAPTURE(arch,body);
      auto graph = decode(arch,withReturn(arch,body));
      const Function root{&graph,entry};
      REQUIRE(root.isValid());
      const auto *block = graph.getIGNILBlock(entry);
      REQUIRE(block);
      REQUIRE(block->unlifted().empty());
      if (body == Bytes{0x0f,0xbc,0xc1})
        CHECK(std::any_of(block->ops().begin(),block->ops().end(),[](const auto &op) {
          return op.opcode == ignil::Opcode::UNDEF;
        }));
      LLVMLifter lifter{arch};
      REQUIRE(lifter.lift(root,graph));
      verify(lifter);
    }
  }
  auto graph = decode(Architecture::ARM64,withReturn(Architecture::ARM64,{0x1f,0x20,0x03,0xd5}));
  LLVMLifter lifter{Architecture::ARM64};
  REQUIRE(lifter.lift(Function{&graph,entry},graph));
  verify(lifter);
}

TEST_CASE("V2 and static modes keep precise unsupported instruction exits",
          "[llvm][legacy-unlifted]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64,Architecture::ARM64})
    for (auto mode : {ExecutionMode::StatefulV2,ExecutionMode::StaticAnalysis}) {
      CAPTURE(arch,mode);
      const auto code = arch == Architecture::ARM64
          ? Bytes{0x80,0x46,0x82,0xd2,0x20,0x14,0xc0,0xda,0x80,0xca,0x8e,0xd2,0xc0,0x03,0x5f,0xd6}
          : Bytes{0xb8,0x78,0x56,0x34,0x12,0x0f,0x38,0xf1,0x03,0xb8,0x10,0x32,0x54,0x76,0xc3};
      const auto badPC = arch == Architecture::ARM64 ? entry + 4 : entry + 5;
      auto graph = decode(arch,code);
      const Function root{&graph,entry};
      REQUIRE(root.isValid());
      REQUIRE(graph.getIGNILBlock(entry));
      REQUIRE(marked(*graph.getIGNILBlock(entry),badPC));
      LLVMLifter lifter{arch};
      REQUIRE(lifter.setExecutionMode(mode));
      REQUIRE(lifter.lift(root,graph));
      checkUnsupported(lifter,arch,mode,badPC);
      // Check the lowering boundary as well as its exit fields. The native
      // reference tests separately compare the actual Guest state/memory.
      bool prefix = false, suffix = false;
      const auto before = arch == Architecture::ARM64 ? 0x1234u : 0x12345678u;
      const auto after = arch == Architecture::ARM64 ? 0x7654u : 0x76543210u;
      for (const auto &function : lifter.module()) for (const auto &block : function)
        for (const auto &instruction : block) for (const auto &operand : instruction.operands())
          if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(operand.get()))
            if (constant->getBitWidth() <= 64) {
              prefix |= constant->getZExtValue() == before;
              suffix |= constant->getZExtValue() == after;
            }
      CHECK(prefix);
      CHECK_FALSE(suffix);
      verify(lifter);
    }
}

TEST_CASE("V2 and static modes retain marker only block exits",
          "[llvm][legacy-unlifted]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64})
    for (auto mode : {ExecutionMode::StatefulV2,ExecutionMode::StaticAnalysis}) {
      CAPTURE(arch,mode);
      auto graph = markerOnlyBlock(arch);
      LLVMLifter lifter{arch};
      REQUIRE(lifter.setExecutionMode(mode));
      REQUIRE(lifter.lift(Function{&graph,entry},graph));
      checkUnsupported(lifter,arch,mode,entry);
      verify(lifter);
    }
}
