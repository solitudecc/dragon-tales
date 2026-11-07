#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace {
using namespace dragon;
struct Memory {std::uint64_t address;std::vector<std::uint8_t> bytes;};
struct AddressCase {
  Architecture arch;
  std::vector<std::uint8_t> code;
  std::map<unsigned,std::uint64_t> regs; // AX CX DX BX SP BP SI DI R8 ...
  std::vector<Memory> memory;
  std::uint64_t pc=0x100000;
};
std::vector<std::uint8_t> hex(std::string_view text) {
  auto digit=[](char c)->unsigned {
    if(c>='0' && c<='9')return c-'0';
    if(c>='a' && c<='f')return c-'a'+10;
    FAIL("bad addressing fixture hex");return 0;
  };
  REQUIRE(text.size()%2==0);std::vector<std::uint8_t> result;
  for(std::size_t i=0;i<text.size();i+=2)result.push_back((digit(text[i])<<4)|digit(text[i+1]));
  return result;
}
std::vector<std::uint8_t> word(std::uint64_t value,unsigned size=8) {
  std::vector<std::uint8_t> result(size);
  for(unsigned i=0;i<size;++i)result[i]=value>>(i*8);
  return result;
}
std::vector<std::uint8_t> pattern(unsigned size,unsigned salt) {
  std::vector<std::uint8_t> result(size);
  for(unsigned i=0;i<size;++i)result[i]=salt+37*i;
  return result;
}
struct CloseUc {void operator()(uc_engine *uc) const {uc_close(uc);}};

