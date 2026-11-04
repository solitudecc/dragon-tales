#include <catch2/catch_test_macros.hpp>

#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/ignil/IGNILSimplify.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace arm = dragon::arm;
namespace x86 = dragon::x86;
using dragon::ignil::Block;
using dragon::ignil::Const;
using dragon::ignil::deadStoreEliminate;
using dragon::ignil::Op;
using dragon::ignil::Opcode;
using dragon::ignil::Reg;
using dragon::ignil::SimplifyStats;
using dragon::ignil::Value;

namespace {
constexpr std::uint64_t kAddr = 0x1000;

// Does any surviving op still write `reg` (same offset+width)?
bool writesReg(const Block &b, const Reg &reg) {
  return std::any_of(b.begin(), b.end(), [&](const Op &op) {
    if (!op.dst)
      return false;
    const auto *r = std::get_if<Reg>(&*op.dst);
    return r && r->offset == reg.offset && r->width == reg.width;
  });
}
} // namespace

// ===========================================================================
// Unit tests: pass semantics on hand-built blocks (no lifter dependency).
// ===========================================================================

TEST_CASE("DSE - overwritten-before-read register store is removed",
          "[ignil][dse]") {
  // rdi = 1 ; rdi = 2   (the classic dead store: first write never read)
  Block block{kAddr};
  block.append({Opcode::COPY, Value{x86::RDI}, {Value{Const{1, 64}}}, kAddr});
  block.append(
          {Opcode::COPY, Value{x86::RDI}, {Value{Const{2, 64}}}, kAddr + 7});
  block.append({Opcode::RET, std::nullopt, {}, kAddr + 14});

  SimplifyStats stats;
  Block out = deadStoreEliminate(block, std::nullopt, stats);

  REQUIRE(stats.opsRemoved == 1);
  REQUIRE(out.size() == 2); // second COPY + RET
  REQUIRE(std::get<Const>(out.ops()[0].srcs[0]).value == 2);
}

TEST_CASE("DSE - a store read before being overwritten is kept",
          "[ignil][dse]") {
  // rdi = 1 ; rax = rdi ; rdi = 2   -- first rdi is read by the rax copy
  Block block{kAddr};
  block.append({Opcode::COPY, Value{x86::RDI}, {Value{Const{1, 64}}}, kAddr});
  block.append({Opcode::COPY, Value{x86::RAX}, {Value{x86::RDI}}, kAddr + 7});
  block.append(
          {Opcode::COPY, Value{x86::RDI}, {Value{Const{2, 64}}}, kAddr + 10});
  block.append({Opcode::RET, std::nullopt, {}, kAddr + 17});

  Block out = deadStoreEliminate(block, std::nullopt);
  REQUIRE(out.size() == 4); // nothing removed
}

TEST_CASE("DSE - sub-register writes alias by byte", "[ignil][dse]") {
  SECTION("writing AX kills a prior AL write") {
    // al = 1 ; ax = 2   -- AX (bytes [0,2)) covers AL (byte 0), so al=1 is dead
    Block block{kAddr};
    block.append({Opcode::COPY, Value{x86::AL}, {Value{Const{1, 8}}}, kAddr});
    block.append(
            {Opcode::COPY, Value{x86::AX}, {Value{Const{2, 16}}}, kAddr + 2});
    block.append({Opcode::RET, std::nullopt, {}, kAddr + 4});

    SimplifyStats stats;
    Block out = deadStoreEliminate(block, std::nullopt, stats);
    REQUIRE(stats.opsRemoved == 1);
    REQUIRE_FALSE(writesReg(out, x86::AL));
  }

  SECTION("AL and AH are independent bytes - neither kills the other") {
    // al = 1 ; ah = 2   -- AL is byte 0, AH is byte 1, no overlap: both live
    Block block{kAddr};
    block.append({Opcode::COPY, Value{x86::AL}, {Value{Const{1, 8}}}, kAddr});
    block.append(
            {Opcode::COPY, Value{x86::AH}, {Value{Const{2, 8}}}, kAddr + 2});
    block.append({Opcode::RET, std::nullopt, {}, kAddr + 4});

    Block out = deadStoreEliminate(block, std::nullopt);
    REQUIRE(writesReg(out, x86::AL));
    REQUIRE(writesReg(out, x86::AH));
  }
}

