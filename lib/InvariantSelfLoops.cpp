#include "InvariantSelfLoops.hpp"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <utility>
#include <vector>
namespace dragon::llvmlifter::detail {
namespace {
constexpr const char *Generated="dragon.guarded-transitions.generated";
struct FunctionPlan {
  llvm::Function *function=nullptr;
  std::vector<llvm::BasicBlock *> candidates,accepted;
  std::size_t cloneWork=0;
  bool supported=true;
};
bool verify(llvm::Function &function,std::string &error) {
  llvm::raw_string_ostream out(error);return !llvm::verifyFunction(function,&out);
}
llvm::Function *trial(llvm::Function &source,llvm::ValueToValueMapTy &map) {
  auto *copy=llvm::Function::Create(source.getFunctionType(),llvm::GlobalValue::InternalLinkage,
    source.getAddressSpace(),source.getName()+".spin.trial",source.getParent());
  copy->copyAttributesFrom(&source);auto argument=copy->arg_begin();
  for(auto &old:source.args()){argument->setName(old.getName());map[&old]=&*argument++;}
  llvm::SmallVector<llvm::ReturnInst *,8> returns;
  // Recursive references remain ordinary references to the original function.
  llvm::CloneFunctionInto(copy,&source,map,llvm::CloneFunctionChangeType::LocalChangesOnly,returns);
  copy->setLinkage(llvm::GlobalValue::InternalLinkage);copy->setVisibility(llvm::GlobalValue::DefaultVisibility);
  copy->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);copy->setComdat(nullptr);return copy;
}
bool commit(llvm::Function &source,llvm::Function &copy,std::string &error) {
  auto *backup=llvm::Function::Create(source.getFunctionType(),llvm::GlobalValue::InternalLinkage,
    source.getAddressSpace(),source.getName()+".spin.rollback",source.getParent());
  backup->splice(backup->end(),&source);auto argument=source.arg_begin();
  for(auto &value:copy.args())value.replaceAllUsesWith(&*argument++);
  source.splice(source.end(),&copy);
  if(!verify(source,error)) {
    copy.splice(copy.end(),&source);source.splice(source.end(),backup);backup->eraseFromParent();return false;
  }
  backup->eraseFromParent();return true;
}
}
InvariantSelfLoopsStats runInvariantSelfLoops(llvm::Module &module,const InvariantSelfLoopsOptions &options) {
  InvariantSelfLoopsStats result;
  auto reject=[&](const char *message,bool budget=false){result.complete=false;++result.rejected;result.budgetExhausted+=budget;result.message=message;};
  auto charge=[&](std::size_t n){if(n>options.maxWork-result.work){reject("invariant-self-loops work budget exhausted",true);return false;}result.work+=n;return true;};
  if(options.maxLoops>65536 || options.maxWork>1000000 || options.maxBlocks>65536){reject("invariant-self-loops option hard limit exceeded",true);return result;}
  std::vector<FunctionPlan> plans;std::size_t blocks=0;
  for(auto &function:module) {
    if(!charge(1))return result;
    if(function.isDeclaration())continue;
    result.available=true;FunctionPlan plan;plan.function=&function;
    plan.supported=!function.hasPersonalityFn() && !function.getSubprogram() &&
      !function.hasFnAttribute(llvm::Attribute::MustProgress) && !function.hasFnAttribute(llvm::Attribute::WillReturn);
    for(auto &block:function) {
      if(++blocks>options.maxBlocks){reject("invariant-self-loops block scan budget exhausted",true);return result;}
      if(!charge(1))return result;
      if(block.isEHPad() || block.hasAddressTaken())plan.supported=false;
      auto *term=block.getTerminator();if(!term){reject("invalid invariant-self-loops input block");return result;}
      for(auto &instruction:block) {
        const std::size_t cost=1+static_cast<std::size_t>(instruction.getNumOperands());if(!charge(cost))return result;
        plan.cloneWork+=cost;++result.scannedInstructions;
        if(instruction.getDebugLoc() || instruction.hasDbgRecords() || instruction.isUsedByMetadata())plan.supported=false;
      }
      if(auto *branch=llvm::dyn_cast<llvm::BranchInst>(term);branch && branch->isConditional() &&
          (branch->getSuccessor(0)==&block || branch->getSuccessor(1)==&block)) {
        plan.candidates.push_back(&block);++result.candidateLoops;
      }
    }
    if(!plan.candidates.empty())plans.push_back(std::move(plan));
  }
  if(!result.available)return result;
  if(llvm::verifyModule(module)){reject("invalid invariant-self-loops input module");return result;}
  std::size_t accepted=0;
  for(auto &plan:plans) {
    for(auto *block:plan.candidates) {
      if(!plan.supported){reject("invariant-self-loops does not support progress EH debug or blockaddress identity");continue;}
      auto *branch=llvm::cast<llvm::BranchInst>(block->getTerminator());
      if(branch->getSuccessor(0)!=block || branch->getSuccessor(1)==block || branch->getMetadata(llvm::LLVMContext::MD_loop)) {
        reject("invariant-self-loops requires one true self edge without loop metadata");continue;
      }
      auto *compare=llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
      auto *state=compare?llvm::dyn_cast<llvm::PHINode>(compare->getOperand(0)):nullptr;
      if(!compare || compare->getParent()!=block || compare->getPredicate()!=llvm::ICmpInst::ICMP_UGT ||
          !state || state->getParent()!=block || !state->getType()->isIntegerTy() || state->getType()->getIntegerBitWidth()>128 ||
          !llvm::isa<llvm::ConstantInt>(compare->getOperand(1))) {
        reject("invariant-self-loops requires scalar PHI versus constant unsigned comparison");continue;
      }
      bool valid=true;std::size_t ordinary=0;
      for(auto &instruction:*block) {
        if(!charge(1))return result;
        auto *phi=llvm::dyn_cast<llvm::PHINode>(&instruction);
        if(!phi){++ordinary;if(&instruction!=compare && &instruction!=branch)valid=false;continue;}
        unsigned self=0,external=0;
        for(unsigned n=0;n<phi->getNumIncomingValues();++n) {
          if(!charge(1))return result;
          auto *value=phi->getIncomingValue(n);
          if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(value))valid=false;
          if(phi->getIncomingBlock(n)==block){++self;if(value!=phi)valid=false;}else ++external;
        }
        if(self!=1 || !external)valid=false;
      }
      if(!valid || ordinary!=2){reject("invariant-self-loops requires pure self-preserving PHIs and external entry");continue;}
      plan.accepted.push_back(block);++accepted;
    }
  }
  // Reserve all copying/added-block work before the first transaction. Each
  // affected function is cloned once, irrespective of its number of loops.
  if(accepted>options.maxLoops){reject("invariant-self-loops isolation budget exhausted",true);return result;}
  if(accepted>options.maxBlocks-blocks){reject("invariant-self-loops added block budget exhausted",true);return result;}
  for(const auto &plan:plans)if(!plan.accepted.empty())if(!charge(plan.cloneWork+plan.accepted.size()))return result;
  for(const auto &plan:plans) {
    if(plan.accepted.empty())continue;
    llvm::ValueToValueMapTy map;auto *copy=trial(*plan.function,map);std::size_t removed=0;
    for(auto *old:plan.accepted) {
      auto *block=llvm::cast<llvm::BasicBlock>(map.lookup(old));auto *branch=llvm::cast<llvm::BranchInst>(block->getTerminator());
      auto *spin=llvm::BasicBlock::Create(module.getContext(),block->getName()+".spin",copy);
      auto *back=llvm::BranchInst::Create(spin,spin);back->setMetadata(Generated,llvm::MDNode::get(module.getContext(),{}));
      // Keep the original comparison and its first branch. Branching on
      // poison/undef remains at that original boundary; no value is frozen.
      branch->setSuccessor(0,spin);
      for(auto &phi:block->phis()){phi.removeIncomingValue(phi.getBasicBlockIndex(block),false);++removed;}
    }
    std::string error;
    if(!verify(*copy,error) || !commit(*plan.function,*copy,error)){copy->eraseFromParent();reject("invariant-self-loops transaction verification failed");result.message+=" : "+error;continue;}
    copy->eraseFromParent();result.changed=true;result.isolatedLoops+=plan.accepted.size();result.addedBlocks+=plan.accepted.size();result.removedSelfIncomings+=removed;
  }
  return result;
}
}
