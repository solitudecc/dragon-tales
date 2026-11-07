#include "disassemblers/arm/armAnalysis.hpp"
#include "disassemblers/arm/armIGNILLifter.hpp"
#include "analysis/TableContents.hpp"

// Public headers
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/analysis/SwitchDetector.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/disassembler/Instruction.hpp>
#include <dragon/disassembler/arm/armInfo.hpp>

#ifdef DRAGON_WITH_Z3
#include <dragon/analysis/ValueEnumerator.hpp>
#endif

// C++ headers
#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

dragon::arm::ARMAnalysis::ARMAnalysis(Architecture architecture,
                                      const Disassembler &disassembler,
                                      const Configuration &configuration,
                                      const llvm::MCInstrInfo &instrInfo,
                                      const llvm::MCRegisterInfo &regInfo) :
    architecture_(architecture), disassembler_(disassembler),
    configuration_(configuration), instrInfo_(instrInfo), regInfo_(regInfo) {
  // ARM and AArch64 use different LLVM opcode/register ID namespaces. Only
  // initialize the classifier for the target whose tables we actually have;
  // initializing both here poisoned the other architecture's first use.
  if (architecture == Architecture::ARM32) {
    static const ARMInfo armInfo32{Architecture::ARM32, instrInfo, regInfo};
    armInfo_ = &armInfo32;
  } else {
    static const ARMInfo armInfo64{Architecture::ARM64, instrInfo, regInfo};
    armInfo_ = &armInfo64;
  }
  logger_ = internal::Logger{configuration_.logging};
  armIgnilLifter_ = std::make_unique<ARMIGNILLifter>(logger_, architecture,
                                                     instrInfo, regInfo);
}

dragon::arm::ARMAnalysis::~ARMAnalysis() = default;

bool dragon::arm::ARMAnalysis::analyze(std::span<const std::uint8_t> bytes,
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

  // The caller explicitly asked us to start analysis here - trust that as a
  // function entry independent of prologue matching, same reasoning and
  // same fix as X86Analysis::analyze(). This is what leaves ARM64
  // frame-pointer-omitting leaf functions unrecognized in
  // ToDo/LifterGaps.md #3: no `stp x29,x30`/`mov x29,sp` to match, and
  // until now no other signal claimed the entry address either.
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

  // Discover block starts, Function starts, and end of functions
  discoverBlockStarts(instructions, blockStarts);
  discoverFunctionStarts(instructions, blockStarts, fnStarts);
  discoverEpilogues(instructions, blockStarts, fnStarts);

  // Add the instructions to the graph
  for (auto &instruction: instructions) {
    graph.addInstruction(std::move(instruction));
  }

  // Indirect-jump resolution. Under linear sweep only recognised jump tables
  // whose targets are instructions the sweep already decoded are used (see
  // resolveIndirectJumps): a free-form slice or Z3 guess can land
  // mid-instruction or inside data the sweep decoded as code.

  LiftState liftState;
  liftPendingBlocks(graph, liftState, commitPending(graph, blockStarts, fnStarts));
  resolveIndirectJumps(graph, bytes, baseAddr, hints, blockStarts, fnStarts);

  return true;
}

void dragon::arm::ARMAnalysis::discoverBlockStarts(
        std::vector<Instruction> &instructions,
        std::unordered_set<std::uint64_t> &blockStarts) const {
  if (instructions.empty()) {
    return;
  }

  blockStarts.insert(instructions[0].address());

  for (const auto &I: instructions) {
    if (I.hasIndirectTarget()) {
      logger_.log("Instruction {} at address 0x{:x} has an indirect target.",
                  I.disassembly(), I.address());
    }

    if (I.isJump()) {
      for (auto addr: I.branches()) {
        blockStarts.insert(addr);
        logger_.log("Discovered 0x{:x} as beginning of block", addr);
      }
      if (I.isConditional()) {
        std::uint64_t addr = I.address() + I.size();
        blockStarts.insert(addr);
        logger_.log("Discovered 0x{:x} as beginning of block", addr);
      }
    }
  }

  for (auto &I: instructions) {
    if (blockStarts.contains(I.address())) {
      I.setBlockStart(true);
    }
  }
}