TEST_CASE("DSE - 32-bit write zero-extends over the 64-bit parent",
          "[ignil][dse]") {
  // eax = 1 ; rax = 2   -- rax fully overwrites the eax write (incl. the
  // implicit upper-half zero), so eax = 1 is dead.
  Block block{kAddr};
  block.append({Opcode::COPY, Value{x86::EAX}, {Value{Const{1, 32}}}, kAddr});
  block.append(
          {Opcode::COPY, Value{x86::RAX}, {Value{Const{2, 64}}}, kAddr + 6});
  block.append({Opcode::RET, std::nullopt, {}, kAddr + 13});

  SimplifyStats stats;
  Block out = deadStoreEliminate(block, std::nullopt, stats);
  REQUIRE(stats.opsRemoved == 1);
  REQUIRE_FALSE(writesReg(out, x86::EAX));
}

TEST_CASE("DSE - a 32-bit write's zero-extend range comes from the register file",
          "[ignil][dse]") {
  using dragon::ignil::DeadStoreOptions;

  // On AArch64 S0 is a 32-bit *lane view* of the 128-bit V0 at the same
  // offset, so `fmov s0, ...` zeroes all 16 bytes of V0 - not 8, the way W0
  // zeroes the 8 bytes of X0. A store is only droppable when every byte it
  // clobbers is dead, so under-stating that range at 8 bytes would let a write
  // whose upper V0 bytes are still live be removed.
  SECTION("an S write is kept when only the far half of its parent V is live") {
    Block block{kAddr};
    block.append({Opcode::COPY, Value{arm::S0}, {Value{Const{1, 32}}}, kAddr});
    block.append({Opcode::RET, std::nullopt, {}, kAddr + 4});

    // Live out: only V0's upper 8 bytes - past anything an 8-byte parent
    // covers, but squarely inside the 16 bytes the write really zeroes.
    const std::vector<Reg> liveOut{Reg{arm::kOffV0 + 8, 64, "V0[127:64]"}};

    DeadStoreOptions withFile;
    withFile.registerFile = arm::registerFile();
    REQUIRE(writesReg(deadStoreEliminate(block, liveOut, withFile), arm::S0));

    // Pins the documented fallback: with no register file the parent is assumed
    // to be 8 bytes, so the clobber is under-stated as V0[63:0], the live upper
    // half is never considered, and the write is dropped - leaving those bytes
    // stale instead of zeroed. This is why registerFile() must be passed for
    // any block that can touch FP/SIMD registers.
    REQUIRE_FALSE(writesReg(
            deadStoreEliminate(block, liveOut, DeadStoreOptions{}), arm::S0));
  }

  SECTION("an S write is still killed by a later write to its parent V") {
    Block block{kAddr};
    block.append({Opcode::COPY, Value{arm::S0}, {Value{Const{1, 32}}}, kAddr});
    block.append(
            {Opcode::COPY, Value{arm::V0}, {Value{Const{2, 128}}}, kAddr + 4});
    block.append({Opcode::RET, std::nullopt, {}, kAddr + 8});

    DeadStoreOptions opts;
    opts.registerFile = arm::registerFile();
    SimplifyStats stats;
    Block out = deadStoreEliminate(block, std::nullopt, stats, opts);

    REQUIRE(stats.opsRemoved == 1);
    REQUIRE_FALSE(writesReg(out, arm::S0));
    REQUIRE(writesReg(out, arm::V0));
  }

  SECTION("a W write still zero-extends over exactly its 8-byte X parent") {
    Block block{kAddr};
    block.append({Opcode::COPY, Value{arm::X0}, {Value{Const{1, 64}}}, kAddr});
    block.append(
            {Opcode::COPY, Value{arm::W0}, {Value{Const{2, 32}}}, kAddr + 4});
    block.append({Opcode::RET, std::nullopt, {}, kAddr + 8});

    DeadStoreOptions opts;
    opts.registerFile = arm::registerFile();
    SimplifyStats stats;
    Block out = deadStoreEliminate(block, std::nullopt, stats, opts);

    REQUIRE(stats.opsRemoved == 1); // the X0 write is fully clobbered by W0
    REQUIRE_FALSE(writesReg(out, arm::X0));
  }

  SECTION("the x86 register file leaves EAX->RAX behaviour unchanged") {
    Block block{kAddr};
    block.append({Opcode::COPY, Value{x86::RAX}, {Value{Const{1, 64}}}, kAddr});
    block.append(
            {Opcode::COPY, Value{x86::EAX}, {Value{Const{2, 32}}}, kAddr + 7});
    block.append({Opcode::RET, std::nullopt, {}, kAddr + 12});

    DeadStoreOptions opts;
    opts.registerFile = x86::registerFile();
    SimplifyStats stats;
    Block out = deadStoreEliminate(block, std::nullopt, stats, opts);

    REQUIRE(stats.opsRemoved == 1);
    REQUIRE_FALSE(writesReg(out, x86::RAX));
  }
}

