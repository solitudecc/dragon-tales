#include "GuardedTransitions.hpp"
#include "GuardedTransition.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace dragon::llvmlifter::detail {
namespace {
constexpr const char *Generated = "dragon.guarded-transitions.generated";
constexpr std::size_t ScanLimit = 1000000;
struct Hints {
  std::string function;
  std::map<std::uint64_t, std::uint8_t> bytes;
};
int hex(char c) {
  if(c>='0'&&c<='9') return c-'0';
  if(c>='a'&&c<='f') return c-'a'+10;
  if(c>='A'&&c<='F') return c-'A'+10;
  return -1;
}
bool readHints(llvm::StringRef text, Hints &h) {
  // Bound parsing itself, before allocating a JSON tree.
  if(text.size()>65536) return false;
  auto value=llvm::json::parse(text);
  if(!value) { llvm::consumeError(value.takeError()); return false; }
  auto *o=value->getAsObject(); if(!o) return false;
  auto name=o->getString("view_symbol"); auto *rows=o->getArray("memory");
  if(!name || name->empty() || name->contains('\0') || !rows || rows->size()>256) return false;
  h.function=name->str();
  for(const auto &item:*rows) {
    auto *row=item.getAsObject(); if(!row) return false;
    auto off=row->getString("offset"), bytes=row->getString("bytes");
    if(!off || !off->starts_with("0x") || off->size()<=2 || !bytes ||
       bytes->empty() || bytes->size()%2 || bytes->size()/2>4096-h.bytes.size()) return false;
    std::uint64_t address=0;
    if(off->drop_front(2).getAsInteger(16,address)) return false;
    const auto count=bytes->size()/2;
    if(count>std::numeric_limits<std::uint64_t>::max()-address) return false;
    for(std::size_t i=0;i<count;++i) {
      int a=hex((*bytes)[2*i]), b=hex((*bytes)[2*i+1]);
      if(a<0||b<0 || !h.bytes.emplace(address+i,static_cast<std::uint8_t>(a*16+b)).second) return false;
    }
  }
  return true;
}
llvm::ConstantInt *candidate(llvm::BasicBlock &block, const Hints &h) {
  auto *jump=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());
  auto *load=jump && jump->getNumSuccessors()<=512?llvm::dyn_cast<llvm::LoadInst>(jump->getCondition()):nullptr;
  if(!load || load->getParent()!=&block || !load->isSimple() || load->getPointerAddressSpace()!=0) return nullptr;
  auto *type=llvm::dyn_cast<llvm::IntegerType>(load->getType());
  if(!type || type->getBitWidth()<8 || type->getBitWidth()>128 || type->getBitWidth()%8) return nullptr;
  auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
  if(!gep || !gep->getSourceElementType()->isIntegerTy(8) || gep->getNumIndices()!=1 || gep->getPointerAddressSpace()!=0) return nullptr;
  auto *index=llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
  if(!index || index->getBitWidth()!=64) return nullptr;
  auto *base=llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());
  auto *f=block.getParent();
  if(!base || !base->isSimple() || !base->getType()->isPointerTy() || f->arg_size()<2 ||
     !f->getArg(1)->getType()->isPointerTy() || base->getPointerOperand()!=f->getArg(1)) return nullptr;
  const auto address=index->getZExtValue();
  const auto count=type->getBitWidth()/8;
  if(count>std::numeric_limits<std::uint64_t>::max()-address) return nullptr;
  llvm::APInt bits(type->getBitWidth(),0);
  for(unsigned i=0;i<count;++i) {
    auto found=h.bytes.find(address+i); if(found==h.bytes.end()) return nullptr;
    bits|=llvm::APInt(type->getBitWidth(),found->second).shl(i*8);
  }
  auto *value=llvm::ConstantInt::get(block.getContext(),bits);
  return jump->findCaseValue(value)==jump->case_default()?nullptr:value;
}
struct Edge { std::size_t seed, target; unsigned slot; };
bool generated(const llvm::BasicBlock &b) { return b.getTerminator()->getMetadata(Generated)!=nullptr; }
std::vector<llvm::BasicBlock *> blocks(llvm::Function &f) {
  std::vector<llvm::BasicBlock *> result; for(auto &b:f)result.push_back(&b); return result;
}
}
GuardedTransitionsStats runGuardedTransitions(llvm::Module &module,const GuardedTransitionsOptions &o) {
  GuardedTransitionsStats s;
  const auto *md=module.getNamedMetadata("dragon.guarded-entry");
  if(!md) return s;
  s.available=true;
  auto reject=[&](const char *why,bool budget=false) {
    s.complete=false; ++s.rejected; if(budget)++s.budgetExhausted; s.message=why;
  };
  if(o.maxCandidates>4096 || o.maxRewrites>4096 || o.maxBlocks>4096 || o.maxInstructions>262144) {
    reject("guarded-transitions option hard limit exceeded",true); return s;
  }
  auto *text=md->getNumOperands()==1 && md->getOperand(0)->getNumOperands()==1
      ?llvm::dyn_cast_or_null<llvm::MDString>(md->getOperand(0)->getOperand(0).get()):nullptr;
  Hints hints;
  if(!text || !readHints(text->getString(),hints)) {reject("invalid guarded transition candidate manifest");return s;}
  auto *f=module.getFunction(hints.function);
  if(!f || f->isDeclaration() || !module.getDataLayout().isLittleEndian() ||
     module.getDataLayout().getPointerSizeInBits(0)!=64 ||
     module.getDataLayout().getIndexSizeInBits(0)!=64) {
    reject("guarded view function or supported byte order unavailable");return s;
  }
  if(f->size()>65536) {reject("candidate scan block budget exhausted",true);return s;}
  auto initial=blocks(*f); const auto originalCount=initial.size();
  std::size_t work=0;
  for(auto *b:initial) {
    if(b->size()>ScanLimit-work) {reject("candidate scan instruction budget exhausted",true);return s;}
    work+=b->size();
  }
  if(llvm::verifyFunction(*f)) {reject("invalid guarded view input function");return s;}
  std::set<std::size_t> targets;
  for(std::size_t n=0;n<initial.size();++n)
    if(!generated(*initial[n]) && candidate(*initial[n],hints))targets.insert(n);
  std::map<llvm::BasicBlock *,std::size_t> ordinals;
  for(std::size_t n=0;n<initial.size();++n)ordinals.emplace(initial[n],n);
  std::vector<Edge> edges;
  bool truncated=false;
  for(std::size_t n=0;n<initial.size();++n) {
    auto *b=initial[n]; if(generated(*b))continue;
    auto *term=b->getTerminator();
    if(!llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term))continue;
    for(unsigned slot=0;slot<term->getNumSuccessors();++slot) {
      if(++work>ScanLimit) {reject("candidate scan edge budget exhausted",true);return s;}
      const auto target=ordinals.at(term->getSuccessor(slot));
      if(!targets.contains(target))continue;
      ++s.candidateEdges;
      if(edges.size()<o.maxCandidates)edges.push_back({n,target,slot});else truncated=true;
    }
  }
  // Nothing derived from body pointers crosses the first core transaction.
  initial.clear(); ordinals.clear();
  if(truncated) {s.complete=false;++s.budgetExhausted;s.message="candidate budget exhausted";}
  for(const auto &edge:edges) {
    if(s.rewrittenEdges>=o.maxRewrites || s.clonedBlocks>=o.maxBlocks || s.clonedInstructions>=o.maxInstructions) {
      s.complete=false;++s.budgetExhausted;s.message="module transition budget exhausted";break;
    }
    auto current=blocks(*f);
    if(current.size()<originalCount) {reject("core original block identity contract violated");break;}
    auto *seed=current[edge.seed],*target=current[edge.target];
    auto *term=seed->getTerminator();
    auto *value=candidate(*target,hints);
    ++s.attemptedEdges;
    if(!value || generated(*seed) || generated(*target) || !llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term) || edge.slot>=term->getNumSuccessors() || term->getSuccessor(edge.slot)!=target) {
      reject("frozen original candidate edge no longer applicable");continue;
    }
    const auto oldCount=current.size();
    GuardedTransitionRequest request{seed,edge.slot,target,{target},{},value};
    const auto result=cloneGuardedTransition(*f,request,
        {std::min<std::size_t>(4096,o.maxBlocks-s.clonedBlocks),
         std::min<std::size_t>(65536,o.maxInstructions-s.clonedInstructions)});
    current.clear(); seed=nullptr;target=nullptr;term=nullptr;value=nullptr;
    if(!result.changed) {
      s.complete=false;++s.rejected;s.budgetExhausted+=result.budgetExhausted;s.message=result.message;continue;
    }
    s.changed=true;++s.rewrittenEdges;
    s.clonedBlocks+=result.clonedBlocks;s.clonedInstructions+=result.clonedInstructions;
    auto updated=blocks(*f);
    for(std::size_t n=oldCount;n<updated.size();++n) {
      auto *t=updated[n]->getTerminator();
      t->setMetadata(Generated,llvm::MDNode::get(module.getContext(),{}));
      if(llvm::isa<llvm::SwitchInst>(t))++s.fallbackSwitchesCreated;
    }
  }
  auto final=blocks(*f);
  for(auto n:targets)if(llvm::isa<llvm::SwitchInst>(final[n]->getTerminator()))++s.originalSwitchesRetained;
  s.unresolvedEdges=s.candidateEdges-s.rewrittenEdges;
  return s;
}
}
