#include "LLVMPreparedRegion.hpp"
#include <dragon/disassembler/Disassembler.hpp>

namespace dragon::llvmlifter::detail {

PreparedRegion::PreparedRegion(std::unique_ptr<const Graph> graph, std::uint64_t entry,
    const std::set<std::uint64_t> &starts, bool rebuilt)
    : graph_(std::move(graph)), entry_(entry), rebuilt_(rebuilt) {
  for (auto address : starts) blocks_.emplace(address, BasicBlock{graph_.get(), address});
}

std::unique_ptr<PreparedRegion> PreparedRegion::prepare(const Function &fn, const Graph &source,
    std::span<const std::uint64_t> additional, const StaticRegionOptions &options) {
  if (!fn.isValid() || !options.maxBlocks || options.maxBlocks > 4096 ||
      !options.maxDispatchCases || options.maxDispatchCases > 65536 ||
      additional.size() > options.maxBlocks || fn.blocks().size() > options.maxBlocks)
    return nullptr;
  std::set<std::uint64_t> starts;
  for (const auto &[address, block] : fn.blocks()) {
    const auto instructions = block.instructions();
    if (instructions.empty() || source.get(address) != instructions.front()) return nullptr;
    starts.insert(address);
  }
  for (auto address : additional) {
    const auto *instruction = source.get(address);
    if (!instruction || (options.rebuildDecodedBlocks &&
        (instruction->bytes().empty() || !instruction->size()))) return nullptr;
    starts.insert(address);
    if (starts.size() > options.maxBlocks) return nullptr;
  }
  auto work = std::make_unique<Graph>(source.clone());
  if (options.rebuildDecodedBlocks) {
    // Split all paths first, then rebuild prefixes and suffixes consistently.
    for (auto address : starts) work->addBlock(address);
    Disassembler dis{source.architecture(), source.configuration()};
    std::size_t instructionCount = 0;
    for (auto address : starts) {
      BasicBlock block{work.get(), address};
      if (!block.isValid()) return nullptr;
      const auto instructions = block.instructions();
      if (instructions.empty() || instructions.size() > 65536 - instructionCount) return nullptr;
      instructionCount += instructions.size();
      std::vector<Instruction> trace;
      trace.reserve(instructions.size());
      for (const auto *instruction : instructions) {
        if (!instruction->mcInst()) return nullptr;
        trace.push_back(*instruction);
      }
      auto ir = dis.liftTrace(trace);
      const auto &ops = ir.ops();
      const bool terminated = !ops.empty() && (ops.back().opcode == ignil::Opcode::JMP ||
          ops.back().opcode == ignil::Opcode::CJMP || ops.back().opcode == ignil::Opcode::RET);
      if (!terminated && block.successors().size() == 1)
        ir.append({ignil::Opcode::JMP, std::nullopt,
            {ignil::Const{*block.successors().begin(), 64}}, block.terminatorAddress()});
      if (auto *existing = work->getIGNILBlock(address)) *existing = std::move(ir);
      else work->addIGNILBlock(std::move(ir));
    }
    // The rebuilt function can acquire split members, just as in the original
    // recursive region implementation. Preserve them as well as chosen starts.
    Function root{work.get(), fn.startAddress()};
    if (!root.isValid()) return nullptr;
    for (const auto &[address, block] : root.blocks()) starts.insert(address);
    if (starts.size() > options.maxBlocks) return nullptr;
  }
  std::size_t dynamicJumps = 0;
  for (auto address : starts) {
    BasicBlock block{work.get(), address};
    const auto instructions = block.instructions();
    const auto *ir = work->getIGNILBlock(address);
    if (!work->isValidBlock(address) || !block.isValid() || instructions.empty() ||
        instructions.front()->address() != address || !ir) return nullptr;
    for (const auto &op : ir->ops()) if (op.opcode == ignil::Opcode::JMP) {
      if (op.srcs.size() != 1) return nullptr;
      if (!std::holds_alternative<ignil::Const>(op.srcs[0])) ++dynamicJumps;
    }
  }
  if (dynamicJumps && starts.size() > options.maxDispatchCases / dynamicJumps) return nullptr;
  return std::unique_ptr<PreparedRegion>(new PreparedRegion{
      std::move(work), fn.startAddress(), starts, options.rebuildDecodedBlocks});
}

std::optional<OpOrigin> PreparedRegion::origin(std::uint64_t block, std::size_t opIndex) const {
  if (!blocks_.contains(block)) return std::nullopt;
  const auto *ir = graph_->getIGNILBlock(block);
  if (!ir || opIndex >= ir->ops().size()) return std::nullopt;
  return OpOrigin{instance_, block, opIndex, ir->ops()[opIndex].address};
}

bool PreparedRegion::owns(const OpOrigin &candidate) const {
  if (candidate.instance != instance_) return false;
  const auto expected = origin(candidate.block, candidate.opIndex);
  return expected && *expected == candidate;
}

} // namespace dragon::llvmlifter::detail
