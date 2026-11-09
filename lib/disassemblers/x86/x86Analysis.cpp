#include "disassemblers/x86/x86Analysis.hpp"
#include "disassemblers/x86/X86IGNILLifter.hpp"
#include "analysis/ConstantValue.hpp"
#include "analysis/TableContents.hpp"

// Public headers
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/IndirectTargets.hpp>
#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/disassembler/Instruction.hpp>
#include <dragon/disassembler/x86/x86Info.hpp>

#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>

#ifdef DRAGON_WITH_Z3
#include <dragon/analysis/ValueEnumerator.hpp>
#endif

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

dragon::x86::X86Analysis::X86Analysis(Architecture architecture,
                                      const Disassembler &disassembler,
                                      const Configuration &configuration,
                                      const llvm::MCInstrInfo &instrInfo,
                                      const llvm::MCRegisterInfo &regInfo) :
    architecture_(architecture), disassembler_(disassembler),
    configuration_(configuration), instrInfo_(instrInfo), regInfo_(regInfo) {
  static const X86Info x86Info32{Architecture::X86_32, instrInfo, regInfo};
  static const X86Info x86Info64{Architecture::X86_64, instrInfo, regInfo};
  logger_ = internal::Logger{configuration_.logging};
  x86IgnilLifter_ = std::make_unique<X86IGNILLifter>(logger_, architecture,
                                                     instrInfo, regInfo);
  x86Info_ = (architecture == Architecture::X86_32) ? &x86Info32 : &x86Info64;
}

dragon::x86::X86Analysis::~X86Analysis() = default;

// =============================================================================
// Public entry point
// =============================================================================

bool dragon::x86::X86Analysis::analyze(std::span<const std::uint8_t> bytes,
                                       std::uint64_t baseAddr, Graph &graph,
                                       const Disassembler::AnalysisHints &hints)
        const {
  std::unordered_set<std::uint64_t> blockStarts;
  std::unordered_set<std::uint64_t> fnStarts;

  // Entry points inside these bytes: the traversal roots, and function
  // starts on their own. Without any, the start of `bytes` is the entry.
  std::vector<std::uint64_t> roots;
  for (std::uint64_t a: hints.entryPoints)
    if (a >= baseAddr && a - baseAddr < bytes.size())
      roots.push_back(a);
  const bool explicitEntries = !hints.entryPoints.empty();
  if (!explicitEntries)
    roots.push_back(baseAddr);

  std::vector<Instruction> instructions =
          disassembler_.disassemble(bytes, baseAddr, roots);

  // The caller explicitly asked us to start analysis here, which is itself
  // a strong, deliberate signal that this address is a function entry -
  // independent of whether its bytes happen to match a recognized prologue
  // pattern. Without this, a leaf/frame-pointer-omitted function passed
  // directly to disassemble() (rather than reached via a call site, which
  // *is* already trusted unconditionally below) was never recognized as a
  // function at all, and Graph::functions() came back empty for it.
  //
  // Entry points supplied by the caller (an image's entry point and function
  // symbols) are trusted the same way. They used to be registered with
  // Graph::addFunction() only after analysis, never as block starts, so every
  // function reached only through a pointer - most of a library's API - came
  // out as a function with no blocks.
  if (!instructions.empty()) {
    if (explicitEntries) {
      for (std::uint64_t a: roots) {
        fnStarts.insert(a);
        blockStarts.insert(a);
      }
    } else {
      fnStarts.insert(baseAddr);
    }
  }

  discoverBlockStarts(instructions, blockStarts);
  discoverFunctionStarts(instructions, blockStarts, fnStarts);
  discoverEpilogues(instructions, blockStarts, fnStarts);

  for (auto &insn: instructions)
    graph.addInstruction(std::move(insn));


  // A linear sweep can consume a real entry inside another instruction's
  // immediate (junk-byte anti-disassembly). Follow only explicit control-flow
  // evidence, retaining both decodes. integrateNewBlock follows any further
  // direct targets revealed by the new path and stops at known instructions.
  // Copy the seeds: integration can add more starts to the sets.
  const std::vector<std::uint64_t> directStarts(blockStarts.begin(),
                                                blockStarts.end());
  bool directDecodeComplete = true;
  for (auto target : directStarts) {
    integrateNewBlock(target, bytes, baseAddr, graph, blockStarts, fnStarts,
                      directDecodeComplete);
    if (!directDecodeComplete) break;
  }
  discoverFallthroughJoins(graph, blockStarts);

  // Indirect-jump resolution. Under linear sweep only recognised jump tables
  // whose targets are instructions the sweep already decoded are used (see
  // resolveIndirectJumps): a free-form slice or Z3 guess can land
  // mid-instruction or inside data the sweep decoded as code.

  {
    LiftState liftState;
    liftPendingBlocks(graph, liftState,
                      commitPending(graph, blockStarts, fnStarts));
  }
  if (directDecodeComplete)
    resolveIndirectJumps(graph, bytes, baseAddr, hints, blockStarts, fnStarts);
  else {
    logger_.log("Direct recovery decoding budget exhausted");
    if (hints.indirectTargets)
      hints.indirectTargets->stopped("direct recovery decoding budget exhausted");
  }

  return true;
}

