#include "disassemblers/arm/armAnalysis.hpp"
#include "disassemblers/arm/armIGNILLifter.hpp"

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
#include <unordered_set>
#include <vector>

dragon::arm::ARMAnalysis::ARMAnalysis(Architecture architecture,
                                      const Disassembler &disassembler,
                                      const Configuration &configuration,
                                      const llvm::MCInstrInfo &instrInfo,
                                      const llvm::MCRegisterInfo &regInfo) :
    architecture_(architecture), disassembler_(disassembler),
    configuration_(configuration), instrInfo_(instrInfo), regInfo_(regInfo) {
  static const ARMInfo armInfo32{Architecture::ARM32, instrInfo, regInfo};
  static const ARMInfo armInfo64{Architecture::ARM64, instrInfo, regInfo};
  logger_ = internal::Logger{configuration_.logging};
  armIgnilLifter_ = std::make_unique<ARMIGNILLifter>(logger_, architecture,
                                                     instrInfo, regInfo);
  armInfo_ = (architecture == Architecture::ARM32) ? &armInfo32 : &armInfo64;
}

dragon::arm::ARMAnalysis::~ARMAnalysis() = default;

bool dragon::arm::ARMAnalysis::analyze(std::span<const std::uint8_t> bytes,
                                       std::uint64_t baseAddr,
                                       Graph &graph) const {
  std::unordered_set<std::uint64_t> blockStarts;
  std::unordered_set<std::uint64_t> fnStarts;

  std::vector<Instruction> instructions =
          disassembler_.disassemble(bytes, baseAddr);

  // The caller explicitly asked us to start analysis here - trust that as a
  // function entry independent of prologue matching, same reasoning and
  // same fix as X86Analysis::analyze(). This is what leaves ARM64
  // frame-pointer-omitting leaf functions unrecognized in
  // ToDo/LifterGaps.md #3: no `stp x29,x30`/`mov x29,sp` to match, and
  // until now no other signal claimed the entry address either.
  if (!instructions.empty())
    fnStarts.insert(baseAddr);

  // Discover block starts, Function starts, and end of functions
  discoverBlockStarts(instructions, blockStarts);
  discoverFunctionStarts(instructions, blockStarts, fnStarts);
  discoverEpilogues(instructions, blockStarts, fnStarts);

  // Add the instructions to the graph
  for (auto &instruction: instructions) {
    graph.addInstruction(std::move(instruction));
  }

  // Indirect-jump resolution via backward slicing and Z3 if available
  // it requires a control-flow-accurate block set. Linear sweep decodes the
  // whole section sequentially, so resolved targets routinely land
  // mid-instructions or inside data, corrupting the existing block layout.
  const bool canResolve = configuration_.disassemblyStrategy !=
                          DisassemblyStrategy::LINEAR_SWEEP;

  bool changed = true;
  while (changed) {
    changed = false;
    commitPending(graph, blockStarts, fnStarts);
    liftPendingBlocks(graph);
    if (canResolve &&
        resolveIndirectJumps(graph, bytes, baseAddr, blockStarts, fnStarts)) {
      changed = true;
    }
  }

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
    // Skip nop/trap padding after the epilogue, up to 16 bytes.
    size_t j = i + matched;
    std::uint64_t paddingBytes = 0;
    while (j < n && paddingBytes < 16) {
      const auto &pad = instructions[j];
      if (!armInfo_->isNop(pad) && !armInfo_->isTrap(pad))
        break;
      paddingBytes += pad.size();
      ++j;
    }

    if (j < n) {
      auto &next = instructions[j];
      if (blockStarts.contains(next.address())) {
        // Already a known branch target - it belongs to the current function's
        // CFG, not a new function entry (e.g. the else-block after a then-ret).
        logger_.log("Epilogue at 0x{:x}: 0x{:x} is a branch target, skipping "
                    "function-start promotion",
                    instructions[i].address(), next.address());
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

  for (size_t i = 0, e = instructions.size(); i < e; ++i) {
    const size_t windowSize = std::min(e - i, size_t{3});
    const Instruction *window[3] = {};
    for (size_t j = 0; j < windowSize; ++j)
      window[j] = &instructions[i + j];
    if (size_t matched =
                armInfo_->isCommonFunctionPrologue({window, windowSize})) {
      std::uint64_t addr = instructions[i].address();
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

void dragon::arm::ARMAnalysis::commitPending(
        Graph &graph, const std::unordered_set<std::uint64_t> &blockStarts,
        const std::unordered_set<std::uint64_t> &fnStarts) const {
  // Keep the per-instruction flags in step with the Graph's sets: call
  // targets and resolved indirect-jump targets reach the sets without the
  // instruction already at that address being flagged, and the flags are
  // what Instruction::isBlockStart()/isFunctionStart() report through the
  // C and Python APIs.
  for (std::uint64_t addr: blockStarts) {
    graph.addBlock(addr);
    if (Instruction *insn = graph.get(addr))
      insn->setBlockStart(true);
  }
  for (std::uint64_t addr: fnStarts) {
    graph.addFunction(addr);
    if (Instruction *insn = graph.get(addr))
      insn->setFunctionStart(true);
  }
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

void dragon::arm::ARMAnalysis::liftPendingBlocks(Graph &graph) const {
  for (BasicBlock &block: graph.blocks()) {
    ignil::Block *existing = graph.getIGNILBlock(block.startAddress());
    if (existing && !coversMoreThan(*existing, block))
      continue;
    auto ib = armIgnilLifter_->liftBlock(block, graph);
    logger_.log("{} block @ 0x{:x} -> {} IGNIL ops",
                existing ? "Re-lifted (split)" : "Lifted",
                block.startAddress(), ib.size());
    if (existing)
      *existing = std::move(ib); // in place: borrowed pointers stay valid
    else
      graph.addIGNILBlock(std::move(ib));
  }
}

std::vector<std::uint64_t>
dragon::arm::ARMAnalysis::collectTargets(const slice::Expr &result,
                                         std::uint64_t blockAddr) const {
  if (const auto *c = std::get_if<slice::Const>(&result.data))
    return {c->value};

#ifdef DRAGON_WITH_Z3
  if (configuration_.useZ3ForPathDiscovery) {
    z3::context ctx;
    auto targets = slice::enumeratedValues(result, ctx);
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
        std::unordered_set<std::uint64_t> &fnStarts) const {
  if (graph.get(target))
    return false;

  // Try to retrieve new blocks with the disassembler
  auto newInstrs = disassembler_.disassembleBlock(bytes, baseAddr, target);
  if (newInstrs.empty())
    return false;

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

bool dragon::arm::ARMAnalysis::resolveIndirectJumps(
        Graph &graph, std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr, std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &fnStarts) const {

  bool changed = false;

  for (const auto &block: graph.blocks()) {
    const auto *ignilBlock = graph.getIGNILBlock(block.startAddress());
    if (!ignilBlock || ignilBlock->ops().empty())
      continue;

    const auto &last = ignilBlock->ops().back();
    if (last.opcode != ignil::Opcode::JMP || last.srcs.empty() ||
        !std::holds_alternative<ignil::Reg>(last.srcs[0]))
      continue;

    const auto &reg = std::get<ignil::Reg>(last.srcs[0]);
    BackwardSlicer slicer{5};
    slice::Expr result =
            slicer.slice(ignil::Value{reg}, ignilBlock->startAddress(), graph);

    std::vector<std::uint64_t> targets;
    if (auto sw = slice::detectPossibleSwitch(result, block.startAddress(),
                                              graph)) {
      if (sw->numCases > 0) {
        logger_.log("Switch at 0x{:x}: table=0x{:x} entries={} entrySize={}",
                    block.startAddress(), sw->tableBase, sw->numCases,
                    sw->entrySize);
        targets = slice::readTableTargets(*sw, bytes, baseAddr);
      } else {
        logger_.log(
                "Switch at 0x{:x}: table=0x{:x} (bounds unknown, falling back)",
                block.startAddress(), sw->tableBase);
        targets = collectTargets(result, block.startAddress());
      }
    } else {
      targets = collectTargets(result, block.startAddress());
    }

    for (std::uint64_t target: targets) {
      if (target < baseAddr || target >= baseAddr + bytes.size()) {
        logger_.log("Resolved target 0x{:x} is out of range, skipping", target);
        continue;
      }

      logger_.log("Resolved indirect jump @ 0x{:x} -> 0x{:x}",
                  block.startAddress(), target);

      if (blockStarts.insert(target).second)
        changed = true;

      if (integrateNewBlock(target, bytes, baseAddr, graph, blockStarts,
                            fnStarts))
        changed = true;
    }
  }

  return changed;
}
