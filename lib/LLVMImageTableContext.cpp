#include "LLVMImageTableContext.hpp"
#include "LLVMPhiLowBits.hpp"
#include "LLVMPathValueRanges.hpp"
#include "LLVMPhiConstant.hpp"
#include "LLVMOpaqueIntegerAdmission.hpp"
#include <dragon/lifters/llvm/DispatchCorridor.hpp>
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
#include <deque>
#include <functional>
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
    if (found == sp.end() || !found->second) return nullptr;
    auto *observed = static_cast<Value *>(found->second);
    // A weak observation is not an SSA use, so guarded-use substitution need
    // not rewrite it when SP stayed equal to its original entry snapshot.
    // Apply only that exact SSA equality in the accepted dominance region.
    if (observed == g.initialSP && dom.dominates(g.accepted, block))
      return ConstantInt::get(cast<IntegerType>(observed->getType()), g.sp);
    return dyn_cast<ConstantInt>(observed);
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
// Private Loop-only scheduling alternative to shadowConstants.
// Requires <deque>; uses the existing ConstantFacts, Work and fail helpers.
// The old shadowConstants body and all of its callers remain unchanged.
//
// No IR/CFG mutation. The whitelist, annotation checks, PHI self-arm skip,
// complete equal-constant PHI rule, and ConstantFoldInstOperands invocation
// below match shadowConstants. New facts are candidate-local and published
// only after this entire worklist closes within the unchanged shared Work.
using LoopConstantPlan =
    std::map<std::size_t, std::pair<WeakVH, ConstantInt *>>;
bool loopShadowConstants(Function &f, ConstantFacts &facts, Work &work,
                         LoopConstantPlan &plan) {
  constexpr std::size_t hardLimit = 65536;
  ConstantFacts discovered;
  std::deque<Instruction *> ready;
  std::set<Instruction *> queued;
  std::map<Instruction *, std::size_t> ordinals;

  auto known = [&](Value *v) -> ConstantInt * {
    if (auto *c = dyn_cast<ConstantInt>(v)) return c;
    if (auto found = facts.find(v); found != facts.end()) return found->second;
    auto found = discovered.find(v);
    return found == discovered.end() ? nullptr : found->second;
  };
  auto candidate = [&](Instruction &i) {
    if (!i.getType()->isIntegerTy() || i.getType()->getIntegerBitWidth() > 64 ||
        facts.contains(&i) || discovered.contains(&i) ||
        i.hasPoisonGeneratingAnnotations()) return false;
    if (isa<PHINode>(i)) return true;
    switch (i.getOpcode()) {
    case Instruction::Add: case Instruction::Sub: case Instruction::Mul:
    case Instruction::And: case Instruction::Or: case Instruction::Xor:
    case Instruction::Shl: case Instruction::LShr: case Instruction::AShr:
    case Instruction::UDiv: case Instruction::SDiv:
    case Instruction::URem: case Instruction::SRem:
    case Instruction::ZExt: case Instruction::SExt: case Instruction::Trunc:
    case Instruction::ICmp: case Instruction::Select: return true;
    default: return false;
    }
  };
  auto enqueue = [&](Instruction *i) {
    if (!work.spend()) return false; // Every queue attempt, including a dedupe.
    if (queued.contains(i)) return true;
    if (ready.size() == hardLimit) {
      work.stats.budgetExhausted = true;
      return fail(work.stats, "loop shadow constant queue cap");
    }
    queued.insert(i);
    ready.push_back(i);
    return true;
  };

  std::size_t scanned = 0;
  for (auto &block : f) for (auto &i : block) {
    if (!work.spend()) return false; // One initial scan of each instruction.
    if (scanned == hardLimit) {
      work.stats.budgetExhausted = true;
      return fail(work.stats, "loop shadow constant instruction cap");
    }
    ordinals.emplace(&i, scanned++);
    if (candidate(i) && !enqueue(&i)) return false;
  }

  while (!ready.empty()) {
    if (!work.spend()) return false; // Every dequeue/evaluation.
    auto *i = ready.front();
    ready.pop_front();
    queued.erase(i);
    if (!candidate(*i)) continue;

    ConstantInt *result = nullptr;
    if (auto *phi = dyn_cast<PHINode>(i)) {
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
      SmallVector<Constant *, 4> operands;
      for (auto &operand : i->operands()) {
        if (!work.spend()) return false;
        auto *c = known(operand.get());
        if (!c) { operands.clear(); break; }
        operands.push_back(c);
      }
      if (operands.size() == i->getNumOperands())
        result = dyn_cast_or_null<ConstantInt>(ConstantFoldInstOperands(
            i, operands, f.getParent()->getDataLayout(), nullptr, false));
    }
    if (!result) continue;

    if (!work.spend()) return false; // A single unknown-to-known transition.
    discovered.emplace(i, result);
    // Only a newly known operand can change this monotone transfer function.
    // Scan every actual use, including duplicate operand slots, charging each
    // one. Only scheduling is deduplicated. Original instruction ordinals,
    // never pointer/use-list order, choose the deterministic enqueue order.
    // PHI incoming coverage above is unchanged.
    std::map<std::size_t, Instruction *> users;
    for (auto &use : i->uses()) {
      if (!work.spend()) return false;
      auto *user = dyn_cast<Instruction>(use.getUser());
      if (!user || user->getFunction() != &f || !candidate(*user)) continue;
      auto ordinal = ordinals.find(user);
      if (ordinal == ordinals.end())
        return fail(work.stats, "loop shadow user is not an original instruction");
      users.emplace(ordinal->second, user);
    }
    for (const auto &[ordinal, user] : users)
      if (!enqueue(user)) return false; // Every unique user is still charged.
  }

  // Freeze a deterministic consumption order while the original ordinals are
  // available. This is still only a hypothesis: no use may change until the
  // complete original write set has closed under this exact cell overlay.
  LoopConstantPlan planned;
  auto remember = [&](Value *value, ConstantInt *constant) {
    if (!work.spend(3)) return false; // lookup, record and eventual retirement
    auto *i = dyn_cast<Instruction>(value);
    auto ordinal = i ? ordinals.find(i) : ordinals.end();
    if (!i || ordinal == ordinals.end() || i->getFunction() != &f ||
        !constant || i->getType() != constant->getType())
      return fail(work.stats,"loop constant plan lost an original typed instruction");
    if (!planned.emplace(ordinal->second,
                         std::pair<WeakVH, ConstantInt *>{i,constant}).second)
      return fail(work.stats,"duplicate original instruction in loop constant plan");
    return true;
  };
  for (const auto &[value, constant] : facts)
    if (!remember(value,constant)) return false;
  for (const auto &[value, constant] : discovered)
    if (!remember(value,constant)) return false;
  // Work stays spent on failure. No partial overlay or plan escapes.
  if (!work.spend(discovered.size()+1)) return false;
  facts.insert(discovered.begin(), discovered.end());
  plan.swap(planned);
  return true;
}

bool consumeLoopConstants(Function &f, const LoopConstantPlan &plan, Work &work) {
  auto spend = [&](std::size_t n = 1) {
    if (work.spend(n)) return true;
    return fail(work.stats,"closed loop constant consumption work exhausted");
  };
  for (const auto &[ordinal, entry] : plan) {
    if (!spend()) return false;
    auto *i = dyn_cast_or_null<Instruction>(static_cast<Value *>(entry.first));
    auto *constant = entry.second;
    if (!i || !i->getParent() || i->getFunction() != &f || !constant ||
        i->getType() != constant->getType() || i->hasPoisonGeneratingAnnotations() ||
        i->isTerminator() || isa<CallBase,FreezeInst>(i))
      return fail(work.stats,"closed loop constant plan changed shape");
    if (auto *load = dyn_cast<LoadInst>(i)) {
      if (!load->isSimple()) return fail(work.stats,"closed loop constant LOAD changed shape");
    } else if (i->mayReadOrWriteMemory())
      return fail(work.stats,"closed loop constant plan contains an effect");
    // Pay for reading each actual use and for RAUW's second visit. Keep all
    // original instructions, effects, branches and their metadata in place.
    for (auto &use : i->uses())
      if (!spend(2)) return false;
    i->replaceAllUsesWith(constant);
  }
  return true;
}