// =============================================================================
// CFG seed discovery
// =============================================================================

void dragon::x86::X86Analysis::discoverBlockStarts(
        std::vector<Instruction> &instructions,
        std::unordered_set<std::uint64_t> &basicBlockStarts) const {
  if (instructions.empty())
    return;

  basicBlockStarts.insert(instructions[0].address());

  for (const auto &I: instructions) {
    if (I.hasIndirectTarget()) {
      logger_.log("Instruction {} at address 0x{:x} has an indirect target.",
                  I.disassembly(), I.address());
    }

    if (I.isJump()) {
      for (auto addr: I.branches()) {
        basicBlockStarts.insert(addr);
        logger_.log("Discovered 0x{:x} as beginning of block", addr);
      }
      if (I.isConditional()) {
        std::uint64_t addr = I.address() + I.size();
        basicBlockStarts.insert(addr);
        logger_.log("Discovered 0x{:x} as beginning of block", addr);
      }
    }
  }

  for (auto &I: instructions) {
    if (basicBlockStarts.contains(I.address()))
      I.setBlockStart(true);
  }
}

void dragon::x86::X86Analysis::discoverEpilogues(
        std::vector<Instruction> &instructions,
        std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &functionStarts) const {
  // lld uses 0xcc alignment after a function, but LLVM's INT3 descriptor
  // lacks isTrap. Recognize it only while skipping post-epilogue padding:
  // classifying every INT3 as an epilogue also promotes unrelated following
  // instructions inside jump-table regions to new functions.
  const auto isInt3Padding = [](const Instruction &insn) {
    const auto &bytes = insn.bytes();
    return bytes.size() == 1 && bytes[0] == 0xcc;
  };
  const auto isPadding = [this, &isInt3Padding](const Instruction &insn) {
    return x86Info_->isNop(insn) || x86Info_->isTrap(insn) || isInt3Padding(insn);
  };
  // A literal pool can follow RET just like another function. A concrete
  // memory reference to that address makes a layout-only guess ambiguous;
  // direct calls, explicit entries and prologue candidates still take priority.
  std::unordered_set<std::uint64_t> dataReferences;
  for (const auto &insn : instructions) {
    const auto *mc = insn.mcInst();
    if (!mc) continue;
    const auto &desc = instrInfo_.get(mc->getOpcode());
    if (!desc.mayLoad() && !desc.mayStore()) continue;
    bool addressOverride = false;
    for (auto byte : insn.bytes()) {
      if (byte == 0x67) { addressOverride = true; break; }
      if (byte == 0x66 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
          byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e ||
          byte == 0x64 || byte == 0x65 ||
          (architecture_ == Architecture::X86_64 && byte >= 0x40 && byte <= 0x4f))
        continue;
      break;
    }
    if (addressOverride) continue; // do not guess a different address width
    // LLVM's x86 memory tuple is base/scale/index/displacement/segment.
    for (unsigned k=0; k+4<mc->getNumOperands(); ++k) {
      const auto &base=mc->getOperand(k), &scale=mc->getOperand(k+1);
      const auto &index=mc->getOperand(k+2), &disp=mc->getOperand(k+3);
      const auto &segment=mc->getOperand(k+4);
      if (!base.isReg() || !scale.isImm() || scale.getImm()!=1 ||
          !index.isReg() || index.getReg()!=0 || !disp.isImm() ||
          !segment.isReg() || segment.getReg()!=0)
        continue;
      std::uint64_t address=static_cast<std::uint64_t>(disp.getImm());
      if (base.getReg()!=0) {
        const std::string_view name=regInfo_.getName(base.getReg());
        if (name!="RIP" && name!="EIP") continue;
        address += insn.address()+insn.size();
      }
      if (architecture_ == Architecture::X86_32) address &= 0xffffffffULL;
      dataReferences.insert(address);
      break;
    }
  }
  // Use the entries known before epilogue guessing to delimit regions. A
  // dispatch in one function must not disable discovery in a separate known
  // function, and a RET inside an unresolved dispatch must not split its cases.
  std::vector<std::uint64_t> knownEntries(functionStarts.begin(), functionStarts.end());
  std::sort(knownEntries.begin(), knownEntries.end());
  const auto regionFor = [&](std::uint64_t address) {
    auto next = std::upper_bound(knownEntries.begin(), knownEntries.end(), address);
    return next == knownEntries.begin() ? instructions.front().address() : *std::prev(next);
  };
  std::unordered_set<std::uint64_t> indirectRegions;
  for (const auto &insn : instructions)
    if (insn.isJump() && insn.hasIndirectTarget() && insn.branches().empty())
      indirectRegions.insert(regionFor(insn.address()));
  const size_t n = instructions.size();
  size_t i = 0;
  while (i < n) {
    const size_t windowSize = std::min(n - i, size_t{2});
    const Instruction *window[2] = {};
    for (size_t j = 0; j < windowSize; ++j)
      window[j] = &instructions[i + j];

    const size_t matched =
            x86Info_->isCommonFunctionEpilogue({window, windowSize});
    if (!matched) {
      ++i;
      continue;
    }

    // hlt followed by an unconditional jmp is a kernel idle loop - the jmp
    // stays inside the same function, so don't promote the target to a
    // function start (block boundary only).
    const bool isHltEpilogue =
            matched == 1 && x86Info_->isHalt(instructions[i]);

    // Skip nop/trap padding after the epilogue. Alignment padding runs past
    // 16 bytes (gcc pads aarch64 functions with up to 7 nops); with a
    // 16-byte cap the rest of the run became a "function" made of nops.
    constexpr std::uint64_t kMaxPadding = 64;
    size_t j = i + matched;
    std::uint64_t paddingBytes = 0;
    while (j < n && paddingBytes < kMaxPadding) {
      const auto &pad = instructions[j];
      if (!isPadding(pad))
        break;
      paddingBytes += pad.size();
      ++j;
    }

    // Still padding after the cap - a run of zero words (`udf #0`) or nops
    // longer than any alignment, i.e. data. Not a function; and resume past
    // the whole run so each pad is not taken for another epilogue.
    const bool stillPadding = j < n && isPadding(instructions[j]);
    if (stillPadding) {
      while (j < n && isPadding(instructions[j]))
        ++j;
      i = j;
      continue;
    }

    if (j < n) {
      auto &next = instructions[j];
      if (isHltEpilogue && next.isJump() && !next.isConditional()) {
        blockStarts.insert(next.address());
        logger_.log("hlt+jmp idle loop at 0x{:x}, 0x{:x} is block boundary only",
                    instructions[i].address(), next.address());
      } else if (blockStarts.contains(next.address())) {
        // Already a known branch target - it belongs to the current function's
        // CFG, not a new function entry (e.g. the else-block after a then-ret).
        logger_.log("Epilogue at 0x{:x}: 0x{:x} is a branch target, skipping "
                    "function-start promotion",
                    instructions[i].address(), next.address());
      } else if (dataReferences.contains(next.address()) &&
                 !functionStarts.contains(next.address())) {
        logger_.log("Post-RET candidate at 0x{:x} is also referenced as data; "
                    "not promoting it to a function", next.address());
      } else if (indirectRegions.contains(regionFor(next.address())) &&
                 !functionStarts.contains(next.address())) {
        // RET (with or without alignment) also separates jump-table cases.
        // Until the indirect targets are proved, layout cannot distinguish a
        // case from an uncalled leaf. Retain explicit/call/prologue entries;
        // let resolution establish other case block starts conservatively.
        logger_.log("Post-RET candidate at 0x{:x} may be an indirect case; "
                    "not promoting it to a function", next.address());
      } else {
        functionStarts.insert(next.address());
        blockStarts.insert(next.address());
        next.setFunctionStart(true);
        next.setBlockStart(true);
        logger_.log(
                "Discovered function start via epilogue at address 0x{:x}",
                next.address());
      }
    }

    i += matched;
  }
}

