#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
using namespace dragon;
using namespace dragon::llvmlifter;

TEST_CASE("LLVM static mode retains unknown calls without a dispatcher", "[llvm][static]") {
  for (auto arch : {Architecture::X86_32, Architecture::X86_64, Architecture::ARM64}) {
    INFO("architecture=" << static_cast<unsigned>(arch));
    Configuration config; config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
    Disassembler dis{arch,config}; Graph graph{arch,config};
    const std::vector<std::uint8_t> code=arch==Architecture::ARM64
        ? std::vector<std::uint8_t>{0x00,0x04,0x00,0x94,0xc0,0x03,0x5f,0xd6}
        : std::vector<std::uint8_t>{0xe8,0xfb,0x0f,0,0,0xc3};
    dis.disassemble(code,0x1000,graph); Function root{&graph,0x1000}; REQUIRE(root.isValid());
    LLVMLifter lifter{arch};
    // Numeric value intentionally lets this regression compile on old main.
    REQUIRE(lifter.setExecutionMode(static_cast<ExecutionMode>(2)));
    REQUIRE(lifter.lift(root,graph));
    const auto name="dragon_static_"+ArchitectureName(arch)+"_1000";
    auto *fn=lifter.module().getFunction(name); REQUIRE(fn); CHECK(fn->arg_size()==4);
    auto *external=lifter.module().getFunction("dragon_static_external"); REQUIRE(external);
    CHECK(external->isDeclaration()); CHECK(external->arg_size()==4);
    CHECK_FALSE(external->doesNotAccessMemory()); CHECK_FALSE(external->onlyReadsMemory());
    CHECK_FALSE(external->hasFnAttribute(llvm::Attribute::WillReturn));
    CHECK_FALSE(external->doesNotThrow());
    CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
    const auto ir=lifter.ir(); CHECK(ir.find("call void @dragon_static_external")!=std::string::npos);
    CHECK(ir.find("budget.")==std::string::npos); CHECK(ir.find("execute_v2")==std::string::npos);
    CHECK(ir.find("inttoptr")==std::string::npos); CHECK(ir.find("unreachable")==std::string::npos);
    CHECK_FALSE(lifter.setExecutionMode(ExecutionMode::Legacy));
    ignil::Block trace{0x3000}; trace.append({ignil::Opcode::RET,std::nullopt,{},0x3000});
    CHECK_FALSE(lifter.liftBlock(trace));
  }
}

TEST_CASE("LLVM static mode preserves conditional out of function exits", "[llvm][static]") {
  Configuration config; config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
  Disassembler dis{Architecture::X86_64,config}; Graph graph{Architecture::X86_64,config};
  dis.disassemble(std::vector<std::uint8_t>{0x85,0xff,0x0f,0x84,0xf8,0x1f,0,0,0xc7,0x03,7,0,0,0,0xc3},0x1000,graph);
  Function root{&graph,0x1000}; REQUIRE(root.isValid()); LLVMLifter lifter{Architecture::X86_64};
  REQUIRE(lifter.setExecutionMode(static_cast<ExecutionMode>(2))); REQUIRE(lifter.lift(root,graph));
  lifter.runMem2Reg(); lifter.runInstCombine(); lifter.runGVN(); lifter.runDCE();
  CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  CHECK(lifter.ir().find("unreachable")==std::string::npos);
  CHECK(lifter.ir().find("store i32 7")!=std::string::npos);
}

TEST_CASE("LLVM static internal declarations accept definitions in either order", "[llvm][static]") {
  for (bool callerFirst : {true,false}) {
    Configuration config; config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
    Disassembler dis{Architecture::X86_64,config}; Graph graph{Architecture::X86_64,config};
    std::vector<std::uint8_t> code{0xe8,0x0b,0,0,0,0xc3};code.resize(16,0xcc);
    code.insert(code.end(),{0xb8,7,0,0,0,0xc3});dis.disassemble(code,0x1000,graph);
    Function root{&graph,0x1000},callee{&graph,0x1010}; REQUIRE(root.isValid());REQUIRE(callee.isValid());
    LLVMLifter lifter{Architecture::X86_64};REQUIRE(lifter.setExecutionMode(static_cast<ExecutionMode>(2)));
    REQUIRE(lifter.lift(callerFirst ? root : callee,graph));
    if (callerFirst) {
      auto *declaration=lifter.module().getFunction("dragon_static_x86_64_1010");
      REQUIRE(declaration);CHECK(declaration->isDeclaration());
      CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
    }
    REQUIRE(lifter.lift(callerFirst ? callee : root,graph));
    CHECK_FALSE(lifter.module().getFunction("dragon_static_x86_64_1010")->isDeclaration());
    CHECK(lifter.ir().find("call void @dragon_static_x86_64_1010")!=std::string::npos);
    CHECK(lifter.ir().find("dragon_static_x86_64_1010.1")==std::string::npos);
    CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  }
}

TEST_CASE("LLVM failed static callee lift preserves its used declaration", "[llvm][static]") {
  Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
  Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
  std::vector<std::uint8_t> code{0xe8,0x0b,0,0,0,0xc3};code.resize(16,0xcc);code.push_back(0xc3);
  dis.disassemble(code,0x1000,graph);Function root{&graph,0x1000},callee{&graph,0x1010};
  LLVMLifter lifter{Architecture::X86_64};REQUIRE(lifter.setExecutionMode(static_cast<ExecutionMode>(2)));
  REQUIRE(lifter.lift(root,graph));auto *declaration=lifter.module().getFunction("dragon_static_x86_64_1010");REQUIRE(declaration);
  ignil::Block invalid{0x1010};invalid.append({ignil::Opcode::COPY,ignil::Reg{0,64,"RAX"},{ignil::Temp{999,64}},0x1010});
  REQUIRE(graph.getIGNILBlock(0x1010));*graph.getIGNILBlock(0x1010)=std::move(invalid);
  CHECK_FALSE(lifter.lift(callee,graph));
  CHECK(lifter.module().getFunction("dragon_static_x86_64_1010")==declaration);
  CHECK(declaration->isDeclaration());CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
}