TEST_CASE("DSE - precise live-out removes a final store that nullopt keeps",
          "[ignil][dse]") {
  // rax = 1 (last writer). Under nullopt (assume-all-live) it is live-out and
  // kept; with a live-out set that excludes RAX it is dead and removed.
  Block block{kAddr};
  block.append({Opcode::COPY, Value{x86::RAX}, {Value{Const{1, 64}}}, kAddr});
  block.append({Opcode::RET, std::nullopt, {}, kAddr + 7});

  REQUIRE(deadStoreEliminate(block, std::nullopt).size() == 2);

  std::vector<Reg> liveOut = {x86::RBX}; // RAX not live-out
  Block out = deadStoreEliminate(block, liveOut);
  REQUIRE(out.size() == 1); // only RET survives
  REQUIRE(out.ops()[0].opcode == Opcode::RET);
}

TEST_CASE("DSE preserves register state at interior transfers",
          "[ignil][dse][dse-transfer]") {
  for (auto opcode:{Opcode::CALL,Opcode::JMP,Opcode::CJMP,Opcode::RET}) {
    INFO(dragon::ignil::opcodeName(opcode));
    Block block{kAddr};
    block.append({Opcode::COPY,x86::RAX,{Const{3,64}},kAddr});
    block.append({Opcode::COPY,x86::RAX,{Const{7,64}},kAddr+1});
    std::vector<Value> operands;
    if (opcode==Opcode::CALL || opcode==Opcode::JMP) operands={Const{0x2000,64}};
    if (opcode==Opcode::CJMP) operands={Const{1,1},Const{0x2000,64},Const{kAddr+3,64}};
    block.append({opcode,std::nullopt,operands,kAddr+2});
    block.append({Opcode::COPY,x86::RAX,{Const{8,64}},kAddr+3});
    block.append({Opcode::COPY,x86::RAX,{Const{9,64}},kAddr+4});
    SimplifyStats stats;
    const auto out=deadStoreEliminate(block,std::nullopt,stats);
    // Values are observable at the first transfer as well as at trace exit.
    const auto writesAt=[&](std::uint64_t address) {
      return std::any_of(out.begin(),out.end(),[&](const Op &op) {
        return op.address==address && op.dst.has_value();
      });
    };
    CHECK(writesAt(kAddr+1));
    CHECK(writesAt(kAddr+4));
    CHECK_FALSE(writesAt(kAddr));
    CHECK_FALSE(writesAt(kAddr+3));
    CHECK(stats.opsRemoved==2);
  }
}

TEST_CASE("DSE keeps unknown callee inputs beyond the caller live out set",
          "[ignil][dse][dse-transfer]") {
  for (bool tail:{false,true}) {
    INFO("continuation " << tail);
    Block block{kAddr};
    const std::vector<Reg> inputs{x86::RDI,x86::CF,Reg{x86::kOffXMM0H,64,"XMM0H"}};
    for (unsigned i=0;i<inputs.size();++i)
      block.append({Opcode::COPY,inputs[i],{Const{i+1,inputs[i].width}},kAddr+i});
    block.append({Opcode::CALL,std::nullopt,{Const{0x2000,64}},kAddr+3});
    if (tail) {
      for (unsigned i=0;i<inputs.size();++i)
        block.append({Opcode::COPY,inputs[i],{Const{0,inputs[i].width}},kAddr+4+i});
    }
    const auto out=deadStoreEliminate(block,std::vector<Reg>{});
    for (unsigned i=0;i<inputs.size();++i) {
      INFO("input " << inputs[i].name);
      CHECK(std::any_of(out.begin(),out.end(),[&](const Op &op) {
        return op.address==kAddr+i && op.dst.has_value();
      }));
    }
  }
}