void dragon::x86::X86Analysis::discoverFunctionStarts(
        std::vector<Instruction> &instructions,
        std::unordered_set<std::uint64_t> &basicBlockStarts,
        std::unordered_set<std::uint64_t> &functionStarts) const {
  for (auto &I: instructions) {
    if (I.isCall()) {
      for (auto callee: I.callees()) {
        functionStarts.insert(callee);
        basicBlockStarts.insert(callee);
        logger_.log("Discovered callee at address 0x{:x}", callee);
      }
    }
  }

  // Targets of conditional branches inside the code. A prologue there is
  // usually a shrink-wrapped one - gcc moves `stp x29, x30, [sp, #-N]!` /
  // `push rbp` past an early-exit test, onto the slow path of the same
  // function (`b.ge 1f; ...; ret; 1: stp x29, x30, ...`) - so it is a block
  // start, not a new function, unless something calls it or the caller
  // named it as an entry point (both already in functionStarts).
  std::unordered_set<std::uint64_t> conditionalTargets;
  for (const auto &I: instructions)
    if (I.isJump() && I.isConditional())
      conditionalTargets.insert(I.branches().begin(), I.branches().end());

  for (size_t i = 0, e = instructions.size(); i < e; ++i) {
    const size_t windowSize = std::min(e - i, size_t{3});
    const Instruction *window[3] = {};
    for (size_t j = 0; j < windowSize; ++j)
      window[j] = &instructions[i + j];
    if (size_t matched =
                x86Info_->isCommonFunctionPrologue({window, windowSize})) {
      std::uint64_t addr = instructions[i].address();
      // CET landing pads also mark computed-goto labels and return
      // continuations. A bare ENDBR is not independent function evidence.
      // Keep explicit roots/callees and full frame prologues. Leave block
      // discovery to actual transfers, so post-RET evidence can still find
      // separate leaves without mistaking every landing pad for an entry.
      if (architecture_ == Architecture::X86_64 && matched == 1 &&
          !functionStarts.contains(addr)) {
        continue;
      }
      // Likewise right after a conditional branch, whose fall-through it is:
      // `b.ne 1f; stp x29, x30, [sp, #-48]!` is the same function. (A
      // function proper follows a return, a jump, a trap, padding or a call.)
      bool fallsInto = false;
      if (i > 0) {
        const Instruction &prev = instructions[i - 1];
        fallsInto = prev.address() + prev.size() == addr && prev.isJump() &&
                    prev.isConditional();
      }
      if ((conditionalTargets.contains(addr) || fallsInto) &&
          !functionStarts.contains(addr)) {
        logger_.log("Prologue at 0x{:x} is inside a function (shrink-wrapped), "
                    "not a function start",
                    addr);
        i += matched - 1;
        continue;
      }
      functionStarts.insert(addr);
      basicBlockStarts.insert(addr);
      instructions[i].setFunctionStart(true);
      instructions[i].setBlockStart(true);
      instructions[i].setPrologue(true);
      logger_.log("Discovered function start via prologue at address 0x{:x}",
                  addr);
      i += matched - 1;
    }
  }
}

