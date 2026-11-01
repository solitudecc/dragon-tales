#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Graph.hpp>

#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include "utils/Utils.hpp"

#include <algorithm>
#include <type_traits>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dragon {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

struct SuccessorInfo {
  std::vector<std::uint64_t> known;
  bool hasIndirect = false;
};

// Collect all direct successors of a block by inspecting its terminal Op.
// Returns up to two target addresses (JMP → one, CJMP → two).
SuccessorInfo successorsOf(const ignil::Block &block) {
  if (block.empty())
    return {};
  // Retrieve the last instruction, it must be a jump, ret,
  // etc.
  const ignil::Op &last = block.ops().back();
  SuccessorInfo info;

  auto pushValue = [&](const ignil::Value &v) {
    // If the value from the JMP instruction is a constant
    // we don't need any calculation
    if (const auto *c = std::get_if<ignil::Const>(&v))
      info.known.push_back(c->value);
    else
      info.hasIndirect = true;
  };
  if (last.opcode == ignil::Opcode::JMP && !last.srcs.empty()) {
    pushValue(last.srcs[0]);
  } else if (last.opcode == ignil::Opcode::CJMP && last.srcs.size() >= 3) {
    pushValue(last.srcs[1]); // taken
    pushValue(last.srcs[2]); // fallthrough
  }
  return info;
}

// The parent registers of `arch`'s register file, used to size the implicit
// zeroing done by a 32-bit write (EAX/W0 clear 8 bytes, S0 clears V0's 16).
std::vector<ignil::RegExtent> registerFileFor(Architecture arch) {
  switch (arch) {
    case Architecture::X86_32:
    case Architecture::X86_64:
      return x86::registerFile();
    case Architecture::ARM32:
    case Architecture::ARM64:
      return arm::registerFile();
    default:
      return {};
  }
}

std::uint32_t byteCount(ignil::Width w) { return (w + 7u) / 8u; }

// Bytes of the register space a write to `w` replaces, starting at w.offset.
// The IGNIL convention (shared with SymbolicExecutor and dead-store
// elimination): a 32-bit write zero-extends into the rest of its parent
// (EAX -> RAX, W0 -> X0, S0 -> V0); 8- and 16-bit writes leave the
// surrounding bytes alone.
std::uint32_t clobberBytes(const ignil::Reg &w,
                           const std::vector<ignil::RegExtent> &regFile) {
  const std::uint32_t own = byteCount(w.width);
  if (w.width != 32)
    return own;
  for (const ignil::RegExtent &p: regFile)
    if (p.offset <= w.offset && w.offset + own <= p.offset + p.byteSize)
      return p.offset + p.byteSize - w.offset;
  return std::max<std::uint32_t>(own, 8);
}

std::uint64_t lowMask(unsigned bits) {
  return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
}

slice::Expr mkConst(std::uint64_t v, ignil::Width w) {
  return slice::Expr{slice::Const{v & lowMask(w), w}};
}

slice::Expr mkBin(ignil::Opcode op, slice::Expr l, slice::Expr r) {
  return slice::Expr{slice::BinOp{op, std::make_shared<slice::Expr>(std::move(l)),
                                  std::make_shared<slice::Expr>(std::move(r))}};
}

// ZEXT/TRUNC `e` (currently `from` bits wide) to `to` bits; no-op if equal.
slice::Expr resize(slice::Expr e, ignil::Width from, ignil::Width to) {
  if (from == to)
    return e;
  return slice::Expr{slice::UnaryOp{
          to > from ? ignil::Opcode::ZEXT : ignil::Opcode::TRUNC,
          std::make_shared<slice::Expr>(std::move(e)), to}};
}

// Made of constants and operations on them only: the same on every path.
bool isConstantOnly(const slice::Expr &e) {
  return e.visit([](const auto &n) -> bool {
    using T = std::decay_t<decltype(n)>;
    if constexpr (std::is_same_v<T, slice::Const>)
      return true;
    else if constexpr (std::is_same_v<T, slice::BinOp>)
      return n.left && n.right && isConstantOnly(*n.left) &&
             isConstantOnly(*n.right);
    else if constexpr (std::is_same_v<T, slice::UnaryOp>)
      return n.operand && isConstantOnly(*n.operand);
    else
      return false;
  });
}

} // namespace

// ---------------------------------------------------------------------------
// BackwardSlicer
// ---------------------------------------------------------------------------