TEST_CASE("DSE retains the argument write in a lifted caller",
          "[ignil][dse][dse-transfer]") {
  // mov rdi,rcx; call 0x2000; mov rdi,9; ret. The callee returns RDI in RAX.
  // A full V2/Unicorn run must return the original RCX, not the initial RDI.
  const std::vector<std::uint8_t> bytes{0x48,0x89,0xcf,0xe8,0xf8,0x0f,0,0,
      0x48,0xc7,0xc7,9,0,0,0,0xc3};
  dragon::Configuration config;
  config.disassemblyStrategy=dragon::DisassemblyStrategy::LINEAR_SWEEP;
  dragon::Disassembler dis{dragon::Architecture::X86_64,config};
  dragon::Graph graph{dragon::Architecture::X86_64,config};
  dis.disassemble(bytes,kAddr,graph);
  const auto *block=graph.getIGNILBlock(kAddr);REQUIRE(block);
  REQUIRE(block->unlifted().empty());
  for (bool simplify:{false,true}) {
    INFO("simplify " << simplify);
    const auto input=simplify ? dragon::ignil::simplifyBlock(*block) : *block;
    const auto out=deadStoreEliminate(input);
    CHECK(std::any_of(out.begin(),out.end(),[](const Op &op) {
      return op.address==kAddr && op.opcode==Opcode::COPY && op.dst &&
          std::holds_alternative<Reg>(*op.dst) && std::get<Reg>(*op.dst).offset==x86::RDI.offset;
    }));
  }
}

// ===========================================================================
// Salwan's VMProtect dead-store example.
// https://github.com/JonathanSalwan/Triton  (dead_store_elimination.py)
//
// The published block is a wall of junk (rcl/shld/setcc/and/... writing
// registers and flags) interleaved with the real work: a run of pops ending
// in ret. Triton's block DSE collapses it to just the pops + popfq + ret,
// because every junk write is overwritten by a later junk write or a pop
// before it is ever read. We reproduce that here on the *lifted IGNIL*.
//
// The original addresses are non-contiguous (VMProtect threads the block
// through jumps), so we lay the shown instruction bytes out contiguously and
// linear-sweep them into one block: DSE acts on the instruction sequence, not
// the original layout.
//
// We drive it with the conservative std::nullopt (assume-everything-live-out)
// model - exactly Triton's - and it still collapses fully: every junk write is
// overwritten within the block before any read (a later junk write, a pop, or
// the `popfq` that re-loads all flags), while the pops / popfq / ret are the
// last writers and survive. This relies on `popfq` lifting to real ops
// (LifterGaps.md #6, fixed): without it the junk flag writes would never be
// clobbered and would anchor a live chain. The 8/16-bit ALU forms this sample
// uses still lift to UNDEF (an open gap), but that does not block the collapse
// here - those UNDEF register defs are themselves overwritten before use.
// ===========================================================================