void compareAddress(const AddressCase &test) {
  const bool is64=test.arch==Architecture::X86_64;
  const unsigned count=is64?16:8;
  const int regs64[]={UC_X86_REG_RAX,UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_RBX,
    UC_X86_REG_RSP,UC_X86_REG_RBP,UC_X86_REG_RSI,UC_X86_REG_RDI,
    UC_X86_REG_R8,UC_X86_REG_R9,UC_X86_REG_R10,UC_X86_REG_R11,
    UC_X86_REG_R12,UC_X86_REG_R13,UC_X86_REG_R14,UC_X86_REG_R15};
  const int regs32[]={UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_EDX,UC_X86_REG_EBX,
    UC_X86_REG_ESP,UC_X86_REG_EBP,UC_X86_REG_ESI,UC_X86_REG_EDI};
  const int vectors[]={UC_X86_REG_XMM0,UC_X86_REG_XMM1,UC_X86_REG_XMM2,UC_X86_REG_XMM3,
    UC_X86_REG_XMM4,UC_X86_REG_XMM5,UC_X86_REG_XMM6,UC_X86_REG_XMM7,
    UC_X86_REG_XMM8,UC_X86_REG_XMM9,UC_X86_REG_XMM10,UC_X86_REG_XMM11,
    UC_X86_REG_XMM12,UC_X86_REG_XMM13,UC_X86_REG_XMM14,UC_X86_REG_XMM15};
  const std::pair<ignil::Reg,unsigned> flags[]={{x86::CF,0},{x86::PF,2},{x86::AF,4},
    {x86::ZF,6},{x86::SF,7},{x86::DF,10},{x86::OF,11}};
  CAPTURE(test.arch,test.code,test.pc);
  Disassembler dis{test.arch,{}};const auto listing=dis.disassemble(test.code,test.pc);
  std::size_t decoded=0;for(const auto &insn:listing)decoded+=insn.size();
  REQUIRE(decoded==test.code.size());
  const auto block=dis.liftTrace(listing);REQUIRE(block.unlifted().empty());
  uc_engine *raw=nullptr;REQUIRE(uc_open(UC_ARCH_X86,is64?UC_MODE_64:UC_MODE_32,&raw)==UC_ERR_OK);
  std::unique_ptr<uc_engine,CloseUc> uc{raw};
  std::set<std::uint64_t> pages;
  auto pagesFor=[&](std::uint64_t address,std::size_t size) {
    REQUIRE(size>0);REQUIRE(address<=~std::uint64_t{0}-size);
    const auto end=(address+size-1)&~std::uint64_t{4095};
    for(auto page=address&~std::uint64_t{4095};page<=end;page+=4096)pages.insert(page);
  };
  pagesFor(test.pc,test.code.size());
  for(const auto &region:test.memory)pagesFor(region.address,region.bytes.size());
  for(auto page:pages)REQUIRE(uc_mem_map(raw,page,4096,UC_PROT_ALL)==UC_ERR_OK);
  REQUIRE(uc_mem_write(raw,test.pc,test.code.data(),test.code.size())==UC_ERR_OK);
  ignil::SymbolicExecutor executor;executor.mapMemory(test.pc,test.code);
  for(const auto &region:test.memory) {
    REQUIRE(uc_mem_write(raw,region.address,region.bytes.data(),region.bytes.size())==UC_ERR_OK);
    executor.mapMemory(region.address,region.bytes);
  }
  std::array<std::uint64_t,16> initial{};
  for(unsigned i=0;i<count;++i)initial[i]=0x1234567800000100ull+0x111*i;
  initial[4]=0x3000;
  for(auto [index,value]:test.regs) {REQUIRE(index<count);initial[index]=value;}
  const auto registerWidth=static_cast<ignil::Width>(is64?64:32);
  for(unsigned i=0;i<count;++i) {
    if(!is64)initial[i]&=0xffffffff;
    REQUIRE(uc_reg_write(raw,is64?regs64[i]:regs32[i],&initial[i])==UC_ERR_OK);
    executor.setRegister({i*8,registerWidth,"address-input"},initial[i]);
    const std::array<std::uint64_t,2> xmm{0x0123456789abcdefull+i*17,0xfedcba9876543210ull-i*23};
    REQUIRE(uc_reg_write(raw,vectors[i],xmm.data())==UC_ERR_OK);
    executor.setRegister({x86::kOffXMM0+8*i,64,"XMM-low"},xmm[0]);
    executor.setRegister({x86::kOffXMM0H+8*i,64,"XMM-high"},xmm[1]);
  }
  const std::uint64_t initialFlags=0x246;
  REQUIRE(uc_reg_write(raw,UC_X86_REG_EFLAGS,&initialFlags)==UC_ERR_OK);
  for(auto [reg,bit]:flags)executor.setRegister(reg,(initialFlags>>bit)&1);
  const std::uint32_t cw=0x37f;REQUIRE(uc_reg_write(raw,UC_X86_REG_FPCW,&cw)==UC_ERR_OK);
  executor.setRegister({484,16,"FPCW"},cw);
  REQUIRE(uc_emu_start(raw,test.pc,test.pc+test.code.size(),0,100)==UC_ERR_OK);
  std::uint64_t pc=0;REQUIRE(uc_reg_read(raw,is64?UC_X86_REG_RIP:UC_X86_REG_EIP,&pc)==UC_ERR_OK);
  REQUIRE(pc==test.pc+test.code.size());
  REQUIRE(executor.run(block).complete());
  // evaluate() requires uniqueness, so an unseeded wrong address cannot
  // accidentally pass by choosing the expected value from one model.
  for(unsigned i=0;i<count;++i) {
    CAPTURE(i);
    std::uint64_t expected=0;REQUIRE(uc_reg_read(raw,is64?regs64[i]:regs32[i],&expected)==UC_ERR_OK);
    CHECK(executor.evaluate(executor.readRegister({i*8,registerWidth,"address-output"}))==expected);
    std::array<std::uint64_t,2> xmm{};REQUIRE(uc_reg_read(raw,vectors[i],xmm.data())==UC_ERR_OK);
    CHECK(executor.evaluate(executor.readRegister({x86::kOffXMM0+8*i,64,"XMM-low"}))==xmm[0]);
    CHECK(executor.evaluate(executor.readRegister({x86::kOffXMM0H+8*i,64,"XMM-high"}))==xmm[1]);
  }
  std::uint64_t expectedFlags=0;REQUIRE(uc_reg_read(raw,UC_X86_REG_EFLAGS,&expectedFlags)==UC_ERR_OK);
  for(auto [reg,bit]:flags) {
    CAPTURE(bit);CHECK(executor.evaluate(executor.readRegister(reg))==((expectedFlags>>bit)&1));
  }
  auto compareMemory=[&](std::uint64_t address,std::size_t size) {
    std::vector<std::uint8_t> bytes(size);REQUIRE(uc_mem_read(raw,address,bytes.data(),size)==UC_ERR_OK);
    for(std::size_t i=0;i<size;i+=8) {
      const auto amount=std::min<std::size_t>(8,size-i);std::uint64_t expected=0;
      for(std::size_t n=0;n<amount;++n)expected|=std::uint64_t(bytes[i+n])<<(8*n);
      CAPTURE(address,i);CHECK(executor.evaluate(executor.readMemory(address+i,amount*8))==expected);
    }
  };
  compareMemory(test.pc,test.code.size());
  for(const auto &region:test.memory)compareMemory(region.address,region.bytes.size());
}
}

