#include "GuardedStateCorridors.hpp"
#include "GuardedTransition.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace dragon::llvmlifter::detail {
namespace {
constexpr const char *Generated="dragon.guarded-transitions.generated";
constexpr std::size_t MaxWork=1000000,MaxNodes=128,MaxDepth=32,MaxValues=8;
struct Work {
  std::size_t count=0;bool exhausted=false;
  bool charge(std::size_t n=1){if(n>MaxWork-count){exhausted=true;return false;}count+=n;return true;}
};
struct Hints {std::string function;std::map<std::uint64_t,std::uint8_t> bytes;};
int hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
bool hints(llvm::StringRef text,Hints &result) {
  if(text.size()>65536)return false;
  auto json=llvm::json::parse(text);if(!json){llvm::consumeError(json.takeError());return false;}
  auto *object=json->getAsObject();if(!object)return false;
  auto name=object->getString("view_symbol");auto *memory=object->getArray("memory");
  if(!name || name->empty() || name->contains('\0') || !memory || memory->size()>256)return false;
  result.function=name->str();
  for(const auto &item:*memory) {
    auto *row=item.getAsObject();if(!row)return false;
    auto offset=row->getString("offset"),bytes=row->getString("bytes");std::uint64_t address=0;
    if(!offset || !offset->starts_with("0x") || offset->size()<=2 || offset->drop_front(2).getAsInteger(16,address) ||
       !bytes || bytes->empty() || bytes->size()%2 || bytes->size()/2>4096-result.bytes.size() || bytes->size()/2>std::numeric_limits<std::uint64_t>::max()-address)return false;
    for(std::size_t n=0;n<bytes->size()/2;++n){int a=hex((*bytes)[2*n]),b=hex((*bytes)[2*n+1]);if(a<0||b<0||!result.bytes.emplace(address+n,a*16+b).second)return false;}
  }
  return true;
}
using Values=std::vector<llvm::APInt>;
using MaybeValues=std::optional<Values>;
struct Key {llvm::Value *value;bool historical;bool operator==(const Key &)const=default;};
struct KeyHash {std::size_t operator()(const Key &k)const{return std::hash<llvm::Value *>{}(k.value)^(k.historical?0x9e3779b9u:0);}};
// This enumerator predicts target-table addresses. Its arithmetic is modular;
// flags are neither stripped nor used to establish absence of poison. The
// original expressions still execute before the actual target guard.
struct Enumerator {
  llvm::BasicBlock *seed;
  const std::vector<llvm::BasicBlock *> &path;
  Work &work;std::size_t nodes=0;bool limited=false;
  std::unordered_map<llvm::BasicBlock *,std::size_t> positions;
  std::unordered_map<Key,Values,KeyHash> memo;
  std::unordered_set<Key,KeyHash> active;
  Enumerator(llvm::BasicBlock *s,const std::vector<llvm::BasicBlock *> &p,Work &w):seed(s),path(p),work(w) {
    for(std::size_t n=0;n<p.size();++n)positions.emplace(p[n],n);
  }
  bool append(Values &to,const llvm::APInt &value) {
    if(!work.charge())return false;
    if(std::find(to.begin(),to.end(),value)==to.end())to.push_back(value);
    if(to.size()>MaxValues){limited=true;return false;}return true;
  }
  MaybeValues get(llvm::Value *value,bool historical=false,unsigned depth=0) {
    if(!work.charge())return {};
    auto *type=llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if(!type || type->getBitWidth()>128 || llvm::isa<llvm::UndefValue,llvm::PoisonValue>(value))return {};
    Key key{value,historical};auto found=memo.find(key);if(found!=memo.end())return found->second;
    if(depth>MaxDepth || ++nodes>MaxNodes){limited=true;return {};}
    if(!active.insert(key).second)return {};
    auto result=compute(value,historical,depth);active.erase(key);
    if(result)memo.emplace(key,*result);return result;
  }
  MaybeValues compute(llvm::Value *value,bool historical,unsigned depth) {
    if(auto *constant=llvm::dyn_cast<llvm::ConstantInt>(value))return Values{constant->getValue()};
    if(auto *phi=llvm::dyn_cast<llvm::PHINode>(value)) {
      auto found=positions.find(phi->getParent());
      if(!historical && found!=positions.end()) {
        auto n=found->second;auto *predecessor=n?path[n-1]:seed;
        if(!work.charge(phi->getNumIncomingValues()))return {};
        int arm=phi->getBasicBlockIndex(predecessor);if(arm<0)return {};
        // Root incoming operands describe the prior execution, even when their
        // definitions reside in the region that will be cloned freshly.
        return get(phi->getIncomingValue(arm),n==0,depth+1);
      }
      // An unselected fresh PHI must enter the copied path before it can
      // select a real edge. Otherwise a shared terminal could consume the
      // opportunity without specializing any business incoming context.
      if(!historical)return {};
      Values result;
      for(auto &incoming:phi->incoming_values()) {
        auto values=get(incoming,true,depth+1);if(!values)return {};
        for(const auto &v:*values)if(!append(result,v))return {};
      }
      return result.empty()?MaybeValues{}:MaybeValues{result};
    }
    if(auto *select=llvm::dyn_cast<llvm::SelectInst>(value)) {
      auto yes=get(select->getTrueValue(),historical,depth+1),no=get(select->getFalseValue(),historical,depth+1);
      if(!yes || !no)return {};Values result=*yes;for(const auto &v:*no)if(!append(result,v))return {};return result;
    }
    if(auto *cast=llvm::dyn_cast<llvm::CastInst>(value)) {
      const auto opcode=cast->getOpcode();if(opcode!=llvm::Instruction::ZExt && opcode!=llvm::Instruction::SExt && opcode!=llvm::Instruction::Trunc)return {};
      auto input=get(cast->getOperand(0),historical,depth+1);if(!input)return {};Values result;auto width=cast->getType()->getIntegerBitWidth();
      for(const auto &v:*input)if(!append(result,opcode==llvm::Instruction::SExt?v.sextOrTrunc(width):v.zextOrTrunc(width)))return {};return result;
    }
    auto *binary=llvm::dyn_cast<llvm::BinaryOperator>(value);if(!binary)return {};
    const auto opcode=binary->getOpcode();
    if(opcode!=llvm::Instruction::Add && opcode!=llvm::Instruction::Sub &&
       opcode!=llvm::Instruction::Mul && opcode!=llvm::Instruction::Shl &&
       opcode!=llvm::Instruction::And && opcode!=llvm::Instruction::Or &&
       opcode!=llvm::Instruction::Xor)return {};
    auto left=get(binary->getOperand(0),historical,depth+1);
    auto right=get(binary->getOperand(1),historical,depth+1);
    if(!left || !right) {
      if(opcode!=llvm::Instruction::And || work.exhausted)return {};
      auto *mask=llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(0));
      auto *unknown=binary->getOperand(1);
      if(!mask){mask=llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));unknown=binary->getOperand(0);}
      if(!mask || llvm::isa<llvm::UndefValue,llvm::PoisonValue>(unknown))return {};
      // Candidate range only: never substitute the AND, strip its source's
      // flags/freeze, or infer that a table entry remains unchanged. A failed
      // earlier dependency visit keeps its existing budget diagnostic.
      std::vector<unsigned> positions;
      const auto &bits=mask->getValue();
      for(unsigned bit=0;bit<bits.getBitWidth();++bit) {
        if(!work.charge())return {};
        if(bits[bit]){if(positions.size()==3)return {};positions.push_back(bit);}
      }
      Values result;
      for(unsigned subset=0;subset<(1u<<positions.size());++subset) {
        llvm::APInt value(bits.getBitWidth(),0);
        for(unsigned n=0;n<positions.size();++n)if(subset&(1u<<n))value.setBit(positions[n]);
        if(!append(result,value))return {};
      }
      return result;
    }
    Values result;
    for(const auto &a:*left)for(const auto &b:*right) {
      llvm::APInt v=a;
      if(opcode==llvm::Instruction::Add)v=a+b;
      else if(opcode==llvm::Instruction::Sub)v=a-b;
      else if(opcode==llvm::Instruction::Mul)v=a*b;
      else if(opcode==llvm::Instruction::And)v=a&b;
      else if(opcode==llvm::Instruction::Or)v=a|b;
      else if(opcode==llvm::Instruction::Xor)v=a^b;
      else {if(b.uge(a.getBitWidth()))return {};v=a.shl(static_cast<unsigned>(b.getZExtValue()));}
      if(!append(result,v))return {};
    }
    return result;
  }
};
llvm::Value *indexOf(llvm::BasicBlock &block) {
  auto *jump=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());auto *load=jump && jump->getNumSuccessors()<=512?llvm::dyn_cast<llvm::LoadInst>(jump->getCondition()):nullptr;
  if(!load || load->getParent()!=&block || !load->isSimple() || !load->getType()->isIntegerTy(64) || load->getPointerAddressSpace()!=0)return nullptr;
  auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());if(!gep || gep->getPointerAddressSpace()!=0 || !gep->getSourceElementType()->isIntegerTy(8) || gep->getNumIndices()!=1 || !gep->getOperand(1)->getType()->isIntegerTy(64))return nullptr;
  auto *base=llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());auto *f=block.getParent();
  if(!base || !base->isSimple() || !base->getType()->isPointerTy() || f->arg_size()<2 || base->getPointerOperand()!=f->getArg(1))return nullptr;
  return llvm::isa<llvm::ConstantInt>(gep->getOperand(1))?nullptr:gep->getOperand(1);
}
bool safeBlock(llvm::BasicBlock &b,bool terminal=false) {
  if(b.isEHPad() || b.hasAddressTaken() || !(terminal?llvm::isa<llvm::SwitchInst>(b.getTerminator()):llvm::isa<llvm::BranchInst>(b.getTerminator())))return false;
  for(auto &i:b) {
    if(llvm::isa<llvm::CallBase,llvm::AllocaInst,llvm::FenceInst,llvm::AtomicRMWInst,llvm::AtomicCmpXchgInst>(i))return false;
    if(auto *l=llvm::dyn_cast<llvm::LoadInst>(&i);l && !l->isSimple())return false;
    if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i);s && !s->isSimple())return false;
  }
  return true;
}
bool generated(llvm::BasicBlock &b){return b.getTerminator()->getMetadata(Generated)!=nullptr;}
std::vector<llvm::BasicBlock *> blocks(llvm::Function &f){std::vector<llvm::BasicBlock *> out;for(auto &b:f)out.push_back(&b);return out;}
struct Edge {std::size_t seed;unsigned slot;std::vector<std::size_t> path;std::vector<unsigned> slots;std::vector<std::uint64_t> predictions;};
// Only the shape supplies a prediction recipe. The original state LOAD and
// table-address operations remain in the copied execution, including flags.
struct StoreTableShape {std::uint64_t base;bool useOr;llvm::Value *guest;};
std::optional<StoreTableShape> storeTableShape(llvm::Value *index,Work &work) {
  if(!work.charge(8))return {};
  auto *sum=llvm::dyn_cast<llvm::BinaryOperator>(index);
  if(!sum || (sum->getOpcode()!=llvm::Instruction::Add && sum->getOpcode()!=llvm::Instruction::Or))return {};
  auto *base=llvm::dyn_cast<llvm::ConstantInt>(sum->getOperand(1));auto *scaled=sum->getOperand(0);
  if(!base){base=llvm::dyn_cast<llvm::ConstantInt>(sum->getOperand(0));scaled=sum->getOperand(1);}
  if(!base || !base->getType()->isIntegerTy(64))return {};
  auto *multiply=llvm::dyn_cast<llvm::BinaryOperator>(scaled);if(!multiply)return {};
  llvm::Value *state=nullptr;
  if(multiply->getOpcode()==llvm::Instruction::Shl) {
    auto *amount=llvm::dyn_cast<llvm::ConstantInt>(multiply->getOperand(1));
    if(amount && amount->equalsInt(3))state=multiply->getOperand(0);
  } else if(multiply->getOpcode()==llvm::Instruction::Mul) {
    for(unsigned n=0;n<2;++n)if(auto *factor=llvm::dyn_cast<llvm::ConstantInt>(multiply->getOperand(n));factor && factor->equalsInt(8))state=multiply->getOperand(1-n);
  }
  auto *load=llvm::dyn_cast_or_null<llvm::LoadInst>(state);
  if(!load || !load->isSimple() || !load->getType()->isIntegerTy(64) || load->getPointerAddressSpace()!=0)return {};
  auto *address=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
  if(!address || !address->getSourceElementType()->isIntegerTy(8) || address->getNumIndices()!=1 || !address->getOperand(1)->getType()->isIntegerTy(64))return {};
  auto *guest=llvm::dyn_cast<llvm::LoadInst>(address->getPointerOperand());auto *function=load->getFunction();
  if(!guest || !guest->isSimple() || !guest->getType()->isPointerTy() || function->arg_size()<2 || guest->getPointerOperand()!=function->getArg(1))return {};
  return StoreTableShape{base->getZExtValue(),sum->getOpcode()==llvm::Instruction::Or,guest};
}
MaybeValues storeAddresses(llvm::BasicBlock &seed,const std::vector<llvm::BasicBlock *> &path,
    llvm::Value *index,Work &work,bool &limited) {
  auto shape=storeTableShape(index,work);if(!shape)return {};
  // A hint does not assert that this STORE aliases the later state LOAD.
  // Scan the actual seed in reverse and retain its nearest finite Guest STORE.
  for(auto it=seed.rbegin();it!=seed.rend();++it) {
    if(!work.charge())return {};
    auto *store=llvm::dyn_cast<llvm::StoreInst>(&*it);
    if(!store || !store->isSimple() || !store->getValueOperand()->getType()->isIntegerTy(64) || store->getPointerAddressSpace()!=0)continue;
    if(!work.charge(4))return {};
    auto *address=llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
    if(!address || address->getPointerOperand()!=shape->guest || !address->getSourceElementType()->isIntegerTy(8) || address->getNumIndices()!=1 || !address->getOperand(1)->getType()->isIntegerTy(64))continue;
    Enumerator enumerate(&seed,path,work);
    // The seed already executed. Never select one of its historical PHIs with
    // the later fresh path's incoming edge or guess a cyclic/unknown arm.
    auto values=enumerate.get(store->getValueOperand(),true);limited|=enumerate.limited;
    if(work.exhausted)return {};
    if(!values)continue;
    Values result;
    for(const auto &state:*values) {
      if(!work.charge())return {};
      const std::uint64_t scaled=state.getZExtValue()<<3;
      result.emplace_back(64,shape->useOr?(shape->base|scaled):(shape->base+scaled));
    }
    return result;
  }
  // The old seed scan remains first. Only when it supplies no finite hint,
  // consider stores that will really execute in this selected path. A fresh
  // PHI here chooses its actual incoming edge; a root input still becomes a
  // historical value through Enumerator's normal execution-view rule.
  for(auto block=path.rbegin();block!=path.rend();++block) {
    for(auto it=(*block)->rbegin();it!=(*block)->rend();++it) {
      if(!work.charge())return {};
      auto *store=llvm::dyn_cast<llvm::StoreInst>(&*it);
      if(!store || !store->isSimple() || !store->getValueOperand()->getType()->isIntegerTy(64) || store->getPointerAddressSpace()!=0)continue;
      if(!work.charge(4))return {};
      auto *address=llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
      if(!address || address->getPointerOperand()!=shape->guest || !address->getSourceElementType()->isIntegerTy(8) || address->getNumIndices()!=1 || !address->getOperand(1)->getType()->isIntegerTy(64))continue;
      if(!work.charge(path.size()))return {};
      Enumerator enumerate(&seed,path,work);
      auto values=enumerate.get(store->getValueOperand(),false);limited|=enumerate.limited;
      if(work.exhausted)return {};
      if(!values)continue;
      Values result;
      for(const auto &state:*values) {
        if(!work.charge())return {};
        const std::uint64_t scaled=state.getZExtValue()<<3;
        result.emplace_back(64,shape->useOr?(shape->base|scaled):(shape->base+scaled));
      }
      return result;
    }
  }
  return {};
}
std::optional<std::vector<std::uint64_t>> predict(llvm::BasicBlock &seed,const std::vector<llvm::BasicBlock *> &path,const Hints &h,Work &work,bool &limited,bool storePredictions) {
  auto *index=indexOf(*path.back());if(!index)return {};
  Enumerator enumerate(&seed,path,work);auto addresses=enumerate.get(index);limited|=enumerate.limited;
  if(!addresses && storePredictions && !work.exhausted)addresses=storeAddresses(seed,path,index,work,limited);
  if(!addresses)return {};auto *jump=llvm::cast<llvm::SwitchInst>(path.back()->getTerminator());std::vector<std::uint64_t> values;
  for(const auto &address:*addresses) {
    auto n=address.getZExtValue();if(n>std::numeric_limits<std::uint64_t>::max()-8)return {};
    std::uint64_t candidate=0;
    for(unsigned b=0;b<8;++b){if(!work.charge())return {};auto byte=h.bytes.find(n+b);if(byte==h.bytes.end())return {};candidate|=static_cast<std::uint64_t>(byte->second)<<(8*b);}
    if(!work.charge(jump->getNumCases()))return {};
    auto *constant=llvm::ConstantInt::get(llvm::Type::getInt64Ty(seed.getContext()),candidate);if(jump->findCaseValue(constant)==jump->case_default())return {};
    if(std::find(values.begin(),values.end(),candidate)==values.end())values.push_back(candidate);
  }
  return values.empty()?std::optional<std::vector<std::uint64_t>>{}:values;
}
}
GuardedTransitionsStats runGuardedStateCorridors(llvm::Module &module,const GuardedTransitionsOptions &options) {
  return runGuardedStateCorridors(module,options,false);
}
GuardedTransitionsStats runGuardedStateCorridors(llvm::Module &module,const GuardedTransitionsOptions &options,bool storePredictions) {
  GuardedTransitionsStats result;auto *metadata=module.getNamedMetadata("dragon.guarded-entry");if(!metadata)return result;result.available=true;
  auto reject=[&](const char *message,bool budget=false){result.complete=false;++result.rejected;result.budgetExhausted+=budget;result.message=message;};
  if(options.maxCandidates>4096 || options.maxRewrites>4096 || options.maxBlocks>4096 || options.maxInstructions>262144){reject("state-corridor option hard limit exceeded",true);return result;}
  auto *text=metadata->getNumOperands()==1 && metadata->getOperand(0)->getNumOperands()==1?llvm::dyn_cast_or_null<llvm::MDString>(metadata->getOperand(0)->getOperand(0).get()):nullptr;
  Hints hint;if(!text || !hints(text->getString(),hint)){reject("invalid state-corridor prediction manifest");return result;}
  auto *function=module.getFunction(hint.function);
  if(!function || function->isDeclaration() || !module.getDataLayout().isLittleEndian() || module.getDataLayout().getPointerSizeInBits(0)!=64 || module.getDataLayout().getIndexSizeInBits(0)!=64){reject("state-corridor view or supported layout unavailable");return result;}
  auto initial=blocks(*function);const auto originalCount=initial.size();if(originalCount>65536){reject("state-corridor block scan budget exhausted",true);return result;}
  // LLVM's instruction-list size is linear. Count each original block once
  // during the already charged scan; reverse searches must not rescan it.
  std::vector<std::size_t> instructionCounts(originalCount,0);
  Work work;for(std::size_t n=0;n<originalCount;++n)for(auto &instruction:*initial[n]) {
    if(!work.charge(1) || !work.charge(instruction.getNumOperands())){reject("state-corridor instruction scan budget exhausted",true);return result;}
    ++instructionCounts[n];
  }
  if(llvm::verifyFunction(*function)){reject("invalid state-corridor input function");return result;}
  llvm::DominatorTree dom(*function);std::vector<bool> reachable;std::map<llvm::BasicBlock *,std::size_t> ordinal;
  for(std::size_t n=0;n<initial.size();++n){ordinal.emplace(initial[n],n);reachable.push_back(dom.isReachableFromEntry(initial[n]));}
  std::set<std::size_t> targets;
  auto scanFailure=[&](){reject("state-corridor total scan budget exhausted",true);result.unresolvedEdges=result.candidateEdges;result.originalSwitchesRetained=targets.size();return result;};
  std::vector<signed char> admission(initial.size(),-1);
  auto eligible=[&](std::size_t n){
    auto *b=initial[n];auto *term=b->getTerminator();if(!llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term))return false;
    if(admission[n]<0){if(!work.charge(instructionCounts[n]))return false;admission[n]=safeBlock(*b,llvm::isa<llvm::SwitchInst>(term))?1:0;}
    return admission[n]!=0;
  };
  struct Incoming {std::size_t block;unsigned slot;};std::vector<std::vector<Incoming>> incoming(initial.size());
  for(std::size_t n=0;n<initial.size();++n) {
    if(!reachable[n])continue;auto *term=initial[n]->getTerminator();if(!llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term))continue;
    for(unsigned slot=0;slot<term->getNumSuccessors();++slot){if(!work.charge())return scanFailure();incoming[ordinal.at(term->getSuccessor(slot))].push_back({n,slot});}
  }
  struct Search {std::size_t terminal;std::vector<std::size_t> path;std::vector<unsigned> slots;std::size_t instructions;};
  std::deque<Search> queue;std::map<std::size_t,std::set<std::size_t>> visited;bool limited=false;
  for(std::size_t n=0;n<initial.size();++n)if(reachable[n] && !generated(*initial[n]) && indexOf(*initial[n]) && eligible(n)) {
    if(instructionCounts[n]>2048){limited=true;continue;}
    queue.push_back({n,{n},{},instructionCounts[n]});visited[n].insert(n);
  }
  if(work.exhausted)return scanFailure();
  std::vector<Edge> branchEdges,switchEdges;std::set<std::pair<std::size_t,unsigned>> selected;std::size_t searches=0;
  while(!queue.empty()) {
    auto q=std::move(queue.front());queue.pop_front();
    for(const auto &in:incoming[q.path.front()]) {
      if(++searches>32768){limited=true;queue.clear();break;}
      if(!work.charge(1+q.path.size()))return scanFailure();
      if(!reachable[in.block] || generated(*initial[in.block]) || std::find(q.path.begin(),q.path.end(),in.block)!=q.path.end())continue;
      if(!eligible(in.block)){if(work.exhausted)return scanFailure();continue;}
      if(!work.charge(q.path.size()))return scanFailure();
      std::vector<llvm::BasicBlock *> path;for(auto n:q.path)path.push_back(initial[n]);
      bool rootPhisRetained=true;for(auto &phi:path.front()->phis()){if(!work.charge())return scanFailure();if(phi.getNumIncomingValues()<=1)rootPhisRetained=false;}
      auto predicted=rootPhisRetained?predict(*initial[in.block],path,hint,work,limited,storePredictions):std::optional<std::vector<std::uint64_t>>{};if(work.exhausted)return scanFailure();
      if(predicted) {
        if(selected.emplace(in.block,in.slot).second) {
          ++result.candidateEdges;targets.insert(q.terminal);
          auto &edges=llvm::isa<llvm::BranchInst>(initial[in.block]->getTerminator())?branchEdges:switchEdges;
          if(edges.size()<options.maxCandidates)edges.push_back({in.block,in.slot,q.path,q.slots,*predicted});else limited=true;
        }
        continue;
      }
      // A switch may be a selected seed edge, but is not traversed as an
      // intermediate dispatcher: that would duplicate a whole central region.
      if(!llvm::isa<llvm::BranchInst>(initial[in.block]->getTerminator()))continue;
      if(q.path.size()==32 || instructionCounts[in.block]>2048-q.instructions){limited=true;continue;}
      auto &seen=visited[q.terminal];if(seen.contains(in.block))continue;
      if(seen.size()==512){limited=true;continue;}seen.insert(in.block);
      auto next=q;next.path.insert(next.path.begin(),in.block);next.slots.insert(next.slots.begin(),in.slot);next.instructions+=instructionCounts[in.block];queue.push_back(std::move(next));
    }
  }
  std::vector<Edge> edges=std::move(branchEdges);
  for(auto &edge:switchEdges){if(edges.size()==options.maxCandidates){limited=true;break;}edges.push_back(std::move(edge));}
  initial.clear();ordinal.clear();
  if(limited){result.complete=false;++result.budgetExhausted;result.message="state-corridor expression or search budget exhausted";}
  for(const auto &edge:edges) {
    if(result.rewrittenEdges>=options.maxRewrites || result.clonedBlocks>=options.maxBlocks || result.clonedInstructions>=options.maxInstructions){result.complete=false;++result.budgetExhausted;result.message="state-corridor module budget exhausted";break;}
    auto current=blocks(*function);if(current.size()<originalCount){reject("core original block identity contract violated");break;}
    auto *seed=current[edge.seed];auto *term=seed->getTerminator();std::vector<llvm::BasicBlock *> path;for(auto n:edge.path)path.push_back(current[n]);
    ++result.attemptedEdges;bool valid=!generated(*seed) && llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term) && edge.slot<term->getNumSuccessors() && term->getSuccessor(edge.slot)==path.front() && indexOf(*path.back());
    for(std::size_t n=0;n<path.size();++n){if(generated(*path[n]))valid=false;if(n+1<path.size()){auto *t=path[n]->getTerminator();if(edge.slots[n]>=t->getNumSuccessors() || t->getSuccessor(edge.slots[n])!=path[n+1])valid=false;}}
    if(!valid){reject("frozen state-corridor edge no longer applicable");continue;}
    GuardedTransitionSetRequest request{seed,edge.slot,path.front(),path,edge.slots,{}};
    for(auto prediction:edge.predictions)request.candidates.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),prediction));
    const auto oldCount=current.size();auto changed=cloneGuardedTransitionSet(*function,request,
      {std::min<std::size_t>(32,options.maxBlocks-result.clonedBlocks),std::min<std::size_t>(2048,options.maxInstructions-result.clonedInstructions)});
    if(!changed.changed){result.complete=false;++result.rejected;result.budgetExhausted+=changed.budgetExhausted;result.message=changed.message;continue;}
    result.changed=true;++result.rewrittenEdges;++result.fallbackSwitchesCreated;result.clonedBlocks+=changed.clonedBlocks;result.clonedInstructions+=changed.clonedInstructions;
    auto updated=blocks(*function);for(std::size_t n=oldCount;n<updated.size();++n)updated[n]->getTerminator()->setMetadata(Generated,llvm::MDNode::get(module.getContext(),{}));
  }
  auto final=blocks(*function);for(auto n:targets)if(llvm::isa<llvm::SwitchInst>(final[n]->getTerminator()))++result.originalSwitchesRetained;
  result.unresolvedEdges=result.candidateEdges-result.rewrittenEdges;return result;
}
}
