#include <dragon/analysis/ArgumentRecovery.hpp>

#include <algorithm>
#include <format>
#include <map>

#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Function.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

namespace dragon::analysis {

namespace {

/// Integer-argument registers, in calling-convention order, for `arch`.
std::vector<ignil::Reg> argumentRegisters(Architecture arch,
                                          CallingConvention cc) {
  switch (arch) {
    case Architecture::X86_64:
      // Microsoft x64 (Windows): rcx, rdx, r8, r9.
      if (cc == CallingConvention::Win64)
        return {x86::RCX, x86::RDX, x86::R8, x86::R9};
      // System V AMD64: rdi, rsi, rdx, rcx, r8, r9.
      return {x86::RDI, x86::RSI, x86::RDX, x86::RCX, x86::R8, x86::R9};
    case Architecture::ARM64:
      // AAPCS64: x0..x7.
      return {arm::X0, arm::X1, arm::X2, arm::X3,
              arm::X4, arm::X5, arm::X6, arm::X7};
    default:
      return {};
  }
}

/// The register a return value is delivered in, for `arch`.
std::optional<ignil::Reg> returnRegister(Architecture arch) {
  switch (arch) {
    case Architecture::X86_64:
      return x86::RAX;
    case Architecture::ARM64:
      return arm::X0;
    default:
      return std::nullopt;
  }
}

} // namespace

std::set<std::uint32_t> liveInRegisters(const Function &fn,
                                        const Graph &graph) {
  if (!fn.isValid())
    return {};

  const auto &blocks = fn.blocks();

  // Per-block gen/kill over register offsets, plus intra-function successors.
  //   gen  - offsets read before any write in the block (upward-exposed uses)
  //   kill - offsets written anywhere in the block
  struct BlockInfo {
    std::set<std::uint32_t> gen;
    std::set<std::uint32_t> kill;
    std::vector<std::uint64_t> succs;
  };
  std::map<std::uint64_t, BlockInfo> info;

  for (const auto &[addr, bb] : blocks) {
    BlockInfo bi;
    if (const ignil::Block *ib = graph.getIGNILBlock(addr)) {
      std::set<std::uint32_t> definedLocal;
      for (const auto &op : ib->ops()) {
        for (const auto &src : op.srcs)
          if (const auto *r = std::get_if<ignil::Reg>(&src))
            if (!definedLocal.count(r->offset))
              bi.gen.insert(r->offset);
        if (op.dst)
          if (const auto *r = std::get_if<ignil::Reg>(&*op.dst)) {
            definedLocal.insert(r->offset);
            bi.kill.insert(r->offset);
          }
      }
    }
    for (std::uint64_t s : bb.successors())
      if (blocks.count(s))
        bi.succs.push_back(s);
    info[addr] = std::move(bi);
  }

  // Backward live-variable fixpoint:
  //   liveOut[B] = U liveIn[S] for S in succ(B)
  //   liveIn[B]  = gen[B] U (liveOut[B] \ kill[B])
  std::map<std::uint64_t, std::set<std::uint32_t>> liveIn;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[addr, bi] : info) {
      std::set<std::uint32_t> out;
      for (std::uint64_t s : bi.succs) {
        const auto &li = liveIn[s];
        out.insert(li.begin(), li.end());
      }
      std::set<std::uint32_t> in = bi.gen;
      for (std::uint32_t r : out)
        if (!bi.kill.count(r))
          in.insert(r);
      if (in != liveIn[addr]) {
        liveIn[addr] = std::move(in);
        changed = true;
      }
    }
  }

  return liveIn[fn.startAddress()];
}

FunctionSignature recoverSignature(const Function &fn, const Graph &graph,
                                   Architecture arch) {
  FunctionSignature sig;
  if (!fn.isValid())
    return sig;

  const std::set<std::uint32_t> liveIn = liveInRegisters(fn, graph);

  // Widest width each register offset is *read* at anywhere in the body - used
  // to size arguments (a function that only ever reads EDI takes an i32, not
  // an i64). Falls back to the full register width when a register is live-in
  // but, oddly, never read at a determinable width.
  std::map<std::uint32_t, ignil::Width> readWidth;
  for (const auto &[addr, bb] : fn.blocks()) {
    const ignil::Block *ib = graph.getIGNILBlock(addr);
    if (!ib)
      continue;
    for (const auto &op : ib->ops())
      for (const auto &src : op.srcs)
        if (const auto *r = std::get_if<ignil::Reg>(&src)) {
          auto &w = readWidth[r->offset];
          w = std::max(w, r->width);
        }
  }

  // Arguments: the contiguous convention prefix up to the highest argument
  // register that is live-in, each sized by how wide it is actually read.
  const std::vector<ignil::Reg> argRegs =
          argumentRegisters(arch, graph.configuration().callingConvention);
  int highest = -1;
  for (int i = 0; i < static_cast<int>(argRegs.size()); ++i)
    if (liveIn.count(argRegs[i].offset))
      highest = i;
  for (int i = 0; i <= highest; ++i) {
    ignil::Reg r = argRegs[i];
    if (auto it = readWidth.find(r.offset); it != readWidth.end() && it->second)
      r.width = it->second;
    sig.arguments.push_back({r, static_cast<unsigned>(i)});
  }

  // Return value: present iff the convention's return register is written
  // somewhere in the body. Its width is the widest write observed.
  if (auto ret = returnRegister(arch)) {
    ignil::Width width = 0;
    for (const auto &[addr, bb] : fn.blocks()) {
      const ignil::Block *ib = graph.getIGNILBlock(addr);
      if (!ib)
        continue;
      for (const auto &op : ib->ops())
        if (op.dst)
          if (const auto *r = std::get_if<ignil::Reg>(&*op.dst))
            if (r->offset == ret->offset)
              width = std::max(width, r->width);
    }
    if (width != 0) {
      ignil::Reg r = *ret;
      r.width = width;
      sig.returnValue = r;
    }
  }

  return sig;
}

std::string toString(const FunctionSignature &sig, std::uint64_t address) {
  std::string out = sig.returnValue
                            ? "i" + std::to_string(sig.returnValue->width)
                            : "void";
  out += std::format(" sub_{:x}(", address);
  for (std::size_t i = 0; i < sig.arguments.size(); ++i) {
    const auto &a = sig.arguments[i];
    if (i)
      out += ", ";
    out += "i" + std::to_string(a.reg.width);
    if (!a.reg.name.empty())
      out += " " + a.reg.name;
  }
  if (sig.arguments.empty())
    out += "void";
  out += ")";
  return out;
}

} // namespace dragon::analysis
