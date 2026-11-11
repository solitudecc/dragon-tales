#include "GuardedTargetSets.hpp"
#include "GuardedTransition.hpp"
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
#include <set>
#include <vector>
namespace dragon::llvmlifter::detail {
namespace {
constexpr const char *Generated="dragon.guarded-transitions.generated";
constexpr std::size_t ScanLimit=1000000;
struct Hints {std::string function;std::map<std::uint64_t,std::vector<std::uint64_t>> tables;};
int hex(char c) {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
bool parseHints(llvm::StringRef text,Hints &h) {
  if(text.size()>65536)return false;
  auto json=llvm::json::parse(text);if(!json){llvm::consumeError(json.takeError());return false;}
  auto *object=json->getAsObject();if(!object)return false;
  auto name=object->getString("function");auto *rows=object->getArray("tables");
  if(!name || name->empty() || name->size()>4096 || name->contains('\0') || !rows || rows->empty() || rows->size()>32)return false;
  h.function=name->str();std::size_t total=0;
  for(const auto &item:*rows) {
    auto *row=item.getAsObject();if(!row)return false;
    auto base=row->getString("base"),bytes=row->getString("bytes");std::uint64_t address=0;
    if(!base || !base->starts_with("0x") || base->size()<=2 || base->drop_front(2).getAsInteger(16,address) ||
       !bytes || bytes->empty() || bytes->size()%16 || bytes->size()/2>4096-total ||
       bytes->size()/2>std::numeric_limits<std::uint64_t>::max()-address || h.tables.contains(address))return false;
    total+=bytes->size()/2;std::vector<std::uint64_t> values;
    for(std::size_t n=0;n<bytes->size();n+=16) {
      std::uint64_t value=0;
      for(unsigned i=0;i<8;++i) {
        int a=hex((*bytes)[n+2*i]),b=hex((*bytes)[n+2*i+1]);if(a<0||b<0)return false;
        value|=static_cast<std::uint64_t>(a*16+b)<<(8*i);
      }
      if(std::find(values.begin(),values.end(),value)==values.end())values.push_back(value);
      if(values.size()>16)return false;
    }
    h.tables.emplace(address,std::move(values));
  }
  return true;
}
const std::vector<std::uint64_t> *candidates(llvm::BasicBlock &block,const Hints &h) {
  auto *jump=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());
  auto *load=jump && jump->getNumSuccessors()<=512?llvm::dyn_cast<llvm::LoadInst>(jump->getCondition()):nullptr;
  if(!load || load->getParent()!=&block || !load->isSimple() || !load->getType()->isIntegerTy(64) || load->getPointerAddressSpace()!=0)return nullptr;
  auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
  if(!gep || !gep->getSourceElementType()->isIntegerTy(8) || gep->getNumIndices()!=1 || gep->getPointerAddressSpace()!=0)return nullptr;
  auto *base=llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());auto *function=block.getParent();
  if(!base || !base->isSimple() || !base->getType()->isPointerTy() || function->arg_size()<2 || base->getPointerOperand()!=function->getArg(1))return nullptr;
  auto *add=llvm::dyn_cast<llvm::BinaryOperator>(gep->getOperand(1));
  if(!add || add->getOpcode()!=llvm::Instruction::Add || !add->getType()->isIntegerTy(64))return nullptr;
  auto *offset=llvm::dyn_cast<llvm::ConstantInt>(add->getOperand(1));llvm::Value *scaled=add->getOperand(0);
  if(!offset){offset=llvm::dyn_cast<llvm::ConstantInt>(add->getOperand(0));scaled=add->getOperand(1);}
  auto *operation=llvm::dyn_cast<llvm::BinaryOperator>(scaled);if(!offset || !operation)return nullptr;
  auto *amount=llvm::dyn_cast<llvm::ConstantInt>(operation->getOperand(1));
  if(operation->getOpcode()==llvm::Instruction::Shl) {if(!amount || !amount->equalsInt(3))return nullptr;}
  else if(operation->getOpcode()==llvm::Instruction::Mul) {
    if(!amount)amount=llvm::dyn_cast<llvm::ConstantInt>(operation->getOperand(0));
    if(!amount || !amount->equalsInt(8))return nullptr;
  } else return nullptr;
  auto found=h.tables.find(offset->getZExtValue());return found==h.tables.end()?nullptr:&found->second;
}
bool generated(llvm::BasicBlock &b){return b.getTerminator()->getMetadata(Generated)!=nullptr;}
std::vector<llvm::BasicBlock *> blocks(llvm::Function &f){std::vector<llvm::BasicBlock *> result;for(auto &b:f)result.push_back(&b);return result;}
struct Edge {std::size_t seed,target;unsigned slot;std::vector<std::uint64_t> values;};
}
GuardedTransitionsStats runGuardedTargetSets(llvm::Module &module,const GuardedTransitionsOptions &options) {
  GuardedTransitionsStats result;
  auto *metadata=module.getNamedMetadata("dragon.indirect-target-hints");if(!metadata)return result;
  result.available=true;
  auto reject=[&](const char *message,bool budget=false) {result.complete=false;++result.rejected;result.budgetExhausted+=budget;result.message=message;};
  if(options.maxCandidates>4096 || options.maxRewrites>4096 || options.maxBlocks>4096 || options.maxInstructions>262144) {reject("guarded-target-sets options exceed hard limits",true);return result;}
  auto *text=metadata->getNumOperands()==1 && metadata->getOperand(0)->getNumOperands()==1?llvm::dyn_cast_or_null<llvm::MDString>(metadata->getOperand(0)->getOperand(0).get()):nullptr;
  Hints hints;if(!text || !parseHints(text->getString(),hints)){reject("invalid independent target hints");return result;}
  auto *function=module.getFunction(hints.function);
  if(!function || function->isDeclaration() || !module.getDataLayout().isLittleEndian() || module.getDataLayout().getPointerSizeInBits(0)!=64 || module.getDataLayout().getIndexSizeInBits(0)!=64) {reject("hint function or supported data layout unavailable");return result;}
  auto initial=blocks(*function);const auto originalCount=initial.size();if(initial.size()>65536){reject("target-set block scan budget exhausted",true);return result;}
  std::size_t work=0;
  for(auto *b:initial) {if(b->size()>ScanLimit-work){reject("target-set instruction scan budget exhausted",true);return result;}work+=b->size();}
  if(llvm::verifyFunction(*function)){reject("invalid target-set input function");return result;}
  std::map<llvm::BasicBlock *,std::size_t> ordinal;
  std::map<std::size_t,std::vector<std::uint64_t>> targets;
  for(std::size_t n=0;n<initial.size();++n) {
    ordinal.emplace(initial[n],n);if(generated(*initial[n]))continue;
    auto *values=candidates(*initial[n],hints);if(!values)continue;
    auto *jump=llvm::cast<llvm::SwitchInst>(initial[n]->getTerminator());bool valid=true;
    for(auto value:*values)if(jump->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),value))==jump->case_default())valid=false;
    if(!valid){reject("target hint is not an existing decoded case");continue;}
    targets.emplace(n,*values);
  }
  std::vector<Edge> edges;bool truncated=false;
  for(std::size_t n=0;n<initial.size();++n) {
    auto *b=initial[n];if(generated(*b))continue;auto *term=b->getTerminator();
    if(!llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term))continue;
    for(unsigned slot=0;slot<term->getNumSuccessors();++slot) {
      if(++work>ScanLimit) {reject("target-set edge scan budget exhausted",true);result.unresolvedEdges=result.candidateEdges;result.originalSwitchesRetained=targets.size();return result;}
      auto found=targets.find(ordinal.at(term->getSuccessor(slot)));if(found==targets.end())continue;
      ++result.candidateEdges;
      if(edges.size()<options.maxCandidates)edges.push_back({n,found->first,slot,found->second});else truncated=true;
    }
  }
  initial.clear();ordinal.clear();
  if(truncated){result.complete=false;++result.budgetExhausted;result.message="target-set candidate budget exhausted";}
  for(const auto &edge:edges) {
    if(result.rewrittenEdges>=options.maxRewrites || result.clonedBlocks>=options.maxBlocks || result.clonedInstructions>=options.maxInstructions) {result.complete=false;++result.budgetExhausted;result.message="target-set module budget exhausted";break;}
    auto current=blocks(*function);if(current.size()<originalCount){reject("core original block identity contract violated");break;}auto *seed=current[edge.seed],*target=current[edge.target];auto *term=seed->getTerminator();
    ++result.attemptedEdges;auto *values=candidates(*target,hints);
    if(!values || *values!=edge.values || generated(*seed) || generated(*target) || !llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term) || edge.slot>=term->getNumSuccessors() || term->getSuccessor(edge.slot)!=target) {reject("frozen target-set edge no longer applicable");continue;}
    GuardedTransitionSetRequest request{seed,edge.slot,target,{target},{},{}};
    for(auto value:edge.values)request.candidates.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),value));
    const auto count=current.size();auto changed=cloneGuardedTransitionSet(*function,request,
      {std::min<std::size_t>(4096,options.maxBlocks-result.clonedBlocks),std::min<std::size_t>(65536,options.maxInstructions-result.clonedInstructions)});
    if(!changed.changed){result.complete=false;++result.rejected;result.budgetExhausted+=changed.budgetExhausted;result.message=changed.message;continue;}
    result.changed=true;++result.rewrittenEdges;++result.fallbackSwitchesCreated;
    result.clonedBlocks+=changed.clonedBlocks;result.clonedInstructions+=changed.clonedInstructions;
    auto updated=blocks(*function);for(std::size_t n=count;n<updated.size();++n)updated[n]->getTerminator()->setMetadata(Generated,llvm::MDNode::get(module.getContext(),{}));
  }
  auto final=blocks(*function);for(const auto &[n,values]:targets){(void)values;if(llvm::isa<llvm::SwitchInst>(final[n]->getTerminator()))++result.originalSwitchesRetained;}
  result.unresolvedEdges=result.candidateEdges-result.rewrittenEdges;return result;
}
}
