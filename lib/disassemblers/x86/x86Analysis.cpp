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

#ifdef DRAGON_WITH_Z3
#include <dragon/analysis/ValueEnumerator.hpp>
#endif

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
                                       std::uint64_t baseAddr,
                                       Graph &graph) const {
  std::unordered_set<std::uint64_t> blockStarts;
  std::unordered_set<std::uint64_t> fnStarts;

  std::vector<Instruction> instructions =
          disassembler_.disassemble(bytes, baseAddr);

  // The caller explicitly asked us to start analysis here, which is itself
  // a strong, deliberate signal that this address is a function entry -
  // independent of whether its bytes happen to match a recognized prologue
  // pattern. Without this, a leaf/frame-pointer-omitted function passed
  // directly to disassemble() (rather than reached via a call site, which
  // *is* already trusted unconditionally below) was never recognized as a
  // function at all, and Graph::functions() came back empty for it.
  if (!instructions.empty())
    fnStarts.insert(baseAddr);

  discoverBlockStarts(instructions, blockStarts);
  discoverFunctionStarts(instructions, blockStarts, fnStarts);
  discoverEpilogues(instructions, blockStarts, fnStarts);

  for (auto &insn: instructions)
    graph.addInstruction(std::move(insn));

  // Indirect-jump resolution via backward slicing / Z3 requires a
  // control-flow-accurate block set. Linear sweep decodes the whole section
  // sequentially, so resolved targets routinely land mid-instruction or inside
  // data (jump tables, SIMD constants), corrupting the existing block layout.
  const bool canResolve = configuration_.disassemblyStrategy !=
                          DisassemblyStrategy::LINEAR_SWEEP;

  bool changed = true;
  while (changed) {
    changed = false;
    commitPending(graph, blockStarts, fnStarts);
    liftPendingBlocks(graph);
    if (canResolve &&
        resolveIndirectJumps(graph, bytes, baseAddr, blockStarts, fnStarts))
      changed = true;
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

    // Skip nop/trap padding after the epilogue, up to 16 bytes.
    size_t j = i + matched;
    std::uint64_t paddingBytes = 0;
    while (j < n && paddingBytes < 16) {
      const auto &pad = instructions[j];
      if (!x86Info_->isNop(pad) && !x86Info_->isTrap(pad))
        break;
      paddingBytes += pad.size();
      ++j;
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

  for (size_t i = 0, e = instructions.size(); i < e; ++i) {
    const size_t windowSize = std::min(e - i, size_t{3});
    const Instruction *window[3] = {};
    for (size_t j = 0; j < windowSize; ++j)
      window[j] = &instructions[i + j];
    if (size_t matched =
                x86Info_->isCommonFunctionPrologue({window, windowSize})) {
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

void dragon::x86::X86Analysis::commitPending(
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

void dragon::x86::X86Analysis::liftPendingBlocks(Graph &graph) const {
  for (BasicBlock &block: graph.blocks()) {
    ignil::Block *existing = graph.getIGNILBlock(block.startAddress());
    if (existing && !coversMoreThan(*existing, block))
      continue;
    auto ib = x86IgnilLifter_->liftBlock(block, graph);
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
dragon::x86::X86Analysis::collectTargets(const slice::Expr &result,
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

bool dragon::x86::X86Analysis::integrateNewBlock(
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

bool dragon::x86::X86Analysis::resolveIndirectJumps(
        Graph &graph, std::span<const std::uint8_t> bytes,
        std::uint64_t baseAddr, std::unordered_set<std::uint64_t> &blockStarts,
        std::unordered_set<std::uint64_t> &fnStarts) const {

  bool changed = false;

  for (const auto &block: graph.blocks()) {
    const auto *ignilBlock = graph.getIGNILBlock(block.startAddress());
    if (!ignilBlock || ignilBlock->ops().empty())
      continue;

    const auto &last = ignilBlock->ops().back();
    // An indirect jump: through a register (`jmp rax`, `br x0`) or straight
    // through memory (`jmp [eax*4 + table]`, lifted as t = LOAD(..); JMP t -
    // the usual i386 jump-table dispatch). Either is sliced from its operand.
    if (last.opcode != ignil::Opcode::JMP || last.srcs.empty() ||
        std::holds_alternative<ignil::Const>(last.srcs[0]))
      continue;

    BackwardSlicer slicer{5};
    slice::Expr result =
            slicer.slice(last.srcs[0], ignilBlock->startAddress(), graph);
    // A memory-indirect jump's target is a value loaded from memory the
    // slicer cannot read, so only a recognised jump table resolves it -
    // enumerating the free load (Z3 fallback) would yield arbitrary targets,
    // e.g. for every `jmp [import_slot]` thunk.
    const bool viaMemory = std::holds_alternative<ignil::Temp>(last.srcs[0]);

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
        if (!viaMemory)
        targets = collectTargets(result, block.startAddress());
      }
    } else {
      if (!viaMemory)
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
