#include "LLVMMBASimplification.hpp"

#ifdef DRAGON_WITH_Z3
#include <z3++.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Transforms/Utils/Local.h>
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {
enum Kind { Input, Constant, Add, Sub, Mul, And, Or, Xor, Shl };
struct Node {
  Kind kind;
  unsigned left = 0, right = 0;
  llvm::Instruction *instruction = nullptr;
  llvm::APInt value;
  Node(Kind k, unsigned w, unsigned a = 0, unsigned b = 0)
      : kind(k), left(a), right(b), value(w, 0) {}
};
bool eligible(const llvm::Instruction *i) {
  switch (i->getOpcode()) {
    case llvm::Instruction::Add: case llvm::Instruction::Sub:
    case llvm::Instruction::Mul: case llvm::Instruction::Shl:
    case llvm::Instruction::And: case llvm::Instruction::Or:
    case llvm::Instruction::Xor: return true;
    default: return false;
  }
}
bool totalIntegerBoundary(llvm::Value *v) {
  if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(v)) return false;
  if (llvm::isa<llvm::Argument, llvm::PHINode, llvm::FreezeInst>(v)) return true;
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(v)) return !load->isAtomic() && !load->isVolatile();
  auto *i = llvm::dyn_cast<llvm::Instruction>(v);
  if (!i) return false;
  for (auto &op : i->operands())
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(op.get())) return false;
  if (auto *b = llvm::dyn_cast<llvm::BinaryOperator>(i)) {
    const auto opcode = i->getOpcode();
    if ((opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub ||
         opcode == llvm::Instruction::Mul || opcode == llvm::Instruction::Shl) &&
        (b->hasNoUnsignedWrap() || b->hasNoSignedWrap())) return false;
    if (i->isShift()) {
      auto *amount = llvm::dyn_cast<llvm::ConstantInt>(i->getOperand(1));
      if (!amount || amount->getValue().uge(i->getType()->getIntegerBitWidth())) return false;
      if (auto *exact = llvm::dyn_cast<llvm::PossiblyExactOperator>(i); exact && exact->isExact()) return false;
    }
  }
  return eligible(i) || i->getOpcode() == llvm::Instruction::LShr ||
      i->getOpcode() == llvm::Instruction::AShr || llvm::isa<llvm::ICmpInst, llvm::SelectInst>(i) ||
      i->getOpcode() == llvm::Instruction::ZExt || i->getOpcode() == llvm::Instruction::SExt ||
      i->getOpcode() == llvm::Instruction::Trunc;
}

