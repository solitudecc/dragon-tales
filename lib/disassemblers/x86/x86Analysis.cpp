#include "disassemblers/x86/x86Analysis.hpp"
#include "disassemblers/x86/X86IGNILLifter.hpp"

// Public headers
#include <dragon/analysis/BackwardSlicer.hpp>
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

  // Indirect-jump resolution. Under linear sweep only recognised jump tables
  // whose targets are instructions the sweep already decoded are used (see
  // resolveIndirectJumps): a free-form slice or Z3 guess can land
  // mid-instruction or inside data the sweep decoded as code.

  {
    LiftState liftState;
    liftPendingBlocks(graph, liftState,
                      commitPending(graph, blockStarts, fnStarts));
  }
  resolveIndirectJumps(graph, bytes, baseAddr, hints, blockStarts, fnStarts);

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
        Graph &graph, const std::unordered_set<std::uint64_t> &blockStarts,
        const std::unordered_set<std::uint64_t> &fnStarts) const {
  // Keep the per-instruction flags in step with the Graph's sets: call
  // targets and resolved indirect-jump targets reach the sets without the
  // instruction already at that address being flagged, and the flags are
  // what Instruction::isBlockStart()/isFunctionStart() report through the
  // C and Python APIs.
  std::vector<std::uint64_t> added;
  for (std::uint64_t addr: blockStarts) {
    if (!graph.isValidBlock(addr))
      added.push_back(addr);
    graph.addBlock(addr);
    if (Instruction *insn = graph.get(addr))
      insn->setBlockStart(true);
  }
  for (std::uint64_t addr: fnStarts) {
    graph.addFunction(addr);
    if (Instruction *insn = graph.get(addr))
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
  for (std::uint64_t s: newStarts) {
    todo.insert(s);
    // The block below a new start may have been split by it - or, having
    // ended where the decoded code ended, now runs on into it.
    auto it = valid.lower_bound(s);
    if (it != valid.begin())
      todo.insert(*std::prev(it));
  }
  const std::set<std::uint64_t> fresh(newStarts.begin(), newStarts.end());
  for (std::uint64_t addr: todo)
    lift(addr, !fresh.contains(addr) && !state.lacking.contains(addr));
}

std::vector<std::uint64_t>
dragon::x86::X86Analysis::collectTargets(const slice::Expr &result,
                                         std::uint64_t blockAddr) const {
  if (const auto *c = std::get_if<slice::Const>(&result.data))
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
        std::unordered_set<std::uint64_t> &fnStarts) const {
  if (graph.get(target))
    return false;

  // Decode the code reachable from `target` that the graph does not hold
  // yet: not just the first block - its branch targets, the fall-through of
  // a conditional branch and its callees are code too. Decoding only one
  // block registered those as block starts with no instruction behind them
  // (250 empty blocks in a recursive i686 Lua analysis once its jump tables
  // resolved). Known code stops the walk.
  constexpr std::size_t kMaxNewInstructions = 1 << 20;
  std::vector<Instruction> newInstrs;
  std::unordered_set<std::uint64_t> seen;
  std::vector<std::uint64_t> work{target};
  while (!work.empty() && newInstrs.size() < kMaxNewInstructions) {
    const std::uint64_t at = work.back();
    work.pop_back();
    if (graph.get(at) || seen.contains(at))
      continue;
    for (auto &insn: disassembler_.disassembleBlock(bytes, baseAddr, at)) {
      if (graph.get(insn.address()) || seen.contains(insn.address()))
        break; // ran into code already decoded
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
    // A free LOAD must never be enumerated as arbitrary addresses; linear
    // sweep additionally requires a recognised table at decoded boundaries.
    const bool tablesOnly =
            linear || std::holds_alternative<ignil::Temp>(last.srcs[0]);
    std::vector<std::uint64_t> values;
    if (auto sw = slice::detectPossibleSwitch(result, block.startAddress(),
                                              cfg, depth, &cfgPreds);
        sw && sw->numCases > 0) {
      values = slice::readTableTargets(*sw, bytes, baseAddr);
      if (values.empty() && hints.readMemory) {
        std::vector<std::uint8_t> table(std::size_t{sw->numCases} *
                                        sw->entrySize);
        if (hints.readMemory(sw->tableBase, table))
          values = slice::readTableTargets(*sw, table, sw->tableBase);
      }
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
    for (int depth: {5, 16, 64, 1}) {
      auto targets = targetsAtDepth(block, cfg, cfgPreds, depth);
      if (!targets.empty())
        return targets;
    }
    return std::set<std::uint64_t>{};
  };

  // A failed hypothesis is never retried in this analysis. Restarting from
  // the original graph also removes candidates that depended on it; those
  // can be rediscovered independently. Each restart bans at least one jump.
  std::set<std::uint64_t> rejected;
  for (;;) {
    Graph tentative = copyGraph(graph);
    auto starts = blockStarts;
    auto functions = fnStarts;
    std::map<std::uint64_t, std::set<std::uint64_t>> candidates;
    bool restart = false;

    LiftState liftState; // per tentative graph: first round lifts it all
    for (;;) {
      liftPendingBlocks(tentative, liftState,
                        commitPending(tentative, starts, functions));
      Graph cfg = reachableGraph(tentative, hints);
      // cfg is not modified in this round (new edges go to `tentative`), so
      // one predecessor map serves every slice taken in it.
      const auto cfgPreds = BackwardSlicer::predecessors(cfg);
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
      for (const auto &[addr, expected]: candidates) {
        auto it = jumps.find(addr);
        const bool complete = std::ranges::all_of(expected, [&](auto target) {
          return BasicBlock{&tentative, target}.isValid();
        });
        if (!complete || it == jumps.end() ||
            targetsFor(it->second, cfg, cfgPreds) != expected) {
          logger_.log("Rejecting tentative indirect jump @ 0x{:x}", addr);
          rejected.insert(addr);
          restart = true;
          break;
        }
      }
      if (restart)
        break;

      bool changed = false;
      for (const auto &[addr, block]: jumps) {
        if (rejected.contains(addr) || candidates.contains(addr) ||
            !tentative.get(addr)->branches().empty())
          continue;
        const auto targets = targetsFor(block, cfg, cfgPreds);
        if (targets.empty())
          continue;
        candidates.emplace(addr, targets);
        logger_.log("Tentative indirect jump @ 0x{:x}: {} targets", addr,
                    targets.size());
        auto *jump = tentative.get(addr);
        for (auto target: targets) {
          jump->addBranch(target);
          starts.insert(target);
          integrateNewBlock(target, bytes, baseAddr, tentative, starts,
                             functions);
        }
        jump->setEdges(jump->branches().size());
        changed = true;
      }
      if (changed)
        continue;

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
