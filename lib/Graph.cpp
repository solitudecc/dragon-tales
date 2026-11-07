#include <dragon/disassembler/Graph.hpp>
#include "LLVMImportMetadata.hpp"
#include <algorithm>
#include <utility>

// ---------------------------------------------------------------------------
// GraphQueue - internal work-queue used during disassembly.
// Not part of the public API; defined here to keep the header clean.
// ---------------------------------------------------------------------------

namespace dragon {

class GraphQueue {
  public:
  void enqueue(std::uint64_t addr) {
    if (processed_.contains(addr) || pending_.contains(addr))
      return;
    pending_.insert(addr);
    queue_.insert(addr);
  }

  [[nodiscard]] bool is_queued(std::uint64_t addr) const {
    return queue_.contains(addr);
  }
  [[nodiscard]] bool is_pending(std::uint64_t addr) const {
    return pending_.contains(addr);
  }
  [[nodiscard]] bool is_processed(std::uint64_t addr) const {
    return processed_.contains(addr);
  }
  [[nodiscard]] bool is_valid(std::uint64_t addr) const {
    return valid_.contains(addr);
  }
  [[nodiscard]] bool is_invalid(std::uint64_t addr) const {
    return invalid_.contains(addr);
  }
  [[nodiscard]] bool empty() const { return queue_.empty(); }

  std::optional<std::uint64_t> next() {
    if (queue_.empty())
      return std::nullopt;
    const auto it = queue_.begin();
    std::uint64_t addr = *it;
    queue_.erase(it);
    pending_.erase(addr);
    processed_.insert(addr);
    return addr;
  }

  void mark_processed(std::uint64_t addr) { processed_.insert(addr); }

  void mark_valid(std::uint64_t addr) {
    if (processed_.contains(addr))
      valid_.insert(addr);
  }

  void mark_invalid(std::uint64_t addr) {
    if (!valid_.contains(addr))
      invalid_.insert(addr);
  }

  void enqueue_extend(const std::set<std::uint64_t> &addrs) {
    for (std::uint64_t addr : addrs)
      enqueue(addr);
  }

  [[nodiscard]] const std::set<std::uint64_t> &processed() const {
    return processed_;
  }
  [[nodiscard]] const std::set<std::uint64_t> &valid() const { return valid_; }
  [[nodiscard]] const std::set<std::uint64_t> &invalid() const {
    return invalid_;
  }

  private:
  std::set<std::uint64_t> queue_;
  std::set<std::uint64_t> pending_;
  std::set<std::uint64_t> processed_;
  std::set<std::uint64_t> valid_;
  std::set<std::uint64_t> invalid_;
};

// ---------------------------------------------------------------------------
// Graph::Queues - groups the three GraphQueues together for the pimpl.
// ---------------------------------------------------------------------------

struct Graph::Queues {
  GraphQueue instructions_q;
  GraphQueue blocks_q;
  GraphQueue functions_q;
  std::map<std::uint64_t,std::string> importSlots;
  std::set<std::uint64_t> conflictingImportSlots;
};

Graph Graph::clone() const {
  Graph copy{architecture_,configuration_};
  copy.listing_=listing_;
  copy.ignilBlocks_=ignilBlocks_;
  *copy.queues_=*queues_;
  return copy;
}

void detail::GraphLLVMMetadata::setImports(Graph &graph,const std::map<std::uint64_t,std::string>& slots) {
  graph.queues_->importSlots=slots;
  graph.queues_->conflictingImportSlots.clear();
}
const std::map<std::uint64_t,std::string>& detail::GraphLLVMMetadata::imports(const Graph &graph) {
  return graph.queues_->importSlots;
}

} // namespace dragon

// -- Graph -------------------------------------------------------------------

dragon::Graph::Graph(Architecture architecture, Configuration config) :
    architecture_(architecture), configuration_(std::move(config)),
    queues_(std::make_unique<Queues>()) {}

dragon::Graph::~Graph() = default;
dragon::Graph::Graph(Graph &&) noexcept = default;
dragon::Graph &dragon::Graph::operator=(Graph &&) noexcept = default;

void dragon::Graph::addInstruction(Instruction insn) {
  std::uint64_t addr = insn.address();
  listing_.emplace(addr, std::move(insn));
  queues_->instructions_q.mark_processed(addr);
  queues_->instructions_q.mark_valid(addr);
}

void dragon::Graph::addBlock(std::uint64_t addr) {
  queues_->blocks_q.mark_processed(addr);
  queues_->blocks_q.mark_valid(addr);
}

void dragon::Graph::addFunction(std::uint64_t addr) {
  queues_->functions_q.mark_processed(addr);
  queues_->functions_q.mark_valid(addr);
}

void dragon::Graph::insert(Instruction insn) {
  std::uint64_t addr = insn.address();
  listing_.emplace(addr, std::move(insn));
}

void dragon::Graph::update(const Instruction &insn) {
  if (auto it = listing_.find(insn.address()); it != listing_.end())
    it->second = insn;
}

void dragon::Graph::addIGNILBlock(ignil::Block block) {
  std::uint64_t addr = block.startAddress();
  ignilBlocks_.emplace(addr, std::move(block));
}

dragon::ignil::Block *dragon::Graph::getIGNILBlock(std::uint64_t addr) {
  auto it = ignilBlocks_.find(addr);
  return it != ignilBlocks_.end() ? &it->second : nullptr;
}

const dragon::ignil::Block *
dragon::Graph::getIGNILBlock(std::uint64_t addr) const {
  const auto it = ignilBlocks_.find(addr);
  return it != ignilBlocks_.end() ? &it->second : nullptr;
}

