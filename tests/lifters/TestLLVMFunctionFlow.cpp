#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <set>
#include <string>
#include <vector>

namespace {
std::vector<std::uint8_t> fixture(std::string_view id) {
  auto file=llvm::MemoryBuffer::getFile(DRAGON_FUNCTION_FLOW_FIXTURES);
  REQUIRE(bool(file));
  auto json=llvm::json::parse((*file)->getBuffer());
  REQUIRE(bool(json));
  const auto *object=json->getAsObject();REQUIRE(object);
  const auto *cases=object->getArray("cases");REQUIRE(cases);
  for (const auto &value:*cases) {
    const auto *item=value.getAsObject();REQUIRE(item);
    const auto name=item->getString("id");REQUIRE(name);
    if (*name!=llvm::StringRef(id.data(),id.size())) continue;
    const auto text=item->getString("code_hex");REQUIRE(text);
    const auto hex=text->str();REQUIRE(hex.size()%2==0);
    std::vector<std::uint8_t> bytes;
    for (std::size_t i=0;i<hex.size();i+=2) bytes.push_back(std::stoul(hex.substr(i,2),nullptr,16));
    return bytes;
  }
  FAIL("missing function-flow fixture");return {};
}
}

TEST_CASE("LLVM function-flow baseline setup is valid", "[llvm][function-flow-setup]") {
  using namespace dragon;
  for (const auto id:{"control_store","control_branch","control_loop","control_call_zero","A","B"}) {
    INFO(id);Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
    Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
    dis.disassemble(fixture(id),0x1000,graph);Function root{&graph,0x1000};REQUIRE(root.isValid());
    for (const auto &[address,block]:root.blocks()) {const auto *ir=graph.getIGNILBlock(address);REQUIRE(ir);CHECK(ir->unlifted().empty());}
    llvmlifter::LLVMLifter lifter{Architecture::X86_64};const bool accepted=lifter.lift(root,graph);
    if (std::string_view(id)=="control_store" || std::string_view(id)=="control_branch" || std::string_view(id)=="control_loop") REQUIRE(accepted);
    if (accepted) {REQUIRE(lifter.module().getFunction("sub_1000"));CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));}
  }
}

// These are normative assertions, intentionally red on 0aa4b73. Catch's
// !shouldfail keeps this staged baseline visible without calling the bad
// semantics correct. Step 2 removes B's marker; step 3 removes A's marker.
// The independent native/Unicorn harness defaults to a nonzero exit for A/B.
TEST_CASE("LLVM function-flow A must not discard an unknown callee return", "[llvm][function-flow][!shouldfail]") {
  using namespace dragon;
  Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
  Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
  dis.disassemble(fixture("A"),0x1000,graph);Function root{&graph,0x1000};REQUIRE(root.isValid());
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  // A future safe Legacy refusal is acceptable; V2 execution is checked by
  // the function-level oracle after that ABI has been approved.
  if (!lifter.lift(root,graph)) {SUCCEED("unsafe legacy function rejected");return;}
  REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  lifter.runMem2Reg();lifter.runInstCombine();
  bool unknownReturnControlsBranch=false;
  for (const auto &block:*lifter.module().getFunction("sub_1000"))
    if (const auto *branch=llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());branch && branch->isConditional())
      unknownReturnControlsBranch |= !llvm::isa<llvm::ConstantInt>(branch->getCondition());
  CHECK(unknownReturnControlsBranch);
}

TEST_CASE("LLVM function-flow B must preserve a defined path across an external branch", "[llvm][function-flow]") {
  using namespace dragon;
  Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
  Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
  dis.disassemble(fixture("B"),0x1000,graph);Function root{&graph,0x1000};REQUIRE(root.isValid());
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  if (!lifter.lift(root,graph)) {SUCCEED("unsafe legacy function rejected");return;}
  REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  const auto *function=lifter.module().getFunction("sub_1000");REQUIRE(function);
  std::vector<const llvm::BasicBlock*> pending{&function->getEntryBlock()};std::set<const llvm::BasicBlock*> seen;
  bool reachableUnreachable=false;
  while (!pending.empty()) {
    const auto *block=pending.back();pending.pop_back();if(!seen.insert(block).second)continue;
    reachableUnreachable |= llvm::isa<llvm::UnreachableInst>(block->getTerminator());
    for (const auto *next:llvm::successors(block))pending.push_back(next);
  }
  CHECK_FALSE(reachableUnreachable);
}

TEST_CASE("LLVM Legacy rejects control transfers its ABI cannot report", "[llvm][dispatch-regression]") {
  using namespace dragon;
  const std::vector<std::vector<std::uint8_t>> inputs{
    {0xe9,0xfb,0x1f,0,0},                         // jmp 0x3000
    {0xe9,0xfb,0xef,0xff,0xff},                   // jmp 0 (valid numeric target)
    {0xff,0xe0},                                  // jmp rax, unresolved
    {0x85,0xff,0x0f,0x85,0xf8,0x1f,0,0,0xc3},    // jne 0x3000; ret
  };
  for (const auto &bytes:inputs) {
    Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
    Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
    dis.disassemble(bytes,0x1000,graph);Function root{&graph,0x1000};REQUIRE(root.isValid());
    llvmlifter::LLVMLifter lifter{Architecture::X86_64};
    CHECK_FALSE(lifter.lift(root,graph));
    CHECK(lifter.module().empty());
  }
}

TEST_CASE("LLVM Legacy cannot treat an enumerated switch default as UB", "[llvm][dispatch-regression]") {
  using namespace dragon;
  Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
  Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
  dis.disassemble(std::vector<std::uint8_t>{0xff,0xe0,0xc3,0xc3},0x1000,graph);
  auto *jump=graph.get(0x1000);REQUIRE(jump);jump->addBranch(0x1002);jump->addBranch(0x1003);
  graph.addBlock(0x1002);graph.addBlock(0x1003);
  // Supply the case semantics explicitly: failure must concern the default
  // edge, never a missing-IGNIL setup error after an unresolved terminator.
  for (const auto address:{0x1002u,0x1003u}) {
      if (!graph.get(address)) graph.addInstruction(Instruction(address,1,"ret","",{0xc3},true,false,false,false,false,false));

    ignil::Block block{address};
    block.append({ignil::Opcode::RET,std::nullopt,{},address});
    graph.addIGNILBlock(std::move(block));
  }
  Function root{&graph,0x1000};REQUIRE(root.isValid());
  for (const auto &[address,block]:root.blocks()) REQUIRE(graph.getIGNILBlock(address));
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  CHECK_FALSE(lifter.lift(root,graph));
  CHECK(lifter.module().empty());
}
