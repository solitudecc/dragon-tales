#include <catch2/catch_test_macros.hpp>
#include <dragon/assembler/Assembler.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include <vector>
namespace {
using namespace dragon;
const ignil::Reg top{480,8,"top"},tag{482,16,"tag"},cw{484,16,"cw"},sw{486,16,"sw"};
ignil::Reg low(unsigned i) { return {400+10*i,64,"physical-low"}; }
ignil::Reg high(unsigned i) { return {408+10*i,16,"physical-high"}; }
void initial(ignil::SymbolicExecutor &ex,uint64_t sig=0xc000000000000000ull,uint16_t exp=0x3fff,uint16_t control=0x37f) {
  ex.setRegister(top,0);ex.setRegister(tag,0xfffc);ex.setRegister(cw,control);ex.setRegister(sw,0);
  for(unsigned i=0;i<8;++i) { ex.setRegister(low(i),i ? 0 : sig);ex.setRegister(high(i),i ? 0 : exp); }
  ex.setRegister(x86::RBX,0x200000);ex.setRegister(x86::RAX,0x123456789abcdef0ull);
  for (const auto r : {x86::CF,x86::PF,x86::AF,x86::ZF,x86::SF,x86::OF}) ex.setRegister(r,1);
  ex.mapMemory(0x200000,std::vector<uint8_t>(128,0xaa));
}
ignil::Block lift(const char *text,Architecture arch=Architecture::X86_32) {
  Assembler a{arch,Configuration{}};Disassembler d{arch,Configuration{}};
  return d.liftTrace(d.disassemble(a.assemble(text),0x100000));
}
}
TEST_CASE("x87 exact movement control and comparison forms are lifted", "[x87]") {
  const char *forms[]={"fld dword ptr [ebx]","fld qword ptr [ebx]","fld tbyte ptr [ebx]","fld st(0)",
    "fldz","fld1","fild word ptr [ebx]","fild dword ptr [ebx]","fild qword ptr [ebx]",
    "fst dword ptr [ebx]","fst qword ptr [ebx]","fst st(3)","fstp dword ptr [ebx]","fstp qword ptr [ebx]","fstp tbyte ptr [ebx]","fstp st(3)",
    "fist word ptr [ebx]","fist dword ptr [ebx]","fistp word ptr [ebx]","fistp dword ptr [ebx]","fistp qword ptr [ebx]",
    "fisttp word ptr [ebx]","fisttp dword ptr [ebx]","fisttp qword ptr [ebx]",
    "fxch st(3)","ffree st(3)","fincstp","fdecstp","fnstcw word ptr [ebx]","fldcw word ptr [ebx]","fnstsw ax","fnstsw word ptr [ebx]",
    "fcomi st, st(3)","fucomi st, st(3)","fcomip st, st(3)","fucomip st, st(3)","fcom st(3)","fcomp st(3)","fucom st(3)","fucomp st(3)",
    "fcompp","fucompp","fcom dword ptr [ebx]","fcomp qword ptr [ebx]","ftst","fxam","fchs","fabs","fninit","fnclex","fnop","wait"};
  for(const auto text:forms) {
    INFO(text);auto block=lift(text);REQUIRE(block.unlifted().empty());
    if(block.empty()) continue;
    llvmlifter::LLVMLifter llvm{Architecture::X86_32};REQUIRE(llvm.liftBlock(block));CHECK_FALSE(llvm::verifyModule(llvm.module(),&llvm::errs()));
  }
}
TEST_CASE("x87 stores obey RC and report native x86 exception status", "[x87][x87-native]") {
  // scripts/validation/x87_native.c reproduces these on Rosetta/real x86.
  struct Case { const char *text;uint64_t sig;uint16_t exp,rc;uint64_t result;unsigned width;uint16_t status,mask; };
  const Case cases[]={
    {"fistp dword ptr [ebx]",0xc000000000000000,0x3fff,0,2,32,0xa20,0x3aff},
    {"fistp dword ptr [ebx]",0xc000000000000000,0xbfff,0,0xfffffffe,32,0xa20,0x3aff},
    {"fistp dword ptr [ebx]",0xc000000000000000,0x3fff,1,1,32,0x820,0x3aff},
    {"fistp dword ptr [ebx]",0xc000000000000000,0xbfff,1,0xfffffffe,32,0xa20,0x3aff},
    {"fistp dword ptr [ebx]",0x8000000000000000,0x7ffe,0,0x80000000,32,0x801,0x3aff},
    {"fstp qword ptr [ebx]",0x8000000000000000,0x7ffe,0,0x7ff0000000000000,64,0xa28,0x3aff},
    {"fstp qword ptr [ebx]",0x8000000000000000,0x7ffe,3,0x7fefffffffffffff,64,0x828,0x3aff},
    {"fstp qword ptr [ebx]",0x8000000000000000,1,0,0,64,0x830,0x3aff},
    {"fstp qword ptr [ebx]",0x8000000000000000,1,2,1,64,0xa30,0x3aff},
    {"fstp qword ptr [ebx]",0xfffffffffffff800,0x3c00,0,0x0010000000000000,64,0xa30,0x3aff},
    {"fisttp dword ptr [ebx]",0xc000000000000000,0xbfff,1,0xffffffff,32,0x820,0x3aff},
    {"fstp dword ptr [ebx]",0x8000000000001234,0x7fff,0,0x7fc00000,32,0x801,0x38ff},
    {"fstp qword ptr [ebx]",0x8000000000001234,0x7fff,0,0x7ff8000000000002,64,0x801,0x38ff},
    {"fisttp qword ptr [ebx]",0x8000000000000000,0x7fff,0,0x8000000000000000,64,0x801,0x3aff},
    {"fstp tbyte ptr [ebx]",0x8000000000000001,0x7fff,0,0x8000000000000001,64,0x800,0x38ff},
  };
  for(const auto &c:cases) { INFO(c.text);INFO(c.rc);auto b=lift(c.text);REQUIRE(b.unlifted().empty());
    ignil::SymbolicExecutor ex;initial(ex,c.sig,c.exp,0x37f|(c.rc<<10));REQUIRE(ex.run(b).ok());
    CHECK(ex.evaluate(ex.readMemory(0x200000,c.width))==c.result);
    CHECK(ex.evaluate(ex.readRegister(sw)&ex.bv(c.mask,16))==(c.status&c.mask));CHECK(ex.evaluate(ex.readRegister(top))==1);
  }
}
TEST_CASE("x87 physical stack preserves extended bits and handles masked faults", "[x87]") {
  SECTION("raw copy and top wrap") {ignil::SymbolicExecutor ex;initial(ex,0x8000000000000001,0x7fff);
    auto b=lift("fld st(0); fxch st(1); fstp tbyte ptr [ebx]");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());
    CHECK(ex.evaluate(ex.readMemory(0x200000,64))==0x8000000000000001ull);CHECK(ex.evaluate(ex.readMemory(0x200008,16))==0x7fff);
    CHECK(ex.evaluate(ex.readRegister(top))==0);CHECK(ex.evaluate(ex.readRegister(low(0)))==0x8000000000000001ull);
  }
  SECTION("empty pop") {ignil::SymbolicExecutor ex;initial(ex);ex.setRegister(tag,0xffff);auto b=lift("fstp tbyte ptr [ebx]");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());
    CHECK(ex.evaluate(ex.readMemory(0x200000,64))==0xc000000000000000ull);CHECK(ex.evaluate(ex.readMemory(0x200008,16))==0xffff);
    CHECK(ex.evaluate(ex.readRegister(sw)&ex.bv(0x3aff,16))==0x841);
  }
  SECTION("overflow push") {ignil::SymbolicExecutor ex;initial(ex);ex.setRegister(tag,0);auto b=lift("fld1");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());
    CHECK(ex.evaluate(ex.readRegister(low(7)))==0xc000000000000000ull);CHECK(ex.evaluate(ex.readRegister(high(7)))==0xffff);
    CHECK(ex.evaluate(ex.readRegister(sw)&ex.bv(0x3aff,16))==0x3a41);
  }
  SECTION("reserved control bits") {for (const auto input:{0,0xffff}) {ignil::SymbolicExecutor ex;initial(ex);ex.mapMemory(0x200000,{uint8_t(input),uint8_t(input>>8)});
    auto b=lift("fldcw word ptr [ebx]");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK(ex.evaluate(ex.readRegister(cw))==((input&0x1f3f)|0x40));}}
  SECTION("invalid comparison has priority over memory denormal") {ignil::SymbolicExecutor ex;initial(ex,0xc000000000000001,0x7fff);ex.mapMemory(0x200000,{1,0,0,0});
    auto b=lift("fcom dword ptr [ebx]");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK(ex.evaluate(ex.readRegister(sw)&ex.bv(0x47ff,16))==0x4501);}
  SECTION("IEEE signaling NaNs are quieted on load") {for(const auto width:{32,64}) {ignil::SymbolicExecutor ex;initial(ex);uint64_t input=width==32 ? 0x7f801234 : 0x7ff0000000001234ull;std::vector<uint8_t> data(8);for(unsigned i=0;i<8;++i)data[i]=input>>(i*8);ex.mapMemory(0x200000,data);
    auto b=lift(width==32 ? "fld dword ptr [ebx]" : "fld qword ptr [ebx]");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK(ex.evaluate(ex.readRegister(low(7)))==(width==32 ? 0xc012340000000000ull : 0xc00000000091a000ull));CHECK(ex.evaluate(ex.readRegister(high(7)))==0x7fff);}}
  SECTION("FCOMI clears AF SF and OF") {ignil::SymbolicExecutor ex;initial(ex);auto b=lift("fcomi st, st(0)");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());
    for(const auto r:{x86::CF,x86::PF,x86::AF,x86::SF,x86::OF})CHECK(ex.evaluate(ex.readRegister(r))==0);
    CHECK(ex.evaluate(ex.readRegister(x86::ZF))==1);
  }
  SECTION("FXAM keeps unsupported extended encodings distinct from normals") {
    // Lua block 0x43319d: Unicorn dev incorrectly sets C2=1. Intel SDM and
    // x87_native.c on Rosetta give C3/C2/C0=000 for this J=0 E!=0 encoding.
    // This oracle regression already passes the old Dragon backend.
    for (const auto arch:{Architecture::X86_32,Architecture::X86_64}) {
      ignil::SymbolicExecutor ex;initial(ex);
      ex.mapMemory(0x200000,{0x5b,0x3b,0xb0,0xdb,0xad,0xd2,0x74,0x44,0x5c,0x7a});
      auto b=lift("fld tbyte ptr [ebx]; fxam; fnstsw ax; and ax, 17664; cmp ax, 256",arch);
      REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());
      CHECK(ex.evaluate(ex.readRegister(sw)&ex.bv(0x4500,16))==0);
      CHECK(ex.evaluate(ex.readRegister(x86::EAX))==0x9abc0000);
      CHECK(ex.evaluate(ex.readRegister(x86::CF))==1);
      CHECK(ex.evaluate(ex.readRegister(x86::SF))==1);
    }
  }
  SECTION("unmodelled environment stores invalidate memory") {ignil::SymbolicExecutor ex;initial(ex);auto b=lift("fnstenv [ebx]");REQUIRE_FALSE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK_FALSE(ex.evaluate(ex.readMemory(0x200000,64)).has_value());}
  SECTION("MMX aliases invalidate x87 and EMMS empties tags") {ignil::SymbolicExecutor ex;initial(ex);auto b=lift("paddb mm0, mm1");REQUIRE_FALSE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK_FALSE(ex.evaluate(ex.readRegister(low(0))).has_value());
    auto clear=lift("emms");REQUIRE(clear.unlifted().empty());REQUIRE(ex.run(clear).ok());CHECK(ex.evaluate(ex.readRegister(tag))==0xffff);}
  SECTION("extended state restore also invalidates XMM aliases") {ignil::SymbolicExecutor ex;initial(ex);const ignil::Reg lo{143,64,"XMM0"},hi{271,64,"XMM0H"};ex.setRegister(lo,123);ex.setRegister(hi,456);
    auto b=lift("fxrstor [ebx]");REQUIRE_FALSE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK_FALSE(ex.evaluate(ex.readRegister(lo)).has_value());CHECK_FALSE(ex.evaluate(ex.readRegister(hi)).has_value());}
  SECTION("unmasked exceptions leave trap-dependent status unknown") {ignil::SymbolicExecutor ex;initial(ex,0x8000000000000000,0x7ffe,0x377);auto b=lift("fstp qword ptr [ebx]");REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK_FALSE(ex.evaluate(ex.readMemory(0x200000,64)).has_value());CHECK_FALSE(ex.evaluate(ex.readRegister(sw)&ex.bv(0xff,16)).has_value());}
  SECTION("arithmetic stays unknown") {ignil::SymbolicExecutor ex;initial(ex);auto b=lift("fadd st, st(1)");REQUIRE_FALSE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK_FALSE(ex.evaluate(ex.readRegister(low(0))).has_value());}
}

