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
#include <deque>

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
// Business hints are predictions, never memory facts. In particular, an
// unrelated STORE may select the wrong entry: only the later actual target
// equality can authorize the fast edge.
struct DynamicTable {
  llvm::LoadInst *base = nullptr;
  llvm::SwitchInst *jump = nullptr;
  std::uint64_t offset = 0;
};
std::optional<DynamicTable> dynamicTable(llvm::BasicBlock &block) {
  auto *jump=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());
  auto *load=jump?llvm::dyn_cast<llvm::LoadInst>(jump->getCondition()):nullptr;
  if(!load || load->getParent()!=&block || !load->isSimple() ||
     !load->getType()->isIntegerTy(64) || load->getPointerAddressSpace()!=0 ||
     jump->getNumSuccessors()>512) return {};
  auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
  if(!gep || !gep->getSourceElementType()->isIntegerTy(8) ||
     gep->getNumIndices()!=1 || gep->getPointerAddressSpace()!=0) return {};
  auto *base=llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());
  auto *f=block.getParent();
  if(!base || !base->isSimple() || !base->getType()->isPointerTy() ||
     f->arg_size()<2 || base->getPointerOperand()!=f->getArg(1))return {};
  auto *add=llvm::dyn_cast<llvm::BinaryOperator>(gep->getOperand(1));
  if(!add || add->getOpcode()!=llvm::Instruction::Add ||
     !add->getType()->isIntegerTy(64))return {};
  auto *offset=llvm::dyn_cast<llvm::ConstantInt>(add->getOperand(1));
  llvm::Value *scaled=add->getOperand(0);
  if(!offset) {offset=llvm::dyn_cast<llvm::ConstantInt>(add->getOperand(0));scaled=add->getOperand(1);}
  auto *shift=llvm::dyn_cast<llvm::BinaryOperator>(scaled);
  if(!offset || !shift || shift->getOpcode()!=llvm::Instruction::Shl)return {};
  auto *amount=llvm::dyn_cast<llvm::ConstantInt>(shift->getOperand(1));
  auto *state=llvm::dyn_cast<llvm::LoadInst>(shift->getOperand(0));
  if(!amount || !amount->equalsInt(3) || !state || !state->isSimple() ||
     !state->getType()->isIntegerTy(64))return {};
  return DynamicTable{base,jump,offset->getZExtValue()};
}
llvm::ConstantInt *businessHint(llvm::BasicBlock &seed,const DynamicTable &table,const Hints &h,
                                std::size_t &work,bool &exhausted) {
  for(auto i=seed.rbegin();i!=seed.rend();++i) {
    if(work==ScanLimit){exhausted=true;return nullptr;}
    ++work;
    auto *store=llvm::dyn_cast<llvm::StoreInst>(&*i);
    if(!store || !store->isSimple() || store->getPointerAddressSpace()!=0)continue;
    auto *state=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
    if(!state || !state->getType()->isIntegerTy(64) || !gep ||
       !gep->getSourceElementType()->isIntegerTy(8) || gep->getNumIndices()!=1 ||
       gep->getPointerAddressSpace()!=0 || gep->getPointerOperand()!=table.base)continue;
    const auto address=(llvm::APInt(64,table.offset)+state->getValue()*llvm::APInt(64,8)).getZExtValue();
    if(address>std::numeric_limits<std::uint64_t>::max()-8)continue;
    llvm::APInt bits(64,0);bool present=true;
    for(unsigned n=0;n<8;++n) {
      auto found=h.bytes.find(address+n);if(found==h.bytes.end()){present=false;break;}
      bits|=llvm::APInt(64,found->second).shl(n*8);
    }
    if(!present)continue;
    auto *hint=llvm::ConstantInt::get(seed.getContext(),bits);
    if(table.jump->findCaseValue(hint)!=table.jump->case_default())return hint;
  }
  return nullptr;
}
bool corridorBlock(const llvm::BasicBlock &b,bool terminal=false) {
  if(b.isEHPad() || b.hasAddressTaken() ||
     !(terminal?llvm::isa<llvm::SwitchInst>(b.getTerminator()):llvm::isa<llvm::BranchInst>(b.getTerminator())))return false;
  for(const auto &i:b) {
    if(llvm::isa<llvm::CallBase,llvm::AllocaInst,llvm::FenceInst,llvm::AtomicRMWInst,llvm::AtomicCmpXchgInst>(i))return false;
    if(auto *l=llvm::dyn_cast<llvm::LoadInst>(&i);l && !l->isSimple())return false;
    if(auto *st=llvm::dyn_cast<llvm::StoreInst>(&i);st && !st->isSimple())return false;
  }
  return true;
}
struct Edge {
  std::size_t seed, target; unsigned slot;
  std::vector<std::size_t> path;
  std::vector<unsigned> slots;
  bool business = false;
  std::uint64_t prediction = 0;
};
bool generated(const llvm::BasicBlock &b) { return b.getTerminator()->getMetadata(Generated)!=nullptr; }
std::vector<llvm::BasicBlock *> blocks(llvm::Function &f) {
  std::vector<llvm::BasicBlock *> result; for(auto &b:f)result.push_back(&b); return result;
}
}
GuardedTransitionsStats runGuardedTransitions(llvm::Module &module,const GuardedTransitionsOptions &o) {
  return runGuardedTransitions(module,o,false);
}
GuardedTransitionsStats runGuardedTransitions(llvm::Module &module,const GuardedTransitionsOptions &o,bool businessCorridors) {
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
  if(businessCorridors) {
    constexpr std::size_t MaxPathBlocks=32,MaxPathInstructions=2048;
    constexpr std::size_t MaxSearchSteps=32768,MaxTerminalVisits=512;
    bool scanExhausted=false;
    auto scanFailure=[&]() {
      reject("business instruction scan budget exhausted",true);
      s.unresolvedEdges=s.candidateEdges;s.originalSwitchesRetained=targets.size();
      return s;
    };
    std::vector<signed char> admission(initial.size(),-1);
    auto eligible=[&](std::size_t n,bool terminal=false) {
      auto *b=initial[n];
      if(!(terminal?llvm::isa<llvm::SwitchInst>(b->getTerminator()):llvm::isa<llvm::BranchInst>(b->getTerminator())))return false;
      if(admission[n]<0) {
        // The whole original block is charged once for effect admission.
        if(b->size()>ScanLimit-work){scanExhausted=true;return false;}
        work+=b->size();admission[n]=corridorBlock(*b,terminal)?1:0;
      }
      return admission[n]!=0;
    };
    struct Incoming {std::size_t block;unsigned slot;};
    std::vector<std::vector<Incoming>> incoming(initial.size());
    for(std::size_t n=0;n<initial.size();++n) {
      auto *t=initial[n]->getTerminator();
      if(!llvm::isa<llvm::BranchInst,llvm::SwitchInst>(t))continue;
      for(unsigned slot=0;slot<t->getNumSuccessors();++slot) {
        if(++work>ScanLimit) {
          reject("business incoming-edge scan budget exhausted",true);
          s.unresolvedEdges=s.candidateEdges;s.originalSwitchesRetained=targets.size();return s;
        }
        incoming[ordinals.at(t->getSuccessor(slot))].push_back({n,slot});
      }
    }
    struct Search {std::size_t terminal;std::vector<std::size_t> path;std::vector<unsigned> slots;std::size_t instructions;};
    std::deque<Search> queue;
    std::map<std::size_t,std::set<std::size_t>> visited;
    for(std::size_t n=0;n<initial.size();++n)
      if(!generated(*initial[n]) && eligible(n,true) && dynamicTable(*initial[n])) {
        if(initial[n]->size()>MaxPathInstructions){truncated=true;continue;}
        queue.push_back({n,{n},{},initial[n]->size()});visited[n].insert(n);
      }
    if(scanExhausted)return scanFailure();
    std::set<std::pair<std::size_t,unsigned>> selectedEdges;
    std::size_t searchSteps=0;
    // Breadth-first interleaving of all terminal searches. Only original
    // blocks/edges enter this queue; each successor slot gets one selected path.
    while(!queue.empty()) {
      auto q=std::move(queue.front());queue.pop_front();
      auto table=dynamicTable(*initial[q.terminal]);
      for(const auto &in:incoming[q.path.front()]) {
        if(++searchSteps>MaxSearchSteps) {truncated=true;queue.clear();break;}
        if(work==ScanLimit)return scanFailure();
        ++work;
        if(generated(*initial[in.block]) ||
           std::find(q.path.begin(),q.path.end(),in.block)!=q.path.end())continue;
        if(!eligible(in.block)) {if(scanExhausted)return scanFailure();continue;}
        auto *hint=businessHint(*initial[in.block],*table,hints,work,scanExhausted);
        if(scanExhausted)return scanFailure();
        if(hint) {
          if(selectedEdges.emplace(in.block,in.slot).second) {
            ++s.candidateEdges;targets.insert(q.terminal);
            if(edges.size()<o.maxCandidates)
              edges.push_back({in.block,q.path.front(),in.slot,q.path,q.slots,true,hint->getZExtValue()});
            else truncated=true;
          }
          continue;
        }
        if(q.path.size()==MaxPathBlocks || initial[in.block]->size()>MaxPathInstructions-q.instructions) {
          truncated=true;continue;
        }
        auto &seen=visited[q.terminal];
        if(seen.contains(in.block))continue;
        if(seen.size()==MaxTerminalVisits){truncated=true;continue;}
        seen.insert(in.block);
        auto next=q;next.path.insert(next.path.begin(),in.block);next.slots.insert(next.slots.begin(),in.slot);
        next.instructions+=initial[in.block]->size();queue.push_back(std::move(next));
      }
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
    std::vector<llvm::BasicBlock *> path{target};
    bool pathValid=true;
    if(edge.business) {
      path.clear();for(auto n:edge.path)path.push_back(current[n]);
      auto table=dynamicTable(*path.back());
      value=table?llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),edge.prediction):nullptr;
      if(table && table->jump->findCaseValue(value)==table->jump->case_default())value=nullptr;
      for(std::size_t n=0;n<path.size();++n) {
        if(generated(*path[n]) || !corridorBlock(*path[n],n+1==path.size()))pathValid=false;
        if(n+1<path.size()) {
          auto *t=path[n]->getTerminator();const auto slot=edge.slots[n];
          if(slot>=t->getNumSuccessors() || t->getSuccessor(slot)!=path[n+1])pathValid=false;
        }
      }
    }
    ++s.attemptedEdges;
    if(!pathValid || !value || generated(*seed) || generated(*target) || !llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term) || edge.slot>=term->getNumSuccessors() || term->getSuccessor(edge.slot)!=target) {
      reject("frozen original candidate edge no longer applicable");continue;
    }
    const auto oldCount=current.size();
    GuardedTransitionRequest request{seed,edge.slot,target,path,edge.slots,value};
    const auto result=cloneGuardedTransition(*f,request,
        {std::min<std::size_t>(edge.business?32:4096,o.maxBlocks-s.clonedBlocks),
         std::min<std::size_t>(edge.business?2048:65536,o.maxInstructions-s.clonedInstructions)});
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