// =============================================================================
// Fixpoint helpers
// =============================================================================

std::vector<std::uint64_t> dragon::x86::X86Analysis::commitPending(
        Graph &graph, std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &fnStarts) const {
  // A target can be decoded by direct recovery or a tentative indirect
  // expansion after the initial sweep. Reapply historical evidence at every
  // commit, before lifting/revalidation, using only this graph's decoded
  // targets. Missing targets stay with loader recovery and its root policy.
  for (const auto &[address, insn] : graph.listing()) {
    if (insn.isJump()) {
      for (auto target : insn.branches())
        if (graph.get(target)) blockStarts.insert(target);
      if (insn.isConditional() && graph.get(address + insn.size()))
        blockStarts.insert(address + insn.size());
    }
    if (insn.isCall())
      for (auto callee : insn.callees())
        if (graph.get(callee)) {
          blockStarts.insert(callee);
          fnStarts.insert(callee);
        }
  }

  // Keep the per-instruction flags in step with the Graph's sets: call
  // targets and resolved indirect-jump targets reach the sets without the
  // instruction already at that address being flagged, and the flags are
  // what Instruction::isBlockStart()/isFunctionStart() report through the
  // C and Python APIs.
  // A discovered address is only a candidate until its first instruction
  // exists. Keep real external targets on instructions, without advertising
  // undecoded addresses as valid block/function views.
  std::vector<std::uint64_t> added;
  for (std::uint64_t addr: blockStarts) {
    Instruction *insn = graph.get(addr);
    if (!insn) continue;
    if (!graph.isValidBlock(addr))
      added.push_back(addr);
    graph.addBlock(addr);
    insn->setBlockStart(true);
  }
  for (std::uint64_t addr: fnStarts) {
    Instruction *insn = graph.get(addr);
    if (!insn) continue;
    graph.addFunction(addr);
    insn->setFunctionStart(true);
  }
  return added;
}

namespace {
// True when an IGNIL block lifted earlier holds ops past the block's current
// terminator: the block was split since (a later fixpoint round found a new
// block start inside it - typically a resolved indirect-jump target), so the
// lift is stale and describes code that now belongs to the next block.
bool coversMoreThan(const dragon::ignil::Block &ib,
                    const dragon::BasicBlock &block) {
  for (const auto &op: ib)
    if (op.address > block.terminatorAddress())
      return true;
  return false;
}
} // namespace

