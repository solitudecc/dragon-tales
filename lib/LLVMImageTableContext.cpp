#include "LLVMImageTableContext.hpp"
#include <dragon/lifters/llvm/Execution.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <optional>
#include <set>

using namespace llvm;
namespace dragon::llvmlifter::detail {
namespace {
struct Work {
  ImageTableContextStats &stats;
  std::size_t limit;
  bool spend(std::size_t n = 1) {
    if (n > limit - stats.work) {
      stats.budgetExhausted = true;
      stats.message = "shared image-table proof work exhausted";
      return false;
    }
    stats.work += n;
    return true;
  }
};
bool fail(ImageTableContextStats &s, const Twine &why) {
  s.message = why.str();
  return false;
}
std::optional<std::uint64_t> offset(Value *pointer, Value *base) {
  if (pointer == base) return 0;
  auto *gep = dyn_cast<GetElementPtrInst>(pointer);
  if (!gep || gep->getPointerOperand() != base || gep->getNumIndices() != 1 ||
      !gep->getSourceElementType()->isIntegerTy(8)) return {};
  auto *c = dyn_cast<ConstantInt>(gep->idx_begin()->get());
  if (!c || c->getValue().getActiveBits() > 64) return {};
  return c->getZExtValue();
}
bool fallbackBlock(BasicBlock *block, Function &view) {
  if (block->size() != 2 || !isa<ReturnInst>(block->getTerminator())) return false;
  auto *call = dyn_cast<CallInst>(&block->front());
  if (!call || !call->getCalledFunction() || call->getCalledFunction() == &view ||
      call->arg_size() != 4 || call->getFunctionType() != view.getFunctionType() ||
      call->getNumOperandBundles() || call->isConvergent()) return false;
  for (unsigned i = 0; i != 4; ++i)
    if (call->getArgOperand(i) != view.getArg(i)) return false;
  return true;
}
struct Guards {
  struct RegisterFact {
    WeakTrackingVH observation;
    ConstantInt *expected = nullptr;
  };
  BasicBlock *accepted = nullptr;
  BasicBlock *fallback = nullptr;
  Value *guest = nullptr;
  LoadInst *initialSP = nullptr;
  AllocaInst *spSlot = nullptr;
  std::uint64_t sp = 0;
  std::set<BasicBlock *> chain;
  // Facts constrain the already executed SSA observation, not the register
  // address or a future reload. Value handles follow only LLVM's equivalent
  // replacements during the private normalization step.
  std::vector<RegisterFact> registers;
};
bool guardFacts(Function &function, const ImageTableContextRequest &request,
                Guards &g, Work &work) {
  auto &stats = work.stats;
  if (function.isDeclaration() || function.arg_size() != 4 ||
      function.isVarArg() || !function.getReturnType()->isVoidTy() ||
      function.hasPersonalityFn() || function.hasFnAttribute(Attribute::Convergent) ||
      function.hasFnAttribute(Attribute::NoDuplicate))
    return fail(stats, "unsupported view function");
  for (auto &a : function.args())
    if (!a.getType()->isPointerTy() || a.getType()->getPointerAddressSpace())
      return fail(stats, "view does not use the descriptor ABI");
  auto &entry = function.getEntryBlock();
  auto *entrySwitch = dyn_cast<SwitchInst>(entry.getTerminator());
  if (!entrySwitch || entrySwitch->getNumCases() != 1 ||
      !entrySwitch->getCondition()->getType()->isIntegerTy(64))
    return fail(stats, "view must retain its raw single-case entry switch");
  Value *registers = nullptr;
  for (auto &i : entry) {
    if (!work.spend()) return false;
    if (auto *store = dyn_cast<StoreInst>(&i)) {
      auto *slot = dyn_cast<AllocaInst>(store->getPointerOperand());
      if (!store->isSimple() || !slot || slot->getParent() != &entry)
        return fail(stats, "noncanonical entry side effect");
    } else if (i.mayHaveSideEffects() && !i.isTerminator())
      return fail(stats, "noncanonical entry effect");
    auto *load = dyn_cast<LoadInst>(&i);
    if (!load || !load->isSimple() || !load->getType()->isPointerTy()) continue;
    if (load->getPointerOperand() == function.getArg(1)) {
      if (g.guest) return fail(stats, "ambiguous Guest base");
      g.guest = load;
    }
    auto at = offset(load->getPointerOperand(), function.getArg(0));
    if (at && *at == offsetof(dragon_guest_state_v2_t, regs_bytes)) {
      if (registers) return fail(stats, "ambiguous register base");
      registers = load;
    }
  }
  if (!g.guest || !registers) return fail(stats, "missing descriptor base");
  for (auto &i : entry) if (auto *load = dyn_cast<LoadInst>(&i)) {
    auto at = offset(load->getPointerOperand(), registers);
    if (!load->isSimple() || !load->getType()->isIntegerTy(64) || !at || *at != 32)
      continue;
    if (g.initialSP) return fail(stats, "ambiguous initial SP snapshot");
    g.initialSP = load;
  }
  if (!g.initialSP) return fail(stats, "missing initial SP snapshot");
  for (auto &i : entry) if (auto *store = dyn_cast<StoreInst>(&i)) {
    if (store->getValueOperand() != g.initialSP) continue;
    auto *slot = dyn_cast<AllocaInst>(store->getPointerOperand());
    if (!slot || !slot->getAllocatedType()->isIntegerTy(64) ||
        !isAllocaPromotable(slot) || g.spSlot)
      return fail(stats, "SP snapshot is not one private promotable slot");
    g.spSlot = slot;
  }
  if (!g.spSlot) return fail(stats, "missing raw SP register slot");

  std::vector<std::optional<std::uint8_t>> guarded(request.targets.size() * 8);
  bool hasSP = false;
  BasicBlock *previous = &entry;
  auto *block = entrySwitch->case_begin()->getCaseSuccessor();
  while (true) {
    if (!work.spend() || !g.chain.insert(block).second)
      return fail(stats, "cyclic or exhausted entry checks");
    if (block->getSinglePredecessor() != previous)
      return fail(stats, "entry facts have another incoming path");
    auto *branch = dyn_cast<BranchInst>(block->getTerminator());
    if (!branch) return fail(stats, "unsupported entry check terminator");
    for (auto &i : *block) {
      if (!work.spend()) return false;
      if (i.mayHaveSideEffects() && !i.isTerminator())
        return fail(stats, "entry checks have side effects");
    }
    if (branch->isUnconditional()) {
      if (block->size() != 1 || !hasSP ||
          std::any_of(guarded.begin(), guarded.end(), [](auto x) { return !x; }))
        return fail(stats, "incomplete executable SP or table checks");
      g.accepted = block;
      stats.guardedBytes = guarded.size();
      return true;
    }
    if (!g.fallback) {
      for (unsigned n = 0; n != 2; ++n)
        if (fallbackBlock(branch->getSuccessor(n), function)) {
          if (g.fallback) return fail(stats, "ambiguous entry fallback");
          g.fallback = branch->getSuccessor(n);
        }
    }
    if (!g.fallback) return fail(stats, "missing ordinary entry fallback");
    unsigned success;
    if (branch->getSuccessor(1) == g.fallback) success = 0;
    else if (branch->getSuccessor(0) == g.fallback) success = 1;
    else return fail(stats, "entry check failure does not use ordinary fallback");
    auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
    if (cmp && ((cmp->getPredicate() == ICmpInst::ICMP_EQ && success == 0) ||
                (cmp->getPredicate() == ICmpInst::ICMP_NE && success == 1))) {
      auto *constant = dyn_cast<ConstantInt>(cmp->getOperand(1));
      auto *actual = cmp->getOperand(0);
      if (!constant) {
        constant = dyn_cast<ConstantInt>(cmp->getOperand(0));
        actual = cmp->getOperand(1);
      }
      if (constant && actual == g.initialSP && constant->getBitWidth() == 64) {
        if (hasSP && g.sp != constant->getZExtValue())
          return fail(stats, "conflicting SP checks");
        hasSP = true;
        g.sp = constant->getZExtValue();
      }
      auto *load = dyn_cast<LoadInst>(actual);
      if (constant && load && load != g.initialSP && load->isSimple() &&
          load->getParent() == &entry && load->getType()->isIntegerTy() &&
          constant->getBitWidth() % 8 == 0 && constant->getBitWidth() <= 64 &&
          offset(load->getPointerOperand(), registers)) {
        if (!work.spend()) return false;
        bool found = false;
        for (const auto &fact : g.registers) {
          if (!work.spend()) return false;
          if (fact.observation != load) continue;
          found = true;
          if (fact.expected->getValue() != constant->getValue())
            return fail(stats, "conflicting initial register checks");
        }
        if (!found) g.registers.push_back({load, constant});
      }
      if (constant && load && load->isSimple() && load->getType()->isIntegerTy() &&
          constant->getBitWidth() % 8 == 0 && constant->getBitWidth() <= 64) {
        auto at = offset(load->getPointerOperand(), g.guest);
        const auto bytes = constant->getBitWidth() / 8;
        if (at && *at >= request.tableOffset &&
            *at - request.tableOffset <= guarded.size() &&
            bytes <= guarded.size() - (*at - request.tableOffset)) {
          for (unsigned n = 0; n != bytes; ++n) {
            if (!work.spend()) return false;
            const auto position = *at - request.tableOffset + n;
            auto byte = static_cast<std::uint8_t>(constant->getZExtValue() >> (n * 8));
            auto expected = static_cast<std::uint8_t>(request.targets[position / 8] >> ((position % 8) * 8));
            if (byte != expected || (guarded[position] && *guarded[position] != byte))
              return fail(stats, "table guard bytes disagree with candidate");
            guarded[position] = byte;
          }
        }
      }
    }
    previous = block;
    block = branch->getSuccessor(success);
  }
}
void instCombine(Function &f) {
  LoopAnalysisManager loops;
  FunctionAnalysisManager functions;
  CGSCCAnalysisManager cgscc;
  ModuleAnalysisManager modules;
  PassBuilder passes;
  passes.registerLoopAnalyses(loops);
  passes.registerFunctionAnalyses(functions);
  passes.registerCGSCCAnalyses(cgscc);
  passes.registerModuleAnalyses(modules);
  passes.crossRegisterProxies(loops, functions, cgscc, modules);
  FunctionPassManager manager;
  manager.addPass(InstCombinePass());
  manager.run(f, functions);
}
using SPObservations = std::map<BasicBlock *, WeakTrackingVH>;
bool flattenGuestPointers(Function &f, Value *guest, Work &work) {
  for (auto &block : f) for (auto it = block.begin(); it != block.end();) {
    auto &instruction = *it++;
    if (!work.spend()) return false;
    Value *pointer = nullptr;
    if (auto *load = dyn_cast<LoadInst>(&instruction)) pointer = load->getPointerOperand();
    if (auto *store = dyn_cast<StoreInst>(&instruction)) pointer = store->getPointerOperand();
    if (!pointer) continue;
    SmallVector<Value *, 8> terms;
    Value *base = pointer;
    while (base != guest) {
      if (!work.spend()) return false;
      auto *gep = dyn_cast<GetElementPtrInst>(base);
      if (!gep || gep->getNumIndices() != 1 ||
          !gep->getSourceElementType()->isIntegerTy(8) || gep->isInBounds() ||
          gep->hasNoUnsignedWrap() || gep->hasNoUnsignedSignedWrap()) break;
      auto *index = gep->idx_begin()->get();
      if (!index->getType()->isIntegerTy() || index->getType()->getIntegerBitWidth() > 64) break;
      if (terms.size() == 32) {
        work.stats.budgetExhausted = true;
        return fail(work.stats, "Guest byte-pointer chain budget exhausted");
      }
      terms.push_back(index);
      base = gep->getPointerOperand();
    }
    if (base != guest || terms.size() < 2) continue;
    IRBuilder<> builder(&instruction);
    Value *address = nullptr;
    // LLVM sign-extends a GEP index to the pointer-index width. Every original
    // i8 GEP here is unflagged, so the complete sum is exact modulo i64.
    for (auto term = terms.rbegin(); term != terms.rend(); ++term) {
      if (!work.spend()) return false;
      auto *wide = builder.CreateSExtOrTrunc(*term, builder.getInt64Ty());
      address = address ? builder.CreateAdd(address, wide) : wide;
    }
    auto *flat = builder.CreateGEP(builder.getInt8Ty(), guest, address,
                                    "image.table.guest.pointer");
    if (auto *load = dyn_cast<LoadInst>(&instruction)) load->setOperand(LoadInst::getPointerOperandIndex(), flat);
    if (auto *store = dyn_cast<StoreInst>(&instruction)) store->setOperand(StoreInst::getPointerOperandIndex(), flat);
  }
  return true;
}
bool normalize(Function &f, Guards &g, SPObservations &sp, Work &work) {
  // A value handle does not keep an otherwise unused SP expression alive.
  // Give each observation a real use only while InstCombine normalizes the
  // private trial. Remove these uses before table discovery or any proof.
  auto *anchorFunction = f.getParent()->getFunction("llvm.fake.use");
  const bool createdAnchorFunction = !anchorFunction;
  if (!anchorFunction) {
    if (!work.spend()) return false;
    anchorFunction = Intrinsic::getOrInsertDeclaration(f.getParent(), Intrinsic::fake_use);
  }
  SmallVector<WeakTrackingVH, 64> anchors;
  for (auto &block : f) {
    if (!work.spend(2)) return false;
    auto *insertion = block.getTerminator();
    if (auto *call = dyn_cast_or_null<CallInst>(insertion->getPrevNode());
        isa<ReturnInst>(insertion) && call && call->isMustTailCall())
      insertion = call; // The private promotable SP slot cannot escape to it.
    IRBuilder<> builder(insertion);
    auto *value = builder.CreateLoad(builder.getInt64Ty(), g.spSlot,
                                    "image.table.sp.observation");
    sp.emplace(&block, value);
    anchors.emplace_back(builder.CreateCall(anchorFunction, {value}));
  }
  SmallVector<AllocaInst *, 64> allocas;
  for (auto &i : f.getEntryBlock()) if (auto *a = dyn_cast<AllocaInst>(&i)) {
    if (!work.spend()) return false;
    if (!isAllocaPromotable(a)) return fail(work.stats, "nonprivate raw alloca");
    allocas.push_back(a);
  }
  DominatorTree dom(f);
  PromoteMemToReg(allocas, dom);
  instCombine(f);
  for (auto &anchor : anchors) {
    if (!work.spend()) return false;
    if (anchor) cast<Instruction>(static_cast<Value *>(anchor))->eraseFromParent();
  }
  if (createdAnchorFunction) {
    if (!work.spend()) return false;
    if (!anchorFunction->use_empty())
      return fail(work.stats, "private SP observation escaped normalization");
    anchorFunction->eraseFromParent();
  }
  // Do not run DCE here: the now unused observation nodes must remain until
  // explicit proof folding replaces them. Handles track changes, not liveness.
  return flattenGuestPointers(f, g.guest, work);
}
struct Table {
  SwitchInst *dispatch = nullptr;
  LoadInst *load = nullptr;
  Value *index = nullptr;
  std::vector<BasicBlock *> targets;
};
Value *tableIndex(Value *address, std::uint64_t base, std::size_t count) {
  auto *binary = dyn_cast<BinaryOperator>(address);
  if (!binary || (binary->getOpcode() != Instruction::Add &&
                  binary->getOpcode() != Instruction::Or)) return nullptr;
  Value *scale = nullptr;
  for (unsigned n = 0; n != 2; ++n)
    if (auto *c = dyn_cast<ConstantInt>(binary->getOperand(n));
        c && c->getValue().getActiveBits() <= 64 && c->getZExtValue() == base)
      scale = binary->getOperand(1 - n);
  // InstCombine may narrow a guarded shift, then extend its result. This is
  // the same table geometry only when every table-domain value fits the
  // narrower shift; arbitrary outside-domain values still use the old lookup.
  if (auto *extend = dyn_cast_or_null<ZExtInst>(scale)) scale = extend->getOperand(0);
  auto *shift = dyn_cast_or_null<BinaryOperator>(scale);
  if (!shift || shift->getOpcode() != Instruction::Shl ||
      !shift->getType()->isIntegerTy()) return nullptr;
  const unsigned shiftBits = shift->getType()->getIntegerBitWidth();
  if (shiftBits <= 3 || shiftBits > 64 ||
      (shiftBits < 64 && count - 1 > (((std::uint64_t(1) << shiftBits) - 1) >> 3))) return nullptr;
  auto *amount = dyn_cast<ConstantInt>(shift->getOperand(1));
  if (!amount || !amount->equalsInt(3)) return nullptr;
  if (binary->getOpcode() == Instruction::Or) {
    const auto covering = (std::bit_ceil(count) - 1) * 8;
    if (base & covering) return nullptr;
  }
  Value *index = shift->getOperand(0);
  if (auto *cast = dyn_cast<ZExtInst>(index)) index = cast->getOperand(0);
  if (!index->getType()->isIntegerTy() || index->getType()->getIntegerBitWidth() > 64)
    return nullptr;
  const unsigned width = index->getType()->getIntegerBitWidth();
  if (width < 64 && count - 1 > ((std::uint64_t(1) << width) - 1)) return nullptr;
  return index;
}
bool findTable(Function &f, const Guards &g, const ImageTableContextRequest &r,
               Table &table, Work &work) {
  for (auto &block : f) {
    if (!work.spend()) return false;
    auto *sw = dyn_cast<SwitchInst>(block.getTerminator());
    auto *load = sw ? dyn_cast<LoadInst>(sw->getCondition()) : nullptr;
    auto *gep = load ? dyn_cast<GetElementPtrInst>(load->getPointerOperand()) : nullptr;
    if (!load || !load->isSimple() || !load->getType()->isIntegerTy(64) ||
        load->getParent() != &block || load->getNextNode() != sw || !gep ||
        gep->getPointerOperand() != g.guest || gep->getNumIndices() != 1 ||
        !gep->getSourceElementType()->isIntegerTy(8)) continue;
    Value *index = tableIndex(gep->idx_begin()->get(), r.tableOffset, r.targets.size());
    if (!index) continue;
    std::map<std::uint64_t, BasicBlock *> cases;
    for (auto c : sw->cases()) {
      if (!work.spend()) return false;
      cases.emplace(c.getCaseValue()->getZExtValue(), c.getCaseSuccessor());
    }
    std::vector<BasicBlock *> targets;
    for (auto value : r.targets) {
      if (!work.spend()) return false;
      const auto found = cases.find(value);
      if (found == cases.end()) return fail(work.stats, "table target is absent from actual target switch");
      targets.push_back(found->second);
    }
    if (table.dispatch) return fail(work.stats, "multiple eligible table reads");
    // Other uses are the original UnknownTarget snapshot, exclusively reached
    // through this switch's default. They must remain dominated by its LOAD.
    for (auto *user : load->users()) {
      if (!work.spend()) return false;
      if (user == sw) continue;
      auto *i = dyn_cast<Instruction>(user);
      if (!i || i->getParent() != sw->getDefaultDest() ||
          sw->getDefaultDest()->getSinglePredecessor() != &block)
        return fail(work.stats, "target LOAD has unsupported live-outs");
    }
    table = {sw, load, index, std::move(targets)};
  }
  return table.dispatch || fail(work.stats, "no supported indexed table switch");
}
bool replaceCandidateSwitch(Table &table, Work &work) {
  auto *old = table.dispatch;
  auto *block = old->getParent();
  std::map<BasicBlock *, unsigned> before, after;
  for (unsigned n = 0; n != old->getNumSuccessors(); ++n) {
    if (!work.spend()) return false;
    ++before[old->getSuccessor(n)];
  }
  ++after[old->getDefaultDest()];
  for (auto *target : table.targets) ++after[target];
  for (auto &targetBlock : *block->getParent()) {
    auto *target = &targetBlock;
    const auto found = before.find(target);
    if (found == before.end()) continue;
    const auto count = found->second;
    for (auto &phi : target->phis()) {
      if (!work.spend()) return false;
      auto *value = phi.getIncomingValueForBlock(block);
      for (unsigned n = count; n < after[target]; ++n) phi.addIncoming(value, block);
      for (unsigned n = after[target]; n < count; ++n) {
        const int at = phi.getBasicBlockIndex(block);
        if (at < 0) return fail(work.stats, "incomplete original PHI edge coverage");
        phi.removeIncomingValue(at, false);
      }
    }
  }
  auto *replacement = SwitchInst::Create(table.index, old->getDefaultDest(),
                                          table.targets.size(), old);
  for (std::size_t n = 0; n != table.targets.size(); ++n) {
    if (!work.spend()) return false;
    replacement->addCase(ConstantInt::get(cast<IntegerType>(table.index->getType()), n),
                         table.targets[n]);
    }
  old->eraseFromParent();
  table.dispatch = replacement;
  return true;
}
bool substituteGuardedSP(Function &f, const Guards &g, Work &work) {
  DominatorTree dom(f);
  auto *constant = ConstantInt::get(g.initialSP->getType(), g.sp);
  for (auto it = g.initialSP->use_begin(); it != g.initialSP->use_end();) {
    if (!work.spend()) return false;
    Use &use = *it++;
    auto *i = dyn_cast<Instruction>(use.getUser());
    if (i && dom.dominates(g.accepted, i->getParent())) use.set(constant);
  }
  return true;
}
bool substituteGuardedRegisters(Function &f, const Guards &g, Work &work) {
  if (!substituteGuardedSP(f, g, work)) return false;
  if (g.registers.empty()) return true;
  DominatorTree dom(f);
  for (const auto &fact : g.registers) {
    if (!work.spend()) return false;
    Value *observation = fact.observation;
    if (!observation || observation->getType() != fact.expected->getType())
      return fail(work.stats, "guarded register observation did not survive normalization");
    if (auto *constant = dyn_cast<ConstantInt>(observation)) {
      if (constant != fact.expected)
        return fail(work.stats, "normalized register observation disagrees with its check");
      continue;
    }
    auto *definition = dyn_cast<Instruction>(observation);
    if (!definition || definition->getFunction() != &f)
      return fail(work.stats, "guarded register observation escaped its function");
    for (auto it = observation->use_begin(); it != observation->use_end();) {
      if (!work.spend()) return false;
      Use &use = *it++;
      auto *user = dyn_cast<Instruction>(use.getUser());
      if (user && user->getFunction() == &f && dom.dominates(g.accepted, user->getParent()))
        use.set(fact.expected);
    }
  }
  return true;
}
bool foldConstants(Function &f, Work &work) {
  bool changed;
  do {
    changed = false;
    for (auto &block : f) for (auto it = block.begin(); it != block.end();) {
      auto &i = *it++;
      if (!work.spend()) return false;
      Value *replacement = nullptr;
      if (auto *phi = dyn_cast<PHINode>(&i)) {
        for (auto &incoming : phi->incoming_values()) {
          if (incoming.get() == phi) continue;
          if (!replacement) replacement = incoming.get();
          else if (replacement != incoming.get()) { replacement = nullptr; break; }
        }
      } else if (!i.isTerminator() && !i.mayReadOrWriteMemory() && !isa<CallBase>(i))
        replacement = ConstantFoldInstruction(&i, f.getParent()->getDataLayout());
      if (replacement && replacement != &i) {
        i.replaceAllUsesWith(replacement);
        i.eraseFromParent();
        changed = true;
      }
    }
  } while (changed);
  return true;
}
using ConstantFacts = std::map<Value *, ConstantInt *>;
struct Range { std::uint64_t low, high; unsigned bits; };
std::uint64_t mask(unsigned bits) { return bits == 64 ? UINT64_MAX : (std::uint64_t(1) << bits) - 1; }
class Ranges {
 public:
  explicit Ranges(Work &work, const ConstantFacts *facts = nullptr)
      : work(work), facts(facts) {}
  std::optional<Range> get(Value *v) { nodes = 0; active.clear(); return visit(v, 0); }
 private:
  Work &work;
  const ConstantFacts *facts;
  unsigned nodes = 0;
  std::set<Value *> active;
  std::optional<Range> visit(Value *v, unsigned depth) {
    if (!work.spend()) return {};
    if (depth > 32 || ++nodes > 128) {
      work.stats.budgetExhausted = true;
      work.stats.message = "image-table address expression budget exhausted";
      return {};
    }
    if (!v->getType()->isIntegerTy() || v->getType()->getIntegerBitWidth() > 64 ||
        isa<UndefValue, PoisonValue>(v)) return {};
    const unsigned bits = v->getType()->getIntegerBitWidth();
    const Range full{0, mask(bits), bits};
    if (auto *c = dyn_cast<ConstantInt>(v)) return Range{c->getZExtValue(), c->getZExtValue(), bits};
    if (facts) if (auto found = facts->find(v); found != facts->end()) {
      const auto value = found->second->getZExtValue();
      return Range{value, value, bits};
    }
    if (isa<FreezeInst>(v)) return full; // a new defined observation, never its operand's constant
    if (!active.insert(v).second) return full;
    auto done = [&](std::optional<Range> r) { active.erase(v); return r; };
    auto *i = dyn_cast<Instruction>(v);
    if (!i) return done(full);
    if (auto *load = dyn_cast<LoadInst>(i)) return done(load->isSimple() ? std::optional(full) : std::nullopt);
    if (auto *cast = dyn_cast<CastInst>(i)) {
      auto a = visit(cast->getOperand(0), depth + 1);
      if (!a) return done({});
      if (cast->getOpcode() == Instruction::ZExt ||
          (cast->getOpcode() == Instruction::Trunc && a->high <= mask(bits)))
        return done(Range{a->low, a->high, bits});
      if (cast->getOpcode() == Instruction::Trunc || cast->getOpcode() == Instruction::SExt)
        return done(full);
      return done({});
    }
    if (auto *phi = dyn_cast<PHINode>(i)) {
      Range result{mask(bits), 0, bits};
      for (auto &incoming : phi->incoming_values()) {
        auto a = visit(incoming.get(), depth + 1);
        if (!a) return done({});
        result.low = std::min(result.low, a->low);
        result.high = std::max(result.high, a->high);
      }
      return done(result.low <= result.high ? std::optional(result) : std::nullopt);
    }
    if (auto *select = dyn_cast<SelectInst>(i)) {
      auto condition = visit(select->getCondition(), depth + 1);
      auto a = visit(select->getTrueValue(), depth + 1);
      auto b = visit(select->getFalseValue(), depth + 1);
      if (!condition || !a || !b) return done({});
      return done(Range{std::min(a->low, b->low), std::max(a->high, b->high), bits});
    }
    auto *binary = dyn_cast<BinaryOperator>(i);
    if (!binary) {
      // Unsupported integer shapes supply no precision, but cannot hide a
      // visible poison/undef or malformed shift underneath a later mask.
      for (auto &operand : i->operands())
        if (operand->getType()->isIntegerTy() && !visit(operand.get(), depth + 1))
          return done({});
      return done(full);
    }
    auto a = visit(binary->getOperand(0), depth + 1);
    auto b = visit(binary->getOperand(1), depth + 1);
    if (!a || !b) return done({});
    switch (binary->getOpcode()) {
    case Instruction::And:
      if (a->low == a->high) return done(Range{0, a->high, bits});
      if (b->low == b->high) return done(Range{0, b->high, bits});
      return done(full);
    case Instruction::Or: case Instruction::Xor: {
      auto cover = [](std::uint64_t x) { return x == UINT64_MAX ? x : mask(std::bit_width(x)); };
      return done(Range{0, cover(a->high) | cover(b->high), bits});
    }
    case Instruction::Add:
      if (a->high <= mask(bits) - b->high)
        return done(Range{a->low + b->low, a->high + b->high, bits});
      return done(full);
    case Instruction::Sub:
      if (a->low >= b->high)
        return done(Range{a->low - b->high, a->high - b->low, bits});
      return done(full);
    case Instruction::Shl:
      if (b->low != b->high || b->high >= bits) return done({});
      if (a->high <= (mask(bits) >> b->high))
        return done(Range{a->low << b->high, a->high << b->high, bits});
      return done(full);
    case Instruction::LShr:
      if (b->low != b->high || b->high >= bits) return done({});
      return done(Range{a->low >> b->high, a->high >> b->high, bits});
    case Instruction::AShr:
      if (b->low != b->high || b->high >= bits) return done({});
      return done(full);
    default: return done(full);
    }
  }
};
bool isSameUnsignedValue(Value *a, Value *b) {
  while (auto *cast = dyn_cast<ZExtInst>(a)) a = cast->getOperand(0);
  while (auto *cast = dyn_cast<ZExtInst>(b)) b = cast->getOperand(0);
  return a == b;
}
bool indexInTable(Value *value, Instruction *at, BasicBlock *edgeTarget,
                  std::uint64_t count, DominatorTree &dom, Work &work,
                  std::set<Value *> &active, unsigned depth = 0) {
  if (!work.spend() || depth > 32 || isa<UndefValue, PoisonValue>(value) ||
      !active.insert(value).second) return false;
  auto done = [&](bool r) { active.erase(value); return r; };
  if (auto *c = dyn_cast<ConstantInt>(value))
    return done(c->getBitWidth() <= 64 && c->getZExtValue() < count);
  for (auto &block : *at->getFunction()) {
    if (!work.spend()) return done(false);
    auto *branch = dyn_cast<BranchInst>(block.getTerminator());
    auto *cmp = branch && branch->isConditional() ? dyn_cast<ICmpInst>(branch->getCondition()) : nullptr;
    // A PHI labels predecessor blocks, not conditional successor slots. If
    // both slots enter the same block, neither Boolean arm is implied there.
    if (!cmp || branch->getSuccessor(0) == branch->getSuccessor(1)) continue;
    Value *operand = cmp->getOperand(0);
    auto *constant = dyn_cast<ConstantInt>(cmp->getOperand(1));
    auto predicate = cmp->getPredicate();
    if (!constant) {
      constant = dyn_cast<ConstantInt>(cmp->getOperand(0));
      operand = cmp->getOperand(1);
      predicate = ICmpInst::getSwappedPredicate(predicate);
    }
    if (!constant || constant->getBitWidth() > 64 || !isSameUnsignedValue(value, operand)) continue;
    for (unsigned success = 0; success != 2; ++success) {
      auto *target = branch->getSuccessor(success);
      const bool direct = at == branch && edgeTarget == target;
      if (!direct && !dom.dominates(BasicBlockEdge(&block, target), at->getParent())) continue;
      auto p = success == 0 ? predicate : ICmpInst::getInversePredicate(predicate);
      const auto c = constant->getZExtValue();
      if ((p == ICmpInst::ICMP_ULT && c <= count) ||
          (p == ICmpInst::ICMP_ULE && c < count)) return done(true);
    }
  }
  if (auto *cast = dyn_cast<ZExtInst>(value))
    return done(indexInTable(cast->getOperand(0), at, edgeTarget, count, dom, work, active, depth + 1));
  if (auto *phi = dyn_cast<PHINode>(value)) {
    for (unsigned n = 0; n != phi->getNumIncomingValues(); ++n) {
      auto *source = phi->getIncomingBlock(n);
      if (!indexInTable(phi->getIncomingValue(n), source->getTerminator(),
                         phi->getParent(), count, dom, work, active, depth + 1)) return done(false);
    }
    return done(phi->getNumIncomingValues() != 0);
  }
  Ranges ranges(work);
  if (auto r = ranges.get(value); r && r->high < count) return done(true);
  return done(false);
}
bool mathematicalCall(const CallBase &call) {
  const auto *f = call.getCalledFunction();
  return f && (f->getIntrinsicID() == Intrinsic::fshl || f->getIntrinsicID() == Intrinsic::fshr) &&
      call.doesNotAccessMemory() && !call.isConvergent() &&
      !call.hasFnAttr(Attribute::NoDuplicate) && !call.getNumOperandBundles();
}
struct InvariantRegion {
  std::set<BasicBlock *> reachable, step, preserving;
};
struct SeededCell {
  std::uint64_t address;
  unsigned bits;
  StoreInst *seed;
  ConstantInt *value;
  std::vector<LoadInst *> loads;
};
std::optional<std::uint64_t> fixedGuestWord(Value *pointer, Value *guest) {
  auto *gep = dyn_cast<GetElementPtrInst>(pointer);
  if (!gep || gep->hasPoisonGeneratingAnnotations()) return {};
  auto at = offset(pointer, guest);
  if (!at || !gep->idx_begin()->get()->getType()->isIntegerTy(64)) return {};
  return at;
}
bool disjoint(std::uint64_t address, std::uint64_t bytes, std::uint64_t other,
              std::uint64_t otherBytes) {
  return bytes <= UINT64_MAX - address && otherBytes <= UINT64_MAX - other &&
      (address + bytes <= other || other + otherBytes <= address);
}
bool shadowConstants(Function &f, ConstantFacts &facts, Work &work) {
  auto known = [&](Value *v) -> ConstantInt * {
    if (auto *c = dyn_cast<ConstantInt>(v)) return c;
    auto found = facts.find(v);
    return found == facts.end() ? nullptr : found->second;
  };
  bool changed;
  do {
    changed = false;
    for (auto &block : f) for (auto &i : block) {
      if (!work.spend()) return false;
      if (!i.getType()->isIntegerTy() || i.getType()->getIntegerBitWidth() > 64 ||
          facts.contains(&i) || i.hasPoisonGeneratingAnnotations()) continue;
      ConstantInt *result = nullptr;
      if (auto *phi = dyn_cast<PHINode>(&i)) {
        bool agree = true;
        for (auto &incoming : phi->incoming_values()) {
          if (!work.spend()) return false;
          if (incoming.get() == phi) continue;
          auto *c = known(incoming.get());
          if (!c || (result && result != c)) { agree = false; break; }
          result = c;
        }
        if (!agree) result = nullptr;
      } else {
        switch (i.getOpcode()) {
        case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
        case Instruction::And: case Instruction::Or: case Instruction::Xor:
        case Instruction::Shl: case Instruction::LShr: case Instruction::AShr:
        case Instruction::UDiv: case Instruction::SDiv:
        case Instruction::URem: case Instruction::SRem:
        case Instruction::ZExt: case Instruction::SExt: case Instruction::Trunc:
        case Instruction::ICmp: case Instruction::Select: break;
        default: continue;
        }
        SmallVector<Constant *, 4> operands;
        for (auto &operand : i.operands()) {
          if (!work.spend()) return false;
          auto *c = known(operand.get());
          if (!c) { operands.clear(); break; }
          operands.push_back(c);
        }
        if (operands.size() == i.getNumOperands())
          result = dyn_cast_or_null<ConstantInt>(ConstantFoldInstOperands(
              &i, operands, f.getParent()->getDataLayout(), nullptr, false));
      }
      if (result) { facts.emplace(&i, result); changed = true; }
    }
  } while (changed);
  return true;
}
bool findSeededCells(Function &f, const Guards &g, const Table &table,
                     const ImageTableContextRequest &r, const InvariantRegion &region,
                     Work &work, std::vector<SeededCell> &cells, std::string &diagnostics) {
  DominatorTree dom(f);
  using Key = std::pair<std::uint64_t, unsigned>;
  std::map<Key, std::vector<StoreInst *>> seeds;
  for (auto &block : f) if (region.preserving.contains(&block)) for (auto &i : block) {
    if (!work.spend()) return false;
    auto *store = dyn_cast<StoreInst>(&i);
    auto *value = store ? dyn_cast<ConstantInt>(store->getValueOperand()) : nullptr;
    if (store && store->isSimple() && store->getValueOperand()->getType()->isIntegerTy() &&
        dom.dominates(store, table.load)) {
      auto address = fixedGuestWord(store->getPointerOperand(), g.guest);
      if (address) {
        raw_string_ostream out(diagnostics);
        out << " init " << *address << "/" << store->getValueOperand()->getType()->getIntegerBitWidth()
            << " value=";
        store->getValueOperand()->printAsOperand(out, false);
        out << ";";
      }
    }
    if (!store || !store->isSimple() || !value || value->getBitWidth() % 8 ||
        value->getBitWidth() > 64 || !dom.dominates(g.accepted, &block) ||
        !dom.dominates(store, table.load)) continue;
    auto address = fixedGuestWord(store->getPointerOperand(), g.guest);
    if (!address || !disjoint(*address, value->getBitWidth() / 8,
                              r.tableOffset, r.targets.size() * 8)) continue;
    seeds[{*address, value->getBitWidth()}].push_back(store);
  }
  for (auto &[key, candidates] : seeds) {
    if (!work.spend()) return false;
    // A single independently constant initialization, not an inferred entry
    // value or a mutually assumed memory initializer.
    diagnostics += " cell " + std::to_string(key.first) + "/" + std::to_string(key.second) +
        " seeds=" + std::to_string(candidates.size());
    if (candidates.size() != 1) continue;
    SeededCell cell{key.first, key.second, candidates.front(),
                    cast<ConstantInt>(candidates.front()->getValueOperand()), {}};
    bool clobbered = false;
    for (auto &block : f) if (region.preserving.contains(&block)) for (auto &i : block) {
      if (!work.spend()) return false;
      if (auto *load = dyn_cast<LoadInst>(&i); load && load->isSimple() &&
          !load->hasPoisonGeneratingAnnotations() && load->getType()->isIntegerTy(cell.bits) &&
          fixedGuestWord(load->getPointerOperand(), g.guest) == cell.address &&
          dom.dominates(cell.seed, load)) cell.loads.push_back(load);
      auto *store = dyn_cast<StoreInst>(&i);
      if (!store || store == cell.seed || !store->getValueOperand()->getType()->isIntegerTy()) continue;
      auto at = fixedGuestWord(store->getPointerOperand(), g.guest);
      const auto bits = store->getValueOperand()->getType()->getIntegerBitWidth();
      if (at && bits % 8 == 0 && !disjoint(*at, bits / 8, cell.address, cell.bits / 8))
        clobbered = true;
    }
    diagnostics += " loads=" + std::to_string(cell.loads.size()) +
        " fixed_clobber=" + std::to_string(clobbered) + ";";
    if (!clobbered && !cell.loads.empty()) cells.push_back(std::move(cell));
  }
  return true;
}
bool closeControlInvariant(Function &f, const Guards &g, const Table &table,
                     const ImageTableContextRequest &r, const SPObservations &sp,
                     Work &work, InvariantRegion &region) {
  DominatorTree dom(f);
  std::set<Value *> active;
  if (!indexInTable(table.index, table.dispatch, nullptr, r.targets.size(), dom, work, active) ||
      work.stats.budgetExhausted)
    return fail(work.stats, "lookup may use an unproved outside-table path");
  auto &reachable = region.reachable;
  auto &step = region.step;
  auto visit = [&](std::vector<BasicBlock *> pending, std::set<BasicBlock *> &seen, bool stopAtRead) {
    for (std::size_t n = 0; n != pending.size(); ++n) {
      if (!work.spend()) return false;
      auto *block = pending[n];
      if (!seen.insert(block).second) continue;
      if ((g.chain.contains(block) && block != g.accepted) || block == &f.getEntryBlock())
        return fail(work.stats, "business path reenters entry guards");
      if (block->isEHPad() || block->hasAddressTaken())
        return fail(work.stats, "unsupported EH or block identity");
      auto *term = block->getTerminator();
      if (!isa<BranchInst, SwitchInst, ReturnInst>(term))
        return fail(work.stats, "unsupported controller terminator");
      if (stopAtRead && block == table.dispatch->getParent()) continue;
      for (auto *target : successors(block)) pending.push_back(target);
    }
    return true;
  };
  std::vector<BasicBlock *> roots = table.targets;
  roots.push_back(g.accepted);
  if (!visit(roots, reachable, false) || !visit(table.targets, step, true)) return false;
  auto &preserving = region.preserving;
  preserving.insert(table.dispatch->getParent());
  bool more;
  do {
    more = false;
    for (auto &candidate : f) {
      auto *block = &candidate;
      if (!reachable.contains(block)) continue;
      if (!work.spend()) return false;
      if (preserving.contains(block)) continue;
      for (auto *target : successors(block)) if (preserving.contains(target)) {
        preserving.insert(block); more = true; break;
      }
    }
  } while (more);
  auto observation = [&](BasicBlock *block) -> ConstantInt * {
    auto found = sp.find(block);
    return found == sp.end() || !found->second ? nullptr : dyn_cast<ConstantInt>(static_cast<Value *>(found->second));
  };
  auto *dispatchSP = observation(table.dispatch->getParent());
  if (!dispatchSP) return fail(work.stats, "dispatch SP is not closed to a constant");
  for (auto &candidate : f) if (step.contains(&candidate) && preserving.contains(&candidate)) {
    auto *block = &candidate;
    if (!work.spend()) return false;
    auto *currentSP = observation(block);
    if (!currentSP || currentSP->getValue() != dispatchSP->getValue())
      return fail(work.stats, "a table-target step changes the dispatch SP");
  }
  return true;
}
bool preserveWrites(Function &f, const Guards &g, const ImageTableContextRequest &r,
                    const InvariantRegion &region, Work &work,
                    const ConstantFacts *facts = nullptr, const SeededCell *cell = nullptr) {
  const auto tableEnd = r.tableOffset + r.targets.size() * 8;
  Ranges ranges(work, facts);
  for (auto &block : f) if (region.preserving.contains(&block)) for (auto &i : block) {
    if (!work.spend()) return false;
    if (auto *call = dyn_cast<CallBase>(&i)) {
      if (!mathematicalCall(*call)) return fail(work.stats, "call has no table-preservation proof");
      continue;
    }
    if (auto *load = dyn_cast<LoadInst>(&i)) {
      if (!load->isSimple()) return fail(work.stats, "atomic or volatile read in invariant");
      continue;
    }
    auto *store = dyn_cast<StoreInst>(&i);
    if (!store) {
      if (i.mayWriteToMemory() || isa<FenceInst>(i))
        return fail(work.stats, "unknown memory effect in invariant");
      continue;
    }
    auto *gep = dyn_cast<GetElementPtrInst>(store->getPointerOperand());
    if (!store->isSimple() || !store->getValueOperand()->getType()->isIntegerTy() ||
        !gep || gep->getPointerOperand() != g.guest || gep->getNumIndices() != 1 ||
        !gep->getSourceElementType()->isIntegerTy(8))
      return fail(work.stats, "unclassified or observable write in invariant");
    const auto width = store->getValueOperand()->getType()->getIntegerBitWidth();
    auto address = ranges.get(gep->idx_begin()->get());
    if (!width || width % 8 || !address || address->bits != 64 ||
        width / 8 > UINT64_MAX - address->high ||
        !(address->high + width / 8 <= r.tableOffset || address->low >= tableEnd))
    {
      std::string detail;
      raw_string_ostream out(detail);
      out << "STORE range may overwrite a protected table byte in " << block.getName() << ": ";
      i.print(out);
      out << "; address "; gep->idx_begin()->get()->print(out);
      return fail(work.stats, detail);
    }
    if (cell && store != cell->seed &&
        !(address->high + width / 8 <= cell->address ||
          address->low >= cell->address + cell->bits / 8)) {
      std::string detail;
      raw_string_ostream out(detail);
      out << "STORE range [" << address->low << "," << address->high
          << "]/" << width << " may overwrite seeded Guest cell " << cell->address
          << "/" << cell->bits << " in " << block.getName() << ": ";
      i.print(out);
      out << "; address "; gep->idx_begin()->get()->print(out);
      return fail(work.stats, detail);
    }
    ++work.stats.preservedStores;
  }
  return true;
}
bool fullFallback(Table &table, Work &work) {
  auto *block = table.dispatch->getParent();
  // All direct-case PHI inputs must already be available before the target
  // LOAD. The old UnknownTarget snapshot remains under the original LOAD.
  for (auto *target : table.targets) for (auto &phi : target->phis()) {
    if (!work.spend()) return false;
    auto *value = phi.getIncomingValueForBlock(block);
    auto *instruction = dyn_cast<Instruction>(value);
    if (instruction && instruction->getParent() == block &&
        !instruction->comesBefore(table.load))
      return fail(work.stats, "table target has a target-read live-out");
  }
  auto *fallback = block->splitBasicBlock(table.load->getIterator(), "image.table.fallback");
  auto *oldBranch = block->getTerminator();
  auto *sw = SwitchInst::Create(table.index, fallback, table.targets.size(), oldBranch);
  for (std::size_t n = 0; n != table.targets.size(); ++n) {
    if (!work.spend()) return false;
    auto *target = table.targets[n];
    sw->addCase(ConstantInt::get(cast<IntegerType>(table.index->getType()), n), target);
    for (auto &phi : target->phis()) {
      if (!work.spend()) return false;
      phi.addIncoming(phi.getIncomingValueForBlock(fallback), block);
    }
  }
  oldBranch->eraseFromParent();
  return true;
}
bool verify(Function &f, std::string &error) {
  raw_string_ostream output(error);
  return !verifyFunction(f, &output);
}
bool verifyWhole(Module &module, std::string &error) {
  raw_string_ostream output(error);
  return !verifyModule(module, &output);
}
void rebindBodyArguments(Function &body, Function &from, Function &to) {
  for (unsigned n = 0; n != from.arg_size(); ++n) {
    auto *old = from.getArg(n);
    for (auto it = old->use_begin(); it != old->use_end();) {
      Use &use = *it++;
      auto *instruction = dyn_cast<Instruction>(use.getUser());
      if (instruction && instruction->getFunction() == &body) use.set(to.getArg(n));
    }
  }
}
struct NamedMetadataSnapshot {
  Module &module;
  std::map<NamedMDNode *, std::vector<MDNode *>> operands;
  explicit NamedMetadataSnapshot(Module &m) : module(m) {
    for (auto &named : m.named_metadata()) {
      auto &saved = operands[&named];
      for (auto *node : named.operands()) saved.push_back(node);
    }
  }
  void restore() {
    for (auto it = module.named_metadata_begin(); it != module.named_metadata_end();) {
      auto *named = &*it++;
      if (!operands.contains(named)) named->eraseFromParent();
    }
    for (const auto &[named, saved] : operands) {
      named->clearOperands();
      for (auto *node : saved) named->addOperand(node);
    }
  }
};
bool commit(Function &source, Function &candidate, ImageTableContextStats &stats,
            unsigned forceFailure) {
  NamedMetadataSnapshot metadata(*source.getParent());
  ValueToValueMapTy map;
  std::vector<Function *> newDeclarations;
  auto cleanDeclarations = [&]() {
    for (auto *f : newDeclarations) if (f->use_empty()) f->eraseFromParent();
  };
  for (auto &global : candidate.getParent()->global_values()) {
    auto *original = source.getParent()->getNamedValue(global.getName());
    if (!original) {
      auto *f = dyn_cast<Function>(&global);
      if (f && f->isDeclaration() &&
          (f->getIntrinsicID() == Intrinsic::fshl || f->getIntrinsicID() == Intrinsic::fshr)) {
        auto *declaration = Intrinsic::getDeclaration(source.getParent(), f->getIntrinsicID(),
                                                     {f->getReturnType()});
        original = declaration;
        newDeclarations.push_back(declaration);
      }
    }
    if (!original || original->getType() != global.getType()) {
      cleanDeclarations();
      return fail(stats, "candidate global identity cannot be rebound");
    }
    map[&global] = original;
  }
  auto *copy = Function::Create(source.getFunctionType(), GlobalValue::InternalLinkage,
                                source.getName() + ".image.table.commit", source.getParent());
  auto argument = copy->arg_begin();
  for (auto &a : candidate.args()) map[&a] = &*argument++;
  SmallVector<ReturnInst *, 8> returns;
  CloneFunctionInto(copy, &candidate, map, CloneFunctionChangeType::DifferentModule, returns);
  copy->copyAttributesFrom(&source);
  copy->setCallingConv(source.getCallingConv());
  copy->setAttributes(source.getAttributes());
  // The staging function later becomes an empty declaration during the body
  // transaction. Keep that declaration legal for whole-module verification.
  copy->setLinkage(GlobalValue::ExternalLinkage);
  copy->setComdat(nullptr);
  // DifferentModule creates/appends llvm.dbg.cu even without debug info. No
  // module metadata is part of this component's publication contract.
  metadata.restore();
  std::string error;
  if (forceFailure == 1 || !verifyWhole(*source.getParent(), error)) {
    copy->eraseFromParent(); cleanDeclarations();
    metadata.restore();
    return fail(stats, "invalid staged body: " + error);
  }
  auto *backup = Function::Create(source.getFunctionType(), GlobalValue::ExternalLinkage,
                                  source.getName() + ".image.table.rollback", source.getParent());
  backup->copyAttributesFrom(&source);
  backup->setCallingConv(source.getCallingConv());
  backup->setAttributes(source.getAttributes());
  backup->setLinkage(GlobalValue::ExternalLinkage);
  backup->setComdat(nullptr);
  backup->splice(backup->end(), &source);
  rebindBodyArguments(*backup, source, *backup);
  auto old = source.arg_begin();
  for (auto &a : copy->args()) a.replaceAllUsesWith(&*old++);
  source.splice(source.end(), copy);
  if (forceFailure == 2 || !verifyWhole(*source.getParent(), error) ||
      !copy->use_empty() || !backup->use_empty() ||
      copy->isUsedByMetadata() || backup->isUsedByMetadata()) {
    copy->splice(copy->end(), &source);
    source.splice(source.end(), backup);
    rebindBodyArguments(source, *backup, source);
    backup->eraseFromParent(); copy->eraseFromParent();
    cleanDeclarations();
    metadata.restore();
    return fail(stats, "body transplant failed: " + error);
  }
  backup->eraseFromParent(); copy->eraseFromParent();
  return true;
}
bool unsupportedBlockAddress(Value *value, Work &work, std::set<Value *> &seen,
                             unsigned depth = 0) {
  if (!isa<Constant>(value) || !seen.insert(value).second) return false;
  if (!isa<BlockAddress, ConstantExpr, ConstantAggregate>(value)) return false;
  if (!work.spend() || depth > 32) {
    work.stats.budgetExhausted = true; return true;
  }
  if (isa<BlockAddress>(value)) return true;
  for (auto &operand : cast<Constant>(value)->operands())
    if (unsupportedBlockAddress(operand, work, seen, depth + 1)) return true;
  return false;
}
bool unsupportedMetadata(Metadata *metadata, Work &work, std::set<Metadata *> &seen,
                         std::set<Value *> &constants, unsigned depth = 0) {
  if (!metadata || !seen.insert(metadata).second) return false;
  if (!work.spend() || depth > 32) {
    work.stats.budgetExhausted = true; return true;
  }
  if (auto *value = dyn_cast<ValueAsMetadata>(metadata)) {
    auto *v = value->getValue();
    return isa<Instruction, Argument, BasicBlock>(v) ||
           unsupportedBlockAddress(v, work, constants);
  }
  if (auto *node = dyn_cast<MDNode>(metadata))
    for (auto &operand : node->operands())
      if (!work.spend() || unsupportedMetadata(operand.get(), work, seen, constants, depth + 1))
        return true;
  return false;
}
} // namespace

static ImageTableContextStats runImageTableContext(
    Module &, const ImageTableContextRequest &, bool consumeClosedCFG,
    unsigned forceCommitFailure);

ImageTableContextStats runImageTableCheckedContext(Module &module,
                                                    const ImageTableContextRequest &request) {
  return runImageTableCheckedContext(module, request, false);
}

ImageTableContextStats runClosedImageTableContext(Module &module,
                                                 const ImageTableContextRequest &request) {
  return runImageTableCheckedContext(module, request, true);
}

ImageTableContextStats runImageTableCheckedContext(Module &module,
                                                    const ImageTableContextRequest &request,
                                                    bool consumeClosedCFG) {
  return runImageTableContext(module, request, consumeClosedCFG, 0);
}

static ImageTableContextStats runImageTableContext(Module &module,
                                                    const ImageTableContextRequest &request,
                                                    bool consumeClosedCFG,
                                                    unsigned forceCommitFailure) {
  ImageTableContextStats stats;
  Work work{stats, std::min<std::size_t>(request.maxWork, 1000000)};
  const auto &layout = module.getDataLayout();
  if (!layout.isLittleEndian() || layout.getPointerSizeInBits(0) != 64 ||
      layout.getIndexSizeInBits(0) != 64 ||
      request.targets.empty() || request.targets.size() > 512 ||
      request.targets.size() * 8 > UINT64_MAX - request.tableOffset ||
      request.maxBlocks > 4096 || request.maxInstructions > 65536 ||
      request.maxWork > 1000000 || !request.maxBlocks || !request.maxInstructions || !request.maxWork) {
    fail(stats, "unsupported table layout or prototype budget"); return stats;
  }
  auto *source = module.getFunction(request.viewSymbol);
  if (!source) { fail(stats, "view symbol is absent"); return stats; }
  // A transplant discards every old block, including unreachable and terminal
  // blocks. Their address/debug/EH identities cannot be repaired by rebinding
  // globals by name, so reject them before making even a private proof clone.
  if (source->hasPersonalityFn() || source->getSubprogram()) {
    fail(stats, "EH or debug function identity unsupported"); return stats;
  }
  std::set<Value *> constants;
  std::set<Metadata *> metadata;
  SmallVector<std::pair<unsigned, MDNode *>, 8> attachments;
  source->getAllMetadata(attachments);
  for (auto [kind, node] : attachments)
    if (unsupportedMetadata(node, work, metadata, constants)) {
      fail(stats, "function metadata identity unsupported"); return stats;
    }
  for (auto &argument : source->args()) if (argument.isUsedByMetadata()) {
    fail(stats, "local argument metadata identity unsupported"); return stats;
  }
  for (auto &block : *source) {
    if (!work.spend()) return stats;
    if (block.hasAddressTaken() || block.isEHPad() || block.isUsedByMetadata()) {
      fail(stats, "address-taken or EH function block unsupported"); return stats;
    }
    for (auto &instruction : block) {
      if (!work.spend()) return stats;
      if (instruction.getDebugLoc() || instruction.hasDbgRecords() || instruction.isUsedByMetadata()) {
        fail(stats, "debug instruction identity unsupported"); return stats;
      }
      attachments.clear(); instruction.getAllMetadata(attachments);
      for (auto [kind, node] : attachments)
        if (unsupportedMetadata(node, work, metadata, constants)) {
          fail(stats, "instruction metadata identity unsupported"); return stats;
        }
      for (auto &operand : instruction.operands())
        if (unsupportedBlockAddress(operand, work, constants)) {
          fail(stats, "blockaddress or constant identity unsupported"); return stats;
        }
    }
  }
  std::size_t namedOperands = 0;
  for (auto &named : module.named_metadata()) {
    if (named.getNumOperands() > 65536 - namedOperands ||
        !work.spend(1 + named.getNumOperands())) {
      stats.budgetExhausted = true;
      fail(stats, "named metadata transaction cap"); return stats;
    }
    namedOperands += named.getNumOperands();
  }
  std::size_t instructions = 0;
  for (auto &f : module) for (auto &block : f) {
    if (!work.spend(block.size()) || instructions > 1000000 - block.size()) return stats;
    instructions += block.size();
  }
  if (source->size() > request.maxBlocks) { fail(stats, "view block cap exceeded"); return stats; }
  std::size_t selectedInstructions = 0;
  for (auto &block : *source) selectedInstructions += block.size();
  if (selectedInstructions > request.maxInstructions) { fail(stats, "view instruction cap exceeded"); return stats; }
  std::string verification;
  raw_string_ostream verifyOutput(verification);
  if (verifyModule(module, &verifyOutput)) { fail(stats, "invalid source module: " + verification); return stats; }
  auto trial = CloneModule(module);
  auto *function = trial->getFunction(request.viewSymbol);
  Guards guards;
  SPObservations sp;
  if (!guardFacts(*function, request, guards, work) ||
      !normalize(*function, guards, sp, work)) return stats;
  Table table;
  if (!findTable(*function, guards, request, table, work)) return stats;

  ValueToValueMapTy proofMap;
  auto proofModule = CloneModule(*trial, proofMap);
  auto mapped = [&](Value *v) -> Value * {
    if (isa<Constant>(v)) return MapValue(v, proofMap);
    return proofMap.lookup(v);
  };
  auto *proofFunction = cast<Function>(mapped(function));
  Guards proofGuards = guards;
  proofGuards.accepted = cast<BasicBlock>(mapped(guards.accepted));
  proofGuards.fallback = cast<BasicBlock>(mapped(guards.fallback));
  proofGuards.guest = mapped(guards.guest);
  proofGuards.initialSP = cast<LoadInst>(mapped(guards.initialSP));
  proofGuards.registers.clear();
  for (const auto &fact : guards.registers) {
    if (!work.spend() || !fact.observation) return stats;
    auto *observation = mapped(fact.observation);
    if (!observation) { fail(stats, "guarded register observation cannot be mapped"); return stats; }
    proofGuards.registers.push_back({observation, fact.expected});
  }
  proofGuards.chain.clear();
  for (auto &block : *function) if (guards.chain.contains(&block))
    proofGuards.chain.insert(cast<BasicBlock>(mapped(&block)));
  Table proofTable{cast<SwitchInst>(mapped(table.dispatch)), cast<LoadInst>(mapped(table.load)), mapped(table.index), {}};
  for (auto *target : table.targets) proofTable.targets.push_back(cast<BasicBlock>(mapped(target)));
  SPObservations proofSP;
  for (auto &block : *function) {
    const auto found = sp.find(&block);
    if (found == sp.end()) { fail(stats, "SP observation is absent"); return stats; }
    auto value = found->second;
    if (!work.spend()) return stats;
    if (!value) { fail(stats, "SP observation did not survive normalization"); return stats; }
    proofSP.emplace(cast<BasicBlock>(mapped(&block)), mapped(value));
  }
  std::vector<std::pair<Instruction *, WeakTrackingVH>> facts;
  facts.reserve(selectedInstructions);
  for (auto &block : *function) for (auto &i : block) {
    if (!work.spend()) return stats;
    if (i.getType()->isIntegerTy()) facts.emplace_back(&i, mapped(&i));
    }
  if (!replaceCandidateSwitch(proofTable, work)) return stats;
  if (!substituteGuardedRegisters(*proofFunction, proofGuards, work)) return stats;
  WeakTrackingVH proofIndex(proofTable.index);
  if (!foldConstants(*proofFunction, work)) return stats;
  proofTable.index = proofIndex;
  if (!proofTable.index || !verify(*proofFunction, verification)) return stats;
  InvariantRegion region;
  if (!closeControlInvariant(*proofFunction, proofGuards, proofTable, request,
                              proofSP, work, region)) return stats;
  if (!preserveWrites(*proofFunction, proofGuards, request, region, work)) {
    if (stats.budgetExhausted || !StringRef(stats.message).starts_with("STORE range may overwrite a protected table byte"))
      return stats;
    const auto originalMessage = stats.message;
    const auto originalStores = stats.preservedStores;
    std::vector<SeededCell> cells;
    std::string diagnostics;
    if (!findSeededCells(*proofFunction, proofGuards, proofTable, request, region,
                          work, cells, diagnostics)) return stats;
    bool closed = false;
    std::string lastFailure;
    for (const auto &cell : cells) {
      ConstantFacts assumed;
      for (auto *load : cell.loads) {
        if (!work.spend()) return stats;
        assumed.emplace(load, cell.value);
      }
      // This overlay never changes CFG reachability. Every preserving write,
      // including clobber-then-restore paths, must establish the joint step.
      if (!shadowConstants(*proofFunction, assumed, work)) return stats;
      stats.preservedStores = 0;
      if (!preserveWrites(*proofFunction, proofGuards, request, region, work, &assumed, &cell)) {
        if (stats.budgetExhausted) return stats;
        lastFailure = stats.message;
        continue;
      }
      // The base is an actual dominating STORE whose value was constant
      // before any cell hypothesis. The joint table/cell induction is now
      // closed. Preserve the original LOADs and all memory/error checks.
      ConstantFacts observations;
      for (auto *load : cell.loads) {
        if (!work.spend()) return stats;
        observations.emplace(load, cell.value);
        load->replaceAllUsesWith(cell.value);
      }
      for (auto &[old, proofValue] : facts) {
        if (!work.spend()) return stats;
        auto found = observations.find(proofValue);
        if (found != observations.end()) proofValue = found->second;
      }
      // A failure after these private rewrites discards this complete proof;
      // it must never reuse a mutated trial for another cell candidate.
      if (!foldConstants(*proofFunction, work)) return stats;
      proofTable.index = proofIndex;
      InvariantRegion checked;
      stats.preservedStores = 0;
      if (!proofTable.index || !verify(*proofFunction, verification) ||
          !closeControlInvariant(*proofFunction, proofGuards, proofTable, request,
                                  proofSP, work, checked) ||
          !preserveWrites(*proofFunction, proofGuards, request, checked, work, nullptr, &cell))
        return stats;
      closed = true;
      break;
    }
    if (!closed) {
      stats.preservedStores = originalStores;
      stats.message = originalMessage + "; seeded candidates=" + std::to_string(cells.size()) +
          diagnostics + (lastFailure.empty() ? "" : "; last: " + lastFailure);
      return stats;
    }
  }
  stats.closed = true;

  // The candidate graph's constants become facts only after the complete
  // base/step proof. Ordinary functions and guard-failure snapshots do not
  // acquire those facts. No assumption instruction or noalias is introduced.
  DominatorTree originalDom(*function);
  if (!substituteGuardedRegisters(*function, guards, work)) { stats.closed = false; return stats; }
  for (auto &[old, proofValue] : facts) {
    if (!work.spend()) { stats.closed = false; return stats; }
    auto *constant = proofValue ? dyn_cast<ConstantInt>(static_cast<Value *>(proofValue)) : nullptr;
    if (!constant) continue;
    bool used = false;
    for (auto it = old->use_begin(); it != old->use_end();) {
      if (!work.spend()) { stats.closed = false; return stats; }
      Use &use = *it++;
      auto *user = dyn_cast<Instruction>(use.getUser());
      if (user && originalDom.dominates(guards.accepted, user->getParent())) {
        use.set(constant); used = true;
      }
    }
    stats.constantValues += used;
  }
  if (consumeClosedCFG) {
    // The same complete proof has established the first lookup and every
    // table-target step, including every lookup's ORIGINAL bounds condition.
    // By induction no accepted execution can take an outside-domain lookup or
    // any old case that is absent from the checked table. Publish precisely
    // that verified candidate CFG. The actual target LOAD still dominates the
    // index switch and its unchanged UnknownTarget snapshot; no assume or
    // unreachable substitutes for an unproved case. Entry misses still call
    // the untouched ordinary function with its full original target switch.
    selectedInstructions = 0;
    for (auto &block : *proofFunction) selectedInstructions += block.size();
    if (proofFunction->size() > request.maxBlocks ||
        selectedInstructions > request.maxInstructions ||
        !work.spend(selectedInstructions + proofModule->global_size() + proofModule->size()) ||
        !verifyWhole(*proofModule, verification)) {
      stats.closed = false;
      if (stats.message.empty()) fail(stats, "closed image-table view exceeds its budget or is invalid: " + verification);
      return stats;
    }
    if (!commit(*source, *proofFunction, stats, forceCommitFailure)) { stats.closed = false; return stats; }
    stats.changed = true;
    stats.message = "complete joint closure consumed; original target read and UnknownTarget exit retained";
    return stats;
  }
  if (!fullFallback(table, work) || !verifyWhole(*trial, verification)) {
    stats.closed = false;
    if (stats.message.empty()) fail(stats, "invalid final table view: " + verification);
    return stats;
  }
  selectedInstructions = 0;
  for (auto &block : *function) selectedInstructions += block.size();
  if (function->size() > request.maxBlocks || selectedInstructions > request.maxInstructions ||
      !work.spend(selectedInstructions + trial->global_size() + trial->size())) {
    stats.closed = false;
    if (stats.message.empty()) fail(stats, "final image-table view exceeds its budget");
    return stats;
  }
  if (!commit(*source, *function, stats, forceCommitFailure)) { stats.closed = false; return stats; }
  stats.changed = true;
  stats.message = "joint table preservation closed; actual index switch retains full fallback";
  return stats;
}
#ifdef DRAGON_IMAGE_TABLE_CONTEXT_TESTING
ImageTableContextStats runImageTableContextCommitFailureForTest(
    Module &module, const ImageTableContextRequest &request,
    bool consumeClosedCFG, unsigned failureStage) {
  return runImageTableContext(module, request, consumeClosedCFG, failureStage);
}
#endif
} // namespace dragon::llvmlifter::detail