BackwardSlicer::BackwardSlicer(int maxBlocks) : maxBlocks_(maxBlocks) {}

// Every predecessor of every valid block: from the IGNIL terminators (what
// hand-built IGNIL graphs have) and from the BasicBlock successors (which
// also carry resolved indirect-jump targets and fall-throughs).
BackwardSlicer::PredMap
BackwardSlicer::buildPredecessorMap(const Graph &graph) const {
  PredMap result;
  auto &incoming = result.preds;
  for (std::uint64_t addr: graph.validBlockAddresses())
    incoming.emplace(addr, std::vector<std::uint64_t>{});
  auto addEdge = [&](std::uint64_t from, std::uint64_t to) {
    auto it = incoming.find(to);
    if (it != incoming.end() &&
        std::find(it->second.begin(), it->second.end(), from) ==
                it->second.end())
      it->second.push_back(from);
  };
  for (std::uint64_t addr: graph.validBlockAddresses()) {
    if (const ignil::Block *blk = graph.getIGNILBlock(addr))
      for (std::uint64_t succ: successorsOf(*blk).known)
        addEdge(addr, succ);
    if (const BasicBlock bb{&graph, addr}; bb.isValid())
      for (std::uint64_t succ: bb.successors())
        addEdge(addr, succ);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

slice::Expr BackwardSlicer::slice(const ignil::Value &root,
                                  std::uint64_t blockAddr,
                                  const Graph &graph) const {
  PredMap preds = buildPredecessorMap(graph);
  const ignil::Block *blk = graph.getIGNILBlock(blockAddr);
  if (!blk)
    return slice::Expr{slice::Undef{ignil::widthOf(root)}};
  int startIdx = static_cast<int>(blk->ops().size()); // scan all ops
  return sliceValue(root, *blk, startIdx, graph, preds, maxBlocks_);
}

// ---------------------------------------------------------------------------
// sliceValue - resolve a Value backwards through ops
// ---------------------------------------------------------------------------

slice::Expr BackwardSlicer::sliceValue(const ignil::Value &v,
                                       const ignil::Block &block, int beforeIdx,
                                       const Graph &graph, PredMap &preds,
                                       int depth) const {
  return std::visit(
          utils::overloaded{
                  // Constants are trivially resolved.
                  [](const ignil::Const &c) -> slice::Expr {
                    return slice::Expr{slice::Const{c.value, c.width}};
                  },

                  // Temps are single-assignment within a block; scan backwards
                  // for
                  // the defining COPY/binary/unary/LOAD op.
                  [&](const ignil::Temp &t) -> slice::Expr {
                    const auto &ops = block.ops();
                    for (int i = beforeIdx - 1; i >= 0; --i) {
                      // retrieve operation
                      const ignil::Op &op = ops[i];
                      if (!op.dst)
                        continue;
                      // check if destination is a temporal, and it is the same
                      // temporal since we use SSA, there is where the temporal
                      // has been defined.
                      if (const auto *dt = std::get_if<ignil::Temp>(&*op.dst)) {
                        if (dt->id == t.id)
                          return buildFromOp(op, block, i, graph, preds, depth);
                      }
                    }
                    // Not defined in this block - temps never cross blocks,
                    // treat as undef.
                    return slice::Expr{slice::Undef{t.width}};
                  },

                  // Registers: see sliceReg - a write to an overlapping
                  // sub-register is a definition too.
                  [&](const ignil::Reg &r) -> slice::Expr {
                    return sliceReg(r, block, beforeIdx, graph, preds, depth);
                  },
          },
          v);
}

// ---------------------------------------------------------------------------
// sliceReg - the value of register `r` just before ops[beforeIdx]
//
// The register space is byte-addressed and sub-registers alias (RAX/EAX/AX/AL
// share offset 0, AH is byte 1), so the definition of `r` is the latest write
// touching *any* of its bytes, not only one with the same offset and width:
//
//   - the write covers all of `r` (exact match, or `r` lies inside the bytes
//     the write replaces, e.g. reading RAX after `mov eax, ..`, which zeroes
//     the upper half, or reading AH after `mov rax, ..`): `r` is that value,
//     shifted and resized;
//   - the write covers only part of `r` (`mov al, ..` then read RAX): the
//     written bytes are merged into the value `r` had before the write.
// ---------------------------------------------------------------------------

slice::Expr BackwardSlicer::sliceReg(const ignil::Reg &r,
                                     const ignil::Block &block, int beforeIdx,
                                     const Graph &graph, PredMap &preds,
                                     int depth) const {
  using Opc = ignil::Opcode;
  const auto regFile = registerFileFor(graph.architecture());
  const std::uint32_t rs = r.offset, re = r.offset + byteCount(r.width);

  const auto &ops = block.ops();
  for (int i = beforeIdx - 1; i >= 0; --i) {
    const ignil::Op &op = ops[i];
    if (!op.dst)
      continue;
    const auto *w = std::get_if<ignil::Reg>(&*op.dst);
    if (!w)
      continue;
    const std::uint32_t ws = w->offset;
    const std::uint32_t we = w->offset + clobberBytes(*w, regFile);
    if (we <= rs || re <= ws)
      continue; // no byte in common
    if (ws == rs && w->width == r.width)
      return buildFromOp(op, block, i, graph, preds, depth);

    // The written value, as a `wbits`-wide quantity covering [ws, we): bytes
    // past the write's own width are the zeroes of a 32-bit zero-extension.
    const ignil::Width wbits = (we - ws) * 8;
    slice::Expr value = resize(buildFromOp(op, block, i, graph, preds, depth),
                               w->width, wbits);

    if (ws <= rs && re <= we) {
      // Fully covered: extract r's bytes.
      if (rs > ws)
        value = mkBin(Opc::SHR, std::move(value), mkConst((rs - ws) * 8, wbits));
      return resize(std::move(value), wbits, r.width);
    }

    // Partially covered: merge into r's previous value. Needs masks of r's
    // width, which a 64-bit Const can express only up to 64 bits.
    if (r.width > 64 || wbits > 64)
      return slice::Expr{slice::Undef{r.width}};
    // r = (old & ~(mask << pos)) | (part << pos), where part is the written
    // bytes [lo, hi) that fall inside r and pos their bit offset within r.
    const std::uint32_t lo = std::max(ws, rs), hi = std::min(we, re);
    slice::Expr part = std::move(value);
    if (lo > ws)
      part = mkBin(Opc::SHR, std::move(part), mkConst((lo - ws) * 8, wbits));
    const unsigned partBits = (hi - lo) * 8;
    if (partBits < wbits)
      part = resize(std::move(part), wbits, partBits);
    part = resize(std::move(part), partBits, r.width);
    const unsigned pos = (lo - rs) * 8;
    if (pos)
      part = mkBin(Opc::SHL, std::move(part), mkConst(pos, r.width));
    const std::uint64_t keep = ~(lowMask(partBits) << pos) & lowMask(r.width);
    slice::Expr old = sliceReg(r, block, i, graph, preds, depth);
    return mkBin(Opc::OR, mkBin(Opc::AND, std::move(old), mkConst(keep, r.width)),
                 std::move(part));
  }

  // Not defined in this block - try the predecessors.
  if (depth <= 0)
    return slice::Expr{slice::SymReg{r}};
  auto predIt = preds.preds.find(block.startAddress());
  if (predIt == preds.preds.end() || predIt->second.empty())
    return slice::Expr{slice::SymReg{r}}; // entry
  const std::vector<std::uint64_t> &from = predIt->second;

  auto sliceIn = [&](std::uint64_t p) -> std::optional<slice::Expr> {
    const ignil::Block *predBlk = graph.getIGNILBlock(p);
    if (!predBlk || --preds.budget < 0)
      return std::nullopt;
    return sliceReg(r, *predBlk, static_cast<int>(predBlk->ops().size()),
                    graph, preds, depth - 1);
  };

  if (from.size() == 1) {
    auto e = sliceIn(from.front());
    return e ? std::move(*e) : slice::Expr{slice::SymReg{r}};
  }

  // A join. Back at a join already being resolved for this register: this
  // path came round a loop without redefining it, so it adds nothing - the
  // caller skips it.
  const auto key = std::make_tuple(block.startAddress(), r.offset,
                                   static_cast<std::uint32_t>(r.width));
  if (preds.joinsInProgress.contains(key)) {
    const int marker = -2 - static_cast<int>(preds.loopMarkers.size());
    preds.loopMarkers.emplace(marker, key);
    return slice::Expr{slice::Undef{r.width, block.startAddress(), marker}};
  }

  preds.joinsInProgress.insert(key);
  std::optional<slice::Expr> merged;
  std::optional<slice::Expr> outerLoop;
  std::string mergedText;
  bool agree = true;
  for (std::uint64_t p: from) {
    auto e = sliceIn(p);
    if (!e) {
      agree = false;
      break;
    }
    if (const auto *u = std::get_if<slice::Undef>(&e->data)) {
      const auto marker = preds.loopMarkers.find(u->opIndex);
      if (marker != preds.loopMarkers.end() &&
          std::get<1>(marker->second) == r.offset &&
          std::get<2>(marker->second) == r.width &&
          preds.joinsInProgress.contains(marker->second)) {
        // A transparent path can return to an enclosing join through several
        // intermediate joins. Defer that equality to its owner rather than
        // turning it into an unknown at the first intermediate join.
        if (marker->second != key)
          outerLoop = std::move(*e);
        continue;
      }
    }
    const std::string text = slice::toString(*e);
    if (!isConstantOnly(*e) || (merged && text != mergedText)) {
      agree = false;
      break;
    }
    if (!merged) {
      merged = std::move(*e);
      mergedText = text;
    }
  }
  preds.joinsInProgress.erase(key);
  if (agree && merged)
    return std::move(*merged);
  if (agree && outerLoop)
    return std::move(*outerLoop);
  return slice::Expr{slice::SymReg{r}};
}

// ---------------------------------------------------------------------------
// buildFromOp - turn the defining Op into a SliceExpr
// ---------------------------------------------------------------------------

slice::Expr BackwardSlicer::buildFromOp(const ignil::Op &op,
                                        const ignil::Block &block, int opIdx,
                                        const Graph &graph,
                                        PredMap &preds, int depth) const {
  using Opc = ignil::Opcode;

  auto src = [&](int i) -> slice::Expr {
    if (i >= static_cast<int>(op.srcs.size())) {
      ignil::Width w = op.dst ? ignil::widthOf(*op.dst) : ignil::Width{64};
      return slice::Expr{slice::Undef{w}};
    }
    return sliceValue(op.srcs[i], block, opIdx, graph, preds, depth);
  };

  switch (op.opcode) {
    // Data movement - just forward the single source.
    case Opc::COPY:
      return src(0);

    // Memory load - fold address expression.
    case Opc::LOAD: {
      auto addrExpr = src(0);
      ignil::Width w = op.dst ? ignil::widthOf(*op.dst) : 64;
      return slice::Expr{slice::Load{
              std::make_shared<slice::Expr>(std::move(addrExpr)), w}};
    }

    // Binary arithmetic / bitwise / comparison ops.
    case Opc::ADD:
    case Opc::SUB:
    case Opc::MUL:
    case Opc::AND:
    case Opc::OR:
    case Opc::XOR:
    case Opc::SHL:
    case Opc::SHR:
    case Opc::SAR:
    case Opc::SDIV:
    case Opc::UDIV:
    case Opc::SMOD:
    case Opc::UMOD:
    case Opc::EQ:
    case Opc::NEQ:
    case Opc::ULT:
    case Opc::ULE:
    case Opc::UGT:
    case Opc::UGE:
    case Opc::SLT:
    case Opc::SLE:
    case Opc::SGT:
    case Opc::SGE: {
      auto left = src(0);
      auto right = src(1);
      return slice::Expr{slice::BinOp{
              op.opcode, std::make_shared<slice::Expr>(std::move(left)),
              std::make_shared<slice::Expr>(std::move(right))}};
    }

    // Unary ops.
    case Opc::NEG:
    case Opc::NOT:
    case Opc::ZEXT:
    case Opc::SEXT:
    case Opc::TRUNC: {
      auto operand = src(0);
      ignil::Width dstWidth = op.dst ? ignil::widthOf(*op.dst) : 0;
      return slice::Expr{slice::UnaryOp{
              op.opcode, std::make_shared<slice::Expr>(std::move(operand)),
              dstWidth}};
    }

    // UNDEF and control-flow ops produce no useful expression.
    // The value is still one value: tag it with the op that produced it, so
    // two references to the same UNDEF (`sub rax, rax` after it) stay equal
    // while unrelated ones stay independent.
    default: {
      ignil::Width w = op.dst ? ignil::widthOf(*op.dst) : ignil::Width{64};
      return slice::Expr{slice::Undef{w, block.startAddress(), opIdx}};
    }
  }
}

} // namespace dragon