// A region is linear in its SSA boundary values. Arithmetic below a bitwise
// operator, casts and non-uniform word masks form independent boundaries.
// The same boundary Value always has the same identity; different loads do not.
class Region {
public:
  Region(llvm::Instruction *root, unsigned limit, unsigned variables, std::uint64_t &work)
      : root_(root), limit_(limit), maxVariables_(variables), work_(work),
        width(root->getType()->getIntegerBitWidth()) {}
  std::optional<unsigned> parse(llvm::Value *v, bool bitwise = false, unsigned depth = 0) {
    if (auto it = cache_.find(v); it != cache_.end() && it->second[bitwise]) return it->second[bitwise];
    if (depth >= 64 || attempts_ >= limit_ || !work_) { limited = true; return {}; }
    --work_; ++attempts_;
    if (!v->getType()->isIntegerTy(width)) return {};
    auto remember = [&](unsigned index) -> std::optional<unsigned> {
      cache_[v][bitwise] = index; return index;
    };
    auto leaf = [&]() -> std::optional<unsigned> {
      if (v == root_ || !totalIntegerBoundary(v)) return {};
      if (auto it = inputs_.find(v); it != inputs_.end()) return remember(it->second);
      if (values.size() >= maxVariables_) { limited = true; return {}; }
      auto index = unsigned(nodes.size()); Node n{Input, width, unsigned(values.size())};
      nodes.push_back(std::move(n)); values.push_back(v); inputs_[v] = index;
      boundaries.insert(v); return remember(index);
    };
    if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(v)) {
      if (bitwise && !c->isZero() && !c->isMinusOne()) return {};
      auto index = unsigned(nodes.size()); Node n{Constant, width}; n.value = c->getValue();
      nodes.push_back(std::move(n)); return remember(index);
    }
    auto *i = llvm::dyn_cast<llvm::BinaryOperator>(v);
    if (!i || !eligible(i)) return leaf();
    if (!totalIntegerBoundary(i)) return {};
    const auto opcode = i->getOpcode();
    const bool bits = opcode == llvm::Instruction::And || opcode == llvm::Instruction::Or || opcode == llvm::Instruction::Xor;
    if ((bitwise && !bits) || (opcode == llvm::Instruction::Mul &&
        !llvm::isa<llvm::ConstantInt>(i->getOperand(0)) && !llvm::isa<llvm::ConstantInt>(i->getOperand(1)))) return leaf();
    auto left = parse(i->getOperand(0), bits, depth + 1);
    auto right = left ? parse(i->getOperand(1), bits, depth + 1) : std::nullopt;
    if (!left || !right) return bits && !limited ? leaf() : std::nullopt;
    const auto kind = opcode == llvm::Instruction::Add ? Add : opcode == llvm::Instruction::Sub ? Sub :
        opcode == llvm::Instruction::Mul ? Mul : opcode == llvm::Instruction::Shl ? Shl :
        opcode == llvm::Instruction::And ? And : opcode == llvm::Instruction::Or ? Or : Xor;
    auto index = unsigned(nodes.size()); nodes.emplace_back(kind, width, *left, *right);
    nodes.back().instruction = i;
    expanded.insert(i); arithmetic |= !bits; boolean |= bits; return remember(index);
  }
  void trim(unsigned root) {
    expanded.clear(); arithmetic = boolean = false;
    std::unordered_set<unsigned> seen;
    std::vector<unsigned> pending{root};
    while (!pending.empty()) {
      auto index = pending.back(); pending.pop_back();
      if (!seen.insert(index).second) continue;
      const auto &n = nodes[index];
      if (!n.instruction) continue;
      expanded.insert(n.instruction);
      const bool bits = n.kind == And || n.kind == Or || n.kind == Xor;
      boolean |= bits; arithmetic |= !bits;
      pending.push_back(n.left); pending.push_back(n.right);
    }
  }
  unsigned removable() const {
    std::unordered_map<llvm::Instruction *, unsigned> uses;
    std::vector<llvm::Instruction *> dead{root_};
    for (unsigned j = 0; j < dead.size(); ++j) for (auto &op : dead[j]->operands()) {
      auto *i = llvm::dyn_cast<llvm::Instruction>(op.get());
      if (!i || !expanded.contains(i) || boundaries.contains(i)) continue;
      auto [it, inserted] = uses.try_emplace(i, i->getNumUses());
      if (--it->second == 0) dead.push_back(i);
    }
    return unsigned(dead.size());
  }
  llvm::APInt evaluate(unsigned root, unsigned assignment) const {
    std::vector<llvm::APInt> results; results.reserve(nodes.size());
    for (const auto &n : nodes) {
      if (n.kind == Input) { results.emplace_back(width, (assignment >> n.left) & 1); continue; }
      if (n.kind == Constant) { results.push_back(n.value); continue; }
      const auto &a = results[n.left], &b = results[n.right];
      switch (n.kind) {
        case Add: results.push_back(a + b); break;
        case Sub: results.push_back(a - b); break;
        case Mul: results.push_back(a * b); break;
        case And: results.push_back(a & b); break;
        case Or: results.push_back(a | b); break;
        case Xor: results.push_back(a ^ b); break;
        default: results.push_back(a.shl(unsigned(b.getZExtValue()))); break;
      }
    }
    return results[root];
  }
  unsigned width;
  bool limited = false, arithmetic = false, boolean = false;
  std::vector<Node> nodes;
  std::vector<llvm::Value *> values;
  std::unordered_set<llvm::Instruction *> expanded;
  std::unordered_set<llvm::Value *> boundaries;
private:
  llvm::Instruction *root_;
  unsigned limit_, maxVariables_, attempts_ = 0;
  std::uint64_t &work_;
  std::unordered_map<llvm::Value *, std::array<std::optional<unsigned>, 2>> cache_;
  std::unordered_map<llvm::Value *, unsigned> inputs_;
};

