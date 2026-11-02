#include <catch2/catch_test_macros.hpp>
#include <dragon/assembler/Assembler.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#include <dragon/lifters/ignil/IGNILSimplify.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/analysis/BackwardSlicer.hpp>
#include <llvm/IR/Verifier.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <array>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace {
constexpr uint64_t codeAddress=0x100000,dataAddress=0x200000;
struct CloseUnicorn { void operator()(uc_engine *engine) const { uc_close(engine); } };
const std::pair<dragon::ignil::Reg,unsigned> flags[]={{dragon::x86::CF,0},{dragon::x86::PF,2},
    {dragon::x86::AF,4},{dragon::x86::ZF,6},{dragon::x86::SF,7},{dragon::x86::DF,10},{dragon::x86::OF,11}};

void checkString(dragon::Architecture architecture,const std::vector<uint8_t> &code,
    const dragon::ignil::Block &block,bool addr32,unsigned count,bool df,int overlap,bool unmapped=false) {
  using namespace dragon;
  const bool is64=architecture==Architecture::X86_64;
  const int uc64[]={UC_X86_REG_RAX,UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_RBX,UC_X86_REG_RSP,UC_X86_REG_RBP,UC_X86_REG_RSI,UC_X86_REG_RDI};
  const int uc32[]={UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EDX,UC_X86_REG_EBX,UC_X86_REG_ESP,UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI};
  std::array<uint64_t,8> initial{0x1122334455667788ull,count,0x87654321,0x12345678,0x200100,0xabcdef,unmapped ? 0 : dataAddress+128,unmapped ? 0 : dataAddress+128+overlap};
  if (!is64) for (auto &value : initial) value &= 0xffffffff;
  if (addr32) for (unsigned index : {1,6,7}) initial[index] |= 0xaabbccdd00000000ull;
  const uint64_t initialFlags=0x8d7 | (df ? 1u<<10 : 0);
  std::vector<uint8_t> memory(256);
  for (unsigned i=0;i<memory.size();++i) memory[i]=i*37+11;
  uc_engine *raw=nullptr;
  REQUIRE(uc_open(UC_ARCH_X86,is64 ? UC_MODE_64 : UC_MODE_32,&raw)==UC_ERR_OK);
  std::unique_ptr<uc_engine,CloseUnicorn> emulator{raw};
  REQUIRE(uc_mem_map(raw,codeAddress,4096,UC_PROT_ALL)==UC_ERR_OK);
  REQUIRE(uc_mem_write(raw,codeAddress,code.data(),code.size())==UC_ERR_OK);
  if (!unmapped) {
    REQUIRE(uc_mem_map(raw,dataAddress,4096,UC_PROT_ALL)==UC_ERR_OK);
    REQUIRE(uc_mem_write(raw,dataAddress,memory.data(),memory.size())==UC_ERR_OK);
  }
  for (unsigned i=0;i<initial.size();++i) REQUIRE(uc_reg_write(raw,is64 ? uc64[i] : uc32[i],&initial[i])==UC_ERR_OK);
  REQUIRE(uc_reg_write(raw,UC_X86_REG_EFLAGS,&initialFlags)==UC_ERR_OK);
  REQUIRE(uc_emu_start(raw,codeAddress,codeAddress+code.size(),0,1000)==UC_ERR_OK);
  uint64_t pc=0; REQUIRE(uc_reg_read(raw,is64 ? UC_X86_REG_RIP : UC_X86_REG_EIP,&pc)==UC_ERR_OK);
  REQUIRE(pc==codeAddress+code.size());
  ignil::SymbolicExecutor executor;
  for (unsigned i=0;i<initial.size();++i) executor.setRegister(ignil::Reg{i*8,static_cast<ignil::Width>(is64 ? 64 : 32),"input"},initial[i]);
  for (auto [flag,bit] : flags) executor.setRegister(flag,(initialFlags>>bit)&1);
  if (!unmapped) executor.mapMemory(dataAddress,memory);
  REQUIRE(executor.run(block).complete());
  for (unsigned i=0;i<initial.size();++i) {
    uint64_t expected=0; REQUIRE(uc_reg_read(raw,is64 ? uc64[i] : uc32[i],&expected)==UC_ERR_OK);
    CHECK(executor.evaluate(executor.readRegister({i*8,static_cast<ignil::Width>(is64 ? 64 : 32),"output"}))==expected);
  }
  uint64_t expectedFlags=0; REQUIRE(uc_reg_read(raw,UC_X86_REG_EFLAGS,&expectedFlags)==UC_ERR_OK);
  for (auto [flag,bit] : flags) CHECK(executor.evaluate(executor.readRegister(flag))==((expectedFlags>>bit)&1));
  if (!unmapped) {
    REQUIRE(uc_mem_read(raw,dataAddress,memory.data(),memory.size())==UC_ERR_OK);
    for (unsigned i=0;i<memory.size();i+=8) {
      uint64_t expected=0; for (unsigned byte=0;byte<8;++byte) expected |= uint64_t(memory[i+byte])<<(byte*8);
      CHECK(executor.evaluate(executor.readMemory(dataAddress+i,64))==expected);
    }
  }
}
}