void dragon::x86::X86Analysis::liftPendingBlocks(
        Graph &graph, LiftState &state,
        const std::vector<std::uint64_t> &newStarts) const {
  auto lift = [&](std::uint64_t addr, bool force) {
    const BasicBlock block{&graph, addr};
    ignil::Block *existing = graph.getIGNILBlock(addr);
    if (!block.isValid()) {
      if (!existing)
        state.lacking.insert(addr);
      return;
    }
    state.lacking.erase(addr);
    if (existing && !force && !coversMoreThan(*existing, block))
      return;
    auto ib = x86IgnilLifter_->liftBlock(block, graph);
    logger_.log("{} block @ 0x{:x} -> {} IGNIL ops",
                existing ? "Re-lifted (split)" : "Lifted", addr, ib.size());
    if (existing)
      *existing = std::move(ib); // in place: borrowed pointers stay valid
    else
      graph.addIGNILBlock(std::move(ib));
  };

  if (!state.initialized) {
    state.initialized = true;
    for (std::uint64_t addr: graph.validBlockAddresses())
      lift(addr, false);
    return;
  }
  std::set<std::uint64_t> todo(state.lacking.begin(), state.lacking.end());
  const auto &valid = graph.validBlockAddresses();
  todo.insert(newStarts.begin(), newStarts.end());
  // A new boundary can split several overlapping decode paths. Walk their
  // exact sequential predecessors back to their block starts; the nearest
  // start in numeric order need not own this suffix. Each address is visited
  // once across all new starts, without rescanning every block in the image.
  std::vector<std::uint64_t> work(newStarts.begin(), newStarts.end());
  std::unordered_set<std::uint64_t> seen;
  const auto &listing = graph.listing();
  while (!work.empty()) {
    const auto address = work.back();
    work.pop_back();
    if (!seen.insert(address).second)
      continue;
    auto it = listing.lower_bound(address);
    while (it != listing.begin()) {
      --it;
      if (address - it->first > 15)
        break; // x86 instructions cannot span more than 15 bytes
      const auto &insn = it->second;
      if (insn.address() + insn.size() != address || insn.isJump() ||
          insn.isReturn() || insn.isTrap())
        continue;
      if (valid.contains(it->first))
        todo.insert(it->first);
      else
        work.push_back(it->first);
    }
  }
  const std::set<std::uint64_t> fresh(newStarts.begin(), newStarts.end());
  for (std::uint64_t addr: todo)
    lift(addr, !fresh.contains(addr) && !state.lacking.contains(addr));
}

std::vector<std::uint64_t>
dragon::x86::X86Analysis::collectTargets(const slice::Expr &result,
                                         std::uint64_t blockAddr) const {
  if (const auto c = slice::detail::constantValue(result))
    return {c->value};

#ifdef DRAGON_WITH_Z3
  if (configuration_.useZ3ForPathDiscovery) {
    z3::context ctx;
    slice::EnumeratorConfig limits;
    limits.requireComplete = true;
    auto targets = slice::enumeratedValues(result, ctx, {}, limits);
    logger_.log(
            "Z3 enumerated {} candidate target(s) for indirect jump @ 0x{:x}",
            targets.size(), blockAddr);
    return targets;
  }
#else
  // just to avoid warnings
  (void) blockAddr;
#endif

  return {};
}

bool dragon::x86::X86Analysis::integrateNewBlock(
        std::uint64_t target, std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr, Graph &graph,
        std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &fnStarts,
        bool &complete, std::size_t maxNewInstructions) const {
  if (graph.get(target))
    return false;

  // Decode the code reachable from `target` that the graph does not hold
  // yet: not just the first block - its branch targets, the fall-through of
  // a conditional branch and its callees are code too. Decoding only one
  // block registered those as block starts with no instruction behind them
  // (250 empty blocks in a recursive i686 Lua analysis once its jump tables
  // resolved). Known code stops the walk.
  // Providers can reduce, but never raise, the historical expansion ceiling.
  const auto limit = std::min<std::size_t>(maxNewInstructions, 1u << 20);
  std::vector<Instruction> newInstrs;
  std::unordered_set<std::uint64_t> seen;
  std::vector<std::uint64_t> work{target};
  while (!work.empty() && newInstrs.size() < limit) {
    const std::uint64_t at = work.back();
    work.pop_back();
    if (graph.get(at) || seen.contains(at))
      continue;
    for (auto &insn: disassembler_.disassembleBlock(bytes, baseAddr, at)) {
      if (graph.get(insn.address()) || seen.contains(insn.address()))
        break; // ran into code already decoded
      if (newInstrs.size() >= limit) {
        complete = false;
        return false;
      }
      seen.insert(insn.address());
      for (std::uint64_t t: insn.branches())
        work.push_back(t);
      for (std::uint64_t c: insn.callees())
        work.push_back(c);
      if (insn.isJump() && insn.isConditional())
        work.push_back(insn.address() + insn.size());
      newInstrs.push_back(std::move(insn));
    }
  }
  if (!work.empty()) {
    complete = false;
    return false;
  }
  if (newInstrs.empty())
    return false;
  std::ranges::sort(newInstrs, [](const Instruction &a, const Instruction &b) {
    return a.address() < b.address();
  });

  // This will lead to new blocks, but could be that it takes to new
  // functions start too.
  discoverBlockStarts(newInstrs, blockStarts);
  discoverFunctionStarts(newInstrs, blockStarts, fnStarts);
  // finally add instructions to the graph
  for (auto &insn: newInstrs) {
    if (!graph.get(insn.address()))
      graph.addInstruction(std::move(insn));
  }
  return true;
}

