#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Instructions.h>
using namespace dragon;
using namespace dragon::llvmlifter;

TEST_CASE("LLVM static entry policy requires an empty static module", "[llvm][static][static-entry]") {
  LLVMLifter lifter{Architecture::X86_64};
  CHECK_FALSE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StatefulV2));
  CHECK_FALSE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  CHECK_FALSE(lifter.setStaticEntryPolicy(static_cast<StaticEntryPolicy>(2)));
  REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
  CHECK_FALSE(lifter.setExecutionMode(ExecutionMode::StatefulV2));
  REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::Resumable));
  CHECK(lifter.setExecutionMode(ExecutionMode::Legacy));
}

TEST_CASE("LLVM static function entry excludes external resume roots", "[llvm][static][static-entry]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64,Architecture::ARM64})
    for (auto strategy : {DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL})
      for (bool restricted : {false,true}) {
        INFO("arch=" << static_cast<int>(arch) << " restricted=" << restricted);
        Configuration config;config.disassemblyStrategy=strategy;
        Disassembler dis{arch,config};Graph graph{arch,config};
        const auto code=arch==Architecture::ARM64
            ? std::vector<std::uint8_t>{0x40,0,0,0x34,0x20,0,0x80,0x52,0xc0,0x03,0x5f,0xd6}
            : std::vector<std::uint8_t>{0x85,0xff,0x74,5,0xb8,1,0,0,0,0xc3};
        dis.disassemble(code,0x1000,graph);Function fn{&graph,0x1000};REQUIRE(fn.isValid());
        LLVMLifter lifter{arch};REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
        if (restricted) REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
        REQUIRE(lifter.lift(fn,graph));
        const auto name="dragon_static_"+std::string(restricted?"entry_":"")+ArchitectureName(arch)+"_1000";
        auto *llfn=lifter.module().getFunction(name);REQUIRE(llfn);
        auto *dispatch=llvm::dyn_cast<llvm::SwitchInst>(llfn->getEntryBlock().getTerminator());REQUIRE(dispatch);
        CHECK(dispatch->getNumCases()==(restricted?1:fn.blocks().size()));
        CHECK(llfn->hasFnAttribute("dragon.static-entry-policy")==restricted);
        if (restricted) CHECK(llfn->getFnAttribute("dragon.entrypoints").getValueAsString()=="1000");
        CHECK_FALSE(lifter.setStaticEntryPolicy(StaticEntryPolicy::Resumable));
        CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
        lifter.runMem2Reg();lifter.runInstCombine();lifter.runGVN();lifter.runDCE();
        CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
        CHECK(lifter.ir().find("unreachable")==std::string::npos);
      }
}

TEST_CASE("LLVM static function entry exposes preceding register definitions", "[llvm][static][static-entry]") {
  for (bool restricted : {false,true}) {
    Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
    Disassembler dis{Architecture::X86_64,config};Graph graph{Architecture::X86_64,config};
    // mov eax,7; jmp 1007; mov [rbx],eax; ret. Resuming at 1007 may
    // store arbitrary EAX; only the function-start view can fold this store.
    dis.disassemble(std::vector<std::uint8_t>{0xb8,7,0,0,0,0xeb,0,0x89,3,0xc3},0x1000,graph);
    Function fn{&graph,0x1000};REQUIRE(fn.blocks().size()>1);
    LLVMLifter lifter{Architecture::X86_64};REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
    if (restricted) REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
    REQUIRE(lifter.lift(fn,graph));
    lifter.runMem2Reg();lifter.runInstCombine();lifter.runGVN();lifter.runDCE();
    CHECK((lifter.ir().find("store i32 7,")!=std::string::npos)==restricted);
    CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  }
}

TEST_CASE("LLVM static function entry uses matching internal declarations", "[llvm][static][static-entry]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64,Architecture::ARM64})
    for (bool callerFirst : {false,true}) {
      Configuration config;config.disassemblyStrategy=DisassemblyStrategy::LINEAR_SWEEP;
      Disassembler dis{arch,config};Graph graph{arch,config};
      auto code=arch==Architecture::ARM64
          ? std::vector<std::uint8_t>{4,0,0,0x94,0xc0,3,0x5f,0xd6}
          : std::vector<std::uint8_t>{0xe8,0x0b,0,0,0,0xc3};
      code.resize(16,0xcc);
      const auto body=arch==Architecture::ARM64
          ? std::vector<std::uint8_t>{0xe0,0,0x80,0x52,0xc0,3,0x5f,0xd6}
          : std::vector<std::uint8_t>{0xb8,7,0,0,0,0xc3};
      code.insert(code.end(),body.begin(),body.end());dis.disassemble(code,0x1000,graph);
      Function root{&graph,0x1000},callee{&graph,0x1010};REQUIRE(root.isValid());REQUIRE(callee.isValid());
      LLVMLifter lifter{arch};REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
      REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
      REQUIRE(lifter.lift(callerFirst?root:callee,graph));
      const auto name="dragon_static_entry_"+ArchitectureName(arch)+"_1010";
      auto *symbol=lifter.module().getFunction(name);REQUIRE(symbol);
      CHECK(symbol->isDeclaration()==callerFirst);
      REQUIRE(lifter.lift(callerFirst?callee:root,graph));
      CHECK(lifter.module().getFunction(name)==symbol);CHECK_FALSE(symbol->isDeclaration());
      CHECK(lifter.ir().find("call void @"+name+"(")!=std::string::npos);
      CHECK(lifter.ir().find("@dragon_static_"+ArchitectureName(arch)+"_")==std::string::npos);
      CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
    }
}

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
