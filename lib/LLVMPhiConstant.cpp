#include "LLVMPhiConstant.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dragon::llvmlifter::detail {

PhiConstantResult PhiConstantGraph::get(llvm::PHINode *root,
                                       std::size_t availableWork) {
  PhiConstantResult result;
  const auto limit = std::min<std::size_t>(availableWork, 65536);
  auto spend = [&]() {
    if (result.work == limit) {
      result.budgetExhausted = true;
      result.message = "PHI constant graph work cap";
      return false;
    }
    ++result.work;
    return true;
  };
  if (!spend()) return result;
  if (!root || !root->getParent() || root->getFunction() != &function_) {
    result.message = "PHI root is outside the selected Function";
    return result;
  }
  auto refuse = [&](const char *message) {
    result.message = message;
    // A budget refusal must never leave even a negative partial cache entry.
    if (!result.budgetExhausted) refusedRoots_.emplace(root, result.message);
    return result;
  };
  if (auto found = proven_.find(root); found != proven_.end()) {
    result.value = found->second;
    result.message = hypotheses_ ? "cached conditional PHI graph under fixed LOAD hypotheses"
                                 : "cached complete PHI constant graph";
    return result;
  }
  if (auto found = refusedRoots_.find(root); found != refusedRoots_.end()) {
    result.message = found->second;
    return result;
  }
  if (!root->getType()->isIntegerTy() ||
      root->getType()->getIntegerBitWidth() > 64)
    return refuse("PHI root is not a scalar integer of at most 64 bits");

  struct Node {
    llvm::PHINode *phi;
    std::vector<std::size_t> dependencies;
    bool literalAnchor = false;
  };
  std::vector<Node> graph;
  std::map<llvm::PHINode *, std::size_t> indices;
  auto addNode = [&](llvm::PHINode *phi) {
    if (indices.contains(phi)) return true;
    if (!spend()) return false;
    if (graph.size() == 512) {
      result.budgetExhausted = true;
      result.message = "PHI constant graph node cap";
      return false;
    }
    indices.emplace(phi, graph.size());
    graph.push_back({phi, {}, false});
    result.nodes = graph.size();
    return true;
  };
  if (!addNode(root)) return result;
  llvm::ConstantInt *constant = nullptr;
  auto agree = [&](llvm::ConstantInt *value) {
    if (value->getType() != root->getType()) return false;
    if (!constant) constant = value;
    return constant->getValue() == value->getValue();
  };
  for (std::size_t n = 0; n < graph.size(); ++n) {
    if (!spend()) return result;
    auto *phi = graph[n].phi;
    if (phi->getFunction() != &function_ || phi->getType() != root->getType())
      return refuse("PHI dependency changes Function or integer type");

    // Count CFG successor SLOTS, not just predecessor blocks. A malformed
    // missing/extra incoming is refused even if all supplied values agree.
    std::map<llvm::BasicBlock *, std::size_t> remaining;
    for (auto *pred : llvm::predecessors(phi->getParent())) {
      if (!spend()) return result;
      ++remaining[pred];
    }
    if (remaining.empty()) return refuse("PHI has no actual defining predecessor");
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      if (!spend()) return result;
      auto *pred = phi->getIncomingBlock(i);
      auto slot = remaining.find(pred);
      if (!pred || pred->getParent() != &function_ || slot == remaining.end())
        return refuse("PHI incoming does not cover the actual predecessor slots");
      if (!--slot->second) remaining.erase(slot);
      auto *value = phi->getIncomingValue(i);
      if (auto *literal = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        if (!agree(literal)) return refuse("PHI literal constants disagree");
        graph[n].literalAnchor = true;
        continue;
      }
      if (hypotheses_) if (auto *load = llvm::dyn_cast<llvm::LoadInst>(value)) {
        if (!spend()) return result; // Exact-key hypothesis lookup/validation.
        auto hypothesis = hypotheses_->find(load);
        if (!load->getParent() || load->getFunction() != &function_ ||
            !load->isSimple() || load->hasPoisonGeneratingAnnotations() ||
            load->getType() != root->getType() || hypothesis == hypotheses_->end() ||
            !hypothesis->second || hypothesis->second->getType() != load->getType())
          return refuse("LOAD leaf lacks a valid exact-SSA integer hypothesis");
        if (!agree(hypothesis->second))
          return refuse("conditional LOAD hypothesis disagrees with graph constant");
        // This is an anchor only in this object's immutable H. It is not an
        // independent LOAD fact and is never installed into IR or another H.
        graph[n].literalAnchor = true;
        continue;
      }
      auto *dependency = llvm::dyn_cast<llvm::PHINode>(value);
      if (!dependency || dependency->getType() != root->getType() ||
          dependency->getFunction() != &function_)
        return refuse("PHI leaf is not a same-typed PHI or literal integer");
      if (!spend()) return result;
      if (auto found = proven_.find(dependency); found != proven_.end()) {
        if (!agree(found->second)) return refuse("cached PHI constant disagrees");
        graph[n].literalAnchor = true;
        continue;
      }
      if (refusedRoots_.contains(dependency))
        return refuse("PHI dependency has a complete prior refusal");
      if (!addNode(dependency)) return result;
      // addNode may reallocate graph; never retain a Node reference across it.
      graph[n].dependencies.push_back(indices.at(dependency));
    }
    if (!remaining.empty())
      return refuse("PHI omits an actual predecessor slot");
  }
  if (!constant) return refuse("PHI graph has no literal constant anchor");

  // Dependency edges point toward incoming values. Reverse reachability from
  // literal/cached-complete (or this object's validated conditional LOAD)
  // anchors requires EVERY collected node to have an
  // anchor path, including a sub-SCC not needed by the root's first arm.
  std::vector<std::vector<std::size_t>> reverse(graph.size());
  std::vector<bool> anchored(graph.size(), false);
  std::vector<std::size_t> pending;
  for (std::size_t n = 0; n < graph.size(); ++n) {
    if (!spend()) return result;
    for (auto dependency : graph[n].dependencies) {
      if (!spend()) return result;
      reverse[dependency].push_back(n);
    }
    if (graph[n].literalAnchor) {
      if (!spend()) return result;
      anchored[n] = true;
      pending.push_back(n);
    }
  }
  for (std::size_t n = 0; n < pending.size(); ++n) {
    if (!spend()) return result;
    for (auto parent : reverse[pending[n]]) {
      if (!spend()) return result;
      if (!anchored[parent]) {
        if (!spend()) return result;
        anchored[parent] = true;
        pending.push_back(parent);
      }
    }
  }
  for (bool reached : anchored) {
    if (!spend()) return result;
    if (!reached) return refuse("PHI dependency component has no constant anchor");
  }

  // Prepay the entire cache publication. Exhaustion here still publishes NONE.
  for (std::size_t n = 0; n < graph.size(); ++n)
    if (!spend()) return result;
  for (const auto &node : graph) proven_.emplace(node.phi, constant);
  result.value = constant;
  result.message = hypotheses_ ? "conditional PHI graph closes under fixed exact-SSA LOAD hypotheses"
                               : "complete anchored PHI graph has one literal constant";
  return result;
}

} // namespace dragon::llvmlifter::detail