TEST_CASE("Ordinary x86 memory addresses wrap independently of data width", "[x86-address][semantic]") {
  const std::vector<AddressCase> cases{
    {Architecture::X86_64,hex("678b448820"),{{0,0xdeadbeeffffffff0ull},{1,4}},
      {{0x20,word(0x11223344,4)},{0x100000020ull,word(0xaabbccdd,4)}}},
    {Architecture::X86_64,hex("678b448820"),{{0,0x20},{1,0x40000000}},
      {{0x40,word(0x11223344,4)},{0x100000040ull,word(0xaabbccdd,4)}}},
    {Architecture::X86_32,hex("678b4008"),{{3,0xabcdfffc},{6,0x12340008}},
      {{0xc,word(0x11223344,4)},{0x1000c,word(0xaabbccdd,4)}}},
    {Architecture::X86_32,hex("678b43fc"),{{5,0xabcd0000},{7,0x12340000}},
      {{0xfffc,word(0x11223344,4)},{0xfffffffc,word(0xaabbccdd,4)}}},
    {Architecture::X86_64,hex("48678b4308"),{{3,0x12345678fffffffcull}},
      {{4,word(0x11223344,4)},{0x100000004ull,word(0xaabbccdd,4)}}},
    {Architecture::X86_64,hex("67c74308401000008b02"),{{3,0x12345678fffffffcull},{2,0x100000004ull}},
      {{4,word(0,4)},{0x100000004ull,word(0x1060,4)}}},
  };
  for(const auto &test:cases)compareAddress(test);
}

TEST_CASE("Scratch memory forms retain address size and full register restoration", "[x86-address][semantic]") {
  compareAddress({Architecture::X86_64,hex("670fc14308"),{{0,7},{3,0xfffffffc}},
    {{4,word(5,4)},{0x100000004ull,word(9,4)}}});
  compareAddress({Architecture::X86_32,hex("670fc100"),{{0,7},{3,0xabcdfffc},{6,0x12340008}},
    {{4,word(5,4)},{0x10004,word(9,4)}}});
}

TEST_CASE("LEA and EIP relative loads use full instruction context", "[x86-address][semantic]") {
  for(const auto *code:{"67488d0425f0ffffff","678d0425f0ffffff","6467488d0425f0ffffff",
                        "48678d0425f0ffffff"})
    compareAddress({Architecture::X86_64,hex(code),{}, {}});
  compareAddress({Architecture::X86_32,hex("678d06f0ff"),{}, {}});
  compareAddress({Architecture::X86_64,hex("67488d05f0ffffff"),{}, {},0x100002000ull});
  compareAddress({Architecture::X86_64,hex("67488d0508000000"),{}, {},0xfffffffcu});
  compareAddress({Architecture::X86_64,hex("67488b05f0ffffff"),{},
    {{0xfffffff8,word(0x1122334455667788ull)}},0x100000000ull});
  compareAddress({Architecture::X86_64,hex("67488b0508000000"),{},
    {{0xc,word(0x1122334455667788ull)}},0xfffffffcu});
  // A trace must install each instruction's distinct next-PC, not reuse one.
  compareAddress({Architecture::X86_64,hex("67488d05f0ffffff67488d15e0ffffff"),{}, {},0x100002000ull});
  // Operand bytes 0x67 must not select addr32.
  compareAddress({Architecture::X86_64,hex("488d4367"),{{3,0x100000004ull}}, {}});
}

