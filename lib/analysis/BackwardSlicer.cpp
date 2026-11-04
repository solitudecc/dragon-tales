#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Graph.hpp>

#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include "utils/Utils.hpp"
#include "utils/FloatOps.hpp"
#include "analysis/ConstantValue.hpp"

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

std::optional<std::pair<int,int>> localRange(const ignil::Block &block) {
  std::optional<std::pair<int,int>> range;
  for (int i=0;i<static_cast<int>(block.ops().size());++i)
    if (ignil::isLocalControl(block.ops()[i].opcode)) {
      if (!range) range=std::pair{i,i};
      else range->second=i;
    }
  return range;
}

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
BackwardSlicer::Predecessors
BackwardSlicer::predecessors(const Graph &graph) {
  Predecessors result;
  auto &incoming = result;
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

BackwardSlicer::Predecessors BackwardSlicer::predecessors(
        const Graph &graph, const std::set<std::uint64_t> &entries) {
  auto result = predecessors(graph);
  for (auto entry : entries)
    if (auto it = result.find(entry); it != result.end())
      it->second.clear();
  return result;
}

namespace {

// An address as base + offset: the constant terms of its ADD tree summed, the
// one non-constant term (if any) as the base.
struct SplitAddress {
  const slice::Expr *base = nullptr;
  std::uint64_t offset = 0;
  unsigned width = 64;
};

void addTerms(const slice::Expr &e, std::vector<const slice::Expr *> &out) {
  if (const auto *b = std::get_if<slice::BinOp>(&e.data);
      b && b->op == ignil::Opcode::ADD && b->left && b->right) {
    addTerms(*b->left, out);
    addTerms(*b->right, out);
    return;
  }
  out.push_back(&e);
}

std::optional<SplitAddress> splitAddress(const slice::Expr &e) {
  if (const auto c = slice::detail::constantValue(e))
    return SplitAddress{nullptr, c->value, c->width};
  std::vector<const slice::Expr *> terms;
  addTerms(e, terms);
  SplitAddress a;
  for (const slice::Expr *t: terms) {
    if (const auto *c = std::get_if<slice::Const>(&t->data)) {
      a.offset += c->value;
      a.width = c->width;
    } else if (a.base) {
      return std::nullopt; // two variable terms
    } else {
      a.base = t;
    }
  }
  return a;
}

// True when a store of `storeBytes` at `store` cannot touch any byte of a
// load of `loadBytes` at `load`: both are the same base value plus constant
// offsets (or both absolute) and the byte ranges do not intersect.
bool provablyDisjoint(const slice::Expr &load, unsigned loadBytes,
                      const slice::Expr &store, unsigned storeBytes) {
  auto l = splitAddress(load), s = splitAddress(store);
  if (!l || !s || (l->base == nullptr) != (s->base == nullptr))
    return false;
  if (l->base && !slice::sameValue(*l->base, *s->base))
    return false;
  // Offsets wrap at the address width; compare their signed difference.
  const unsigned aw = std::max(l->width, s->width);
  const std::uint64_t m = aw >= 64 ? ~0ull : (1ull << aw) - 1;
  std::uint64_t d = (s->offset - l->offset) & m; // store - load
  std::int64_t sd = aw >= 64 ? static_cast<std::int64_t>(d)
                             : static_cast<std::int64_t>(
                                       (d ^ (1ull << (aw - 1))) -
                                       (1ull << (aw - 1)));
  constexpr std::int64_t kNear = std::int64_t{1} << 30;
  if (sd > kNear || sd < -kNear)
    return false; // too far apart to reason about wrap-around safely
  return sd >= static_cast<std::int64_t>(loadBytes) ||
         sd <= -static_cast<std::int64_t>(storeBytes);
}

bool sameAddress(const slice::Expr &a, const slice::Expr &b) {
  if (slice::sameValue(a, b)) return true;
  auto left = splitAddress(a), right = splitAddress(b);
  if (!left || !right || left->width != right->width ||
      (left->base == nullptr) != (right->base == nullptr))
    return false;
  const auto mask = lowMask(left->width);
  return (left->offset & mask) == (right->offset & mask) &&
         (!left->base || slice::sameValue(*left->base, *right->base));
}

} // namespace

slice::Expr BackwardSlicer::addressOnEdge(
        const slice::Expr &address, const ignil::Block &block,
        const ignil::Block &predecessor, const Graph &graph,
        PredMap &preds, int depth) const {
  if (--preds.memoryBudget < 0)
    return slice::Expr{slice::Undef{64}};
  return address.visit([&](const auto &node) -> slice::Expr {
    using T = std::decay_t<decltype(node)>;
    if constexpr (std::is_same_v<T, slice::SymReg>) {
      if (node.origin == slice::ValueOrigin{block.startAddress(), -1, depth})
        return sliceReg(node.reg, predecessor, predecessor.ops().size(),
                        graph, preds, depth - 1);
      return slice::Expr{node};
    } else if constexpr (std::is_same_v<T, slice::BinOp>) {
      if (!node.left || !node.right) return slice::Expr{slice::Undef{64}};
      return mkBin(node.op, addressOnEdge(*node.left, block, predecessor, graph, preds, depth),
                   addressOnEdge(*node.right, block, predecessor, graph, preds, depth));
    } else if constexpr (std::is_same_v<T, slice::UnaryOp>) {
      if (!node.operand) return slice::Expr{slice::Undef{64}};
      return slice::Expr{slice::UnaryOp{node.op,
          std::make_shared<slice::Expr>(addressOnEdge(*node.operand, block, predecessor, graph, preds, depth)),
          node.resultWidth}};
    } else {
      // A loaded pointer is the value of its original read, not a fresh
      // read on this edge. Keep its producing event/snapshot intact.
      return slice::Expr{node};
    }
  });
}

std::optional<slice::Expr> BackwardSlicer::storedValue(
        const ignil::Block &block, int beforeIdx, const Graph &graph,
        PredMap &preds, int depth, const slice::Expr &address,
        ignil::Width width, std::set<std::uint64_t> &active) const {
  if (preds.memoryBudget <= 0 || !block.unlifted().empty() ||
      !active.insert(block.startAddress()).second)
    return std::nullopt;
  // Active is per traversal path: a shared predecessor on two arms is not
  // itself a cycle. Nested load queries use their own active set and share
  // the total memory budget.
  struct Pop {
    std::set<std::uint64_t> &set;
    std::uint64_t address;
    ~Pop() { set.erase(address); }
  } pop{active, block.startAddress()};
  for (int i = beforeIdx - 1; i >= 0; --i) {
    if (--preds.memoryBudget < 0) return std::nullopt;
    const auto &op = block.ops()[i];
    if (ignil::isLocalControl(op.opcode)) return std::nullopt;
    if (op.opcode == ignil::Opcode::CALL) return std::nullopt;
    if (op.opcode != ignil::Opcode::STORE) continue;
    if (op.srcs.size() != 2) return std::nullopt;
    auto storeAddress = sliceValue(op.srcs[0], block, i, graph, preds, depth);
    const auto storeWidth = ignil::widthOf(op.srcs[1]);
    if (storeWidth == width && sameAddress(address, storeAddress))
      return sliceValue(op.srcs[1], block, i, graph, preds, depth);
    if (!provablyDisjoint(address, byteCount(width), storeAddress, byteCount(storeWidth)))
      return std::nullopt;
  }
  if (depth <= 0) return std::nullopt;
  const auto incoming = preds.preds->find(block.startAddress());
  if (incoming == preds.preds->end() || incoming->second.empty())
    return std::nullopt;
  std::optional<slice::Expr> merged;
  for (auto from : incoming->second) {
    const auto *predecessor = graph.getIGNILBlock(from);
    if (!predecessor || --preds.memoryBudget < 0) return std::nullopt;
    auto edgeAddress = addressOnEdge(address, block, *predecessor, graph, preds, depth);
    auto value = storedValue(*predecessor, predecessor->ops().size(), graph,
                             preds, depth - 1, edgeAddress, width, active);
    if (!value) return std::nullopt;
    if (incoming->second.size() > 1 && !isConstantOnly(*value))
      return std::nullopt;
    if (merged && !slice::sameValue(*merged, *value)) return std::nullopt;
    merged = std::move(value);
  }
  return merged;
}

std::optional<slice::ValueOrigin> BackwardSlicer::memoryState(
        const ignil::Block &start, int beforeIdx, const Graph &graph,
        PredMap &preds, int context, const slice::Expr &loadAddr,
        ignil::Width width) const {
  const unsigned loadBytes = (width + 7) / 8;
  const ignil::Block *block = &start;
  std::set<std::uint64_t> seen;
  int budget = 4096;
  for (int step = 0; step < 64; ++step, --context) {
    if (!seen.insert(block->startAddress()).second)
      return std::nullopt;
    if (!block->unlifted().empty())
      return std::nullopt; // an opaque instruction may write memory
    for (int i = beforeIdx - 1; i >= 0; --i) {
      if (--budget < 0)
        return std::nullopt;
      const ignil::Op &op = block->ops()[i];
      const auto opcode = op.opcode;
      if (opcode == ignil::Opcode::STORE && op.srcs.size() == 2) {
        // A store to other bytes of the same object does not change what
        // this load reads (`mov byte [ebx+0x4f], 1` between a guard and the
        // re-load of `[ebx+0x4d]`). Its address is sliced from its own
        // position; sameValue's origins make the bases equal only when they
        // are the same value there and at the load.
        const slice::Expr storeAddr = sliceValue(
                op.srcs[0], *block, i, graph, preds, std::max(context, 0));
        const unsigned storeBytes = (ignil::widthOf(op.srcs[1]) + 7) / 8;
        if (provablyDisjoint(loadAddr, loadBytes, storeAddr, storeBytes))
          continue;
        return slice::ValueOrigin{block->startAddress(), i, context};
      }
      if (opcode == ignil::Opcode::STORE || opcode == ignil::Opcode::CALL)
        return slice::ValueOrigin{block->startAddress(), i, context};
    }
    auto it = preds.preds->find(block->startAddress());
    if (it == preds.preds->end() || it->second.size() != 1)
      return slice::ValueOrigin{block->startAddress(), -1, context};
    block = graph.getIGNILBlock(it->second.front());
    if (!block)
      return std::nullopt;
    beforeIdx = static_cast<int>(block->ops().size());
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

slice::Expr BackwardSlicer::slice(const ignil::Value &root,
                                  std::uint64_t blockAddr, const Graph &graph,
                                  const Predecessors *precomputed) const {
  PredMap preds;
  if (precomputed)
    preds.preds = precomputed;
  else
    preds.owned = predecessors(graph);
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
  const auto local=localRange(block);
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
                        if (dt->id == t.id) {
                          if (local && i>=local->first && i<=local->second)
                            return slice::Expr{slice::Undef{t.width}};
                          return buildFromOp(op, block, i, graph, preds, depth);
                        }
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
  auto unknown = [&] {
    const auto it = preds.preds->find(block.startAddress());
    if (it == preds.preds->end() || it->second.empty())
      return slice::Expr{slice::SymReg{r}}; // graph inputs keep their public names
    return slice::Expr{slice::SymReg{
            r, slice::ValueOrigin{block.startAddress(), -1, depth}}};
  };
  const auto regFile = registerFileFor(graph.architecture());
  const std::uint32_t rs = r.offset, re = r.offset + byteCount(r.width);

  const auto &ops = block.ops();
  const auto local=localRange(block);
  for (int i = beforeIdx - 1; i >= 0; --i) {
    if (local && beforeIdx>local->first && i>=local->first && i<=local->second) {
      for (int j=local->first;j<=local->second;++j)
        if (ops[j].dst)
          if (const auto *written=std::get_if<ignil::Reg>(&*ops[j].dst)) {
            const auto start=written->offset,end=start+clobberBytes(*written,regFile);
            if (start<re && rs<end) return slice::Expr{slice::Undef{r.width}};
          }
      i=local->first; // unchanged register: resume in the linear prefix
      continue;
    }
    const ignil::Op &op = ops[i];
    // An unknown callee may write any register: the return value, a stdcall
    // callee's argument pop, or non-ABI code. The register after the call is
    // an unknown of its own, identified by the call (the same register read
    // twice after one call is one value); nothing before it is traced.
    if (op.opcode == Opc::CALL)
      return slice::Expr{slice::SymReg{
              r, slice::ValueOrigin{block.startAddress(), i, depth}}};
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

    // An implicit zero-extension defines these bytes independently of the
    // written value, even for UNDEF or a different unknown on each path.
    // Keep the zero explicit so a join can merge it without merging sources.
    if (ws + byteCount(w->width) <= rs && re <= we)
      return mkConst(0, r.width);

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
    return unknown();
  auto predIt = preds.preds->find(block.startAddress());
  if (predIt == preds.preds->end() || predIt->second.empty())
    return unknown(); // entry
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
    return e ? std::move(*e) : unknown();
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
    if (!isConstantOnly(*e) || (merged && !slice::sameValue(*e, *merged))) {
      agree = false;
      break;
    }
    if (!merged) {
      merged = std::move(*e);
    }
  }
  preds.joinsInProgress.erase(key);
  if (agree && merged)
    return std::move(*merged);
  if (agree && outerLoop)
    return std::move(*outerLoop);

  // Full values can disagree while their upper halves are all zero. Query
  // those bytes independently using the same alias, loop and budget rules;
  // never infer zeroes merely from the width of a downstream comparison.
  if (r.width == 64 && !preds.probingUpper && preds.budget > 0) {
    const ignil::Reg upper{r.offset + 4, 32, r.name + "[63:32]"};
    preds.probingUpper = true;
    const auto high = sliceReg(upper, block, beforeIdx, graph, preds, depth);
    preds.probingUpper = false;
    if (const auto *c = std::get_if<slice::Const>(&high.data);
        c && c->value == 0)
      return resize(resize(unknown(), 64, 32), 32, 64);
  }
  return unknown();
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

  if (ignil::detail::floatBinaryOp(op.opcode)) {
    auto left = src(0), right = src(1);
    return slice::Expr{slice::BinOp{op.opcode,
        std::make_shared<slice::Expr>(std::move(left)),
        std::make_shared<slice::Expr>(std::move(right))}};
  }
  if (ignil::detail::floatUnaryOp(op.opcode)) {
    auto operand = src(0);
    return slice::Expr{slice::UnaryOp{op.opcode,
        std::make_shared<slice::Expr>(std::move(operand)),
        op.dst ? ignil::widthOf(*op.dst) : ignil::Width{64}}};
  }
  switch (op.opcode) {
    // Data movement - just forward the single source.
    case Opc::COPY:
      return src(0);

    // Memory load - fold address expression.
    case Opc::LOAD: {
      auto addrExpr = src(0);
      ignil::Width w = op.dst ? ignil::widthOf(*op.dst) : 64;
      std::set<std::uint64_t> active;
      if (auto value = storedValue(block, opIdx, graph, preds, depth, addrExpr, w, active))
        return std::move(*value);
      auto state = memoryState(block, opIdx, graph, preds, depth, addrExpr, w);
      return slice::Expr{slice::Load{
              std::make_shared<slice::Expr>(std::move(addrExpr)), w,
              block.startAddress(), opIdx, depth, std::move(state)}};
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