// The loop entry keeps its additional state private. In particular these
// caches never escape one constant-cell hypothesis or one unchanged CFG.
class LoopRanges {
 public:
  LoopRanges(Work &w, Value *base, const ConstantFacts &overlay, PathValueRanges *paths=nullptr)
      : work(w), guest(base), facts(overlay), paths(paths),
        phis(*cast<Instruction>(base)->getFunction(),overlay),
        opaque(*cast<Instruction>(base)->getFunction(),overlay) {}
  std::optional<Range> get(Value *value, Instruction *at) {
    nodes = points = 0; active.clear(); memoryActive.clear(); paidKnownLeaves.clear();
    paidOpaqueShapes.clear();
    return visit(value, 0, true, at);
  }
  // Only an outer STORE query may reset the local proof counters.
  struct WriteSpan { std::uint64_t start, bytes; };
  std::optional<Range> getForWrite(Value *value, StoreInst *store,
                                 std::uint64_t bytes,
                                 const std::vector<WriteSpan> &protectedSpans) {
    // A nested alias must use the inherited-depth helper, NEVER this reset.
    if (!observationTrials.empty() || independentAnchors) return {};
    nodes = points = 0; active.clear(); memoryActive.clear(); paidKnownLeaves.clear();
    paidOpaqueShapes.clear();
    return rangeForWriteAtOriginalStore(value, 0, store, bytes, protectedSpans);
  }
 private:
  Work &work;
  Value *guest;
  const ConstantFacts &facts;
  PathValueRanges *paths;
  PhiConstantGraph phis;
  OpaqueIntegerAdmission opaque;
  unsigned nodes = 0, points = 0;
  // Same-SSA literal/immutable-H_C leaves need one scalar node per root query.
  // This is accounting only: every visit still pays Work and checks depth.
  // Never populate it from Path/Phi, numeric LOAD caches or provisional D.
  std::set<Value *> paidKnownLeaves;
  // Only complete same-root shapes whose footprint this query already paid.
  // A repeated use still pays its root node, Work and the ordinary depth check.
  // This ledger is independent of numeric D assumptions and cache rollback.
  std::map<PHINode *, std::size_t> paidOpaqueShapes;
  std::set<Value *> active;
  std::set<LoadInst *> memoryActive;
  std::map<LoadInst *, Range> loads;
  // Insert in LoopRanges after the existing numeric `loads` map. These are
  // numeric proof transactions only; none is added to ConstantFacts/Path/Phi.
  using ObservationLoadCache = std::map<LoadInst *, Range>;
  enum class ObservationPhase { Collect, Anchors, Aliases };
  struct ObservationTrial {
    LoadInst *load;
    unsigned scalarDepth;
    ObservationPhase phase = ObservationPhase::Collect;
    std::optional<Range> candidate;
  };
  std::vector<ObservationTrial *> observationTrials;
  const ObservationLoadCache *closedLoadsBeforeTrials = nullptr;
  unsigned independentAnchors = 0;

  struct ObservationTransaction {
    LoopRanges &owner;
    ObservationLoadCache entryLoads;
    ObservationTrial trial;
    bool registered = false, pushed = false, outermost = false, keep = false;

    // The caller prepays copying/retiring entryLoads and fixed stack/swap
    // cleanup before this complete snapshot. New cache keys pay retirement
    // on publication. Destructors never depend on remaining Work.
    ObservationTransaction(LoopRanges &o, LoadInst *load, unsigned depth)
        : owner(o), entryLoads(o.loads), trial{load, depth} {}
    bool enter() {
      if (!owner.memoryActive.insert(trial.load).second) return false;
      registered = true;
      owner.observationTrials.push_back(&trial);
      pushed = true;
      if (owner.observationTrials.size() == 1) {
        owner.closedLoadsBeforeTrials = &entryLoads;
        outermost = true;
      }
      return true;
    }
    ~ObservationTransaction() {
      // Includes every successful nested query that depended on any ancestor
      // candidate. Keep spent Work/nodes/points; restore only numeric facts.
      if (!keep) owner.loads.swap(entryLoads);
      if (pushed) owner.observationTrials.pop_back();
      if (registered) owner.memoryActive.erase(trial.load);
      if (outermost) owner.closedLoadsBeforeTrials = nullptr;
    }
  };

  struct IndependentAnchorCache {
    LoopRanges &owner;
    ObservationLoadCache saved;
    explicit IndependentAnchorCache(LoopRanges &o) : owner(o) {
      // Construct the copy BEFORE changing working state. No provisional or
      // conditional numeric fact from any ancestor reaches an anchor query.
      ObservationLoadCache independent(*owner.closedLoadsBeforeTrials);
      saved.swap(owner.loads);
      owner.loads.swap(independent);
      ++owner.independentAnchors;
    }
    ~IndependentAnchorCache() {
      --owner.independentAnchors;
      owner.loads.swap(saved); // Discard all newly computed independent facts.
    }
  };

  struct OriginalStoreScalarScope {
    LoopRanges &owner;
    std::set<Value *> saved;
    explicit OriginalStoreScalarScope(LoopRanges &o) : owner(o) {
      saved.swap(owner.active);
    }
    ~OriginalStoreScalarScope() { owner.active.swap(saved); }
  };
  // State is per original STORE query. Nested anchors/aliases must never
  // inherit or report into their parent's mask demand.
  struct WriteMaskQuery {
    bool strong;
    bool *eligibleSeen;
  };
  WriteMaskQuery *writeMaskQuery = nullptr;
  struct OriginalStoreMaskScope {
    LoopRanges &owner;
    WriteMaskQuery *previous;
    WriteMaskQuery query;
    OriginalStoreMaskScope(LoopRanges &o, bool strong, bool *eligible)
        : owner(o), previous(o.writeMaskQuery),
          query{strong && !o.independentAnchors,
                o.independentAnchors ? nullptr : eligible} {
      owner.writeMaskQuery = &query;
    }
    ~OriginalStoreMaskScope() { owner.writeMaskQuery = previous; }
  };

  std::optional<Range> rangeAtOriginalStore(Value *value, unsigned depth,
                                           StoreInst *store, bool strong = false,
                                           bool *eligibleSeen = nullptr) {
    if (!work.spend()) return {};
    OriginalStoreMaskScope maskScope(*this, strong, eligibleSeen);
    OriginalStoreScalarScope scalarScope(*this);
    return visit(value, depth, true, store);
  }

  bool writeSpanDisjoint(const Range &address, std::uint64_t bytes,
                        const std::vector<WriteSpan> &protectedSpans) const {
    if (address.bits != 64 || !bytes || address.low > address.high ||
        bytes > UINT64_MAX - address.high) return false;
    for (const auto &span : protectedSpans)
      if (!(address.high + bytes <= span.start ||
            address.low >= span.start + span.bytes)) return false;
    return true;
  }

