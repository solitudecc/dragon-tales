#include <catch2/catch_test_macros.hpp>
#include <dragon/assembler/Assembler.hpp>
#include <dragon/lifters/ignil/IGNILSimplify.hpp>
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <unicorn/x86.h>
#include <array>
#include <memory>
#include <random>

namespace {
struct CloseOptimizerUnicorn {
  void operator()(uc_engine *uc) const { uc_close(uc); }
};
}

TEST_CASE("IGNIL optimized parent snapshots agree with Unicorn",
          "[ignil][simplify][simplify-snapshot]") {
  using namespace dragon;
  using namespace dragon::ignil;
  const std::array<std::uint64_t,8> inputs{0,1,0x1122334455667788ULL,
      0x8000000000000000ULL,0xffffffff00000000ULL,0x00000000ffffffffULL,
      0x7fffffffffffffffULL,~std::uint64_t{0}};
  for (auto architecture : {Architecture::X86_64,Architecture::ARM64}) {
    const bool a64=architecture==Architecture::ARM64;
    Configuration cfg;
    if (!a64) cfg.features="+bmi2";
    Assembler assembler{architecture,cfg};
    std::vector<std::pair<Reg,int>> regs;
    std::vector<std::pair<Reg,unsigned>> flags;
    if (a64) {
      for (unsigned i=0;i<31;++i)
        regs.push_back({Reg{i*8,64,"X"+std::to_string(i)},
            i<29 ? UC_ARM64_REG_X0+int(i) : i==29 ? UC_ARM64_REG_X29 : UC_ARM64_REG_X30});
      regs.push_back({arm::SP,UC_ARM64_REG_SP});
      regs.push_back({arm::XZR,UC_ARM64_REG_XZR});
      flags={{arm::NF,31},{arm::ZF,30},{arm::CF,29},{arm::VF,28}};
    } else {
      const int ids[]={UC_X86_REG_RAX,UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_RBX,
          UC_X86_REG_RSP,UC_X86_REG_RBP,UC_X86_REG_RSI,UC_X86_REG_RDI,
          UC_X86_REG_R8,UC_X86_REG_R9,UC_X86_REG_R10,UC_X86_REG_R11,
          UC_X86_REG_R12,UC_X86_REG_R13,UC_X86_REG_R14,UC_X86_REG_R15};
      for (unsigned i=0;i<16;++i) regs.push_back({Reg{i*8,64,"GPR"},ids[i]});
      flags={{x86::CF,0},{x86::PF,2},{x86::AF,4},{x86::ZF,6},
          {x86::SF,7},{x86::DF,10},{x86::OF,11}};
    }
    for (Width width : {Width{8},Width{16},Width{32}}) {
      std::vector<std::uint8_t> code;
      auto append=[&](const std::string &text) {
        const auto bytes=assembler.assemble(text);
        REQUIRE_FALSE(bytes.empty());
        code.insert(code.end(),bytes.begin(),bytes.end());
      };
      // Equivalent straight-line machine code: extract the old upper value
      // before clearing the source. RORX/BFXIL/LSR/MOV preserve every flag.
      if (a64) {
        if (width==32) append("lsr x1, x0, #32");
        else append("bfxil x1, x0, #32, #"+std::to_string(width));
        append("mov w0, #7");
      } else {
        append("rorx rax, rax, 32");
        append(width==8 ? "mov bl, al" : width==16 ? "mov bx, ax" : "mov ebx, eax");
        append("mov eax, 7");
      }
      for (bool doubleNot : {false,true}) {
        Block original{0x1000};
        const Reg upper{4,width,"upper"},low{0,32,"low"};
        const Reg destination{a64 ? 8u : 24u,width,"result"};
        const Temp saved{0,width};
        const auto operation=doubleNot ? Opcode::NOT : Opcode::COPY;
        original.append({operation,saved,{upper},0x1000});
        original.append({Opcode::COPY,low,{Const{7,32}},0x1001});
        original.append({operation,destination,{saved},0x1002});
        const auto optimized=simplifyBlock(original);
        for (auto input : inputs) {
          INFO("architecture " << int(architecture) << " width " << unsigned(width)
               << " double NOT " << doubleNot << " input " << input);
          uc_engine *raw=nullptr;
          REQUIRE(uc_open(a64 ? UC_ARCH_ARM64 : UC_ARCH_X86,
                         a64 ? UC_MODE_ARM : UC_MODE_64,&raw)==UC_ERR_OK);
          const std::unique_ptr<uc_engine,CloseOptimizerUnicorn> uc{raw};
          REQUIRE(uc_mem_map(raw,0x1000,4096,UC_PROT_ALL)==UC_ERR_OK);
          REQUIRE(uc_mem_write(raw,0x1000,code.data(),code.size())==UC_ERR_OK);
          REQUIRE(uc_mem_map(raw,0x8000,4096,UC_PROT_ALL)==UC_ERR_OK);
          std::vector<std::uint8_t> memory(64);
          for (unsigned i=0;i<memory.size();++i) memory[i]=std::uint8_t(input+i*13);
          REQUIRE(uc_mem_write(raw,0x8000,memory.data(),memory.size())==UC_ERR_OK);
          SymbolicExecutor before,after;
          std::mt19937_64 rng{input^0x513a37};
          for (auto [reg,id] : regs) {
            auto initial=reg.offset==0 ? input : rng();
            if (a64 && reg.offset==arm::XZR.offset) initial=0;
            REQUIRE(uc_reg_write(raw,id,&initial)==UC_ERR_OK);
            before.setRegister(reg,initial); after.setRegister(reg,initial);
          }
          std::uint64_t flagBits=a64 ? 0 : 2;
          for (auto [flag,bit] : flags) {
            const auto value=rng()&1;
            flagBits|=value<<bit;
            before.setRegister(flag,value); after.setRegister(flag,value);
          }
          REQUIRE(uc_reg_write(raw,a64 ? UC_ARM64_REG_NZCV : UC_X86_REG_EFLAGS,&flagBits)==UC_ERR_OK);
          before.mapMemory(0x8000,memory); after.mapMemory(0x8000,memory);
          REQUIRE(uc_emu_start(raw,0x1000,0x1000+code.size(),0,100)==UC_ERR_OK);
          REQUIRE(before.run(original).ok()); REQUIRE(after.run(optimized).ok());
          for (auto [reg,id] : regs) {
            std::uint64_t expected=0;
            REQUIRE(uc_reg_read(raw,id,&expected)==UC_ERR_OK);
            CHECK(before.evaluate(before.readRegister(reg))==expected);
            CHECK(after.evaluate(after.readRegister(reg))==expected);
          }
          REQUIRE(uc_reg_read(raw,a64 ? UC_ARM64_REG_NZCV : UC_X86_REG_EFLAGS,&flagBits)==UC_ERR_OK);
          for (auto [flag,bit] : flags) {
            CHECK(before.evaluate(before.readRegister(flag))==((flagBits>>bit)&1));
            CHECK(after.evaluate(after.readRegister(flag))==((flagBits>>bit)&1));
          }
          REQUIRE(uc_mem_read(raw,0x8000,memory.data(),memory.size())==UC_ERR_OK);
          for (unsigned i=0;i<memory.size();i+=8) {
            std::uint64_t expected=0;
            for (unsigned b=0;b<8;++b) expected|=std::uint64_t(memory[i+b])<<(b*8);
            CHECK(before.evaluate(before.readMemory(0x8000+i,64))==expected);
            CHECK(after.evaluate(after.readMemory(0x8000+i,64))==expected);
          }
        }
      }
    }
  }
}