TEST_CASE("DSE - collapses Salwan's VMProtect junk to the pops + ret",
          "[ignil][dse][x86_64]") {
  const std::vector<std::uint8_t> bytes = {
          0x66, 0xD3, 0xD7,             // rcl   di, cl
          0x58,                         // pop   rax
          0x66, 0x41, 0x0F, 0xA4, 0xDB, 0x01, // shld  r11w, bx, 1
          0x41, 0x5B,                   // pop   r11
          0x80, 0xE6, 0xCA,             // and   dh, 0CAh
          0x66, 0xF7, 0xD7,             // not   di
          0x5F,                         // pop   rdi
          0x66, 0x41, 0xC1, 0xC1, 0x0C, // rol   r9w, 0Ch
          0xF9,                         // stc
          0x41, 0x58,                   // pop   r8
          0xF5,                         // cmc
          0xF8,                         // clc
          0x66, 0x41, 0xC1, 0xE1, 0x0B, // shl   r9w, 0Bh
          0x5A,                         // pop   rdx
          0x66, 0x81, 0xF9, 0xEB, 0xD2, // cmp   cx, 0D2EBh
          0x48, 0x0F, 0xA3, 0xF1,       // bt    rcx, rsi
          0x41, 0x59,                   // pop   r9
          0x66, 0x41, 0x21, 0xE2,       // and   r10w, sp
          0x41, 0xC1, 0xD2, 0x10,       // rcl   r10d, 10h
          0x41, 0x5A,                   // pop   r10
          0x66, 0x0F, 0xBA, 0xF9, 0x0C, // btc   cx, 0Ch
          0x49, 0x0F, 0xCC,             // bswap r12
          0x48, 0x3D, 0x97, 0x74, 0x7D, 0xC7, // cmp rax, 0FFFFFFFFC77D7497h
          0x41, 0x5C,                   // pop   r12
          0x66, 0xD3, 0xC1,             // rol   cx, cl
          0xF5,                         // cmc
          0x66, 0x0F, 0xBA, 0xF5, 0x01, // btr   bp, 1
          0x66, 0x41, 0xD3, 0xFE,       // sar   r14w, cl
          0x5D,                         // pop   rbp
          0x66, 0x41, 0x29, 0xF6,       // sub   r14w, si
          0x66, 0x09, 0xF6,             // or    si, si
          0x01, 0xC6,                   // add   esi, eax
          0x66, 0x0F, 0xC1, 0xCE,       // xadd  si, cx
          0x9D,                         // popfq
          0x0F, 0x9F, 0xC1,             // setnle cl
          0x0F, 0x9E, 0xC1,             // setle cl
          0x4C, 0x0F, 0xBE, 0xF0,       // movsx r14, al
          0x59,                         // pop   rcx
          0xF7, 0xD1,                   // not   ecx
          0x59,                         // pop   rcx
          0x4C, 0x8D, 0xA8, 0xED, 0x19, 0x28, 0xC9, // lea r13, [rax-36D7E613h]
          0x66, 0xF7, 0xD6,             // not   si
          0x41, 0x5E,                   // pop   r14
          0x66, 0xF7, 0xD6,             // not   si
          0x66, 0x44, 0x0F, 0xBE, 0xEA, // movsx r13w, dl
          0x41, 0xBD, 0xB2, 0x6B, 0x48, 0xB7, // mov r13d, 0B7486BB2h
          0x5E,                         // pop   rsi
          0x66, 0x41, 0xBD, 0xCA, 0x44, // mov   r13w, 44CAh
          0x4C, 0x8D, 0xAB, 0x31, 0x11, 0x63, 0x14, // lea r13, [rbx+14631131h]
          0x41, 0x0F, 0xCD,             // bswap r13d
          0x41, 0x5D,                   // pop   r13
          0xC3,                         // ret
  };

  dragon::Configuration cfg;
  cfg.disassemblyStrategy = dragon::DisassemblyStrategy::LINEAR_SWEEP;
  cfg.syntax = dragon::Syntax::INTEL;
  dragon::Disassembler dis(dragon::Architecture::X86_64, cfg);
  dragon::Graph g(dragon::Architecture::X86_64, dragon::Configuration{});
  dis.disassemble(bytes, 0x1000, g);

  REQUIRE_FALSE(g.blocks().empty());
  const dragon::ignil::Block *lifted = g.getIGNILBlock(0x1000);
  REQUIRE(lifted != nullptr);
  REQUIRE_FALSE(lifted->empty());

  // Map each machine address to its instruction mnemonic (first token).
  std::unordered_map<std::uint64_t, std::string> mnemonicAt;
  for (const dragon::BasicBlock &bb: g.blocks()) {
    for (const dragon::Instruction *insn: bb.instructions()) {
      std::string d{insn->disassembly()};
      std::string m = d.substr(0, d.find(' '));
      std::transform(m.begin(), m.end(), m.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      mnemonicAt[insn->address()] = m;
    }
  }

  auto isKeeper = [](const std::string &m) {
    return m.rfind("pop", 0) == 0 /* pop, popfq, popf */ ||
           m.rfind("ret", 0) == 0 /* ret, retn */;
  };

  SimplifyStats stats;
  Block out = deadStoreEliminate(*lifted, std::nullopt, stats);

  std::cout << "\n=== Salwan VMProtect DSE ===\n"
            << "lifted ops: " << lifted->size() << "  ->  after DSE: "
            << out.size() << "  (removed " << stats.opsRemoved << ")\n";

  // The pass must have removed a large amount of junk...
  REQUIRE(stats.opsRemoved > 0);
  REQUIRE(out.size() < lifted->size());

  // ...and every surviving op must belong to a pop / popfq / ret instruction,
  // i.e. none of the junk arithmetic/flag instructions survive.
  for (const Op &op: out) {
    auto it = mnemonicAt.find(op.address);
    if (it == mnemonicAt.end())
      continue; // synthetic op with no machine address
    INFO("surviving op from `" << it->second << "` @ 0x" << std::hex
                               << op.address << "`: " << dragon::ignil::toString(op));
    REQUIRE(isKeeper(it->second));
  }

  // The real work must remain: the stack pops and the return.
  bool sawPop = false, sawRet = false;
  for (const Op &op: out) {
    auto it = mnemonicAt.find(op.address);
    if (it == mnemonicAt.end())
      continue;
    if (it->second.rfind("pop", 0) == 0)
      sawPop = true;
    if (it->second.rfind("ret", 0) == 0)
      sawRet = true;
  }
  REQUIRE(sawPop);
  REQUIRE(sawRet);
}