  std::optional<Range> rangeForWriteAtOriginalStore(
      Value *value, unsigned depth, StoreInst *store, std::uint64_t bytes,
      const std::vector<WriteSpan> &protectedSpans) {
    // The existing caller still pays its per-STORE/alias obligation fee.
    // At most T+C, or one exact D span, are accepted by this private helper.
    if (!bytes || protectedSpans.empty() || protectedSpans.size() > 2) return {};
    for (const auto &span : protectedSpans)
      if (!span.bytes || span.bytes > UINT64_MAX - span.start) return {};
    bool eligible = false;
    auto coarse = rangeAtOriginalStore(value, depth, store, false, &eligible);
    if (!coarse || work.stats.budgetExhausted || coarse->bits != 64) return {};
    if (writeSpanDisjoint(*coarse, bytes, protectedSpans)) return coarse;
    if (!eligible || independentAnchors) return {};
    // Only a valid ambiguous span retries. No second get(), no node/point/
    // depth/Work reset, and no retry after a hazard/null/budget failure.
    if (!work.spend()) return {};
    auto strong = rangeAtOriginalStore(value, depth, store, true);
    if (!strong || work.stats.budgetExhausted ||
        !writeSpanDisjoint(*strong, bytes, protectedSpans)) return {};
    return strong;
  }
  bool budget(unsigned depth, Value *value, Instruction *at) {
    if (!work.spend()) return false;
    if (depth <= 32) {
      const bool knownLeaf = value->getType()->isIntegerTy() &&
          value->getType()->getIntegerBitWidth() <= 64 &&
          !isa<UndefValue, PoisonValue>(value) && known(value);
      if ((knownLeaf && !paidKnownLeaves.insert(value).second) || ++nodes <= 128)
        return true;
    }
    work.stats.budgetExhausted = true;
    std::string operand;
    raw_string_ostream stream(operand);
    value->printAsOperand(stream,false);
    return fail(work.stats, "loop local range expression cap: value="+operand+
        " point="+(at ? at->getParent()->getName().str() : std::string("absent"))+
        " depth="+std::to_string(depth)+" nodes="+std::to_string(nodes));
  }
  ConstantInt *known(Value *v) const {
    if (auto *c = dyn_cast<ConstantInt>(v)) return c;
    auto found = facts.find(v);
    return found == facts.end() ? nullptr : found->second;
  }
  // Replace LoopRanges::loadRange. Keep the former strict implementation as
  // independentLoadRange (the companion file changes its name only).
  std::optional<Range> loadRange(LoadInst *load, unsigned scalarDepth) {
    if (!work.spend() || !load->isSimple() ||
        load->hasPoisonGeneratingAnnotations() ||
        !load->getType()->isIntegerTy() ||
        load->getType()->getIntegerBitWidth() > 64) return {};
    // The exact original SSA observation is the key. Another LOAD of the same
    // cell must complete its OWN initialization graph and independent anchors.
    for (auto *trial : observationTrials) {
      if (!work.spend()) return {};
      if (trial->load != load) continue;
      if (!independentAnchors && trial->phase == ObservationPhase::Aliases)
        return trial->candidate;
      return {}; // All active assumptions are disabled while ranging anchors.
    }
    if (auto found = loads.find(load); found != loads.end()) return found->second;
    if (independentAnchors) {
      // Reserve retirement of a newly inserted scratch-cache key as well as
      // the strict resolver's existing publication fee. No cleanup may ask
      // for more Work after a budget failure.
      if (!work.spend()) return {};
      return independentLoadRange(load, scalarDepth); // Cannot open a new trial.
    }

    const unsigned bits = load->getType()->getIntegerBitWidth();
    auto at = fixedGuestWord(load->getPointerOperand(), guest);
    if (!at || !bits || bits % 8 || bits / 8 > UINT64_MAX - *at ||
        memoryActive.contains(load)) return {};
    // Snapshot visits and activation/rollback are prepaid. Restoring by swap
    // cannot fail even when a later proof consumes the last unit of Work.
    if (!work.spend(2 * loads.size() + 8)) return {};
    ObservationTransaction transaction(*this, load, scalarDepth);
    if (!transaction.enter()) return {};

    using Point = std::pair<BasicBlock *, Instruction *>; // nullptr is END.
    struct DefinitionNode {
      Point start;
      std::vector<std::size_t> dependencies;
      StoreInst *anchor = nullptr;
    };
    std::vector<DefinitionNode> graph;
    std::map<Point, std::size_t> owners;
    std::set<StoreInst *> deferred, anchors;
    std::vector<StoreInst *> deferredOrder, anchorOrder;
    auto addNode = [&](Point point) -> std::optional<std::size_t> {
      if (!work.spend()) return {};
      if (auto found = owners.find(point); found != owners.end()) return found->second;
      if (++points > 128) {
        work.stats.budgetExhausted = true;
        fail(work.stats, "loop nearest-STORE graph cap: cell=" +
             std::to_string(*at) + " block=" + point.first->getName().str() +
             " points=" + std::to_string(points));
        return {};
      }
      const auto n = graph.size();
      graph.push_back({point, {}, nullptr});
      owners.emplace(point, n);
      return n;
    };
    if (!addNode({load->getParent(), load})) return {};
    for (std::size_t n = 0; n < graph.size(); ++n) {
      Point point = graph[n].start;
      std::set<Point> local;
      for (;;) {
        if (!work.spend()) return {};
        auto [owner, inserted] = owners.emplace(point, n);
        if ((!inserted && owner->second != n) || !local.insert(point).second) {
          if (!work.spend()) return {};
          graph[n].dependencies.push_back(owner->second);
          break;
        }
        auto *block = point.first;
        auto it = point.second ? point.second->getIterator() : block->end();
        StoreInst *anchor = nullptr;
        while (it != block->begin()) {
          auto &instruction = *--it;
          if (!work.spend()) return {};
          if (auto *store = dyn_cast<StoreInst>(&instruction)) {
            if (!store->isSimple() || !store->getValueOperand()->getType()->isIntegerTy())
              return {};
            const auto width = store->getValueOperand()->getType()->getIntegerBitWidth();
            auto address = fixedGuestWord(store->getPointerOperand(), guest);
            if (address && *address == *at && width == bits) {
              anchor = store;
              break; // Earlier clobbers are outside this observation segment.
            }
            auto *gep = dyn_cast<GetElementPtrInst>(store->getPointerOperand());
            if (!width || width % 8 || !gep || gep->getPointerOperand() != guest ||
                gep->getNumIndices() != 1 || !gep->getSourceElementType()->isIntegerTy(8) ||
                gep->hasPoisonGeneratingAnnotations()) return {};
            if (address) {
              if (!work.spend() || width / 8 > UINT64_MAX - *address ||
                  !(*address + width / 8 <= *at || *address >= *at + bits / 8))
                return {}; // Any fixed partial overlap or wrapped span refuses.
            } else {
              if (!work.spend()) return {};
              // Every encounter is paid above; unique obligations follow the
              // existing actual graph scan order, never pointer-set order.
              if (deferred.insert(store).second) deferredOrder.push_back(store);
            }
          } else if (auto *call = dyn_cast<CallBase>(&instruction)) {
            if (!mathematicalCall(*call)) return {};
          } else if (auto *read = dyn_cast<LoadInst>(&instruction)) {
            if (!read->isSimple()) return {};
          } else if (instruction.mayWriteToMemory() || isa<FenceInst>(instruction))
            return {};
        }
        if (anchor) {
          graph[n].anchor = anchor;
          break;
        }
        std::vector<BasicBlock *> incoming;
        for (auto *pred : predecessors(block)) {
          if (!work.spend()) return {};
          incoming.push_back(pred); // Every actual slot, duplicates included.
        }
        if (incoming.empty()) return {}; // Entry with no complete definition.
        if (incoming.size() == 1) {
          point = {incoming.front(), nullptr};
          continue; // Every instruction in the compressed segment is charged.
        }
        for (auto *pred : incoming) {
          auto dependency = addNode({pred, nullptr});
          if (!dependency || !work.spend()) return {};
          graph[n].dependencies.push_back(*dependency);
        }
        break;
      }
    }

    // This coverage check is structural: no candidate range can short-circuit
    // it. ALL collected nodes must reach real nearest full-STORE anchors.
    std::vector<std::vector<std::size_t>> reverse(graph.size());
    std::vector<bool> anchored(graph.size(), false);
    std::vector<std::size_t> pending;
    for (std::size_t n = 0; n < graph.size(); ++n) {
      if (!work.spend()) return {};
      for (auto dependency : graph[n].dependencies) {
        if (!work.spend()) return {};
        reverse[dependency].push_back(n);
      }
      if (graph[n].anchor) {
        if (anchors.insert(graph[n].anchor).second)
          anchorOrder.push_back(graph[n].anchor);
        anchored[n] = true;
        pending.push_back(n);
      }
    }
    for (std::size_t n = 0; n < pending.size(); ++n) {
      if (!work.spend()) return {};
      for (auto dependent : reverse[pending[n]]) {
        if (!work.spend()) return {};
        if (!anchored[dependent]) {
          if (!work.spend()) return {};
          anchored[dependent] = true;
          pending.push_back(dependent);
        }
      }
    }
    for (bool covered : anchored) {
      if (!work.spend() || !covered) return {};
    }

    transaction.trial.phase = ObservationPhase::Anchors;
    std::optional<Range> hull;
    if (!closedLoadsBeforeTrials ||
        !work.spend(2 * closedLoadsBeforeTrials->size() + 8)) return {};
    {
      IndependentAnchorCache independent(*this);
      for (auto *store : anchorOrder) {
        if (!work.spend()) return {};
        auto value = rangeAtOriginalStore(store->getValueOperand(), scalarDepth + 1, store);
        if (!value || value->bits != bits) return {};
        if (!hull) hull = value;
        else {
          hull->low = std::min(hull->low, value->low);
          hull->high = std::max(hull->high, value->high);
        }
      }
    } // Restore parent conditional caches before any alias assumption is enabled.
    if (!hull || !work.spend()) return {};
    transaction.trial.candidate = hull;
    transaction.trial.phase = ObservationPhase::Aliases;
    for (auto *store : deferredOrder) {
      if (!work.spend()) return {};
      auto *gep = dyn_cast<GetElementPtrInst>(store->getPointerOperand());
      if (!gep) return {}; // Pure range helpers must not mutate source IR.
      const auto width = store->getValueOperand()->getType()->getIntegerBitWidth();
      auto interval = rangeForWriteAtOriginalStore(
          gep->idx_begin()->get(), scalarDepth + 1, store, width/8,
          {{*at, bits/8}});
      if (!interval) return {};
    }
    // This exact observation (and conditional nested successes) may escape only
    // after every structural/anchor/alias obligation passes. On any earlier
    // return, the transaction restores its complete numeric cache snapshot.
    if (!work.spend(2)) return {}; // Insertion plus eventual cache retirement.
    loads[load] = *hull;
    transaction.keep = true;
    return hull;
  }
// Exact current strict resolver, renamed only. Used solely with all provisional facts disabled.
  std::optional<Range> independentLoadRange(LoadInst *load, unsigned scalarDepth) {
    if (!load->isSimple() || load->hasPoisonGeneratingAnnotations()) return {};
    if (auto found = loads.find(load); found != loads.end()) return found->second;
    const unsigned bits = load->getType()->getIntegerBitWidth();
    auto at = fixedGuestWord(load->getPointerOperand(), guest);
    if (!at || bits % 8 || bits / 8 > UINT64_MAX - *at ||
        !memoryActive.insert(load).second) return {};
    auto done = [&](std::optional<Range> result) {
      memoryActive.erase(load);
      // Only this original LOAD observation is cached, and only after every
      // actual predecessor path and anchor value has closed successfully.
      if (result) loads.emplace(load, *result);
      return result;
    };

    // A prefix before this LOAD is distinct from the END of the same block.
    // Graph edges run BACKWARD through the actual CFG. A compressed segment
    // contains no write to the cell and ends at a join or its nearest STORE.
    using Point = std::pair<BasicBlock *, Instruction *>; // nullptr means END.
    struct DefinitionNode {
      Point start;
      std::vector<std::size_t> dependencies;
      StoreInst *anchor = nullptr;
    };
    std::vector<DefinitionNode> graph;
    std::map<Point, std::size_t> owners;
    auto addNode = [&](Point point) -> std::optional<std::size_t> {
      if (!work.spend()) return {};
      if (auto found = owners.find(point); found != owners.end()) return found->second;
      // points is the PARENT member: nested value/address LOAD queries consume
      // the same remaining 128 definition nodes. Never reset it here.
      if (++points > 128) {
        work.stats.budgetExhausted = true;
        fail(work.stats, "loop nearest-STORE graph cap: cell="+std::to_string(*at)+
            " block="+point.first->getName().str()+" points="+std::to_string(points));
        return {};
      }
      const auto index = graph.size();
      graph.push_back({point, {}, nullptr});
      owners.emplace(point, index);
      return index;
    };
    if (!addNode({load->getParent(), load})) return done({});
    for (std::size_t n = 0; n < graph.size(); ++n) {
      Point point = graph[n].start;
      std::set<Point> local;
      for (;;) {
        if (!work.spend()) return done({}); // every corridor Point
        auto [owner, inserted] = owners.emplace(point, n);
        if ((!inserted && owner->second != n) || !local.insert(point).second) {
          // Structural memoization only. Its owner is either already fully
          // scanned or still in this bounded queue; success checks ALL nodes.
          if (!work.spend()) return done({});
          graph[n].dependencies.push_back(owner->second);
          break;
        }
        auto *block = point.first;
        auto it = point.second ? point.second->getIterator() : block->end();
        StoreInst *anchor = nullptr;
        while (it != block->begin()) {
          auto &instruction = *--it;
          if (!work.spend()) return done({}); // every original instruction
          if (auto *store = dyn_cast<StoreInst>(&instruction)) {
            if (!store->isSimple() || !store->getValueOperand()->getType()->isIntegerTy())
              return done({});
            const auto width = store->getValueOperand()->getType()->getIntegerBitWidth();
            auto address = fixedGuestWord(store->getPointerOperand(), guest);
            if (address && *address == *at && width == bits) {
              anchor = store;
              break; // nearest complete definition cuts off all older effects.
            }
            auto *gep = dyn_cast<GetElementPtrInst>(store->getPointerOperand());
            if (!width || width % 8 || !gep || gep->getPointerOperand() != guest ||
                gep->getNumIndices() != 1 || !gep->getSourceElementType()->isIntegerTy(8) ||
                gep->hasPoisonGeneratingAnnotations()) return done({});
            if (address) {
              // fixedGuestWord already proved this unflagged byte GEP's
              // literal address. Check the complete span directly instead of
              // expanding that same structural literal as a scalar query.
              // Every original write and this interval check are still paid.
              if (!work.spend() || width / 8 > UINT64_MAX - *address ||
                  !(*address + width / 8 <= *at || *address >= *at + bits / 8))
                return done({});
              continue;
            }
            // Test at the ORIGINAL STORE, with the original scalar depth.
            // No final/lookup range may hide a clobber at an intermediate write.
            auto interval = visit(gep->idx_begin()->get(), scalarDepth + 1, true, store);
            if (!interval || interval->bits != 64 ||
                width / 8 > UINT64_MAX - interval->high ||
                !(interval->high + width / 8 <= *at || interval->low >= *at + bits / 8))
              return done({});
          } else if (auto *call = dyn_cast<CallBase>(&instruction)) {
            if (!mathematicalCall(*call)) return done({});
          } else if (auto *read = dyn_cast<LoadInst>(&instruction)) {
            if (!read->isSimple()) return done({});
          } else if (instruction.mayWriteToMemory() || isa<FenceInst>(instruction))
            return done({});
        }
        if (anchor) {
          graph[n].anchor = anchor;
          break;
        }
        std::vector<BasicBlock *> incoming;
        // Enumerate/charge every ACTUAL predecessor SLOT, including duplicates,
        // before compression or an owner-cache lookup can reuse structure.
        for (auto *pred : predecessors(block)) {
          if (!work.spend()) return done({});
          incoming.push_back(pred);
        }
        if (incoming.empty()) return done({}); // entry path with no definition
        if (incoming.size() == 1) {
          point = {incoming.front(), nullptr};
          continue; // read-only single-predecessor corridor, fully scanned above.
        }
        for (auto *pred : incoming) {
          auto dependency = addNode({pred, nullptr});
          if (!dependency || !work.spend()) return done({});
          // addNode may reallocate graph; retain no DefinitionNode reference.
          graph[n].dependencies.push_back(*dependency);
        }
        break;
      }
    }

    // Every collected node must lead backward to a real nearest STORE. A root
    // anchor in one arm does not validate an unanchored dependency sub-SCC.
    std::vector<std::vector<std::size_t>> reverse(graph.size());
    std::vector<bool> anchored(graph.size(), false);
    std::vector<std::size_t> pending;
    std::optional<Range> result;
    for (std::size_t n = 0; n < graph.size(); ++n) {
      if (!work.spend()) return done({});
      for (auto dependency : graph[n].dependencies) {
        if (!work.spend()) return done({});
        reverse[dependency].push_back(n);
      }
      if (auto *store = graph[n].anchor) {
        // An anchor's original SSA may itself depend on an older LOAD. Such
        // queries share scalar nodes/depth/points/Work and memoryActive;
        // recursively depending on this unresolved LOAD still refuses.
        auto value = visit(store->getValueOperand(), scalarDepth + 1, true, store);
        if (!value || value->bits != bits) return done({});
        if (!work.spend()) return done({});
        if (!result) result = value;
        else {
          result->low = std::min(result->low, value->low);
          result->high = std::max(result->high, value->high);
        }
        anchored[n] = true;
        pending.push_back(n);
      }
    }
    for (std::size_t n = 0; n < pending.size(); ++n) {
      if (!work.spend()) return done({});
      for (auto dependent : reverse[pending[n]]) {
        if (!work.spend()) return done({});
        if (!anchored[dependent]) {
          if (!work.spend()) return done({});
          anchored[dependent] = true;
          pending.push_back(dependent);
        }
      }
    }
    for (bool reachable : anchored) {
      if (!work.spend()) return done({});
      if (!reachable) return done({});
    }
    if (!work.spend()) return done({}); // LOAD cache publication is prepaid.
    return done(result);
  }
  std::optional<Range> visit(Value *v, unsigned depth, bool resolveMemory, Instruction *at) {
    if (!budget(depth,v,at) || !v->getType()->isIntegerTy() ||
        v->getType()->getIntegerBitWidth() > 64 || isa<UndefValue, PoisonValue>(v)) return {};
    const auto bits = v->getType()->getIntegerBitWidth();
    const Range full{0, mask(bits), bits};
    if (auto *c = known(v)) return Range{c->getZExtValue(), c->getZExtValue(), bits};
    if (paths) {
      const auto before=paths->work();
      auto bounded=paths->get(v,at,work.limit-work.stats.work);
      if (!work.spend(paths->work()-before)) return {};
      if (paths->budgetExhausted()) {
        work.stats.budgetExhausted=true;
        fail(work.stats,"loop path-value proof work exhausted");return {};
      }
      if (bounded) return Range{bounded->low,bounded->high,bounded->bits};
    }
    if (auto *phi=dyn_cast<PHINode>(v)) {
      auto constant=phis.get(phi,work.limit-work.stats.work);
      if (!work.spend(constant.work)) return {};
      if (constant.budgetExhausted) {
        work.stats.budgetExhausted=true;
        fail(work.stats,constant.message);return {};
      }
      if (constant.value) {
        const auto integer=constant.value->getZExtValue();
        return Range{integer,integer,bits};
      }
      if (!resolveMemory) {
        // A mask operand or an unused select truth needs ordinary bits only,
        // not a loop value or a stable observation. Validate its whole shape
        // iteratively; retain the original operations and every consume. A
        // direct PHI-only copying region is one structural shape node; every
        // physical PHI and actual slot is nevertheless checked and charged.
        const auto paid=paidOpaqueShapes.find(phi);
        const auto allowance=paid==paidOpaqueShapes.end() ? 128-nodes+1 : paid->second;
        auto shape=opaque.getPhiRegions(phi,allowance,work.limit-work.stats.work);
        if (!work.spend(shape.work)) return {};
        if (shape.budgetExhausted) {
          work.stats.budgetExhausted=true;
          fail(work.stats,shape.message);return {};
        }
        if (!shape.admitted) return {};
        if (!shape.uniqueNodes || shape.uniqueNodes>allowance ||
            (paid!=paidOpaqueShapes.end() && shape.uniqueNodes!=paid->second)) {
          fail(work.stats,"loop opaque shape footprint changed within one query");return {};
        }
        if (paid==paidOpaqueShapes.end()) {
          nodes+=shape.uniqueNodes-1; // this use's root node was already paid.
          paidOpaqueShapes.emplace(phi,shape.uniqueNodes);
        }
        return full;
      }
    }
    if (isa<FreezeInst>(v)) return full;
    // Memory-definition gray cycles must fail, rather than becoming scalar Top.
    if (auto *load = dyn_cast<LoadInst>(v)) {
      if (!load->isSimple()) return {};
      if (!resolveMemory) return full;
      if (load->hasPoisonGeneratingAnnotations()) return {};
      // An ordinary integer observation needs no content proof to have its
      // type's full bit range. This is NOT an address/disjointness fact: the
      // original LOAD and every memory effect remain. Only fixed Guest cells
      // may gain a narrower interval from their complete definition history.
      if (!fixedGuestWord(load->getPointerOperand(), guest)) return full;
      return loadRange(load, depth);
    }
    if (!active.insert(v).second) return full;
    auto done = [&](std::optional<Range> result) { active.erase(v); return result; };
    auto *i = dyn_cast<Instruction>(v);
    if (!i) return done(full);
    if (auto *cast = dyn_cast<CastInst>(i)) {
      auto a = visit(cast->getOperand(0), depth + 1, resolveMemory, at);
      if (!a) return done({});
      if (cast->getOpcode() == Instruction::ZExt ||
          (cast->getOpcode() == Instruction::Trunc && a->high <= mask(bits)) ||
          (cast->getOpcode() == Instruction::SExt && a->high <= (mask(a->bits)>>1)))
        return done(Range{a->low, a->high, bits});
      if (cast->getOpcode() == Instruction::Trunc || cast->getOpcode() == Instruction::SExt)
        return done(full);
      return done({});
    }
    if (auto *phi = dyn_cast<PHINode>(i)) {
      Range result{mask(bits), 0, bits};
      for (unsigned slot=0;slot<phi->getNumIncomingValues();++slot) {
        // A later LOAD's guard cannot constrain an older incoming observation.
        auto a = visit(phi->getIncomingValue(slot), depth + 1, resolveMemory,
                       phi->getIncomingBlock(slot)->getTerminator());
        if (!a) return done({});
        result.low = std::min(result.low, a->low); result.high = std::max(result.high, a->high);
      }
      return done(result.low <= result.high ? std::optional(result) : std::nullopt);
    }
    if (auto *select = dyn_cast<SelectInst>(i)) {
      // Its truth value is unnecessary for the union of BOTH value arms.
      auto c = visit(select->getCondition(), depth + 1, false, at);
      auto a = visit(select->getTrueValue(), depth + 1, resolveMemory, at);
      auto b = visit(select->getFalseValue(), depth + 1, resolveMemory, at);
      return done(c && a && b ? std::optional(Range{std::min(a->low,b->low),std::max(a->high,b->high),bits}) : std::nullopt);
    }
    auto *binary = dyn_cast<BinaryOperator>(i);
    if (!binary) {
      for (auto &operand : i->operands())
        if (operand->getType()->isIntegerTy() && !visit(operand.get(), depth + 1, false, at)) return done({});
      return done(full);
    }
    // A real mask bounds its result without needing a range of the old cell.
    // Still traverse BOTH operands for visible poison/undef/illegal shifts.
    auto *constantMask = binary->getOpcode() == Instruction::And
        ? known(binary->getOperand(0)) : nullptr;
    if (!constantMask && binary->getOpcode() == Instruction::And)
      constantMask = known(binary->getOperand(1));
    const bool masked = constantMask != nullptr;
    const bool lowDiscarding = masked && !constantMask->isZero() &&
                               !(constantMask->getZExtValue() & 1);
    if (resolveMemory && lowDiscarding && writeMaskQuery &&
        !writeMaskQuery->strong && writeMaskQuery->eligibleSeen && !independentAnchors)
      *writeMaskQuery->eligibleSeen = true;
    const bool refineMask = resolveMemory && lowDiscarding && writeMaskQuery &&
                            writeMaskQuery->strong && !independentAnchors;
    const bool childMemory = resolveMemory && (!masked || refineMask);
    // Both operand/hazard traversals remain. All low-ones masks still use
    // Coarse even within a strong outer expression; they continue to cut
    // irrelevant memory-value cycles as before.
    auto a = visit(binary->getOperand(0), depth + 1, childMemory, at);
    auto b = visit(binary->getOperand(1), depth + 1, childMemory, at);
    if (!a || !b) return done({});

    switch (binary->getOpcode()) {
    case Instruction::And:
      if (a->low == a->high)
        return done(Range{0, std::min(a->high, b->high), bits});
      if (b->low == b->high)
        return done(Range{0, std::min(a->high, b->high), bits});
      return done(full);
    case Instruction::Or: case Instruction::Xor: {
      auto cover = [](std::uint64_t x) { return x == UINT64_MAX ? x : mask(std::bit_width(x)); };
      return done(Range{0,cover(a->high)|cover(b->high),bits});
    }
    case Instruction::Add:
      if (a->high <= mask(bits)-b->high) return done(Range{a->low+b->low,a->high+b->high,bits});
      return done(full);
    case Instruction::Sub:
      if (a->low >= b->high) return done(Range{a->low-b->high,a->high-b->low,bits});
      return done(full);
    case Instruction::Shl: case Instruction::LShr: case Instruction::AShr:
      // A bounded count need not be a singleton. Every possible count must
      // still be legal; retain the original shift and its poison/flag uses.
      if (b->low > b->high || b->high >= bits) return done({});
      if (binary->getOpcode() == Instruction::LShr)
        return done(Range{a->low>>b->high,a->high>>b->low,bits});
      if (binary->getOpcode() == Instruction::Shl && a->high <= (mask(bits)>>b->high))
        return done(Range{a->low<<b->low,a->high<<b->high,bits});
      return done(full);
    default: return done(full);
    }
  }
};