TEST_CASE("x86 strings preserve direction overlap count and flags", "[strings][semantic]") {
  using namespace dragon;
  for (const auto architecture : {Architecture::X86_32,Architecture::X86_64}) {
    Assembler assembler{architecture,Configuration{}}; Disassembler dis{architecture,Configuration{}};
    for (const bool addr32 : {false,true}) {
      if (addr32 && architecture != Architecture::X86_64) continue;
      for (const auto operation : {"movs","stos","lods","cmps","scas"})
        for (const auto suffix : {'b','w','d','q'}) {
          if (suffix=='q' && architecture==Architecture::X86_32) continue;
          for (const auto prefix : {"","rep ","repne "}) {
            const auto text=std::string(prefix)+operation+suffix;
            DYNAMIC_SECTION(static_cast<int>(architecture) << " addr32=" << addr32 << " " << text) {
              auto code=assembler.assemble(text);
              if (addr32) code.insert(code.begin(),0x67);
              const auto block=dis.liftTrace(dis.disassemble(code,codeAddress));
              REQUIRE(block.unlifted().empty());
              llvmlifter::LLVMLifter lifter{architecture}; REQUIRE(lifter.liftBlock(block));
              CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
              lifter.runMem2Reg(); lifter.runInstCombine();
              CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
              for (const auto count : {0u,1u,2u,5u}) {
                if (std::string(prefix).empty() && count!=1) continue;
                for (const bool df : {false,true})
                  checkString(architecture,code,block,addr32,count,df,df ? -1 : 1);
              }
            }
          }
        }
    }
  }
}

TEST_CASE("REP zero count never accesses an unmapped source or destination", "[strings][semantic]") {
  using namespace dragon;
  Assembler assembler{Architecture::X86_64,Configuration{}}; Disassembler dis{Architecture::X86_64,Configuration{}};
  for (const auto *text : {"rep movsq","rep stosq","rep lodsq","repe cmpsq","repne scasq"}) DYNAMIC_SECTION(text) {
    const auto code=assembler.assemble(text); const auto block=dis.liftTrace(dis.disassemble(code,codeAddress));
    REQUIRE(block.unlifted().empty());
    checkString(Architecture::X86_64,code,block,false,0,true,0,true);
  }
}

TEST_CASE("REP local loops are compiled as CFGs and sliced conservatively", "[strings][semantic]") {
  using namespace dragon;
  Assembler assembler{Architecture::X86_64,Configuration{}}; Disassembler dis{Architecture::X86_64,Configuration{}};
  auto code=assembler.assemble("rep stosq; mov rax, rcx; ret");
  Graph graph{Architecture::X86_64,Configuration{}}; dis.disassemble(code,codeAddress,graph);
  const auto *block=graph.getIGNILBlock(codeAddress); REQUIRE(block); REQUIRE(block->unlifted().empty());
  llvmlifter::LLVMLifter lifter{Architecture::X86_64}; REQUIRE(lifter.lift(Function{&graph,codeAddress},graph));
  lifter.runMem2Reg(); lifter.runInstCombine(); CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  const auto expression=BackwardSlicer{}.slice(x86::RAX,codeAddress,graph);
  CHECK(expression.visit([](const auto &node) { return std::is_same_v<std::decay_t<decltype(node)>,slice::Undef>; }));
  const auto trace=dis.liftTrace(dis.disassemble(assembler.assemble("rep movsb"),codeAddress));
  const auto simplified=ignil::simplifyBlock(trace);
  const auto eliminated=ignil::deadStoreEliminate(trace);
  CHECK(ignil::toString(simplified)==ignil::toString(trace)); CHECK(ignil::toString(eliminated)==ignil::toString(trace));
  ignil::SymbolicExecutor executor;
  executor.symbolizeRegister(x86::RCX,"runtime_count");
  const auto result=executor.run(trace);
  CHECK_FALSE(result.ok()); CHECK(result.exit==ignil::ExitKind::Error);
}