TEST_CASE("x87 pseudo denormals raise denormal operand on comparison", "[x87][x87-native]") {
  // x87_native.c FCOM/FUCOM with {0x8000000000000000,0} produce DE=1.
  for(const auto text:{"fcom st(1)","fucom st(1)"}) {ignil::SymbolicExecutor ex;initial(ex,0x8000000000000000,0);
    ex.setRegister(low(1),0x8000000000000000);ex.setRegister(high(1),0x3fff);ex.setRegister(tag,0xfff2);
    auto b=lift(text);REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK(ex.evaluate(ex.readRegister(sw)&ex.bv(0x47ff,16))==0x102);
  }
}

TEST_CASE("x87 memory respects address size wrap and unknown segments", "[x87]") {
  for(const auto arch:{Architecture::X86_32,Architecture::X86_64}) {
    ignil::SymbolicExecutor ex;initial(ex);const uint64_t bits=0x3ff8000000000000;std::vector<uint8_t> data(8);for(unsigned i=0;i<8;++i)data[i]=bits>>(8*i);
    const bool wide=arch==Architecture::X86_64;ex.setRegister(x86::RBX,wide ? 0xaabbccddfffffffcull : 0xff80);ex.mapMemory(wide ? 0x200000 : 0x2000,data);
    auto b=lift(wide ? "fld qword ptr [ebx + 0x200004]" : "fld qword ptr [bx + 0x2080]",arch);REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK(ex.evaluate(ex.readRegister(low(7)))==0xc000000000000000ull);CHECK(ex.evaluate(ex.readRegister(high(7)))==0x3fff);
  }
  {ignil::SymbolicExecutor ex;initial(ex);const char *text="fld qword ptr [rip + 32]";Assembler assembler{Architecture::X86_64,Configuration{}};const auto size=assembler.assemble(text).size();uint64_t bits=0x3ff8000000000000;std::vector<uint8_t> data(8);for(unsigned i=0;i<8;++i)data[i]=bits>>(8*i);ex.mapMemory(0x100000+size+32,data);
    auto b=lift(text,Architecture::X86_64);REQUIRE(b.unlifted().empty());REQUIRE(ex.run(b).ok());CHECK(ex.evaluate(ex.readRegister(low(7)))==0xc000000000000000ull);CHECK(ex.evaluate(ex.readRegister(high(7)))==0x3fff);}
  {const char *text="fld qword ptr [eip + 32]";auto b=lift(text,Architecture::X86_64);
    if (b.unlifted().empty()) {ignil::SymbolicExecutor ex;initial(ex);Assembler a{Architecture::X86_64,Configuration{}};std::vector<uint8_t> source{0,0,0,0,0,0,0xf8,0x3f},wrong{0,0,0,0,0,0,0xf0,0x3f};ex.mapMemory(0x100000+a.assemble(text).size()+32,source);ex.mapMemory(32,wrong);REQUIRE(ex.run(b).ok());CHECK(ex.evaluate(ex.readRegister(low(7)))==0xc000000000000000ull);}
  }
  CHECK_FALSE(lift("fld qword ptr fs:[ebx]").unlifted().empty());
}