struct Plan {
  Node node;
  std::shared_ptr<Plan> left, right;
  unsigned cost = 0;
  explicit Plan(Node n) : node(std::move(n)) {}
};
using P = std::shared_ptr<Plan>;
P constant(const llvm::APInt &c) { auto p = std::make_shared<Plan>(Node{Constant, c.getBitWidth()}); p->node.value = c; return p; }
P input(unsigned width, unsigned index) { return std::make_shared<Plan>(Node{Input, width, index}); }
P binary(Kind k, P a, P b) {
  auto p = std::make_shared<Plan>(Node{k, a->node.value.getBitWidth()});
  p->cost = a->cost + b->cost + 1; p->left = std::move(a); p->right = std::move(b); return p;
}
P scale(P p, const llvm::APInt &c) { return c.isOne() ? p : binary(Mul, std::move(p), constant(c)); }
struct Boolean { int cost = -1; Kind kind = Input; unsigned left = 0, right = 0; };
using TruthTable = std::array<Boolean, 256>;
TruthTable booleanTable(unsigned variables) {
  TruthTable table; const unsigned all = (1u << (1u << variables)) - 1;
  table[0] = {0, Constant, 0}; table[all] = {0, Constant, 1};
  for (unsigned v = 0; v < variables; ++v) {
    unsigned mask = 0; for (unsigned a = 0; a < (1u << variables); ++a) if (a & (1u << v)) mask |= 1u << a;
    table[mask] = {0, Input, v};
  }
  for (int cost = 1; cost <= 8; ++cost) {
    for (unsigned a = 0; a <= all; ++a) if (table[a].cost >= 0) {
      for (unsigned b = 0; b <= all; ++b) if (table[b].cost >= 0 && table[a].cost + table[b].cost + 1 == cost) {
        for (auto k : {And, Or, Xor}) {
          unsigned m = k == And ? a & b : k == Or ? a | b : a ^ b;
          if (table[m].cost < 0) table[m] = {cost, k, a, b};
        }
      }
    }
  }
  return table;
}
P booleanPlan(unsigned mask, unsigned width, const TruthTable &table) {
  const auto &n = table[mask];
  if (n.kind == Input) return input(width, n.left);
  if (n.kind == Constant) return constant(n.left ? llvm::APInt::getAllOnes(width) : llvm::APInt(width, 0));
  return binary(n.kind, booleanPlan(n.left, width, table), booleanPlan(n.right, width, table));
}
P candidate(const Region &r, unsigned root) {
  std::vector<llvm::APInt> signature;
  for (unsigned a = 0; a < (1u << r.values.size()); ++a) signature.push_back(r.evaluate(root, a));
  auto coefficients = signature;
  for (unsigned bit = 0; bit < r.values.size(); ++bit)
    for (unsigned a = 0; a < coefficients.size(); ++a) if (a & (1u << bit)) coefficients[a] -= coefficients[a ^ (1u << bit)];
  P result;
  if (!coefficients[0].isZero()) result = constant(coefficients[0]);
  for (unsigned a = 1; a < coefficients.size(); ++a) if (!coefficients[a].isZero()) {
    P term;
    for (unsigned v = 0; v < r.values.size(); ++v) if (a & (1u << v))
      term = term ? binary(And, term, input(r.width, v)) : input(r.width, v);
    term = scale(term, coefficients[a]); result = result ? binary(Add, result, term) : term;
  }
  if (!result) result = constant(llvm::APInt(r.width, 0));
  // Refine a shifted/scaled Boolean function. Its truth mask has bit zero
  // clear, so evaluating it on zero word inputs produces zero in every bit.
  std::optional<llvm::APInt> factor; unsigned mask = 0; bool single = true;
  for (unsigned a = 1; a < signature.size(); ++a) {
    auto delta = signature[a] - signature[0]; if (delta.isZero()) continue;
    if (factor && delta != *factor) { single = false; break; }
    factor = delta; mask |= 1u << a;
  }
  if (single && factor) {
    static const std::array<TruthTable, 3> tables{booleanTable(1), booleanTable(2), booleanTable(3)};
    auto &table = tables[r.values.size() - 1];
    if (table[mask].cost >= 0) {
      auto refined = scale(booleanPlan(mask, r.width, table), *factor);
      if (!signature[0].isZero()) refined = binary(Add, constant(signature[0]), refined);
      if (refined->cost < result->cost) result = refined;
    }
  }
  return result;
}
z3::expr bv(z3::context &c, const llvm::APInt &value) {
  llvm::SmallString<48> digits; value.toStringUnsigned(digits);
  return c.bv_val(digits.c_str(), value.getBitWidth());
}
z3::expr operation(Kind k, const z3::expr &a, const z3::expr &b) {
  switch (k) {
    case Add: return a + b; case Sub: return a - b; case Mul: return a * b;
    case And: return a & b; case Or: return a | b; case Xor: return a ^ b;
    default: return z3::shl(a, b);
  }
}
z3::expr lowerPlan(const P &p, z3::context &c, const std::vector<z3::expr> &inputs) {
  if (p->node.kind == Constant) return bv(c, p->node.value);
  if (p->node.kind == Input) return inputs[p->node.left];
  return operation(p->node.kind, lowerPlan(p->left, c, inputs), lowerPlan(p->right, c, inputs));
}
llvm::Value *emit(const P &p, llvm::IRBuilder<> &b, const std::vector<llvm::Value *> &inputs) {
  if (p->node.kind == Input) return inputs[p->node.left];
  if (p->node.kind == Constant) return llvm::ConstantInt::get(b.getContext(), p->node.value);
  auto *a = emit(p->left, b, inputs), *v = emit(p->right, b, inputs);
  switch (p->node.kind) {
    case Add: return b.CreateAdd(a, v); case Sub: return b.CreateSub(a, v); case Mul: return b.CreateMul(a, v);
    case And: return b.CreateAnd(a, v); case Or: return b.CreateOr(a, v); default: return b.CreateXor(a, v);
  }
}
} // namespace