void dragon::arm::ARMAnalysis::discoverEpilogues(
        std::vector<Instruction> &instructions,
        std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &functionStarts) const {
  if (instructions.empty()) return;
  // As on x86, RET plus alignment is ambiguous inside an unresolved
  // dispatcher: the next leaf may be another case. Delimit regions only by
  // entries already justified by roots, direct calls or real prologues, so a
  // dispatch in one region does not suppress discovery in a separate one.
  std::vector<std::uint64_t> knownEntries(functionStarts.begin(),functionStarts.end());
  std::sort(knownEntries.begin(),knownEntries.end());
  const auto regionFor = [&](std::uint64_t address) {
    const auto next = std::upper_bound(knownEntries.begin(),knownEntries.end(),address);
    return next == knownEntries.begin() ? instructions.front().address() : *std::prev(next);
  };
  std::unordered_set<std::uint64_t> indirectRegions;
  for (const auto &instruction : instructions)
    if (instruction.isJump() && !instruction.isReturn() &&
        instruction.hasIndirectTarget() && instruction.branches().empty())
      indirectRegions.insert(regionFor(instruction.address()));
  const size_t n = instructions.size();
  size_t i = 0;
  while (i < n) {
    // ARM64's longest epilogue is 4 instructions (autiasp; ldp x29,x30,[sp],
    // #N; add sp,sp,#N; ret) vs x86's 2, and arm64IsCommonEpilogue hard-fails
    // (returns 0) if the mandatory trailing `ret` falls outside the window -
    // unlike the prologue matcher it does not degrade gracefully to a partial
    // match. So the window has to be sized to the full epilogue, not just 2.
    const size_t windowsSize = std::min(n - i, size_t{4});
    const Instruction *window[4] = {};
    for (size_t j = 0; j < windowsSize; ++j) {
      window[j] = &instructions[i + j];
    }

    const size_t matched =
            armInfo_->isCommonFunctionEpilogue({window, windowsSize});

    if (!matched) {
      ++i;
      continue;
    }
    // Skip nop/trap padding after the epilogue. Alignment padding runs past
    // 16 bytes (gcc pads aarch64 functions with up to 7 nops); with a
    // 16-byte cap the rest of the run became a "function" made of nops.
    constexpr std::uint64_t kMaxPadding = 64;
    size_t j = i + matched;
    std::uint64_t paddingBytes = 0;
    while (j < n && paddingBytes < kMaxPadding) {
      const auto &pad = instructions[j];
      if (!armInfo_->isNop(pad) && !armInfo_->isTrap(pad))
        break;
      paddingBytes += pad.size();
      ++j;
    }

    // Still padding after the cap - a run of zero words (`udf #0`) or nops
    // longer than any alignment, i.e. data. Not a function; and resume past
    // the whole run so each pad is not taken for another epilogue.
    const bool stillPadding =
            j < n && (armInfo_->isNop(instructions[j]) ||
                      armInfo_->isTrap(instructions[j]));
    if (stillPadding) {
      while (j < n && (armInfo_->isNop(instructions[j]) ||
                       armInfo_->isTrap(instructions[j])))
        ++j;
      i = j;
      continue;
    }

    if (j < n) {
      auto &next = instructions[j];
      if (blockStarts.contains(next.address())) {
        // Already a known branch target - it belongs to the current function's
        // CFG, not a new function entry (e.g. the else-block after a then-ret).
        logger_.log("Epilogue at 0x{:x}: 0x{:x} is a branch target, skipping "
                    "function-start promotion",
                    instructions[i].address(), next.address());
      } else if (indirectRegions.contains(regionFor(next.address())) &&
                 !functionStarts.contains(next.address())) {
        logger_.log("Post-RET candidate at 0x{:x} may be an indirect case; "
                    "not promoting it to a function",next.address());
      } else {
        functionStarts.insert(next.address());
        blockStarts.insert(next.address());
        next.setFunctionStart(true);
        next.setBlockStart(true);
        logger_.log("Discovered function start via epilogue at address 0x{:x}",
                    next.address());
      }
    }

    i += matched;
  }
}

