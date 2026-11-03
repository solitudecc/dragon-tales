#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>

TEST_CASE("i386 word CALL and RET retain their abstract control operations", "[llvm][guest-ret]") {
  using namespace dragon;
  for (const auto &[code,opcode]:std::vector<std::pair<std::vector<std::uint8_t>,ignil::Opcode>>{
      {{0x66,0xe8,0xfc,0x0f},ignil::Opcode::CALL},
      {{0x66,0xff,0xd0},ignil::Opcode::CALL},
      {{0x66,0xff,0x13},ignil::Opcode::CALL},
      {{0x66,0xc3},ignil::Opcode::RET},
      {{0x66,0xc2,0xff,0xff},ignil::Opcode::RET}}) {
    Disassembler dis{Architecture::X86_32,Configuration{}};Graph graph{Architecture::X86_32,Configuration{}};
    dis.disassemble(code,0x1000,graph);
    auto *block=graph.getIGNILBlock(0x1000);REQUIRE(block);REQUIRE(block->unlifted().empty());
    REQUIRE_FALSE(block->ops().empty());CHECK(block->ops().back().opcode==opcode);
    if (opcode==ignil::Opcode::RET) CHECK(block->ops().back().srcs.empty());
  }
}

TEST_CASE("i386 word direct CALL truncates its architectural target IP", "[llvm][guest-ret]") {
  using namespace dragon;
  Disassembler dis{Architecture::X86_32,Configuration{}};Graph graph{Architecture::X86_32,Configuration{}};
  dis.disassemble(std::vector<std::uint8_t>{0x66,0xe8,0xfc,0x0f},0x11000,graph);
  auto *block=graph.getIGNILBlock(0x11000);REQUIRE(block);REQUIRE_FALSE(block->ops().empty());
  const auto &call=block->ops().back();REQUIRE(call.opcode==ignil::Opcode::CALL);
  REQUIRE(call.srcs.size()==1);CHECK(std::get<ignil::Const>(call.srcs[0]).value==0x2000);
}

TEST_CASE("A64 RET preserves its explicit register as metadata without operands", "[llvm][guest-ret]") {
  using namespace dragon;
  for (const auto &[code,expected]:std::vector<std::pair<std::vector<std::uint8_t>,unsigned>>{
      {{0x20,0x01,0x5f,0xd6},10},{{0xc0,0x03,0x5f,0xd6},31},{{0xe0,0x03,0x5f,0xd6},32}}) {
    Disassembler dis{Architecture::ARM64,Configuration{}};Graph graph{Architecture::ARM64,Configuration{}};
    dis.disassemble(code,0x1000,graph);
    auto *block=graph.getIGNILBlock(0x1000);REQUIRE(block);REQUIRE(block->unlifted().empty());
    const auto &ret=block->ops().back();REQUIRE(ret.opcode==ignil::Opcode::RET);
    CHECK(ret.srcs.empty());CHECK_FALSE(ret.dst);CHECK(ret.aux==expected);
  }
}

TEST_CASE("V2 x86 near RET produces an architectural Return exit", "[llvm][guest-ret]") {
  using namespace dragon;
  for (auto arch:{Architecture::X86_32,Architecture::X86_64}) {
    Disassembler dis{arch,Configuration{}};Graph graph{arch,Configuration{}};
    dis.disassemble(std::vector<std::uint8_t>{0xc2,0xff,0xff},0x1000,graph);
    Function root{&graph,0x1000};REQUIRE(root.isValid());
    llvmlifter::LLVMLifter lifter{arch};REQUIRE(lifter.setExecutionMode(llvmlifter::ExecutionMode::StatefulV2));
    REQUIRE(lifter.lift(root,graph));CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
    auto *function=lifter.module().getFunction("dragon_v2_"+ArchitectureName(arch)+"_1000");REQUIRE(function);
    bool returned=false;
    for (const auto &bb:*function) for (const auto &instruction:bb)
      if (const auto *store=llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        std::int64_t offset=0;
        auto *base=llvm::GetPointerBaseWithConstantOffset(store->getPointerOperand(),offset,lifter.module().getDataLayout());
        auto *value=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
        if (base==function->getArg(3) && offset==0 && value && value->getZExtValue()==DRAGON_EXIT_RETURN) returned=true;
      }
    CHECK(returned);
  }
}

TEST_CASE("Legacy rejects decoded calls and returns absent from the abstract IL", "[llvm][legacy-transfer]") {
  using namespace dragon;
  for (auto arch:{Architecture::X86_32,Architecture::X86_64}) {
    std::vector<std::vector<std::uint8_t>> cases{{0xff,0x18,0xc3},{0xcb},{0xca,0x08,0x00}};
    if(arch==Architecture::X86_32)cases.push_back({0x9a,0x00,0x20,0x00,0x00,0x08,0x00,0xc3});
    for (const auto& code:cases) {
      INFO(ArchitectureName(arch));INFO(unsigned(code.front()));
      Disassembler dis{arch,Configuration{}};Graph graph{arch,Configuration{}};dis.disassemble(code,0x1000,graph);
      REQUIRE(graph.get(0x1000));CHECK((graph.get(0x1000)->isCall() || graph.get(0x1000)->isReturn()));
      Function root{&graph,0x1000};REQUIRE(root.isValid());llvmlifter::LLVMLifter lifter{arch};
      CHECK_FALSE(lifter.lift(root,graph));CHECK(lifter.module().empty());
    }
  }
}

TEST_CASE("Unknown x86 control instructions are recorded even without register definitions", "[llvm][legacy-transfer]") {
  using namespace dragon;
  for (const auto& code:std::vector<std::vector<std::uint8_t>>{{0x9a,0x00,0x20,0x00,0x00,0x08,0x00,0xc3},{0xcb},{0xca,0x08,0x00},{0xea,0x00,0x20,0x00,0x00,0x08,0x00},{0xff,0x28},{0x66,0xff,0x20}}) {
    Disassembler dis{Architecture::X86_32,Configuration{}};Graph graph{Architecture::X86_32,Configuration{}};
    dis.disassemble(code,0x1000,graph);const auto *block=graph.getIGNILBlock(0x1000);REQUIRE(block);
    CHECK(std::find(block->unlifted().begin(),block->unlifted().end(),0x1000)!=block->unlifted().end());
  }
}

TEST_CASE("Legacy trace refuses a block with unmodelled instruction effects", "[llvm][legacy-transfer]") {
  using namespace dragon;
  ignil::Block block{0x1000};
  block.append({ignil::Opcode::COPY,ignil::Reg{0,64,"RAX"},{ignil::Const{7,64}},0x1000});
  block.noteUnlifted(0x1001);block.append({ignil::Opcode::RET,std::nullopt,{},0x1002});
  llvmlifter::LLVMLifter lifter{Architecture::X86_64};
  CHECK_FALSE(lifter.liftBlock(block));CHECK(lifter.module().empty());
}