void dragon::x86::X86Analysis::discoverFallthroughJoins(
        const Graph &graph,
        std::unordered_set<std::uint64_t> &blockStarts) const {
  // In address order, only instruction ends ahead of the current address
  // can still meet another decode. This keeps the set bounded by x86's
  // maximum instruction length rather than by the size of the whole image.
  std::set<std::uint64_t> ends;
  for (const auto &[address, insn] : graph.listing()) {
    ends.erase(ends.begin(), ends.lower_bound(address));
    if (insn.isReturn() || insn.isTrap() ||
        (insn.isJump() && !insn.isConditional()))
      continue;
    const auto next = address + insn.size();
    if (next <= address)
      continue;
    if (!ends.insert(next).second && graph.get(next))
      blockStarts.insert(next);
  }
}

namespace {

// Keep speculation isolated: a rejected table must leave neither its edges
// nor instructions, function starts, block splits or IGNIL in the real graph.
// A filtered copy is also the CFG used for slicing: decoded-but-unreachable
// case blocks are not predecessors until a tentative dispatch reaches them.
dragon::Graph copyGraph(const dragon::Graph &source,
                        const std::set<std::uint64_t> *blocks = nullptr) {
  dragon::Graph copy{source.architecture(), source.configuration()};
  for (const auto &[addr, insn]: source.listing())
    copy.addInstruction(insn);
  for (std::uint64_t addr: source.validBlockAddresses()) {
    if (blocks && !blocks->contains(addr))
      continue;
    copy.addBlock(addr);
    if (const auto *ib = source.getIGNILBlock(addr))
      copy.addIGNILBlock(*ib);
  }
  for (std::uint64_t addr: source.validFunctionAddresses())
    copy.addFunction(addr);
  return copy;
}

dragon::Graph reachableGraph(
        const dragon::Graph &source,
        const dragon::Disassembler::AnalysisHints &hints) {
  std::set<std::uint64_t> reached;
  // With explicit entries, mirror recursive traversal's roots. A heuristic
  // "function" after a padded RET can actually be a switch case; treating it
  // as an independent entry invents an unknown incoming register state.
  std::vector<std::uint64_t> work = hints.entryPoints;
  if (work.empty())
    work.assign(source.validFunctionAddresses().begin(),
                source.validFunctionAddresses().end());
  for (const auto &[address, instruction] : source.listing())
    work.insert(work.end(), instruction.callees().begin(), instruction.callees().end());
  while (!work.empty()) {
    const auto addr = work.back();
    work.pop_back();
    if (!reached.insert(addr).second)
      continue;
    const dragon::BasicBlock bb{&source, addr};
    if (!bb.isValid())
      continue;
    for (auto succ: bb.successors())
      work.push_back(succ);
    for (const auto *insn: bb.instructions())
      for (auto callee: insn->callees())
        work.push_back(callee);
  }
  return copyGraph(source, &reached);
}

} // namespace