MBASimplifyStats simplifyMBA(llvm::Module &module, const MBASimplifyOptions &options) {
  MBASimplifyStats stats; stats.available = true;
  if (!options.timeoutMs || !options.maxQueries || !options.maxExpressionNodes || !options.maxVariables) return stats;
  const unsigned limit = std::min(options.maxExpressionNodes, 1024u);
  std::uint64_t work = std::min<std::uint64_t>(std::uint64_t{options.maxQueries} * limit * 16, 1048576);
  z3::context context;
  std::vector<llvm::WeakTrackingVH> roots;
  for (auto &f : module) for (auto &i : llvm::instructions(f))
    if (eligible(&i) && i.getType()->isIntegerTy() && i.getType()->getIntegerBitWidth() <= 128) roots.emplace_back(&i);
  for (auto &handle : roots) {
    auto *root = llvm::dyn_cast_or_null<llvm::Instruction>(static_cast<llvm::Value *>(handle));
    if (!root || !eligible(root)) continue;
    ++stats.expressionsVisited;
    if (!work || stats.solverQueries >= options.maxQueries) { ++stats.budgetSkipped; continue; }
    Region region{root, limit, std::min(options.maxVariables, 3u), work};
    auto index = region.parse(root);
    if (!index) { if (region.limited) ++stats.budgetSkipped; continue; }
    region.trim(*index);
    if (!region.arithmetic || !region.boolean || region.expanded.size() < 3) continue;
    auto plan = candidate(region, *index);
    if (plan->cost >= region.expanded.size() || plan->cost > region.removable()) continue;
    ++stats.candidates;
    try {
      std::vector<z3::expr> inputs, expressions;
      for (unsigned j = 0; j < region.values.size(); ++j)
        inputs.push_back(context.bv_const(("mba_" + std::to_string(j)).c_str(), region.width));
      for (const auto &n : region.nodes) {
        if (n.kind == Input) expressions.push_back(inputs[n.left]);
        else if (n.kind == Constant) expressions.push_back(bv(context, n.value));
        else expressions.push_back(operation(n.kind, expressions[n.left], expressions[n.right]));
      }
      z3::solver solver{context}; solver.set("timeout", options.timeoutMs); solver.set("rlimit", options.resourceLimit);
      solver.add(expressions[*index] != lowerPlan(plan, context, inputs)); ++stats.solverQueries;
      const auto proof = solver.check();
      if (proof != z3::unsat) {
        if (proof == z3::unknown) ++stats.unknownQueries; else ++stats.rejectedCandidates;
        continue;
      }
      llvm::IRBuilder<> builder{root}; auto *replacement = emit(plan, builder, region.values);
      root->replaceAllUsesWith(replacement);
      llvm::RecursivelyDeleteTriviallyDeadInstructions(root);
      ++stats.expressionsSimplified;
    } catch (const z3::exception &) { ++stats.unknownQueries; }
  }
  return stats;
}
} // namespace dragon::llvmlifter::detail
#else
namespace dragon::llvmlifter::detail {
MBASimplifyStats simplifyMBA(llvm::Module &, const MBASimplifyOptions &) { return {}; }
}
#endif