constexpr StringLiteral LoopAnchorKind = "dragon.loop.image.private";
enum LoopAnchor : unsigned { LoopGuest = 1, LoopAccepted, LoopLookup, LoopGuard };
void markLoop(Instruction &i, LoopAnchor role) {
  i.setMetadata(LoopAnchorKind, MDNode::get(i.getContext(),
      {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(i.getContext()), role))}));
}
bool loopRegion(Function &f, const Guards &g, const std::vector<LoadInst *> &cuts,
                Work &work, InvariantRegion &region) {
  std::vector<BasicBlock *> pending{g.accepted};
  for (std::size_t n=0; n<pending.size(); ++n) {
    auto *block=pending[n];
    if (!work.spend() || !region.reachable.insert(block).second) {
      if (work.stats.budgetExhausted) return false;
      continue;
    }
    if ((g.chain.contains(block) && block!=g.accepted) || block==&f.getEntryBlock() ||
        block->isEHPad() || block->hasAddressTaken() ||
        !isa<BranchInst,SwitchInst,ReturnInst>(block->getTerminator()))
      return fail(work.stats,"unsupported loop controller or guard reentry");
    for (auto *next:successors(block)) pending.push_back(next);
  }
  for (auto *cut:cuts) if (region.reachable.contains(cut->getParent())) region.preserving.insert(cut->getParent());
  std::vector<BasicBlock *> reverse(region.preserving.begin(),region.preserving.end());
  for (std::size_t n=0;n<reverse.size();++n) for (auto *pred:predecessors(reverse[n])) {
    if (!work.spend()) return false;
    if (region.reachable.contains(pred) && region.preserving.insert(pred).second) reverse.push_back(pred);
  }
  return !region.preserving.empty() || fail(work.stats,"loop has no reachable table cutpoint");
}
struct LoopEffectIdentity {
  WeakVH instruction;
  BasicBlock *block;
  unsigned opcode;
  Type *type, *effectType;
};
struct LoopInventory {
  std::size_t instructions = 0;
  std::vector<std::pair<WeakVH,MDNode *>> markers;
  std::vector<LoopEffectIdentity> effects;
};
bool isLoopEffect(const Instruction &i) {
  return isa<CallBase,LoadInst,StoreInst,FenceInst>(i) || i.mayWriteToMemory();
}
Type *loopEffectType(Instruction &i) {
  if (auto *store=dyn_cast<StoreInst>(&i)) return store->getValueOperand()->getType();
  if (auto *call=dyn_cast<CallBase>(&i)) return call->getFunctionType();
  return i.getType();
}
Instruction *checkedLoopEffect(Function &f, const LoopEffectIdentity &identity, Work &work) {
  if (!work.spend()) return nullptr;
  auto *i=dyn_cast_or_null<Instruction>(static_cast<Value *>(identity.instruction));
  if (!i || i->getParent()!=identity.block || i->getFunction()!=&f ||
      i->getOpcode()!=identity.opcode || i->getType()!=identity.type ||
      loopEffectType(*i)!=identity.effectType || !isLoopEffect(*i)) {
    fail(work.stats,"loop effect inventory changed after recovery");return nullptr;
  }
  return i;
}
bool checkLoopEffect(Instruction &i, const Guards &g, const ImageTableContextRequest &r,
                     Work &work, LoopRanges &ranges, const SeededCell *cell) {
    if (auto *call=dyn_cast<CallBase>(&i)) {
      if (!mathematicalCall(*call)) return fail(work.stats,"loop call has no preservation proof");
      return true;
    }
    if (auto *load=dyn_cast<LoadInst>(&i)) {
      if (!load->isSimple()) return fail(work.stats,"observable loop read");
      return true;
    }
    auto *store=dyn_cast<StoreInst>(&i);
    if (!store) {
      if (i.mayWriteToMemory() || isa<FenceInst>(i)) return fail(work.stats,"unknown loop memory effect");
      return true;
    }
    auto *gep=dyn_cast<GetElementPtrInst>(store->getPointerOperand());
    if (!store->isSimple() || !store->getValueOperand()->getType()->isIntegerTy() ||
        !gep || gep->getPointerOperand()!=g.guest || gep->getNumIndices()!=1 ||
        !gep->getSourceElementType()->isIntegerTy(8)) return fail(work.stats,"unclassified loop write");
    const auto bits=store->getValueOperand()->getType()->getIntegerBitWidth();
    if (!bits || bits % 8)
      return fail(work.stats,"loop STORE has a non-byte width");
    std::vector<LoopRanges::WriteSpan> protectedSpans{
        {r.tableOffset, r.targets.size()*8}};
    if (cell && store != cell->seed)
      protectedSpans.push_back({cell->address, cell->bits/8});
    auto at = ranges.getForWrite(gep->idx_begin()->get(), store, bits/8, protectedSpans);
    if (!at && work.stats.budgetExhausted) return false;
    if (!at)
      return fail(work.stats,"loop STORE may overwrite a protected table or cell byte");
    ++work.stats.preservedStores;
    return true;
}
bool loopWrites(Function &f, const Guards &g, const ImageTableContextRequest &r,
                const InvariantRegion &region, const LoopInventory &inventory,
                Work &work, LoopRanges &ranges, const SeededCell *cell=nullptr) {
  for (const auto &identity:inventory.effects) {
    auto *i=checkedLoopEffect(f,identity,work);
    if (!i) return false;
    if (region.preserving.contains(i->getParent()) &&
        !checkLoopEffect(*i,g,r,work,ranges,cell)) return false;
  }
  return true;
}
bool projectLoopAddresses(Function &f, const Guards &g, const InvariantRegion &region,
                          const LoopInventory &inventory, Work &work,
                          std::vector<WeakTrackingVH> &witnesses) {
  std::vector<Value *> pending;
  std::vector<TruncInst *> candidates;
  std::set<Value *> seen;
  for (const auto &identity:inventory.effects) {
    auto *i=checkedLoopEffect(f,identity,work);
    if (!i) return false;
    if (!region.preserving.contains(i->getParent())) continue;
    auto *store=dyn_cast<StoreInst>(i);
    auto *gep=store ? dyn_cast<GetElementPtrInst>(store->getPointerOperand()) : nullptr;
    if (gep && gep->getPointerOperand()==g.guest && gep->getNumIndices()==1)
      pending.push_back(gep->idx_begin()->get());
  }
  for (std::size_t n=0;n<pending.size();++n) {
    if (!work.spend()) return false;
    auto *value=pending[n];
    if (!seen.insert(value).second) continue;
    if (seen.size()>4096) {
      work.stats.budgetExhausted=true;
      return fail(work.stats,"loop address-dependency discovery cap");
    }
    if (auto *trunc=dyn_cast<TruncInst>(value)) {
      if (candidates.size()==128) {
        work.stats.budgetExhausted=true;
        return fail(work.stats,"loop address projection candidate cap");
      }
      candidates.push_back(trunc);
      continue; // only this explicit original observation, not its internals.
    }
    if (isa<CastInst,BinaryOperator,PHINode,SelectInst>(value))
      for (auto &operand:cast<Instruction>(value)->operands())
        if (operand->getType()->isIntegerTy()) pending.push_back(operand.get());
  }
  std::set<PHINode *> exported;
  for (auto *trunc:candidates) {
    if (!work.spend()) return false;
    // Cheap shape admission avoids repeatedly snapshotting a large function
    // for ordinary LOAD/constant truncations. This creates no proof fact;
    // every admitted pair is rechecked in full by the OriginalIR helper.
    Value *root=trunc->getOperand(0);
    std::set<Value *> roots;
    for (unsigned depth=0;depth<32 && roots.insert(root).second;++depth) {
      if (!work.spend()) return false;
      if (auto *phi=dyn_cast<PHINode>(root);phi && phi->getNumIncomingValues()==1) {
        root=phi->getIncomingValue(0);continue;
      }
      auto *cast=dyn_cast<CastInst>(root);
      if (cast && isa<TruncInst,ZExtInst,SExtInst>(cast) &&
          cast->getOperand(0)->getType()->isIntegerTy() &&
          cast->getOperand(0)->getType()->getIntegerBitWidth()>=trunc->getType()->getIntegerBitWidth()) {
        root=cast->getOperand(0);continue;
      }
      break;
    }
    auto *large=dyn_cast<PHINode>(root);
    bool hasSmall=false, pureUses=true;
    if (large) for (auto &phi:large->getParent()->phis()) {
      if (!work.spend()) return false;
      hasSmall |= phi.getType()==trunc->getType();
    }
    for (auto *user:trunc->users()) {
      if (!work.spend()) return false;
      auto *i=dyn_cast<Instruction>(user);
      pureUses &= i && !i->isTerminator() && !isa<CallBase,AllocaInst>(i) &&
                  !i->mayReadOrWriteMemory() && !i->mayHaveSideEffects();
    }
    if (!hasSmall || !pureUses) continue;
    PhiLowBitsOptions limits;
    limits.maxWork=std::min(limits.maxWork,work.limit-work.stats.work);
    if (!limits.maxWork) {
      work.stats.budgetExhausted=true;
      return fail(work.stats,"loop address projection has no remaining work");
    }
    auto proof=replacePhiLowTruncs(f,{trunc},limits);
    if (!work.spend(proof.work)) return false;
    if (proof.budgetExhausted) {
      work.stats.budgetExhausted=true;
      return fail(work.stats,"loop address projection exhausted: "+proof.message);
    }
    if (!proof.complete) continue; // no mutation and no exported partial fact.
    for (auto *phi:proof.provedLowPhis)
      if (exported.insert(phi).second) witnesses.emplace_back(phi);
  }
  return true;
}
std::unique_ptr<PathValueRanges> loopPathRanges(Function &f,
    const std::vector<WeakTrackingVH> &witnesses, Work &work) {
  std::vector<Value *> stable;
  for (const auto &handle:witnesses)
    if (auto *phi=dyn_cast_or_null<PHINode>(static_cast<Value *>(handle))) stable.push_back(phi);
  if (stable.empty()) return {};
  auto paths=std::make_unique<PathValueRanges>(f,stable,work.limit-work.stats.work,
                                             PathRangeTraversal::BranchConditions);
  if (!work.spend(paths->work())) return {};
  if (paths->budgetExhausted()) {
    work.stats.budgetExhausted=true;
    fail(work.stats,"loop path-value fact construction exhausted");return {};
  }
  return paths;
}
bool loopCandidates(Function &f, const Guards &g, const Table &table, Work &work,
                    std::vector<DispatchCorridorEdge> &edges) {
  std::vector<BasicBlock *> path{table.dispatch->getParent()};
  while (path.front()->phis().empty()) {
    if (!work.spend() || path.size()==6) return fail(work.stats,"loop header path cap or missing PHI head");
    auto *pred=path.front()->getSinglePredecessor();
    if (!pred || std::find(path.begin(),path.end(),pred)!=path.end())
      return fail(work.stats,"loop controller is not one bounded predecessor path");
    path.insert(path.begin(),pred);
  }
  for (auto *block:path) if (g.chain.contains(block) || block==&f.getEntryBlock())
    return fail(work.stats,"loop copied path crosses executable entry checks");
  for (auto &block:f) {
    if (std::find(path.begin(),path.end(),&block)!=path.end()) continue;
    for (unsigned slot=0;slot<block.getTerminator()->getNumSuccessors();++slot) {
      if (!work.spend()) return false;
      if (block.getTerminator()->getSuccessor(slot)==path.front()) {
        if (edges.size()==512) { work.stats.budgetExhausted=true;return fail(work.stats,"loop candidate cap"); }
        edges.push_back({&block,slot,path});
      }
    }
  }
  return !edges.empty() || fail(work.stats,"loop has no original outside-path input");
}
bool recoverLoop(Function &f, const ImageTableContextRequest &r, Work &work,
                 std::size_t expectedGuards, Guards &g, std::vector<LoadInst *> &cuts,
                 LoopInventory &inventory) {
  std::size_t guests=0,accepted=0,guards=0;
  for (auto &block:f) for (auto &i:block) {
    if (!work.spend()) return false;
    ++inventory.instructions;
    // This paid full scan records every kind the write checker inspects,
    // including non-writing calls and reads. Later phases retain all nodes.
    if (isLoopEffect(i)) inventory.effects.push_back(
        {&i,&block,i.getOpcode(),i.getType(),loopEffectType(i)});
    auto *tag=i.getMetadata(LoopAnchorKind);
    if (!tag) continue;
    auto *value=tag->getNumOperands()==1 ? mdconst::dyn_extract<ConstantInt>(tag->getOperand(0)) : nullptr;
    if (!value || value->getBitWidth()!=32) return fail(work.stats,"malformed private loop provenance");
    switch(value->getZExtValue()) {
    case LoopGuest: {
      auto *load=dyn_cast<LoadInst>(&i);
      if (!load || !load->isSimple() || !load->getType()->isPointerTy() ||
          load->getType()->getPointerAddressSpace()!=0 || &block!=&f.getEntryBlock())
        return fail(work.stats,"private loop Guest provenance changed shape");
      g.guest=load;++guests;break;
    }
    case LoopAccepted: {
      auto *branch=dyn_cast<BranchInst>(&i);
      if (!branch || !branch->isUnconditional()) return fail(work.stats,"private accepted edge changed shape");
      g.accepted=&block;g.chain.insert(&block);++accepted;break;
    }
    case LoopGuard:
      if (!i.isTerminator()) return fail(work.stats,"private guard provenance changed shape");
      g.chain.insert(&block);++guards;break;
    case LoopLookup: {
      auto *load=dyn_cast<LoadInst>(&i);
      if (!load || !load->isSimple() || !load->getType()->isIntegerTy(64))
        return fail(work.stats,"private lookup provenance changed shape");
      cuts.push_back(load);break;
    }
    default:return fail(work.stats,"unknown private loop provenance");
    }
    inventory.markers.emplace_back(&i,tag);
  }
  if (guests!=1 || accepted!=1 || guards!=expectedGuards || cuts.empty())
    return fail(work.stats,"private loop anchors were lost or duplicated");
  std::set<BasicBlock *> cutBlocks;
  DominatorTree dom(f);
  for (auto *load:cuts) {
    auto *gep=dyn_cast<GetElementPtrInst>(load->getPointerOperand());
    if (!work.spend() || !gep || gep->getPointerOperand()!=g.guest || gep->getNumIndices()!=1 ||
        !gep->getSourceElementType()->isIntegerTy(8) ||
        !tableIndex(gep->idx_begin()->get(),r.tableOffset,r.targets.size()) ||
        !cutBlocks.insert(load->getParent()).second || !dom.dominates(g.accepted,load->getParent()))
      return fail(work.stats,"unrecognized or unguarded loop table cutpoint");
    // Original adjacency plus the batch implementation permits only scalar
    // branch selection after the read; never hide a later write in this block.
    for (auto *i=load->getNextNode();i;i=i->getNextNode()) {
      if (!work.spend()) return false;
      if (!i->isTerminator() && (i->mayReadOrWriteMemory() || isa<CallBase>(i)))
        return fail(work.stats,"loop cutpoint has a later observable operation");
    }
  }
  for (auto &block:f) for (auto &i:block) {
    if (!work.spend()) return false;
    auto *load=dyn_cast<LoadInst>(&i);
    auto *gep=load ? dyn_cast<GetElementPtrInst>(load->getPointerOperand()) : nullptr;
    if (load && load->getType()->isIntegerTy(64) && gep && gep->getPointerOperand()==g.guest &&
        gep->getNumIndices()==1 && gep->getSourceElementType()->isIntegerTy(8) &&
        tableIndex(gep->idx_begin()->get(),r.tableOffset,r.targets.size()) &&
        std::find(cuts.begin(),cuts.end(),load)==cuts.end())
      return fail(work.stats,"untracked loop table read");
  }
  return true;
}
bool loopCells(Function &f, const Guards &g, const ImageTableContextRequest &r,
                const InvariantRegion &region, const std::vector<LoadInst *> &cuts,
                Work &work, std::vector<SeededCell> &cells) {
  DominatorTree dom(f);
  std::map<std::pair<std::uint64_t,unsigned>,std::vector<StoreInst *>> seeds;
  for (auto &block:f) if (region.preserving.contains(&block)) for (auto &i:block) {
    if (!work.spend()) return false;
    auto *store=dyn_cast<StoreInst>(&i);
    auto *c=store ? dyn_cast<ConstantInt>(store->getValueOperand()) : nullptr;
    if (!store || !store->isSimple() || !c || c->getBitWidth()%8 || c->getBitWidth()>64 ||
        !dom.dominates(g.accepted,&block)) continue;
    bool all=true;
    for (auto *cut:cuts) { if (!work.spend()) return false;all &= dom.dominates(store,cut); }
    auto at=fixedGuestWord(store->getPointerOperand(),g.guest);
    if (all && at && disjoint(*at,c->getBitWidth()/8,r.tableOffset,r.targets.size()*8))
      seeds[{*at,c->getBitWidth()}].push_back(store);
  }
  for (const auto &[key,initializers]:seeds) {
    if (!work.spend()) return false;
    if (initializers.size()!=1) continue;
    SeededCell cell{key.first,key.second,initializers.front(),
                    cast<ConstantInt>(initializers.front()->getValueOperand()),{}};
    bool clobber=false;
    for (auto &block:f) if (region.preserving.contains(&block)) for (auto &i:block) {
      if (!work.spend()) return false;
      if (auto *load=dyn_cast<LoadInst>(&i);load && load->isSimple() &&
          !load->hasPoisonGeneratingAnnotations() && load->getType()->isIntegerTy(cell.bits) &&
          fixedGuestWord(load->getPointerOperand(),g.guest)==cell.address && dom.dominates(cell.seed,load))
        cell.loads.push_back(load);
      auto *store=dyn_cast<StoreInst>(&i);
      if (!store || store==cell.seed || !store->getValueOperand()->getType()->isIntegerTy()) continue;
      auto at=fixedGuestWord(store->getPointerOperand(),g.guest);
      auto bits=store->getValueOperand()->getType()->getIntegerBitWidth();
      if (at && bits%8==0 && !disjoint(*at,bits/8,cell.address,cell.bits/8)) clobber=true;
    }
    if (!clobber && !cell.loads.empty()) cells.push_back(std::move(cell));
  }
  return true;
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
ImageTableContextStats runLoopImageTableContext(Module &module,
                                                const ImageTableContextRequest &request) {
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

  // This named API always publishes its complete proof body; there is no
  // old-value map back into a different, unthreaded trial.
  if (guards.guest->getType()->getPointerAddressSpace()!=0) {
    fail(stats,"loop Guest base is not in the 64-bit descriptor address space");return stats;
  }
  if (module.getNamedMetadata(LoopAnchorKind)) {
    fail(stats,"caller uses the reserved loop provenance name");return stats;
  }
  for (auto &global:module.globals()) {
    if (!work.spend()) return stats;
    if (global.getMetadata(LoopAnchorKind)) {
      fail(stats,"caller global loop provenance collision");return stats;
    }
  }
  for (auto &f:module) {
    if (!work.spend() || f.getMetadata(LoopAnchorKind)) {
      fail(stats,"caller loop provenance collision");return stats;
    }
    for (auto &block:f) for (auto &i:block) {
      if (!work.spend()) return stats;
      if (i.getMetadata(LoopAnchorKind)) { fail(stats,"caller loop provenance collision");return stats; }
    }
  }
  WeakTrackingVH originalIndex(table.index);
  if (!replaceCandidateSwitch(table,work) || !substituteGuardedRegisters(*function,guards,work) ||
      !foldConstants(*function,work)) return stats;
  table.index=originalIndex;
  InvariantRegion originalRegion;
  if (!table.index || !verify(*function,verification) ||
      !closeControlInvariant(*function,guards,table,request,sp,work,originalRegion)) return stats;
  std::vector<DispatchCorridorEdge> candidates;
  if (!loopCandidates(*function,guards,table,work,candidates)) return stats;
  // Constant-only internal origin markers survive the batch's body
  // transaction. They carry identity, not a caller-authored certificate.
  if (!work.spend(2+guards.chain.size())) return stats;
  markLoop(*cast<Instruction>(guards.guest),LoopGuest);
  for (auto *block:guards.chain) markLoop(*block->getTerminator(),
                                        block==guards.accepted ? LoopAccepted : LoopGuard);
  markLoop(*table.load,LoopLookup);
  const auto guardCount=guards.chain.size()-1;
  const auto blocksBefore=function->size();
  std::size_t instructionsBefore=0;
  for (auto &block:*function) instructionsBefore+=block.size();
  if (blocksBefore>=request.maxBlocks || instructionsBefore>=request.maxInstructions || stats.work==work.limit) {
    stats.budgetExhausted=true;fail(stats,"loop has no remaining batch growth/work budget");return stats;
  }
  DispatchCorridorBatchOptions limits;
  limits.maxClonedBlocks=std::min(limits.maxClonedBlocks,request.maxBlocks-blocksBefore);
  limits.maxClonedInstructions=std::min(limits.maxClonedInstructions,request.maxInstructions-instructionsBefore);
  limits.maxWork=std::min(limits.maxWork,work.limit-stats.work);
  auto batch=threadDispatchCorridorBatch(*function,candidates,limits);
  if (!work.spend(batch.work)) return stats;
  if (!batch.complete || batch.budgetExhausted || !batch.changed) {
    stats.budgetExhausted |= batch.budgetExhausted;
    fail(stats,"loop batch refused: "+batch.message);return stats;
  }
  // Every old instruction/block handle was invalidated by that transaction.
  // Pre-batch SP/index truth is inherited ONLY through this internally
  // validated semantics-preserving batch, at clones of that exact lookup.
  // Do not invent SP observations for the new blocks.
  Guards current;
  std::vector<LoadInst *> cuts;
  LoopInventory inventory;
  if (!recoverLoop(*function,request,work,guardCount,current,cuts,inventory)) return stats;
  InvariantRegion region;
  if (!loopRegion(*function,current,cuts,work,region)) return stats;
  // Original-IR relations only. A general LLVM scalar optimization can refine
  // independent undef/freeze choices and is not a memory-preservation proof.
  std::vector<WeakTrackingVH> witnesses;
  if (!projectLoopAddresses(*function,current,region,inventory,work,witnesses)) return stats;
  auto paths=loopPathRanges(*function,witnesses,work);
  if (stats.budgetExhausted) return stats;
  ConstantFacts none;
  std::size_t constantValues=0;
  std::vector<SeededCell> cells;
  if (!loopCells(*function,current,request,region,cuts,work,cells)) return stats;
  // Try independently seeded words before recursively ranging their unknown
  // reloads through a large memory SCC. These are the same full joint proofs,
  // under the same total/query caps; discovery has changed no original write.
  bool closed=false;
  for (const auto &cell:cells) {
    // Both the overlay and ALL load/definition caches are candidate-local.
    ConstantFacts overlay;
    for (auto *load:cell.loads) {
      if (!work.spend()) return stats;
      overlay.emplace(load,cell.value);
    }
    LoopConstantPlan plan;
    if (!loopShadowConstants(*function,overlay,work,plan)) return stats;
    LoopRanges ranges(work,current.guest,overlay,paths.get());
    stats.preservedStores=0;
    if (!loopWrites(*function,current,request,region,inventory,work,ranges,&cell)) {
      if (stats.budgetExhausted) return stats;
      continue;
    }
    // The ENTIRE original post-batch P write set has passed without any
    // CFG pruning, memory forwarding or DSE under this hypothesis.
    paths.reset(); // later SSA changes invalidate these contextual caches.
    for (auto *load:cell.loads) {
      if (!work.spend()) return stats;
      constantValues+=!load->use_empty();
    }
    if (!consumeLoopConstants(*function,plan,work)) return stats;
    auto checkedPaths=loopPathRanges(*function,witnesses,work);
    if (stats.budgetExhausted) return stats;
    LoopRanges checked(work,current.guest,none,checkedPaths.get()); // fresh SSA/point caches.
    stats.preservedStores=0;
    if (!verify(*function,verification) ||
        !loopWrites(*function,current,request,region,inventory,work,checked,&cell)) return stats;
    closed=true;break;
  }
  if (!closed) {
    LoopRanges ordinary(work,current.guest,none,paths.get());
    stats.preservedStores=0;
    if (!loopWrites(*function,current,request,region,inventory,work,ordinary)) return stats;
  }
  // Every operation since recoverLoop is read-only or RAUW-only: projection
  // and constant consumption retain ALL original instructions and CFG nodes.
  // Reuse that paid inventory; whole-module verification still runs below.
  // Any future insert/erase/clone/CFG or effect change MUST rebuild it.
  for (const auto &[handle, tag] : inventory.markers) {
    if (!work.spend()) return stats;
    auto *i=dyn_cast_or_null<Instruction>(static_cast<Value *>(handle));
    if (!i || !i->getParent() || i->getFunction()!=function ||
        i->getMetadata(LoopAnchorKind)!=tag) {
      fail(stats,"private loop marker changed after recovery");return stats;
    }
    i->setMetadata(LoopAnchorKind,nullptr);
  }
  const auto finalInstructions=inventory.instructions;
  if (function->size()>request.maxBlocks || finalInstructions>request.maxInstructions ||
      !work.spend(finalInstructions+trial->global_size()+trial->size()) ||
      !verifyWhole(*trial,verification)) {
    if (stats.message.empty()) fail(stats,"invalid or oversized complete loop proof: "+verification);
    return stats;
  }
  if (!commit(*source,*function,stats,0)) return stats;
  stats.changed=stats.closed=true;stats.constantValues=constantValues;
  stats.message="complete loop table/cell proof consumed; all "+std::to_string(cuts.size())+
                " lookup cutpoints and original target reads retained";
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
