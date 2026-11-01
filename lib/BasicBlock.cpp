#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Graph.hpp>

dragon::BasicBlock::BasicBlock(const Graph *graph, std::uint64_t startAddress) :
    graph_(graph), startAddress_(startAddress), terminatorAddress_(0),
    valid_(false) {
  if (!graph_ || !graph_->isValidBlock(startAddress_))
    return;

  // Walk by exact address (graph_->get), never by std::map iteration order
  // from lower_bound(). Deliberately obfuscated code can decode two
  // genuinely different instructions starting at different offsets into
  // the same overlapping bytes (anti-disassembly trick - see
  // ToDo/Roadmap.md) - both legitimately exist in the graph's shared
  // listing, so "whatever the sorted iterator finds next" is not
  // necessarily part of *this* block's own decode path, and can silently
  // splice in an unrelated instruction (or trip a false "gap") from a
  // different, overlapping path entirely. Chasing addr -> addr+size
  // explicitly only ever follows the chain this specific block's own
  // instructions actually define.
  std::uint64_t cur = startAddress_;
  const Instruction *prevInsn = nullptr;

  while (true) {
    const Instruction *insn = graph_->get(cur);
    if (!insn)
      return; // no instruction at the address this path actually needs

    // Next block starts here (not the entry) - previous insn is the terminator.
    //
    // The Graph's block set is the authority, not just the instruction's
    // isBlockStart flag: the analyses register call targets and resolved
    // indirect-jump targets with Graph::addBlock() without flagging the
    // instruction already sitting there. Checking the flag alone let a block
    // run straight through the start of the next one - `call f` followed by
    // f's own body gave two blocks containing the same instructions.
    if (cur != startAddress_ &&
        (insn->isBlockStart() || graph_->isValidBlock(cur))) {
      if (!prevInsn)
        return;
      terminatorAddress_ = prevInsn->address();
      valid_ = true;
      return;
    }

    if (insn->isJump() || insn->isTrap() || insn->isReturn()) {
      terminatorAddress_ = insn->address();
      valid_ = true;
      return;
    }

    prevInsn = insn;
    cur = insn->address() + insn->size();
  }
}

std::vector<const dragon::Instruction *>
dragon::BasicBlock::instructions() const {
  std::vector<const Instruction *> result;
  if (!valid_)
    return result;

  // Same exact-address chase as the constructor - sorted iteration from
  // lower_bound(startAddress_) could splice in an unrelated instruction
  // from a different, overlapping decode path whose address happens to
  // fall between startAddress_ and terminatorAddress_ numerically.
  std::uint64_t cur = startAddress_;
  while (true) {
    const Instruction *insn = graph_->get(cur);
    if (!insn)
      break; // shouldn't happen when valid_ is true - defensive only
    result.push_back(insn);
    if (cur == terminatorAddress_)
      break;
    cur = insn->address() + insn->size();
  }
  return result;
}

const dragon::Instruction *
dragon::BasicBlock::get(std::uint64_t address) const {
  if (!valid_ || address < startAddress_ || address > terminatorAddress_)
    return nullptr;
  return graph_->get(address);
}

const dragon::Instruction *dragon::BasicBlock::terminator() const {
  if (!valid_)
    return nullptr;
  return graph_->get(terminatorAddress_);
}

static std::set<std::uint64_t>
computeSuccessors(const dragon::Instruction *term) {
  if (!term || term->isReturn() || term->isTrap())
    return {};
  if (term->isJump() && term->isConditional()) {
    auto succs = term->branches();
    succs.insert(term->address() + term->size());
    return succs;
  }
  // An unconditional jump goes only to its targets - none yet for an
  // unresolved indirect `jmp rax` / `br x0`. It used to fall through to the
  // next instruction like an ordinary one, inventing an edge into whatever
  // follows (padding, the next function).
  if (term->isJump())
    return term->branches();
  if (!term->branches().empty())
    return term->branches();
  return {term->address() + term->size()};
}

const std::set<std::uint64_t> &dragon::BasicBlock::successors() const {
  if (!successors_)
    successors_ = computeSuccessors(terminator());
  return *successors_;
}

const std::set<std::uint64_t> &dragon::BasicBlock::predecessors() const {
  if (!predecessors_) {
    std::set<std::uint64_t> result;
    if (valid_) {
      for (std::uint64_t addr : graph_->validBlockAddresses()) {
        if (addr == startAddress_)
          continue;
        BasicBlock bb{graph_, addr};
        if (bb.isValid() && bb.successors().contains(startAddress_))
          result.insert(addr);
      }
    }
    predecessors_ = std::move(result);
  }
  return *predecessors_;
}

void dragon::BasicBlock::clearCache() {
  successors_.reset();
  predecessors_.reset();
}

std::size_t dragon::BasicBlock::size() const { return instructions().size(); }

std::size_t dragon::BasicBlock::byteSize() const {
  if (!valid_)
    return 0;
  const Instruction *term = terminator();
  if (!term)
    return 0;
  return (term->address() + term->size()) - startAddress_;
}

bool dragon::BasicBlock::empty() const { return !valid_; }