TEST_CASE("IGNIL local flow rejects malformed labels without retaining LLVM functions", "[strings][semantic][local-flow]") {
  using namespace dragon;
  for (const unsigned kind : {0u,1u,2u}) {
    ignil::Block block{codeAddress};
    if (kind==1) {
      block.append({ignil::Opcode::LABEL,std::nullopt,{},codeAddress,7});
      block.append({ignil::Opcode::LABEL,std::nullopt,{},codeAddress,7});
    } else {
      if (kind==2) block.append({ignil::Opcode::LABEL,std::nullopt,{},codeAddress,7});
      block.append({kind==2 ? ignil::Opcode::LOCALCJMP : ignil::Opcode::LOCALJMP,std::nullopt,
                    kind==2 ? std::vector<ignil::Value>{ignil::Const{1,8}} : std::vector<ignil::Value>{},codeAddress,7});
    }
    ignil::SymbolicExecutor executor;
    CHECK(executor.run(block).status==ignil::ExecStatus::MalformedOp);
    llvmlifter::LLVMLifter lifter{Architecture::X86_64};
    CHECK_FALSE(lifter.liftBlock(block)); CHECK(lifter.module().empty());
  }
}

TEST_CASE("IGNIL local loops have a budget and honor proved zero repeat counts", "[strings][semantic][local-flow]") {
  using namespace dragon;
  ignil::Block endless{codeAddress};
  endless.append({ignil::Opcode::LABEL,std::nullopt,{},codeAddress,7});
  endless.append({ignil::Opcode::LOCALJMP,std::nullopt,{},codeAddress,7});
  ignil::SymbolicExecutor executor;
  const auto stopped=executor.run(endless);
  CHECK(stopped.status==ignil::ExecStatus::UnsupportedOp); CHECK(stopped.error.find("budget")!=std::string::npos);
  Assembler assembler{Architecture::X86_64,Configuration{}}; Disassembler dis{Architecture::X86_64,Configuration{}};
  const auto block=dis.liftTrace(dis.disassemble(assembler.assemble("rep movsb"),codeAddress));
  ignil::SymbolicExecutor zero;
  zero.symbolizeRegister(x86::RCX,"proved_zero");
  zero.assume(zero.readRegister(x86::RCX)==zero.bv(0,64));
  CHECK(zero.run(block).complete());
}

TEST_CASE("Slicing retains linear definitions and unchanged registers around REP", "[strings][semantic][strings-slice]") {
  using namespace dragon;
  Assembler assembler{Architecture::X86_64,Configuration{}}; Disassembler dis{Architecture::X86_64,Configuration{}};
  for (const auto *text : {"mov rbx, 0x42; rep stosq; mov rax, rbx; ret",
                          "rep stosq; mov rax, 0x42; ret",
                          "rep stosq; mov qword ptr [rsp+16], 0x42; mov rax, qword ptr [rsp+16]; ret"}) DYNAMIC_SECTION(text) {
    Graph graph{Architecture::X86_64,Configuration{}};
    dis.disassemble(assembler.assemble(text),codeAddress,graph);
    const auto expression=BackwardSlicer{}.slice(x86::RAX,codeAddress,graph);
    const auto constant=expression.visit([](const auto &node)->std::optional<uint64_t> {
      if constexpr (std::is_same_v<std::decay_t<decltype(node)>,slice::Const>) return node.value;
      else return std::nullopt;
    });
    CHECK(constant==0x42);
  }
}