TEST_CASE("Moffs addresses are independent of accumulator width", "[x86-address][semantic]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(bool narrow:{false,true})
    for(unsigned width:{8u,16u,32u,64u})for(bool store:{false,true}) {
      if(width==64 && arch==Architecture::X86_32)continue;
      const unsigned addressWidth=arch==Architecture::X86_64?(narrow?32:64):(narrow?16:32);
      const std::uint64_t address=addressWidth==16?0xfffe:
        addressWidth==32?(arch==Architecture::X86_64?0xfffffff0:0x80002040):0x100002040ull;
      std::vector<std::uint8_t> code;if(narrow)code.push_back(0x67);
      if(width==16)code.push_back(0x66);if(width==64)code.push_back(0x48);
      code.push_back(store?(width==8?0xa2:0xa3):(width==8?0xa0:0xa1));
      auto immediate=word(address,addressWidth/8);code.insert(code.end(),immediate.begin(),immediate.end());
      CAPTURE(arch,narrow,width,store);
      compareAddress({arch,code,{{0,0xfedcba9876543210ull}},{{address,pattern(16,0x61)}}});
    }
}

TEST_CASE("BT bitstrings add signed word deltas before address wrapping", "[x86-address][semantic]") {
  struct BitCase {Architecture arch;unsigned width;std::uint64_t index,base,target;unsigned bit;};
  const BitCase cases[]={
    {Architecture::X86_64,32,32,0xfffffffc,0,0},
    {Architecture::X86_64,64,64,0xfffffff8,0,0},
    {Architecture::X86_32,32,32,0xabcdfffc,0,0},
    {Architecture::X86_32,16,16,0xabcdfffe,0,0},
    {Architecture::X86_64,32,0xffffffff,0,0xfffffffc,31},
    {Architecture::X86_64,64,~std::uint64_t{0},0,0xfffffff8,63},
    {Architecture::X86_32,32,0xffffffff,0xabcd0000,0xfffc,31},
    {Architecture::X86_32,16,0xffff,0xabcd0000,0xfffe,15},
    {Architecture::X86_32,32,0x10000,0xabcd0000,0x2000,0},
    {Architecture::X86_64,64,0x100000000ull,0,0x20000000,0},
    {Architecture::X86_64,64,0xfffffffeffffffffull,0,0xdffffff8,63},
  };
  for(const auto &test:cases) {
    auto code=hex(test.arch==Architecture::X86_64?(test.width==64?"67480fa308":"670fa308"):
      (test.width==16?"67660fa30f":"670fa30f"));
    // SETC saves the defined bit result; ADD then defines every arithmetic
    // flag. Compare all modeled flags, without excluding undefined BT flags.
    const auto tail=hex("0f92c283c700");code.insert(code.end(),tail.begin(),tail.end());
    const auto wrongBoundary=test.arch==Architecture::X86_64?0x100000000ull:0x10000ull;
    std::vector<Memory> memory{{0,std::vector<std::uint8_t>(16,0)},
      {wrongBoundary,std::vector<std::uint8_t>(16,0)},
      {test.target,word(std::uint64_t{1}<<test.bit,test.width/8)}};
    CAPTURE(test.width,test.index,test.base,test.target);
    compareAddress({test.arch,code,{{test.arch==Architecture::X86_64?0u:3u,test.base},{1,test.index}},memory});
  }
  // Register-index BTS/BTR/BTC share the word-address rule and must write the
  // same word they read, even after scratch restoration.
  for(const auto *body:{"670fab08","670fb308","670fbb08"}) {
    auto code=hex(body);const auto tail=hex("0f92c283c700");code.insert(code.end(),tail.begin(),tail.end());
    compareAddress({Architecture::X86_64,code,{{0,0xfffffffc},{1,32}},
      {{0,word(1,4)},{0x100000000ull,word(0,4)}}});
  }
  // An encoded imm8 is masked within its word. It must not be treated as a
  // signed register bitstring offset that adds an extra word to the EA.
  for(const auto *body:{"670fba600423","670fba700423"}) {
    auto code=hex(body);const auto tail=hex("0f92c283c700");code.insert(code.end(),tail.begin(),tail.end());
    compareAddress({Architecture::X86_64,code,{{0,0xfffffffc}},
      {{0,word(8,8)},{0x100000000ull,word(0,8)}}});
  }
}