void dragon::x86::X86Analysis::resolveIndirectJumps(
        Graph &graph, std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr, const Disassembler::AnalysisHints &hints,
        std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &fnStarts) const {
  const bool linear = configuration_.disassemblyStrategy ==
                      DisassemblyStrategy::LINEAR_SWEEP;
  const bool extensible=bool(hints.indirectTargets);
  const auto decodeLimit = extensible
      ? hints.indirectTargets->maxNewInstructions() : 1u << 20;
  // Loops in real dispatchers can be substantially deeper than the default
  // five-block slice. The slicer's per-query budget still caps join expansion.
  auto targetsAtDepth = [&](const BasicBlock &block, const Graph &cfg,
                            const BackwardSlicer::Predecessors &cfgPreds,
                            int depth) {
    std::set<std::uint64_t> targets;
    const auto *ib = cfg.getIGNILBlock(block.startAddress());
    if (!ib || ib->empty())
      return targets;
    const auto &last = ib->ops().back();
    if (last.opcode != ignil::Opcode::JMP || last.srcs.empty() ||
        std::holds_alternative<ignil::Const>(last.srcs[0]))
      return targets;

    const auto result = BackwardSlicer{depth}.slice(
            last.srcs[0], block.startAddress(), cfg, &cfgPreds);
    // A free LOAD must never be enumerated as arbitrary addresses; nonconstant
    // linear targets require a recognised table at decoded boundaries.
    const bool tablesOnly =
            linear || std::holds_alternative<ignil::Temp>(last.srcs[0]);
    std::vector<std::uint64_t> values;
    if (auto c = slice::detail::constantValue(result)) {
      // This includes a LOAD already proved constant by store forwarding.
      // No image-memory assumption or optional SMT enumeration is needed.
      values = {c->value};
    } else if (auto sw = slice::detectPossibleSwitch(result, block.startAddress(),
                                              cfg, depth, &cfgPreds);
        sw && sw->numCases > 0) {
      std::set<std::uint64_t> entries(hints.entryPoints.begin(), hints.entryPoints.end());
      values = analysis::detail::provedTableTargets(*sw, result, cfg, entries, hints);
    } else if (!tablesOnly) {
      values = collectTargets(result, block.startAddress());
    }
    for (auto target: values) {
      // Accept the complete table or nothing. Dropping an invalid entry would
      // misrepresent the dispatch as having fewer possible successors.
      if (target < baseAddr || target - baseAddr >= bytes.size() ||
          (linear && !cfg.get(target)))
        return std::set<std::uint64_t>{};
      targets.insert(target);
    }
    return targets;
  };

  auto targetsFor = [&](const BasicBlock &block, const Graph &cfg,
                        const BackwardSlicer::Predecessors &cfgPreds) {
    // Keep the short slice first: expanding an index through a loop can lose
    // its correspondence with the guard. Deeper slices are for hoisted bases
    // that cannot be reached within five blocks.
    //
    // Last, a one-block slice: the index is then a free value at the entry of
    // the guard block. A flattening dispatcher keeps its state in a register,
    // and before its table is resolved only the entry path (`mov ecx, K`) is
    // known, so every longer slice sees a constant and the guard bounds
    // nothing. A guard proved for an arbitrary entry value holds on every
    // incoming path, including ones the table itself reveals later.
    std::set<std::uint64_t> result;
    for (int depth: {5, 16, 64, 1}) {
      result = targetsAtDepth(block, cfg, cfgPreds, depth);
      if (!result.empty())break;
    }
    if(extensible) {
      if(auto additional=hints.indirectTargets->targets(block.startAddress())) {
        for(auto target:*additional) {
          if(target<baseAddr || target-baseAddr>=bytes.size() || (linear && !cfg.get(target)))
            return std::set<std::uint64_t>{};
          result.insert(target);
        }
      }
      if(result.size()>hints.indirectTargets->maxTargets())return std::set<std::uint64_t>{};
    }
    return result;
  };

  // A failed hypothesis is never retried in this analysis. Restarting from
  // the original graph also removes candidates that depended on it; those
  // can be rediscovered independently. Each restart bans at least one jump.
  std::set<std::uint64_t> rejected;
  std::uint32_t recoveryRounds=0;
  for (;;) {
    Graph tentative = copyGraph(graph);
    auto starts = blockStarts;
    auto functions = fnStarts;
    std::map<std::uint64_t, std::set<std::uint64_t>> candidates;
    std::set<std::uint64_t> originalCandidates;
    bool restart = false;

    LiftState liftState; // per tentative graph: first round lifts it all
    for (;;) {
      if(extensible && recoveryRounds++>=hints.indirectTargets->maxRounds()) {
        hints.indirectTargets->stopped("indirect recovery round budget exhausted");
        return; // No tentative instructions/edges have been published.
      }
      liftPendingBlocks(tentative, liftState,
                        commitPending(tentative, starts, functions));
      Graph cfg = reachableGraph(tentative, hints);
      // cfg is not modified in this round (new edges go to `tentative`), so
      // one predecessor map serves every slice taken in it.
      std::set<std::uint64_t> entries = hints.entryPoints.empty()
          ? std::set<std::uint64_t>(cfg.validFunctionAddresses().begin(),
                                    cfg.validFunctionAddresses().end())
          : std::set<std::uint64_t>(hints.entryPoints.begin(), hints.entryPoints.end());
      {
        // Explicit roots suppress heuristic function guesses, but never a real
        // CALL entry. Expansion may decode a new callee also reached by a tail
        // jump; a store on that tail path cannot constrain its independent
        // CALL input. Use the same boundary for all target/content proofs.
        for(const auto &[at,insn]:cfg.listing())
          entries.insert(insn.callees().begin(),insn.callees().end());
      }
      const auto cfgPreds = BackwardSlicer::predecessors(cfg, entries);
      if(extensible && !hints.indirectTargets->prepare(cfg,entries)) {
        hints.indirectTargets->stopped("target analysis did not reach a complete fixed point");
        return;
      }
      // Views only for blocks whose IGNIL ends in an indirect JMP (the only
      // ones targetsFor can resolve), not for every block every round.
      std::map<std::uint64_t, BasicBlock> jumps;
      for (std::uint64_t start: cfg.validBlockAddresses()) {
        const auto *ib = cfg.getIGNILBlock(start);
        if (!ib || ib->empty() || ib->ops().back().opcode != ignil::Opcode::JMP ||
            ib->ops().back().srcs.empty() ||
            std::holds_alternative<ignil::Const>(ib->ops().back().srcs[0]))
          continue;
        const BasicBlock block{&cfg, start};
        if (const auto *term = block.terminator();
            term && term->isJump() && term->hasIndirectTarget())
          jumps.emplace(term->address(), block);
      }

      // Validate *all* hypotheses after every expansion, including ones
      // resolved in earlier rounds: another table can expose a new incoming
      // path or split a dispatch block. Compare complete successor sets.
      bool expanded=false;
      auto addTargets=[&](std::uint64_t addr,const std::set<std::uint64_t> &targets) {
        auto *jump=tentative.get(addr);
        for(auto target:targets)if(!jump->branches().contains(target)) {
          jump->addInferredBranch(target);
          starts.insert(target);
          bool complete = true;
          integrateNewBlock(target,bytes,baseAddr,tentative,starts,functions,
                            complete,decodeLimit);
          if (!complete) {
            logger_.log("Indirect recovery decoding budget exhausted");
            if (extensible)
              hints.indirectTargets->stopped("indirect recovery decoding budget exhausted");
            return false;
          }
        }
        jump->setEdges(jump->branches().size());
        return true;
      };
      for (auto &[addr, expected]: candidates) {
        auto it = jumps.find(addr);
        const bool complete = std::ranges::all_of(expected, [&](auto target) {
          return BasicBlock{&tentative, target}.isValid();
        });
        auto current=it==jumps.end()?std::set<std::uint64_t>{}:targetsFor(it->second,cfg,cfgPreds);
        if(extensible && complete && !current.empty()) {
          auto grown=expected;
          grown.insert(current.begin(),current.end());
          if(grown.size()>hints.indirectTargets->maxTargets())current.clear();
          else {
            if(grown!=expected) {
              if(!addTargets(addr,grown))return;
              expected=std::move(grown);expanded=true;
            }
            continue; // Retaining extra proved edges is a safe over-approximation.
          }
        }
        if (!complete || it == jumps.end() || current != expected) {
          if(extensible && originalCandidates.contains(addr)) {
            hints.indirectTargets->stopped("existing indirect edges could not be revalidated after expansion");
            return;
          }
          logger_.log("Rejecting tentative indirect jump @ 0x{:x}", addr);
          rejected.insert(addr);
          restart = true;
          break;
        }
      }
      if (restart)
        break;
      if(expanded) {discoverFallthroughJoins(tentative,starts);continue;}

      bool changed = false;
      for (const auto &[addr, block]: jumps) {
        if (rejected.contains(addr) || candidates.contains(addr) ||
            (!extensible && !tentative.get(addr)->branches().empty()))
          continue;
        auto targets = targetsFor(block, cfg, cfgPreds);
        const auto &existing=tentative.get(addr)->branches();
        if(extensible && !existing.empty()) {
          if(targets.empty()) {
            hints.indirectTargets->stopped("existing indirect edges lack a complete proof");
            return;
          }
          originalCandidates.insert(addr);
          targets.insert(existing.begin(),existing.end());
          if(targets.size()>hints.indirectTargets->maxTargets()) {
            hints.indirectTargets->stopped("existing target set exceeds recovery budget");
            return;
          }
        }
        if (targets.empty())
          continue;
        candidates.emplace(addr, targets);
        logger_.log("Tentative indirect jump @ 0x{:x}: {} targets", addr,
                    targets.size());
        if(!addTargets(addr,targets))return;
        changed = true;
      }
      if (changed) {
        discoverFallthroughJoins(tentative, starts);
        continue;
      }

      // Publish only a fixed point that survived a complete recheck. Update
      // existing objects in place so borrowed Instruction/IGNIL pointers stay
      // valid when the caller analyses additional bytes into the same Graph.
      if (candidates.empty())
        return;
      for (const auto &[addr, insn]: tentative.listing()) {
        if (graph.get(addr))
          graph.update(insn);
        else
          graph.addInstruction(insn);
      }
      for (auto addr: tentative.validBlockAddresses()) {
        graph.addBlock(addr);
        if (const auto *ib = tentative.getIGNILBlock(addr)) {
          if (auto *existing = graph.getIGNILBlock(addr))
            *existing = *ib;
          else
            graph.addIGNILBlock(*ib);
        }
      }
      for (auto addr: tentative.validFunctionAddresses())
        graph.addFunction(addr);
      blockStarts = std::move(starts);
      fnStarts = std::move(functions);
      return;
    }
  }
}