void dragon::arm::ARMAnalysis::discoverFunctionStarts(
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
                armInfo_->isCommonFunctionPrologue({window, windowSize})) {
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

std::vector<std::uint64_t> dragon::arm::ARMAnalysis::commitPending(
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

void dragon::arm::ARMAnalysis::liftPendingBlocks(
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
    auto ib = armIgnilLifter_->liftBlock(block, graph);
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
dragon::arm::ARMAnalysis::collectTargets(const slice::Expr &result,
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

bool dragon::arm::ARMAnalysis::integrateNewBlock(
        std::uint64_t target, std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr, Graph &graph,
        std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &fnStarts,
        bool &complete) const {
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
      if (newInstrs.size() >= kMaxNewInstructions) {
        complete = false;
        return false; // No partial decode has reached the tentative graph.
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

namespace {
// Analysis views may filter unreachable blocks, but retain the full listing so
// linear-sweep target checks still require real decoded instruction boundaries.
dragon::Graph reachableGraph(const dragon::Graph &source,
                             const dragon::Disassembler::AnalysisHints &hints) {
  std::set<std::uint64_t> reached;
  std::vector<std::uint64_t> work = hints.entryPoints;
  if (work.empty())
    work.assign(source.validFunctionAddresses().begin(), source.validFunctionAddresses().end());
  for (const auto &[address,instruction] : source.listing())
    work.insert(work.end(),instruction.callees().begin(),instruction.callees().end());
  while (!work.empty()) {
    const auto address = work.back();
    work.pop_back();
    if (!reached.insert(address).second) continue;
    const dragon::BasicBlock block{&source,address};
    if (!block.isValid()) continue;
    for (auto successor : block.successors()) work.push_back(successor);
    for (const auto *instruction : block.instructions())
      for (auto callee : instruction->callees()) work.push_back(callee);
  }
  dragon::Graph copy{source.architecture(),source.configuration()};
  for (const auto &[address,instruction] : source.listing()) copy.addInstruction(instruction);
  for (auto address : source.validBlockAddresses()) if (reached.contains(address)) {
    copy.addBlock(address);
    if (const auto *block = source.getIGNILBlock(address)) copy.addIGNILBlock(*block);
  }
  for (auto address : source.validFunctionAddresses()) copy.addFunction(address);
  return copy;
}
} // namespace

void dragon::arm::ARMAnalysis::resolveIndirectJumps(
        Graph &graph, std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr, const Disassembler::AnalysisHints &hints,
        std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &fnStarts) const {
  const bool linear = configuration_.disassemblyStrategy == DisassemblyStrategy::LINEAR_SWEEP;
  auto targetsFor = [&](const BasicBlock &block, const Graph &cfg,
                        const BackwardSlicer::Predecessors &preds,
                        const std::set<std::uint64_t> &entries) {
    std::set<std::uint64_t> result;
    const auto *ir = cfg.getIGNILBlock(block.startAddress());
    if (!ir || ir->empty()) return result;
    const auto &tail = ir->ops().back();
    if (tail.opcode != ignil::Opcode::JMP || tail.srcs.empty() ||
        std::holds_alternative<ignil::Const>(tail.srcs[0])) return result;
    constexpr int depth = 5;
    const auto expression = BackwardSlicer{depth}.slice(tail.srcs[0],block.startAddress(),cfg,&preds);
    const bool tablesOnly = linear || std::holds_alternative<ignil::Temp>(tail.srcs[0]);
    std::vector<std::uint64_t> values;
    if (auto table = slice::detectPossibleSwitch(expression,block.startAddress(),cfg,depth,&preds)) {
      if (table->numCases)
        values = analysis::detail::provedTableTargets(*table,expression,cfg,entries,hints);
      else if (!tablesOnly)
        values = collectTargets(expression,block.startAddress());
    } else if (!tablesOnly) {
      values = collectTargets(expression,block.startAddress());
    }
    // A proved set is accepted in full. A target outside this code range or
    // outside linear sweep's decoded boundaries invalidates the whole set.
    for (auto target : values) {
      if (target < baseAddr || target - baseAddr >= bytes.size() || (linear && !cfg.get(target)))
        return std::set<std::uint64_t>{};
      result.insert(target);
    }
    return result;
  };

  // Restart from the original graph after any failed hypothesis. This removes
  // all its speculative edges, decodes, function guesses and block splits,
  // including candidates which depended on it. Each restart bans one new PC.
  std::set<std::uint64_t> rejected;
  for (;;) {
    Graph tentative = graph.clone();
    auto starts = blockStarts;
    auto functions = fnStarts;
    std::map<std::uint64_t,std::set<std::uint64_t>> candidates;
    LiftState liftState;
    bool restart = false;
    for (;;) {
      liftPendingBlocks(tentative,liftState,commitPending(tentative,starts,functions));
      Graph cfg = reachableGraph(tentative,hints);
      std::set<std::uint64_t> entries = hints.entryPoints.empty()
          ? cfg.validFunctionAddresses()
          : std::set<std::uint64_t>(hints.entryPoints.begin(),hints.entryPoints.end());
      // Every decoded direct callee also admits independent arbitrary inputs,
      // even when a tail path reaches it with initialized table bytes.
      for (const auto &[address,instruction] : cfg.listing())
        entries.insert(instruction.callees().begin(),instruction.callees().end());
      const auto preds = BackwardSlicer::predecessors(cfg,entries);
      std::map<std::uint64_t,BasicBlock> jumps;
      for (auto address : cfg.validBlockAddresses()) {
        const auto *ir = cfg.getIGNILBlock(address);
        if (!ir || ir->empty() || ir->ops().back().opcode != ignil::Opcode::JMP ||
            ir->ops().back().srcs.empty() ||
            std::holds_alternative<ignil::Const>(ir->ops().back().srcs[0])) continue;
        BasicBlock block{&cfg,address};
        if (const auto *last = block.terminator(); last && last->isJump() && last->hasIndirectTarget())
          jumps.emplace(last->address(),std::move(block));
      }
      // Recompute every old candidate against this round's entire CFG and
      // memory fixed point. Newly revealed paths can invalidate STORE facts.
      for (const auto &[address,expected] : candidates) {
        const auto found = jumps.find(address);
        const bool complete = std::ranges::all_of(expected,[&](auto target) {
          return BasicBlock{&tentative,target}.isValid();
        });
        if (!complete || found == jumps.end() ||
            targetsFor(found->second,cfg,preds,entries) != expected) {
          logger_.log("Rejecting tentative indirect jump @ 0x{:x}",address);
          rejected.insert(address);
          restart = true;
          break;
        }
      }
      if (restart) break;

      bool changed = false;
      for (const auto &[address,block] : jumps) {
        // Direct and explicitly caller-supplied evidence is owned by the
        // caller/decoder; recovery never erases it or calls it a new proof.
        if (rejected.contains(address) || candidates.contains(address) ||
            !tentative.get(address)->branches().empty()) continue;
        auto targets = targetsFor(block,cfg,preds,entries);
        if (targets.empty()) continue;
        candidates.emplace(address,targets);
        auto *jump = tentative.get(address);
        for (auto target : targets) {
          jump->addInferredBranch(target);
          starts.insert(target);
          bool complete = true;
          integrateNewBlock(target,bytes,baseAddr,tentative,starts,functions,complete);
          if (!complete) {
            logger_.log("Indirect recovery decoding budget exhausted");
            return; // No speculative graph objects have been published.
          }
        }
        jump->setEdges(jump->branches().size());
        changed = true;
      }
      if (changed) continue;
      if (candidates.empty()) return;

      // Publish only the fully revalidated fixed point. Existing map objects
      // are updated in place so borrowed Instruction/IGNIL pointers survive.
      for (const auto &[address,instruction] : tentative.listing()) {
        if (graph.get(address)) graph.update(instruction);
        else graph.addInstruction(instruction);
      }
      for (auto address : tentative.validBlockAddresses()) {
        graph.addBlock(address);
        if (const auto *ir = tentative.getIGNILBlock(address)) {
          if (auto *existing = graph.getIGNILBlock(address)) *existing = *ir;
          else graph.addIGNILBlock(*ir);
        }
      }
      for (auto address : tentative.validFunctionAddresses()) graph.addFunction(address);
      blockStarts = std::move(starts);
      fnStarts = std::move(functions);
      return;
    }
    if (!restart) return;
  }
}