TEST_CASE("POP addresses its destination after updating the full stack pointer", "[x86-address][semantic]") {
  compareAddress({Architecture::X86_64,hex("678f442410"),{{4,0x1fffffff0ull}},
    {{0,pattern(32,0x11)},{0x100000000ull,pattern(32,0x31)},
     {0x1ffffffe0ull,pattern(32,0x51)}}});
  compareAddress({Architecture::X86_64,hex("67ff742410"),{{4,0x1fffffff8ull}},
    {{0,pattern(32,0x11)},{0x100000000ull,pattern(32,0x31)},
     {0x1ffffffe0ull,pattern(32,0x51)}}});
  compareAddress({Architecture::X86_32,hex("8f442410"),{{4,0x3000}},{{0x2ff0,pattern(64,0x61)}}});
  compareAddress({Architecture::X86_32,hex("67ff30"),{{3,0xabcdfffc},{6,0x12340008},{4,0x3000}},
    {{4,word(0x11223344,4)},{0x10004,word(0xaabbccdd,4)},{0x2ff0,pattern(32,0x61)}}});
}

TEST_CASE("Address wrap selects the first byte and memory spans remain continuous", "[x86-address][semantic]") {
  // A LOAD at EA32=fffffffe consumes the next two bytes above4GiB, not0/1.
  compareAddress({Architecture::X86_64,hex("678b03"),{{3,0xabcdef01fffffffeull}},
    {{0xfffffff0,pattern(32,0x61)},{0,pattern(16,0x11)}}});
  compareAddress({Architecture::X86_32,hex("678b07"),{{3,0xabcdfffe}},
    {{0xfff0,pattern(32,0x61)},{0,pattern(16,0x11)}}});
  // The second SSE qword is a linear +8, after the initial EA has wrapped.
  for(const auto *code:{"670f1003","670f1103"})
    compareAddress({Architecture::X86_64,hex(code),{{3,0xabcdef01fffffff8ull}},
      {{0xfffffff0,pattern(32,0x61)},{0,pattern(32,0x11)}}});
  for(const auto *code:{"670f104308","670f114308"})
    compareAddress({Architecture::X86_64,hex(code),{{3,0xabcdef01fffffff8ull}},
      {{0,pattern(32,0x61)},{0x100000000ull,pattern(32,0x11)}}});
  // String pointer updates wrap at EA width, while this dword spans linearly.
  compareAddress({Architecture::X86_32,hex("67ad"),{{6,0xabcdfffe}},
    {{0xfff0,pattern(32,0x61)},{0,pattern(16,0x11)}}});
  compareAddress({Architecture::X86_64,hex("67ad"),{{6,0xabcdef01fffffffeull}},
    {{0xfffffff0,pattern(32,0x61)},{0,pattern(16,0x11)}}});
  // Dedicated x87 memory dispatch must use the same original prefix context.
  compareAddress({Architecture::X86_64,hex("4867d97b08"),{{3,0xabcdef01fffffffcull}},
    {{4,word(0,2)},{0x100000004ull,word(0xabcd,2)}}});
}