const dragon::Instruction *dragon::Graph::get(std::uint64_t address) const {
  const auto it = listing_.find(address);
  return it != listing_.end() ? &it->second : nullptr;
}

dragon::Instruction *dragon::Graph::get(std::uint64_t address) {
  const auto it = listing_.find(address);
  return it != listing_.end() ? &it->second : nullptr;
}

std::vector<dragon::Instruction *> dragon::Graph::instructions() {
  std::vector<Instruction *> result;
  for (std::uint64_t addr : queues_->instructions_q.valid()) {
    if (auto *insn = get(addr))
      result.push_back(insn);
  }
  return result;
}

std::vector<dragon::BasicBlock> dragon::Graph::blocks() const {
  std::vector<BasicBlock> result;
  for (std::uint64_t addr : queues_->blocks_q.valid()) {
    if (BasicBlock bb{this, addr}; bb.isValid())
      result.push_back(std::move(bb));
  }
  return result;
}

std::vector<dragon::Function> dragon::Graph::functions() const {
  std::vector<Function> result;
  for (std::uint64_t addr : queues_->functions_q.valid()) {
    Function fn(this, addr);
    if (fn.isValid())
      result.push_back(std::move(fn));
  }
  return result;
}

bool dragon::Graph::isValidBlock(std::uint64_t addr) const {
  return queues_->blocks_q.is_valid(addr);
}

bool dragon::Graph::isInvalidBlock(std::uint64_t addr) const {
  return queues_->blocks_q.is_invalid(addr);
}

bool dragon::Graph::isValidFunction(std::uint64_t addr) const {
  return queues_->functions_q.is_valid(addr);
}

const std::set<std::uint64_t> &dragon::Graph::validBlockAddresses() const {
  return queues_->blocks_q.valid();
}

const std::set<std::uint64_t> &dragon::Graph::validFunctionAddresses() const {
  return queues_->functions_q.valid();
}

void dragon::Graph::merge(Graph other) {
  // Independently proved fragments do not establish a proof for their union:
  // the other graph can introduce new incoming paths and independent entries.
  // Preserve caller/decoder evidence, retract inference, and require reanalysis.
  for (auto &[address, instruction] : listing_) instruction.clearInferredBranches();
  for (auto &[address, instruction] : other.listing_) instruction.clearInferredBranches();
  for (auto &[addr, insn] : other.listing_) {
    auto [where,inserted]=listing_.try_emplace(addr,std::move(insn));
    if(inserted)continue;
    auto &kept=where->second;
    // Preserve explicit evidence on compatible copies of the same decoded
    // instruction. Incompatible encodings retain the historical first-wins
    // policy; combining those would mix distinct instruction semantics.
    if(architecture_!=other.architecture_ || kept.size()!=insn.size() ||
       kept.bytes()!=insn.bytes() || kept.isJump()!=insn.isJump() ||
       kept.isCall()!=insn.isCall() || kept.isReturn()!=insn.isReturn() ||
       kept.isConditional()!=insn.isConditional() || kept.isTrap()!=insn.isTrap() ||
       kept.hasIndirectTarget()!=insn.hasIndirectTarget())continue;
    for(auto target:insn.branches())kept.addBranch(target);
    for(auto target:insn.callees())kept.addCallee(target);
    // Preserve explicit edge-count metadata as well as known targets.
    if(!kept.branches().empty())
      kept.setEdges(std::max({kept.edges(),insn.edges(),kept.branches().size()}));
    kept.setBlockStart(kept.isBlockStart() || insn.isBlockStart());
    kept.setFunctionStart(kept.isFunctionStart() || insn.isFunctionStart());
    kept.setPrologue(kept.isPrologue() || insn.isPrologue());
  }

  auto &dst = *queues_;
  const auto &src = *other.queues_;
  dst.conflictingImportSlots.insert(src.conflictingImportSlots.begin(),src.conflictingImportSlots.end());
  for (const auto &[slot,name]:src.importSlots) {
    auto [it,inserted]=dst.importSlots.emplace(slot,name);
    if (!inserted && it->second!=name) dst.conflictingImportSlots.insert(slot);
  }
  for (auto slot:dst.conflictingImportSlots) dst.importSlots.erase(slot);

  for (std::uint64_t addr : src.instructions_q.processed())
    dst.instructions_q.mark_processed(addr);
  for (std::uint64_t addr : src.instructions_q.valid())
    dst.instructions_q.mark_valid(addr);
  for (std::uint64_t addr : src.instructions_q.invalid())
    dst.instructions_q.mark_invalid(addr);

  for (std::uint64_t addr : src.blocks_q.processed())
    dst.blocks_q.mark_processed(addr);
  for (std::uint64_t addr : src.blocks_q.valid())
    dst.blocks_q.mark_valid(addr);
  for (std::uint64_t addr : src.blocks_q.invalid())
    dst.blocks_q.mark_invalid(addr);

  for (std::uint64_t addr : src.functions_q.processed())
    dst.functions_q.mark_processed(addr);
  for (std::uint64_t addr : src.functions_q.valid())
    dst.functions_q.mark_valid(addr);
  for (std::uint64_t addr : src.functions_q.invalid())
    dst.functions_q.mark_invalid(addr);

  while (auto addr = other.queues_->instructions_q.next())
    dst.instructions_q.enqueue(*addr);
  while (auto addr = other.queues_->blocks_q.next())
    dst.blocks_q.enqueue(*addr);
  while (auto addr = other.queues_->functions_q.next())
    dst.functions_q.enqueue(*addr);
}
