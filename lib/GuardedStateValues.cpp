#include "GuardedStateValues.hpp"
#include "GuardedTransition.hpp"
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <vector>
namespace dragon::llvmlifter::detail {
namespace {
constexpr const char *Manifest="dragon.guarded-state-values";
constexpr const char *Generated="dragon.guarded-transitions.generated";
constexpr const char *Consumed="dragon.guarded-state-values.consumed";
constexpr std::size_t MaxWork=1000000;
struct Work {
 std::size_t used=0;bool exhausted=false;
 bool charge(std::size_t n=1){if(n>MaxWork-used){exhausted=true;return false;}used+=n;return true;}
};
struct Hint {std::string function,block;std::uint64_t state,target;};
bool parse(llvm::StringRef text,std::vector<Hint> &out) {
 if(text.size()>65536)return false;auto parsed=llvm::json::parse(text);
 if(!parsed){llvm::consumeError(parsed.takeError());return false;}
 auto *root=parsed->getAsObject();if(!root || root->size()!=1)return false;
 auto *rows=root->getArray("hints");if(!rows || rows->empty() || rows->size()>64)return false;
 std::set<std::pair<std::string,std::string>> seen;
 for(const auto &row:*rows){auto *o=row.getAsObject();if(!o || o->size()!=4)return false;
  auto f=o->getString("function"),b=o->getString("block"),s=o->getString("state"),t=o->getString("target");
  auto name=[](auto v){return v && !v->empty() && v->size()<=512 && !v->contains('\0');};
  auto number=[](auto v,std::uint64_t &n){return v && v->size()>2 && v->starts_with("0x") && !v->drop_front(2).getAsInteger(16,n);};
  Hint h;if(!name(f)||!name(b)||!number(s,h.state)||!number(t,h.target))return false;
  h.function=f->str();h.block=b->str();if(!seen.emplace(h.function,h.block).second)return false;out.push_back(std::move(h));
 }return true;
}
std::vector<llvm::BasicBlock *> blocks(llvm::Function &f){std::vector<llvm::BasicBlock *> result;for(auto &b:f)result.push_back(&b);return result;}
bool scalar(llvm::Type *t){return t->isIntegerTy() && t->getIntegerBitWidth()<=64;}
bool fits(std::uint64_t v,unsigned bits){return bits==64 || (v>>bits)==0;}
bool pathBlock(llvm::BasicBlock &b) {
 if(!llvm::isa<llvm::BranchInst,llvm::SwitchInst>(b.getTerminator()))return false;
 for(auto &i:b){if(llvm::isa<llvm::CallBase,llvm::AllocaInst,llvm::FenceInst,llvm::AtomicRMWInst,llvm::AtomicCmpXchgInst>(i))return false;
  if(auto *l=llvm::dyn_cast<llvm::LoadInst>(&i)){if(!l->isSimple())return false;continue;}
  if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i)){if(!s->isSimple())return false;continue;}
  if(!i.isTerminator() && (i.mayHaveSideEffects()||i.mayReadOrWriteMemory()))return false;
 }return true;
}
// Check visible dependencies without guessing loaded bits. Freeze is a stable
// boundary; LOAD operands are still checked for explicit undef/poison pointers.
bool clean(llvm::Value *v,Work &work,std::set<llvm::Value *> &active,std::set<llvm::Value *> &done,unsigned depth=0) {
 if(!work.charge())return false;
 if(depth>64){work.exhausted=true;return false;}
 if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(v))return false;
 if(done.contains(v))return true;
 auto *i=llvm::dyn_cast<llvm::Instruction>(v);if(!i)return true;
 if(llvm::isa<llvm::FreezeInst>(i))return true;
 if(auto *load=llvm::dyn_cast<llvm::LoadInst>(i))return !llvm::isa<llvm::UndefValue,llvm::PoisonValue>(load->getPointerOperand());
 if(!active.insert(v).second)return false;
 for(auto &operand:i->operands())if(!llvm::isa<llvm::BasicBlock>(operand.get()) && !clean(operand.get(),work,active,done,depth+1))return false;
 active.erase(v);done.insert(v);return true;
}
struct Plan {std::size_t seed;unsigned selected;std::vector<std::size_t> path;std::vector<unsigned> slots;};
// Search selects a prediction path only. Every copied condition remains live.
bool discover(llvm::Function &f,const Hint &h,Work &work,Plan &plan,std::string &error,bool &pathLimited) {
 std::vector<llvm::BasicBlock *> all;
 for(auto &block:f){if(!work.charge())return false;if(all.size()==65536){pathLimited=true;error="function block cap exhausted";return false;}all.push_back(&block);}
 if(f.isDeclaration()||f.hasPersonalityFn()||f.getSubprogram()){error="unsupported function identity";return false;}
 llvm::SmallVector<std::pair<unsigned,llvm::MDNode *>,4> functionMetadata;f.getAllMetadata(functionMetadata);
 if(!functionMetadata.empty()){error="function metadata identity unsupported";return false;}
 for(auto &arg:f.args())if(arg.isUsedByMetadata()){error="argument metadata identity unsupported";return false;}
 std::map<llvm::BasicBlock *,std::size_t> ordinal;std::vector<std::size_t> counts;std::vector<bool> safe;
 std::size_t total=0;llvm::BasicBlock *seed=nullptr;
 for(std::size_t n=0;n<all.size();++n){auto *b=all[n];ordinal.emplace(b,n);std::size_t count=0;
  if(b->isEHPad()||b->hasAddressTaken()||b->isUsedByMetadata()){error="EH or blockaddress identity";return false;}
  for(auto &i:*b){if(!work.charge(1+i.getNumOperands()))return false;++count;
   if(i.getDebugLoc()||i.hasDbgRecords()||i.isUsedByMetadata()){error="body debug or metadata identity";return false;}
  }
  if(!work.charge(count))return false;counts.push_back(count);safe.push_back(pathBlock(*b));total+=count;
  if(b->getName()==h.block)seed=b;
 }
 // Precharge both complete body clones and their verification/scanning passes.
 if(!work.charge(4*(total+all.size()))){error="whole-function transaction work exhausted";return false;}
 if(!seed){error="hint block missing";return false;}
 auto *branch=llvm::dyn_cast<llvm::BranchInst>(seed->getTerminator());
 if(!branch || !branch->isConditional()){error="seed is not a conditional branch";return false;}
 auto *cmp=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
 if(!cmp || !scalar(cmp->getOperand(0)->getType())){error="branch must directly consume scalar icmp";return false;}
 llvm::ConstantInt *constant=llvm::dyn_cast<llvm::ConstantInt>(cmp->getOperand(1));unsigned stateOperand=0;
 if(!constant){constant=llvm::dyn_cast<llvm::ConstantInt>(cmp->getOperand(0));stateOperand=1;}
 auto *state=cmp->getOperand(stateOperand);
 if(!constant || llvm::isa<llvm::Constant>(state) || !fits(h.state,state->getType()->getIntegerBitWidth())){error="unsupported state or literal width";return false;}
 std::set<llvm::Value *> active,done;if(!clean(state,work,active,done)){error="unknown historical cycle or explicit undef/poison dependency";return false;}
 llvm::DominatorTree dom(f);if(!dom.isReachableFromEntry(seed)){error="unreachable seed";return false;}
 auto k=llvm::APInt(state->getType()->getIntegerBitWidth(),h.state);
 bool selected=stateOperand?llvm::ICmpInst::compare(constant->getValue(),k,cmp->getPredicate()):llvm::ICmpInst::compare(k,constant->getValue(),cmp->getPredicate());
 plan.seed=ordinal.at(seed);plan.selected=selected?0:1;
 struct Search {std::vector<std::size_t> path;std::vector<unsigned> slots;std::size_t instructions;};
 auto first=ordinal.at(branch->getSuccessor(plan.selected));std::deque<Search> queue;queue.push_back({{first},{},counts[first]});std::set<std::size_t> seen;
 while(!queue.empty()) {auto q=std::move(queue.front());queue.pop_front();if(!work.charge(1+q.path.size()))return false;
  auto n=q.path.back();if(n==plan.seed || !seen.insert(n).second)continue;
  if(seen.size()>512){work.exhausted=true;error="path search work exhausted";return false;}
  if(!safe[n])continue;
  if(q.path.size()+2>32 || q.instructions+3>2048){pathLimited=true;error="bounded path exceeds transaction limits";continue;}
  auto *term=all[n]->getTerminator();
  if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(term)){
   if(!work.charge(sw->getNumCases()))return false;
   auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition());
   if(!load || load->getParent()!=all[n] || !load->isSimple() || !scalar(load->getType()) || !fits(h.target,load->getType()->getIntegerBitWidth()))continue;
   auto *candidate=llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(load->getType()),h.target);
   if(sw->findCaseValue(candidate)==sw->case_default())continue;
   plan.path=std::move(q.path);plan.slots=std::move(q.slots);return true;
  }
  for(unsigned slot=0;slot<term->getNumSuccessors();++slot){if(!work.charge(1+q.path.size()))return false;auto next=ordinal.at(term->getSuccessor(slot));
   if(std::find(q.path.begin(),q.path.end(),next)!=q.path.end())continue;
   auto r=q;r.path.push_back(next);r.slots.push_back(slot);r.instructions+=counts[next];queue.push_back(std::move(r));
  }
 }
 if(error.empty())error="no bounded effect-safe target path";return false;
}
llvm::Function *trialFor(llvm::Function &source) {
 auto *trial=llvm::Function::Create(source.getFunctionType(),llvm::GlobalValue::InternalLinkage,source.getAddressSpace(),source.getName()+".state.value.trial",source.getParent());trial->copyAttributesFrom(&source);
 llvm::ValueToValueMapTy map;auto arg=trial->arg_begin();for(auto &old:source.args()){arg->setName(old.getName());map[&old]=&*arg++;}
 llvm::SmallVector<llvm::ReturnInst *,8> returns;llvm::CloneFunctionInto(trial,&source,map,llvm::CloneFunctionChangeType::LocalChangesOnly,returns);
 trial->setLinkage(llvm::GlobalValue::InternalLinkage);trial->setVisibility(llvm::GlobalValue::DefaultVisibility);trial->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);trial->setComdat(nullptr);return trial;
}
bool commit(llvm::Function &source,llvm::Function &trial) {
 auto *backup=llvm::Function::Create(source.getFunctionType(),llvm::GlobalValue::InternalLinkage,source.getAddressSpace(),source.getName()+".state.value.rollback",source.getParent());backup->splice(backup->end(),&source);
 auto arg=source.arg_begin();for(auto &a:trial.args())a.replaceAllUsesWith(&*arg++);source.splice(source.end(),&trial);
 if(llvm::verifyFunction(source)){trial.splice(trial.end(),&source);source.splice(source.end(),backup);backup->eraseFromParent();return false;}
 backup->eraseFromParent();return true;
}
GuardedTransitionResult apply(llvm::Function &source,const Hint &h,const Plan &p,const GuardedTransitionOptions &limits) {
 GuardedTransitionResult result;
 if(limits.maxBlocks<2 || limits.maxInstructions<3){result.budgetExhausted=true;result.message="value guard copy budget exhausted";return result;}
 auto *trial=trialFor(source);auto all=blocks(*trial);auto *seed=all[p.seed];auto *old=llvm::cast<llvm::BranchInst>(seed->getTerminator());auto *cmp=llvm::cast<llvm::ICmpInst>(old->getCondition());
 auto *state=llvm::isa<llvm::ConstantInt>(cmp->getOperand(1))?cmp->getOperand(0):cmp->getOperand(1);
 auto *fallback=llvm::BasicBlock::Create(source.getContext(),"state.value.fallback",trial);auto *gate=llvm::BasicBlock::Create(source.getContext(),"state.value.gate",trial);
 auto *target=old->getSuccessor(p.selected);std::vector<std::pair<llvm::PHINode *,llvm::Value *>> selected;
 for(auto &phi:target->phis())selected.emplace_back(&phi,phi.getIncomingValueForBlock(seed));
 // Retain all original successor slots, including duplicate branch destinations.
 std::set<llvm::BasicBlock *> destinations;for(unsigned slot=0;slot<2;++slot)destinations.insert(old->getSuccessor(slot));
 for(auto *dest:destinations)for(auto &phi:dest->phis())for(unsigned i=0;i<phi.getNumIncomingValues();++i)if(phi.getIncomingBlock(i)==seed)phi.setIncomingBlock(i,fallback);
 old->removeFromParent();old->insertInto(fallback,fallback->end());
 llvm::IRBuilder<> b(seed);auto *equal=b.CreateICmpEQ(state,llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(state->getType()),h.state),"state.value.equal");auto *guard=b.CreateCondBr(equal,gate,fallback);guard->setMetadata(Consumed,llvm::MDNode::get(source.getContext(),{}));
 b.SetInsertPoint(gate);b.CreateBr(target);for(auto [phi,value]:selected)phi->addIncoming(value,gate);
 if(llvm::verifyFunction(*trial)){trial->eraseFromParent();result.message="invalid outer value-guard trial";return result;}
 GuardedTransitionRequest q;q.seed=gate;q.seedSuccessor=0;q.seedTarget=target;q.forwardSuccessors=p.slots;for(auto n:p.path)q.blocks.push_back(all[n]);auto *sw=llvm::cast<llvm::SwitchInst>(q.blocks.back()->getTerminator());q.candidate=llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(sw->getCondition()->getType()),h.target);
 auto nested=cloneGuardedTransition(*trial,q,{limits.maxBlocks-2,limits.maxInstructions-3});
 // Nested commit invalidates every body pointer above. Never use them again.
 if(!nested.changed){trial->eraseFromParent();return nested;}
 auto updated=blocks(*trial);for(std::size_t n=all.size();n<updated.size();++n)updated[n]->getTerminator()->setMetadata(Generated,llvm::MDNode::get(source.getContext(),{}));
 if(llvm::verifyFunction(*trial) || !commit(source,*trial)){trial->eraseFromParent();result.message="outer value-guard commit verification failed";return result;}
 trial->eraseFromParent();nested.clonedBlocks+=2;nested.clonedInstructions+=3;nested.complete=false;return nested;
}
}
GuardedTransitionsStats runGuardedStateValues(llvm::Module &module,const GuardedTransitionsOptions &options) {
 GuardedTransitionsStats result;auto *metadata=module.getNamedMetadata(Manifest);if(!metadata)return result;result.available=true;
 auto reject=[&](std::string message,bool budget=false){result.complete=false;++result.rejected;result.budgetExhausted+=budget;result.message=std::move(message);};
 if(options.maxCandidates>4096 || options.maxRewrites>4096 || options.maxBlocks>4096 || options.maxInstructions>262144){reject("state-values option hard limit exceeded",true);return result;}
 auto *text=metadata->getNumOperands()==1 && metadata->getOperand(0)->getNumOperands()==1?llvm::dyn_cast_or_null<llvm::MDString>(metadata->getOperand(0)->getOperand(0).get()):nullptr;
 std::vector<Hint> hints;if(!text || !parse(text->getString(),hints)){reject("invalid state-value hints");return result;}
 if(llvm::verifyModule(module)){reject("invalid input module");return result;}Work work;
 for(const auto &h:hints){auto *function=module.getFunction(h.function);if(!function){reject("hint function missing");continue;}
  bool consumed=false;for(auto &b:*function){if(!work.charge()){reject("state-value total work exhausted",true);break;}if(b.getName()==h.block && b.getTerminator()->getMetadata(Consumed))consumed=true;}
  if(work.exhausted)break;if(consumed)continue;
  Plan plan;std::string error;bool pathLimited=false;if(!discover(*function,h,work,plan,error,pathLimited)){reject(error.empty()?"state-value total work exhausted":error,work.exhausted||pathLimited);if(work.exhausted)break;continue;}
  ++result.candidateEdges;result.complete=false;
  if(result.candidateEdges>options.maxCandidates || result.rewrittenEdges>=options.maxRewrites || result.clonedBlocks>=options.maxBlocks || result.clonedInstructions>=options.maxInstructions){reject("state-value module budget exhausted",true);continue;}
  ++result.attemptedEdges;auto changed=apply(*function,h,plan,{std::min<std::size_t>(32,options.maxBlocks-result.clonedBlocks),std::min<std::size_t>(2048,options.maxInstructions-result.clonedInstructions)});
  if(!changed.changed){reject(changed.message,changed.budgetExhausted);continue;}
  result.changed=true;++result.rewrittenEdges;++result.fallbackSwitchesCreated;++result.originalSwitchesRetained;result.clonedBlocks+=changed.clonedBlocks;result.clonedInstructions+=changed.clonedInstructions;result.message="partial state-value predictions retain state and target fallbacks";
 }
 result.unresolvedEdges=result.candidateEdges-result.rewrittenEdges;return result;
}
}
