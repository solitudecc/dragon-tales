#include "LLVMPhiLowBits.hpp"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

using namespace llvm;
namespace dragon::llvmlifter::detail {
namespace {
bool integer(Value *v) {
  return v && v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() <= 128;
}
bool protectedInstruction(Instruction &i) {
  return i.isTerminator() || isa<CallBase, AllocaInst>(i) ||
         i.mayReadOrWriteMemory() || i.mayHaveSideEffects();
}
struct Work {
  const PhiLowBitsOptions &options;
  PhiLowBitsStats &stats;
  bool spend(std::size_t n = 1) {
    if (n > options.maxWork - stats.work) {
      stats.budgetExhausted = true; stats.message = "phi projection shared work exhausted"; return false;
    }
    stats.work += n; return true;
  }
  bool node(unsigned depth) {
    if (depth > options.maxDepth || stats.nodes == options.maxNodes) {
      stats.budgetExhausted = true; stats.message = "phi projection shared node or depth cap"; return false;
    }
    ++stats.nodes; return true;
  }
};
using Predecessors = std::map<BasicBlock *, std::vector<BasicBlock *>>;
// Duplicated predecessor blocks are actual distinct successor slots. LLVM
// requires identical incoming values for such slots; never silently dedupe a
// missing arm or select a convenient value from conflicting duplicates.
bool inputs(PHINode &phi, const Predecessors &preds, Work &work,
            std::map<BasicBlock *, Value *> &values) {
  auto found = preds.find(phi.getParent());
  if (found == preds.end() || found->second.empty() ||
      phi.getNumIncomingValues() != found->second.size()) return false;
  std::map<BasicBlock *, unsigned> expected, actual;
  for (auto *from : found->second) { if (!work.spend()) return false; ++expected[from]; }
  for (unsigned n = 0; n != phi.getNumIncomingValues(); ++n) {
    if (!work.spend()) return false;
    auto *from = phi.getIncomingBlock(n); auto *value = phi.getIncomingValue(n);
    ++actual[from];
    if (auto old = values.find(from); old != values.end() && old->second != value) return false;
    values[from] = value;
  }
  return expected == actual;
}
struct Effect {
  Instruction *instruction;
  BasicBlock *parent;
  unsigned opcode;
  Type *type;
  std::vector<Value *> operands;
  SmallVector<std::pair<unsigned, MDNode *>, 4> metadata;
};
class Proof {
  Function &function;
  DominatorTree &dom;
  const Predecessors &preds;
  Work &work;
  unsigned width;
  using Pair = std::pair<Value *, Value *>;
  std::map<Pair, unsigned> relations;
  std::map<Value *, unsigned> defined;
  std::map<Value *, std::vector<Value *>> graph;
  bool inFunction(Value *v) {
    if (auto *i = dyn_cast<Instruction>(v))
      return i->getFunction() == &function && dom.isReachableFromEntry(i->getParent());
    return isa<ConstantInt>(v);
  }
  Value *strip(Value *v) {
    if (auto *cast = dyn_cast<CastInst>(v)) {
      if (!cast->hasPoisonGeneratingFlags() && integer(cast->getOperand(0)) &&
          cast->getOperand(0)->getType()->getIntegerBitWidth() >= width &&
          (isa<TruncInst, ZExtInst, SExtInst>(cast))) return cast->getOperand(0);
    }
    if (auto *phi = dyn_cast<PHINode>(v); phi && phi->getNumIncomingValues() == 1) {
      std::map<BasicBlock *, Value *> incoming;
      if (!inputs(*phi, preds, work, incoming)) return nullptr;
      auto *value = phi->getIncomingValue(0);
      if (value == phi) return nullptr;
      // A single-arm forwarding PHI is an identity only when its input is
      // available at that PHI itself, not merely at a historic predecessor.
      if (auto *i = dyn_cast<Instruction>(value); i && !dom.dominates(i, phi)) return nullptr;
      return value;
    }
    return v;
  }
  bool relation(Value *a, Value *b, unsigned depth) {
    if (!work.spend() || !integer(a) || !integer(b) || !inFunction(a) || !inFunction(b) ||
        a->getType()->getIntegerBitWidth() < width || b->getType()->getIntegerBitWidth() < width) return false;
    Pair key{a,b};
    if (relations.contains(key)) return true; // gray is pending, never exported
    if (!work.node(depth)) return false;
    relations[key] = 1;
    if (a == b) { relations[key] = 2; return true; }
    auto *left = strip(a); auto *right = strip(b);
    if (!left || !right) return false;
    if (left != a || right != b) {
      if (!relation(left,right,depth+1)) return false;
      relations[key] = 2; return true;
    }
    if (auto *x = dyn_cast<ConstantInt>(a)) {
      auto *y = dyn_cast<ConstantInt>(b);
      if (!y || x->getValue().trunc(width) != y->getValue().trunc(width)) return false;
      relations[key] = 2; return true;
    }
    auto *x = dyn_cast<PHINode>(a); auto *y = dyn_cast<PHINode>(b);
    if (!x || !y || x->getParent() != y->getParent()) return false;
    std::map<BasicBlock *, Value *> xs,ys;
    if (!inputs(*x,preds,work,xs) || !inputs(*y,preds,work,ys)) return false;
    for (auto *from : preds.at(x->getParent()))
      if (!relation(xs.at(from),ys.at(from),depth+1)) return false;
    relations[key] = 2; return true;
  }
  bool definition(Value *v, unsigned depth) {
    if (!work.spend() || !integer(v) || !inFunction(v)) return false;
    if (defined.contains(v)) return true; // all pending cycles checked below
    if (!work.node(depth)) return false;
    defined[v] = 1;
    auto &children = graph[v];
    if (isa<ConstantInt, FreezeInst>(v)) { defined[v] = 2; return true; }
    auto *i = dyn_cast<Instruction>(v);
    if (!i || i->hasPoisonGeneratingFlags()) return false;
    if (auto *phi = dyn_cast<PHINode>(i)) {
      std::map<BasicBlock *, Value *> incoming;
      if (!inputs(*phi,preds,work,incoming)) return false;
      for (auto *from : preds.at(phi->getParent())) children.push_back(incoming.at(from));
    } else if (isa<TruncInst,ZExtInst,SExtInst>(i)) children.push_back(i->getOperand(0));
    else if (auto *binary = dyn_cast<BinaryOperator>(i)) {
      switch (binary->getOpcode()) {
      case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
      case Instruction::And: case Instruction::Or: case Instruction::Xor:
        children.push_back(binary->getOperand(0)); children.push_back(binary->getOperand(1)); break;
      default: return false;
      }
    } else return false;
    for (auto *child : children) if (!definition(child,depth+1)) return false;
    defined[v] = 2; return true;
  }
  bool realSeeds() {
    std::map<Value *, unsigned> index,low,component;
    std::set<Value *> onStack;
    std::vector<Value *> stack;
    std::vector<std::vector<Value *>> components;
    unsigned next = 1; bool okay = true;
    std::function<void(Value *)> visit = [&](Value *v) {
      if (!okay || !work.spend()) { okay = false; return; }
      index[v] = low[v] = next++; stack.push_back(v); onStack.insert(v);
      for (auto *child : graph.at(v)) {
        if (!work.spend()) { okay = false; return; }
        if (!index.contains(child)) { visit(child); if (!okay) return; low[v] = std::min(low[v],low[child]); }
        else if (onStack.contains(child)) low[v] = std::min(low[v],index[child]);
      }
      if (low[v] != index[v]) return;
      std::vector<Value *> members;
      for (;;) {
        auto *value = stack.back(); stack.pop_back(); onStack.erase(value);
        component[value] = components.size(); members.push_back(value);
        if (value == v) break;
      }
      components.push_back(std::move(members));
    };
    for (const auto &[v,children] : graph) if (!index.contains(v)) { visit(v); if (!okay) return false; }
    for (unsigned n=0;n!=components.size();++n) {
      auto &members = components[n]; bool cyclic = members.size()>1, seed = false;
      for (auto *v : members) for (auto *child : graph.at(v)) {
        if (!work.spend()) return false;
        cyclic |= child == v;
        // A literal increment operand is NOT an initial value for a cycle.
        // Only a fully checked incoming of an actual PHI can seed it.
        if (isa<PHINode>(v) && component.at(child) != n) seed = true;
      }
      if (cyclic && !seed) return false;
    }
    return true;
  }
public:
  Proof(Function &f,DominatorTree &d,const Predecessors &p,Work &w,unsigned bits)
      : function(f),dom(d),preds(p),work(w),width(bits) {}
  PHINode *root(Value *v) {
    PHINode *last = nullptr; std::set<Value *> seen;
    for (unsigned depth=0;seen.insert(v).second;++depth) {
      if (!work.spend()) return nullptr;
      if (depth>work.options.maxDepth) {
        work.stats.budgetExhausted=true;work.stats.message="phi projection root depth cap";return nullptr;
      }
      if (!integer(v) || !inFunction(v)) return nullptr;
      if (auto *phi=dyn_cast<PHINode>(v)) last=phi;
      auto *next=strip(v);
      if (!next) return nullptr;
      if (next==v) return last;
      v=next;
    }
    return nullptr;
  }
  bool prove(TruncInst &trunc,PHINode &small) {
    return relation(trunc.getOperand(0),&small,0) && definition(&trunc,0) &&
           definition(&small,0) && realSeeds();
  }
};
} // namespace

PhiLowBitsStats replacePhiLowTruncs(Function &f,const std::vector<TruncInst *> &requested,
                                  const PhiLowBitsOptions &options) {
  PhiLowBitsStats stats; Work work{options,stats};
  auto fail = [&](const char *reason) {
    stats.complete=false;stats.changed=false;stats.rewrittenTruncs=stats.rewrittenUses=0;
    stats.provedLowPhis.clear();
    if (!stats.budgetExhausted) stats.message=reason;
    return stats;
  };
  if (!f.getParent() || f.isDeclaration() || !options.maxCandidates || options.maxCandidates>128 ||
      !options.maxNodes || options.maxNodes>256 || !options.maxDepth || options.maxDepth>32 ||
      !options.maxWork || options.maxWork>65536) return fail("invalid phi projection request or caps");
  if (requested.empty()) return stats;
  if (requested.size()>options.maxCandidates) {stats.budgetExhausted=true;stats.message="phi projection candidate cap";return fail("");}
  std::set<TruncInst *> unique;
  for (auto *trunc : requested)
    if (!trunc || trunc->getFunction()!=&f || !unique.insert(trunc).second ||
        !integer(trunc) || !integer(trunc->getOperand(0)) || trunc->hasPoisonGeneratingFlags())
      return fail("invalid duplicate or unsafe original Trunc candidate");
  Predecessors preds; std::vector<Effect> effects;
  for (auto &block : f) {
    auto *term=block.getTerminator();if (!term) return fail("unterminated input function");
    if (!work.spend(1+term->getNumSuccessors())) return fail("");
    for (auto *next : successors(&block)) preds[next].push_back(&block);
    for (auto &i : block) {
      if (!work.spend()) return fail("");
      if (!protectedInstruction(i)) continue;
      Effect saved{&i,&block,i.getOpcode(),i.getType(),{}, {}};
      for (auto &operand : i.operands()) {if (!work.spend()) return fail("");saved.operands.push_back(operand);}
      i.getAllMetadata(saved.metadata);effects.push_back(std::move(saved));
    }
  }
  if (verifyFunction(f)) return fail("invalid original function");
  DominatorTree dom(f);
  struct Plan {TruncInst *trunc;PHINode *small;std::vector<Use *> uses;};
  std::vector<Plan> plans;
  for (auto *trunc : requested) {
    ++stats.candidatesVisited;
    if (!dom.isReachableFromEntry(trunc->getParent())) return fail("unreachable original Trunc");
    const unsigned width=trunc->getType()->getIntegerBitWidth();
    Proof finder(f,dom,preds,work,width);auto *large=finder.root(trunc->getOperand(0));
    if (!large) return fail("original operand has no supported PHI root");
    PHINode *chosen=nullptr;
    for (auto &small : large->getParent()->phis()) {
      if (!work.spend()) return fail("");
      if (!small.getType()->isIntegerTy(width) || !dom.dominates(&small,trunc)) continue;
      ++stats.pairsVisited;
      Proof proof(f,dom,preds,work,width); // no partial proof cache escapes a pair
      if (proof.prove(*trunc,small)) {chosen=&small;break;}
      if (stats.budgetExhausted) return fail("");
    }
    if (!chosen) return fail("no complete defined original PHI projection");
    Plan plan{trunc,chosen,{}};
    for (auto &use : trunc->uses()) {
      if (!work.spend()) return fail("");
      auto *user=dyn_cast<Instruction>(use.getUser());
      if (!user || user->getFunction()!=&f || protectedInstruction(*user) || !dom.dominates(chosen,use))
        return fail("projection use crosses an effect cut or lacks dominance");
      plan.uses.push_back(&use);
    }
    plans.push_back(std::move(plan));
  }
  // Prepay postchecks before changing any original use. No CFG/instruction
  // mutation or RAUW of metadata is performed; original Truncs stay alive.
  std::size_t postWork=0;
  for (const auto &effect : effects) postWork+=1+effect.operands.size();
  for (const auto &plan : plans) postWork+=plan.uses.size();
  if (!work.spend(postWork)) return fail("");
  for (const auto &plan : plans) for (auto *use : plan.uses) use->set(plan.small);
  bool okay=!verifyFunction(f);
  for (const auto &saved : effects) {
    auto *i=saved.instruction;
    okay &= i->getParent()==saved.parent && i->getOpcode()==saved.opcode &&
            i->getType()==saved.type && i->getNumOperands()==saved.operands.size();
    for (unsigned n=0;n!=saved.operands.size();++n) okay &= i->getOperand(n)==saved.operands[n];
    SmallVector<std::pair<unsigned,MDNode *>,4> metadata;i->getAllMetadata(metadata);
    okay &= metadata==saved.metadata;
  }
  if (!okay) {
    for (const auto &plan : plans) for (auto *use : plan.uses) use->set(plan.trunc);
    return fail("projection verification or effect identity failed; original uses restored");
  }
  std::set<PHINode *> exported;
  for (const auto &plan : plans) {
    if (exported.insert(plan.small).second) stats.provedLowPhis.push_back(plan.small);
    if (!plan.uses.empty()) {++stats.rewrittenTruncs;stats.rewrittenUses+=plan.uses.size();}
  }
  stats.changed=stats.rewrittenUses!=0;
  stats.message="defined OriginalIR same-block PHI low-bit uses projected";
  return stats;
}
} // namespace dragon::llvmlifter::detail
