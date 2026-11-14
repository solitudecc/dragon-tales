#include "GuardedStateCorridors.hpp"
#include "GuardedTransition.hpp"
#include "GuardedTransitionRootPHI.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
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
constexpr const char *ContinuationGenerated="dragon.guarded-checked-corridors.generated";
constexpr const char *StoreExpressionGenerated="dragon.guarded-store-expressions.generated";
constexpr std::size_t MaxWork=1000000,MaxNodes=128,MaxDepth=32,MaxValues=8;
struct Work {
  std::size_t count=0;bool exhausted=false;
  bool charge(std::size_t n=1){if(n>MaxWork-count){exhausted=true;return false;}count+=n;return true;}
};
// These scalar math intrinsics may be observed only after original execution.
// This whitelist is not a general call-effect or definedness proof.
bool integerFunnelShift(llvm::Instruction &instruction,Work &work) {
  if(!work.charge(6))return false;
  auto *intrinsic=llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
  if(!intrinsic || (intrinsic->getIntrinsicID()!=llvm::Intrinsic::fshl &&
                    intrinsic->getIntrinsicID()!=llvm::Intrinsic::fshr) ||
     !intrinsic->getType()->isIntegerTy() || intrinsic->arg_size()!=3 ||
     intrinsic->getNumOperandBundles() || intrinsic->isConvergent() ||
     intrinsic->mayReadOrWriteMemory() || intrinsic->mayHaveSideEffects())return false;
  auto *callee=intrinsic->getCalledFunction();
  if(!callee || !callee->isDeclaration() || callee->isConvergent() ||
     callee->isVarArg() || callee->getFunctionType()!=intrinsic->getFunctionType())return false;
  for(auto &argument:intrinsic->args()) {
    if(!work.charge())return false;
    if(argument->getType()!=intrinsic->getType() ||
       llvm::isa<llvm::UndefValue,llvm::PoisonValue>(argument.get()))return false;
  }
  return true;
}
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
struct BooleanScenario {
  std::unordered_map<Key,bool,KeyHash> values;
  std::optional<Key> missing;
};
struct StoreExpressionOverride {
  std::optional<Key> atom;
  const Values *values=nullptr;
  bool invalid=false,used=false;
};
// A syntactic Guest atom, not an alias or memory-content proof. Keep this
// separate from the historical i64-only STORE profile's admission rules.
llvm::Value *storeExpressionGuest(llvm::LoadInst &load,Work &work) {
  if(!work.charge(6))return nullptr;
  auto *type=llvm::dyn_cast<llvm::IntegerType>(load.getType());
  if(!type || (type->getBitWidth()!=8 && type->getBitWidth()!=16 &&
               type->getBitWidth()!=32 && type->getBitWidth()!=64) ||
     !load.isSimple() || load.getPointerAddressSpace()!=0)return nullptr;
  auto *address=llvm::dyn_cast<llvm::GetElementPtrInst>(load.getPointerOperand());
  if(!address || address->getPointerAddressSpace()!=0 ||
     !address->getSourceElementType()->isIntegerTy(8) || address->getNumIndices()!=1 ||
     !address->getOperand(1)->getType()->isIntegerTy(64) ||
     llvm::isa<llvm::UndefValue,llvm::PoisonValue>(address->getOperand(1)))return nullptr;
  auto *guest=llvm::dyn_cast<llvm::LoadInst>(address->getPointerOperand());
  auto *function=load.getFunction();
  if(!guest || !guest->isSimple() || !guest->getType()->isPointerTy() ||
     guest->getPointerAddressSpace()!=0 || function->arg_size()<2 ||
     guest->getPointerOperand()!=function->getArg(1))return nullptr;
  return guest;
}
// This enumerator predicts target-table addresses. Its arithmetic is modular;
// flags are neither stripped nor used to establish absence of poison. The
// original expressions still execute before the actual target guard.
struct Enumerator {
  llvm::BasicBlock *seed;
  const std::vector<llvm::BasicBlock *> &path;
  Work &work;std::size_t nodes=0;bool limited=false,booleanUnknown=false,historicalPhiSeen=false,failed=false;
  BooleanScenario *scenario=nullptr;
  const llvm::DominatorTree *historicalDom=nullptr;
  const llvm::DominatorTree *observationDom=nullptr;
  std::unordered_set<llvm::Value *> checkedHistory,activeHistory,cyclicHistory;
  bool allowPartial=false,partialMasks=false,bitSlices=false,usedPartial=false,fatalPartial=false;
  StoreExpressionOverride *storeExpression=nullptr;
  bool strictFailures=false;
  std::unordered_map<llvm::BasicBlock *,std::size_t> positions;
  std::unordered_map<Key,Values,KeyHash> memo;
  std::unordered_set<Key,KeyHash> active;
  Enumerator(llvm::BasicBlock *s,const std::vector<llvm::BasicBlock *> &p,Work &w,BooleanScenario *sc=nullptr,const llvm::DominatorTree *hd=nullptr,bool partial=false,bool masks=false,bool slices=false,StoreExpressionOverride *overrides=nullptr,bool strict=false,const llvm::DominatorTree *observations=nullptr):seed(s),path(p),work(w),scenario(sc),historicalDom(hd),observationDom(observations),allowPartial(partial),partialMasks(masks),bitSlices(slices),storeExpression(overrides),strictFailures(strict || overrides) {
    for(std::size_t n=0;n<p.size();++n)positions.emplace(p[n],n);
  }
  bool append(Values &to,const llvm::APInt &value) {
    if(!work.charge())return false;
    if(std::find(to.begin(),to.end(),value)==to.end())to.push_back(value);
    if(to.size()>MaxValues){limited=true;fatalPartial|=allowPartial;return false;}return true;
  }
  // Only validates the bounded SSA dependency shape of an old Boolean PHI.
  // It does not prove a value, memory contents or absence of flag-produced poison.
  // LOAD/freeze are existing execution boundaries; the original IR still runs.
  // Only the new observation profile closes bounded dependency graphs. A
  // gray edge is pending work in this check, never a cached valid result or a
  // value. All remaining operands/PHI arms must close before any publication.
  bool closedHistoricalBoolean(llvm::Value *root,unsigned rootDepth) {
    struct Shape {bool pending=true,cyclic=false;};
    std::unordered_map<llvm::Value *,Shape> local;
    auto visit=[&](auto &&self,llvm::Value *value,unsigned depth)->std::optional<bool> {
      if(!work.charge())return {};
      if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(value))return {};
      if(checkedHistory.contains(value))return cyclicHistory.contains(value);
      if(auto found=local.find(value);found!=local.end())
        return found->second.pending?true:found->second.cyclic;
      // Only first expansions consume nodes/depth. A gray/cache reference is
      // charged above, but does not restart depth or hide later new operands.
      if(depth>MaxDepth || ++nodes>MaxNodes){limited=true;return {};}
      local.emplace(value,Shape{});
      bool valid=false,cyclic=false;
      if(llvm::isa<llvm::ConstantInt,llvm::Argument,llvm::FreezeInst>(value))valid=true;
      else if(auto *load=llvm::dyn_cast<llvm::LoadInst>(value)) {
        valid=load->isSimple();
        for(auto &operand:load->operands()) {
          if(!work.charge())return {};
          if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(operand.get()))valid=false;
        }
      } else if(auto *intrinsic=llvm::dyn_cast<llvm::IntrinsicInst>(value)) {
        valid=!positions.contains(intrinsic->getParent()) &&
              observationDom->dominates(intrinsic,seed->getTerminator()) &&
              integerFunnelShift(*intrinsic,work);
      } else if(llvm::isa<llvm::PHINode,llvm::SelectInst,llvm::CastInst,
                           llvm::BinaryOperator,llvm::ICmpInst>(value)) {
        valid=true;
        for(auto &operand:llvm::cast<llvm::Instruction>(value)->operands()) {
          auto child=self(self,operand.get(),depth+1);
          if(!child){valid=false;break;}
          cyclic|=*child;
        }
      }
      if(!valid)return {};
      // A false result has no reachable gray edge and only acyclic children;
      // a true result carries a reachable DFS backedge witness. Thus cache
      // cycle bits belong to each node, never indiscriminately to all leaves.
      auto &closed=local.at(value);closed.pending=false;closed.cyclic=cyclic;
      return cyclic;
    };
    auto result=visit(visit,root,rootDepth);
    if(!result)return false;
    if(*result) {
      if(!work.charge(3))return false;
      auto *instruction=llvm::dyn_cast<llvm::Instruction>(root);
      if(!instruction || positions.contains(instruction->getParent()) ||
         !observationDom->dominates(instruction,seed->getTerminator()))return false;
    }
    // Reserve every publication visit before changing the shared cache. Any
    // failure, including a late bad arm/leaf or this budget, discards local.
    if(!work.charge(local.size()))return false;
    for(const auto &[value,shape]:local) {
      checkedHistory.insert(value);
      if(shape.cyclic)cyclicHistory.insert(value);
    }
    return true;
  }
  bool safeHistoricalBoolean(llvm::Value *value,unsigned depth=0) {
    // observationDom is supplied only by guarded-store-expressions. Preserve
    // every historical profile's recursive admission/cache and get() cycles.
    if(observationDom)return closedHistoricalBoolean(value,depth);
    if(!work.charge())return false;
    if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(value))return false;
    if(checkedHistory.contains(value))return true;
    if(depth>MaxDepth || ++nodes>MaxNodes){limited=true;return false;}
    if(!activeHistory.insert(value).second)return false;
    bool valid=false;
    if(llvm::isa<llvm::ConstantInt,llvm::Argument,llvm::FreezeInst>(value))valid=true;
    else if(auto *load=llvm::dyn_cast<llvm::LoadInst>(value)) {
      valid=load->isSimple();
      for(auto &operand:load->operands()) {
        if(!work.charge())return false;
        if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(operand.get()))valid=false;
      }
    } else if(auto *intrinsic=llvm::dyn_cast<llvm::IntrinsicInst>(value)) {
      // A dominating original result is an untrusted observation boundary.
      // Never evaluate it, derive a value/poison fact, or admit a path call.
      valid=observationDom && !positions.contains(intrinsic->getParent()) &&
            observationDom->dominates(intrinsic,seed->getTerminator()) &&
            integerFunnelShift(*intrinsic,work);
    } else if(llvm::isa<llvm::PHINode,llvm::SelectInst,llvm::CastInst,
                         llvm::BinaryOperator,llvm::ICmpInst>(value)) {
      valid=true;
      for(auto &operand:llvm::cast<llvm::Instruction>(value)->operands())
        if(!safeHistoricalBoolean(operand.get(),depth+1)){valid=false;break;}
    }
    activeHistory.erase(value);
    if(valid)checkedHistory.insert(value);
    return valid;
  }
  MaybeValues get(llvm::Value *value,bool historical=false,unsigned depth=0) {
    if(!work.charge())return {};
    auto *type=llvm::dyn_cast<llvm::IntegerType>(value->getType());
    if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(value)){failed=true;fatalPartial|=allowPartial;return {};}
    if(!type || type->getBitWidth()>128)return {};
    Key key{value,historical};auto found=memo.find(key);if(found!=memo.end())return found->second;
    if(depth>MaxDepth || ++nodes>MaxNodes){failed=true;limited=true;fatalPartial|=allowPartial;return {};}
    if(!active.insert(key).second){failed=true;fatalPartial|=allowPartial;return {};}
    auto result=compute(value,historical,depth);active.erase(key);
    if(result)memo.emplace(key,*result);return result;
  }
  MaybeValues rightSlice(llvm::BinaryOperator &binary,bool historical,unsigned depth) {
    // This is an untrusted result-bit-pattern atom, not a value/definedness
    // proof or a recursive interpretation of an old/fresh register PHI.
    auto *source=binary.getOperand(0);
    if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(source)){fatalPartial=true;return {};}
    if(auto *instruction=llvm::dyn_cast<llvm::Instruction>(source);
       instruction && !llvm::isa<llvm::FreezeInst>(instruction)) {
      for(auto &operand:instruction->operands()) {
        if(!work.charge())return {};
        if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(operand.get())){fatalPartial=true;return {};}
      }
    }
    auto amounts=get(binary.getOperand(1),historical,depth+1);
    if(!amounts || fatalPartial || work.exhausted)return {};
    const unsigned width=binary.getType()->getIntegerBitWidth();
    for(const auto &amount:*amounts) {
      if(!work.charge())return {};
      if(amount.uge(width)){fatalPartial=true;return {};}
    }
    Values result;
    for(const auto &amount:*amounts) {
      const auto shift=static_cast<unsigned>(amount.getZExtValue());
      if(auto *constant=llvm::dyn_cast<llvm::ConstantInt>(source)) {
        auto value=binary.getOpcode()==llvm::Instruction::LShr?
          constant->getValue().lshr(shift):constant->getValue().ashr(shift);
        if(!append(result,value))return {};
        continue;
      }
      const unsigned bits=width-shift;
      if(bits>3)continue; // Other outcomes remain in the complete fallback.
      for(unsigned pattern=0;pattern<(1u<<bits);++pattern) {
        llvm::APInt value(bits,pattern);
        value=binary.getOpcode()==llvm::Instruction::LShr?
          value.zextOrTrunc(width):value.sextOrTrunc(width);
        if(!append(result,value))return {};
      }
    }
    if(result.empty())return {};
    usedPartial=true;return result;
  }
  MaybeValues compute(llvm::Value *value,bool historical,unsigned depth) {
    if(auto *constant=llvm::dyn_cast<llvm::ConstantInt>(value))return Values{constant->getValue()};
    if(storeExpression)if(auto *load=llvm::dyn_cast<llvm::LoadInst>(value)) {
      if(nodes>MaxNodes-2){failed=true;limited=true;return {};}nodes+=2;
      if(!storeExpressionGuest(*load,work)){storeExpression->invalid=true;return {};}
      const Key atom{load,historical || !positions.contains(load->getParent())};
      if(storeExpression->atom && *storeExpression->atom!=atom){storeExpression->invalid=true;return {};}
      storeExpression->atom=atom;storeExpression->used=true;
      if(storeExpression->values) {
        if(!work.charge(storeExpression->values->size()))return {};
        for(const auto &candidate:*storeExpression->values)
          if(candidate.getBitWidth()!=load->getType()->getIntegerBitWidth())return {};
        return *storeExpression->values;
      }
      // A zero placeholder discovers a unique atom only. Its result is never
      // published or used as a prediction; real STORE candidates are evaluated
      // in a new, independently memoized execution view below.
      Values placeholder;
      if(!append(placeholder,llvm::APInt(load->getType()->getIntegerBitWidth(),0)))return {};
      return placeholder;
    }
    if(value->getType()->isIntegerTy(1)) {
      bool atom=llvm::isa<llvm::Argument,llvm::ICmpInst,llvm::FCmpInst,llvm::FreezeInst>(value);
      if(auto *load=llvm::dyn_cast<llvm::LoadInst>(value))atom=load->isSimple();
      if(strictFailures && atom && !safeHistoricalBoolean(value,depth+1)){failed=true;return {};}
      if(atom && !scenario)booleanUnknown=true;
      if(atom && !scenario && allowPartial && partialMasks) {
        // Cartesian bit-pattern hints only: no old/fresh identity correlation,
        // definedness or range fact is exported. Actual source operations run.
        if(auto *instruction=llvm::dyn_cast<llvm::Instruction>(value);
           instruction && !llvm::isa<llvm::FreezeInst>(instruction)) {
          for(auto &operand:instruction->operands()) {
            if(!work.charge())return {};
            if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(operand.get())){fatalPartial=true;return {};}
          }
        }
        Values bits;
        if(!append(bits,llvm::APInt(1,0)) || !append(bits,llvm::APInt(1,1)))return {};
        usedPartial=true;return bits;
      }
      if(atom && scenario) {
        // A prior/external definition has one old value, while an instruction
        // executed again in the path has a separate fresh Boolean identity.
        bool old=historical;
        if(auto *instruction=llvm::dyn_cast<llvm::Instruction>(value)) {
          old|=!positions.contains(instruction->getParent());
          if(!llvm::isa<llvm::FreezeInst>(instruction))for(auto &operand:instruction->operands()) {
            if(!work.charge())return {};
            if(llvm::isa<llvm::UndefValue,llvm::PoisonValue>(operand.get()))return {};
          }
        } else old=false; // Function arguments do not execute again.
        Key key{value,old};auto assigned=scenario->values.find(key);
        if(assigned==scenario->values.end()) {if(!scenario->missing)scenario->missing=key;return {};}
        return Values{llvm::APInt(1,assigned->second)};
      }
    }

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
      if(phi->getType()->isIntegerTy(1)) {
        historicalPhiSeen=true;
        if(scenario && historicalDom) {
          if(!historicalDom->dominates(phi,seed->getTerminator()) ||
             !safeHistoricalBoolean(phi))return {};
          // A previously executed PHI has one value, not the union of all
          // possible predecessor values in a single Boolean scenario.
          Key key{phi,true};auto assigned=scenario->values.find(key);
          if(assigned==scenario->values.end()) {
            if(!scenario->missing)scenario->missing=key;
            return {};
          }
          return Values{llvm::APInt(1,assigned->second)};
        }
      }
      Values result;
      for(auto &incoming:phi->incoming_values()) {
        auto values=get(incoming,true,depth+1);if(!values)return {};
        for(const auto &v:*values)if(!append(result,v))return {};
      }
      return result.empty()?MaybeValues{}:MaybeValues{result};
    }
    if(auto *select=llvm::dyn_cast<llvm::SelectInst>(value)) {
      if((allowPartial || strictFailures) && llvm::isa<llvm::UndefValue,llvm::PoisonValue>(select->getCondition())){failed=true;fatalPartial=true;return {};}
      if(strictFailures && !safeHistoricalBoolean(select->getCondition(),depth+1)){failed=true;return {};}
      // The ordinary enumerator still handles finite SSA/select predictions
      // first. The new LOAD-substitution fallback admits only a single typed
      // arithmetic atom, not an unvisited condition/another LOAD hidden in it.
      if(storeExpression){storeExpression->invalid=true;return {};}
      if(scenario) {
        auto condition=get(select->getCondition(),historical,depth+1);
        if(!condition || condition->size()!=1)return {};
        return get((*condition)[0].isZero()?select->getFalseValue():select->getTrueValue(),historical,depth+1);
      }
      auto yes=get(select->getTrueValue(),historical,depth+1),no=get(select->getFalseValue(),historical,depth+1);
      if(!yes || !no) {
        // This separate prediction mode can retain an explicitly finite select
        // arm. It is never a complete value set and never becomes a fact.
        if(!allowPartial || fatalPartial || work.exhausted || (!yes && !no))return {};
        usedPartial=true;return yes?yes:no;
      }
      Values result=*yes;for(const auto &v:*no)if(!append(result,v))return {};return result;
    }
    if(auto *cast=llvm::dyn_cast<llvm::CastInst>(value)) {
      const auto opcode=cast->getOpcode();if(opcode!=llvm::Instruction::ZExt && opcode!=llvm::Instruction::SExt && opcode!=llvm::Instruction::Trunc)return {};
      auto input=get(cast->getOperand(0),historical,depth+1);if(!input)return {};Values result;auto width=cast->getType()->getIntegerBitWidth();
      for(const auto &v:*input)if(!append(result,opcode==llvm::Instruction::SExt?v.sextOrTrunc(width):v.zextOrTrunc(width)))return {};return result;
    }
    auto *binary=llvm::dyn_cast<llvm::BinaryOperator>(value);if(!binary)return {};
    const auto opcode=binary->getOpcode();
    if(bitSlices && allowPartial && !scenario &&
       (opcode==llvm::Instruction::LShr || opcode==llvm::Instruction::AShr))
      return rightSlice(*binary,historical,depth);
    if(opcode!=llvm::Instruction::Add && opcode!=llvm::Instruction::Sub &&
       opcode!=llvm::Instruction::Mul && opcode!=llvm::Instruction::Shl &&
       opcode!=llvm::Instruction::And && opcode!=llvm::Instruction::Or &&
       opcode!=llvm::Instruction::Xor)return {};
    auto left=get(binary->getOperand(0),historical,depth+1);
    auto right=get(binary->getOperand(1),historical,depth+1);
    if(allowPartial && partialMasks && opcode==llvm::Instruction::Shl && right) {
      for(const auto &amount:*right) {
        if(!work.charge())return {};
        if(amount.uge(binary->getType()->getIntegerBitWidth())){fatalPartial=true;return {};}
      }
    }
    if(!left || !right) {
      if(allowPartial && partialMasks && !fatalPartial && !work.exhausted &&
         opcode==llvm::Instruction::And && bool(left)!=bool(right)) {
        // Retain only a zero-mask prediction; every nonzero unknown outcome
        // remains in the actual target LOAD's complete fallback. Never discard
        // a dependency's explicit poison/undef, cycle or resource failure.
        for(const auto &mask:(left?*left:*right)) {
          if(!work.charge())return {};
          if(mask.isZero()){usedPartial=true;return Values{mask};}
        }
      }
      if(opcode!=llvm::Instruction::And || work.exhausted || scenario || allowPartial || storeExpression)return {};
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
      else {if(b.uge(a.getBitWidth())){failed=true;fatalPartial|=allowPartial;return {};}v=a.shl(static_cast<unsigned>(b.getZExtValue()));}
      if(!append(result,v))return {};
    }
    return result;
  }
};
// Only called after the old SSA enumeration failed. Every complete Boolean
// assignment must yield finite values; unknown integer arms never disappear.
MaybeValues correlatedAddresses(llvm::BasicBlock &seed,const std::vector<llvm::BasicBlock *> &path,
    llvm::Value *index,Work &work,bool &limited,const llvm::DominatorTree *historicalDom=nullptr,bool *sawHistoricalPhi=nullptr,bool strictFailures=false,const llvm::DominatorTree *observationDom=nullptr) {
  std::deque<BooleanScenario> pending(1);
  std::unordered_set<Key,KeyHash> keys;
  Values result;std::size_t complete=0;
  while(!pending.empty()) {
    if(!work.charge(1+path.size()))return {};
    auto scenario=std::move(pending.front());pending.pop_front();
    Enumerator enumerate(&seed,path,work,&scenario,historicalDom,false,false,false,nullptr,strictFailures,observationDom);auto values=enumerate.get(index);limited|=enumerate.limited;
    if(sawHistoricalPhi)*sawHistoricalPhi|=enumerate.historicalPhiSeen;
    if(strictFailures && (enumerate.failed || enumerate.limited))return {};
    if(work.exhausted)return {};
    if(scenario.missing) {
      auto key=*scenario.missing;
      if(!work.charge())return {};
      keys.insert(key);
      if(keys.size()>3){limited=true;return {};}
      for(bool bit:{false,true}) {
        if(!work.charge(1+scenario.values.size()))return {};
        auto next=scenario;next.missing.reset();next.values.emplace(key,bit);pending.push_back(std::move(next));
      }
      continue;
    }
    if(!values)return {};
    if(++complete>8){limited=true;return {};}
    for(const auto &v:*values) {
      if(!work.charge())return {};
      if(std::find(result.begin(),result.end(),v)==result.end())result.push_back(v);
      if(result.size()>MaxValues){limited=true;return {};}
    }
  }
  return result.empty()?MaybeValues{}:MaybeValues{result};
}
llvm::Value *indexOf(llvm::BasicBlock &block) {
  auto *jump=llvm::dyn_cast<llvm::SwitchInst>(block.getTerminator());auto *load=jump && jump->getNumSuccessors()<=512?llvm::dyn_cast<llvm::LoadInst>(jump->getCondition()):nullptr;
  if(!load || load->getParent()!=&block || !load->isSimple() || !load->getType()->isIntegerTy(64) || load->getPointerAddressSpace()!=0)return nullptr;
  auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());if(!gep || gep->getPointerAddressSpace()!=0 || !gep->getSourceElementType()->isIntegerTy(8) || gep->getNumIndices()!=1 || !gep->getOperand(1)->getType()->isIntegerTy(64))return nullptr;
  auto *base=llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());auto *f=block.getParent();
  if(!base || !base->isSimple() || !base->getType()->isPointerTy() || f->arg_size()<2 || base->getPointerOperand()!=f->getArg(1))return nullptr;
  return llvm::isa<llvm::ConstantInt>(gep->getOperand(1))?nullptr:gep->getOperand(1);
}
bool safeBlock(llvm::BasicBlock &b,bool terminal=false,bool executedSeed=false,Work *work=nullptr) {
  if(b.isEHPad() || b.hasAddressTaken() || !(terminal?llvm::isa<llvm::SwitchInst>(b.getTerminator()):llvm::isa<llvm::BranchInst>(b.getTerminator())))return false;
  for(auto &i:b) {
    if(llvm::isa<llvm::CallBase>(i)) {
      if(!executedSeed || !work || !integerFunnelShift(i,*work))return false;
      continue;
    }
    if(llvm::isa<llvm::AllocaInst,llvm::FenceInst,llvm::AtomicRMWInst,llvm::AtomicCmpXchgInst>(i))return false;
    if(auto *l=llvm::dyn_cast<llvm::LoadInst>(&i);l && !l->isSimple())return false;
    if(auto *s=llvm::dyn_cast<llvm::StoreInst>(&i);s && !s->isSimple())return false;
  }
  return true;
}
bool generated(llvm::BasicBlock &b){return b.getTerminator()->getMetadata(Generated)!=nullptr;}
std::vector<llvm::BasicBlock *> blocks(llvm::Function &f){std::vector<llvm::BasicBlock *> out;for(auto &b:f)out.push_back(&b);return out;}
struct Edge {std::size_t seed;unsigned slot;std::vector<std::size_t> path;std::vector<unsigned> slots;std::vector<std::uint64_t> predictions;bool partial=false,loop=false;};
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
MaybeValues storeExpressionAddresses(llvm::BasicBlock &seed,
    const std::vector<llvm::BasicBlock *> &path,llvm::Value *index,
    Work &work,bool &limited,const llvm::DominatorTree &dom) {
  if(!work.charge(1+path.size()))return {};
  StoreExpressionOverride probe;
  Enumerator shape(&seed,path,work,nullptr,nullptr,false,false,false,&probe,false,&dom);
  auto placeholder=shape.get(index);limited|=shape.limited;
  if(!placeholder || !probe.atom || probe.invalid || !probe.used ||
     shape.failed || shape.limited || work.exhausted)return {};
  auto *load=llvm::cast<llvm::LoadInst>(probe.atom->value);
  auto *guest=storeExpressionGuest(*load,work);if(!guest)return {};
  bool abort=false;
  auto attempt=[&](llvm::Instruction &instruction,bool historical)->MaybeValues {
    if(!work.charge())return {};
    auto *store=llvm::dyn_cast<llvm::StoreInst>(&instruction);
    if(!store || !store->isSimple() || store->getPointerAddressSpace()!=0 ||
       store->getValueOperand()->getType()!=load->getType())return {};
    if(!work.charge(4))return {};
    auto *address=llvm::dyn_cast<llvm::GetElementPtrInst>(store->getPointerOperand());
    if(!address || address->getPointerAddressSpace()!=0 ||
       address->getPointerOperand()!=guest || !address->getSourceElementType()->isIntegerTy(8) ||
       address->getNumIndices()!=1 || !address->getOperand(1)->getType()->isIntegerTy(64) ||
       llvm::isa<llvm::UndefValue,llvm::PoisonValue>(address->getOperand(1)))return {};
    if(!work.charge(path.size()))return {};
    Enumerator stored(&seed,path,work,nullptr,nullptr,false,false,false,nullptr,true,&dom);
    auto values=stored.get(store->getValueOperand(),historical);limited|=stored.limited;
    // Unknown is not a memory fact. Structural/poison/cycle/budget failures
    // cannot be silently discarded while trying an older finite STORE.
    if(stored.failed || stored.limited || work.exhausted){abort=true;return {};}
    if(!values)return {};
    StoreExpressionOverride replacement;replacement.atom=probe.atom;replacement.values=&*values;
    Enumerator predicted(&seed,path,work,nullptr,nullptr,false,false,false,&replacement,false,&dom);
    auto addresses=predicted.get(index);limited|=predicted.limited;
    if(!addresses || replacement.invalid || !replacement.used || predicted.failed ||
       predicted.limited || work.exhausted){abort=true;return {};}
    return addresses;
  };
  // Preserve the old seed-before-path prediction order, but use the actual
  // integer width and full address DAG. No STORE address is claimed to alias
  // the atom, and neither the STORE nor any real LOAD is changed.
  for(auto it=seed.rbegin();it!=seed.rend();++it) {
    auto addresses=attempt(*it,true);if(addresses)return addresses;
    if(abort || work.exhausted)return {};
  }
  for(auto block=path.rbegin();block!=path.rend();++block)
    for(auto it=(*block)->rbegin();it!=(*block)->rend();++it) {
      auto addresses=attempt(*it,false);if(addresses)return addresses;
      if(abort || work.exhausted)return {};
    }
  return {};
}
std::optional<std::vector<std::uint64_t>> predict(llvm::BasicBlock &seed,const std::vector<llvm::BasicBlock *> &path,const Hints &h,Work &work,bool &limited,bool storePredictions,const llvm::DominatorTree &dom,bool allowPartial,bool &usedPartial,bool partialMasks,bool bitSlices,bool storeExpressions) {
  auto *index=indexOf(*path.back());if(!index)return {};
  Enumerator enumerate(&seed,path,work,nullptr,nullptr,false,false,false,nullptr,storeExpressions,storeExpressions?&dom:nullptr);auto addresses=enumerate.get(index);limited|=enumerate.limited;
  if(storeExpressions && (enumerate.failed || enumerate.limited || work.exhausted))return {};
  bool sawHistoricalPhi=enumerate.historicalPhiSeen;
  if(!addresses && enumerate.booleanUnknown && !work.exhausted)addresses=correlatedAddresses(seed,path,index,work,limited,nullptr,&sawHistoricalPhi,storeExpressions,storeExpressions?&dom:nullptr);
  // Preserve every prior successful prediction, including STORE hints. The
  // new old-PHI identity retry is strictly a failure fallback.
  if(!addresses && storePredictions && !work.exhausted)addresses=storeAddresses(seed,path,index,work,limited);
  if(!addresses && sawHistoricalPhi && !work.exhausted)
    addresses=correlatedAddresses(seed,path,index,work,limited,&dom,nullptr,storeExpressions,storeExpressions?&dom:nullptr);
  if(!addresses && storeExpressions && !work.exhausted) {
    addresses=storeExpressionAddresses(seed,path,index,work,limited,dom);
    if(addresses)usedPartial=true;
  }
  // Partial select hints run only after all I2 prediction attempts fail.
  if(!addresses && allowPartial && !work.exhausted) {
    if(!work.charge(1+path.size()))return {};
    Enumerator partial(&seed,path,work,nullptr,nullptr,true);auto predicted=partial.get(index);limited|=partial.limited;
    if(predicted && partial.usedPartial && !partial.fatalPartial && !work.exhausted){addresses=std::move(predicted);usedPartial=true;}
  }
  // This separate mode runs after every legacy prediction failed. It does not
  // change the old J/F/H algorithms or consume their earlier invocation budget.
  if(!addresses && allowPartial && partialMasks && !work.exhausted) {
    if(!work.charge(1+path.size()))return {};
    Enumerator masked(&seed,path,work,nullptr,nullptr,true,true,bitSlices);
    auto predicted=masked.get(index);limited|=masked.limited;
    if(predicted && masked.usedPartial && !masked.fatalPartial && !work.exhausted){addresses=std::move(predicted);usedPartial=true;}
  }
  if(!addresses)return {};auto *jump=llvm::cast<llvm::SwitchInst>(path.back()->getTerminator());std::vector<std::uint64_t> values;
  for(const auto &address:*addresses) {
    auto n=address.getZExtValue();if(n>std::numeric_limits<std::uint64_t>::max()-8)return {};
    std::uint64_t candidate=0;bool mapped=true;
    for(unsigned b=0;b<8;++b) {
      if(!work.charge())return {}; // Exhaustion is never a missing-hint outcome.
      auto byte=h.bytes.find(n+b);
      if(byte==h.bytes.end()){mapped=false;break;}
      candidate|=static_cast<std::uint64_t>(byte->second)<<(8*b);
    }
    if(!mapped) {
      // STORE expressions still enumerate complete finite addresses. Only
      // hint admission may be partial; original bounds/reads/fallback remain.
      if(!allowPartial && !storeExpressions)return {};
      usedPartial=true;continue;
    }
    if(!work.charge(jump->getNumCases()))return {};
    auto *constant=llvm::ConstantInt::get(llvm::Type::getInt64Ty(seed.getContext()),candidate);
    if(jump->findCaseValue(constant)==jump->case_default()) {
      // A non-case hint never changes the actual target switch/default.
      if(!allowPartial && !storeExpressions)return {};
      usedPartial=true;continue;
    }
    if(std::find(values.begin(),values.end(),candidate)==values.end())values.push_back(candidate);
  }
  return values.empty()?std::optional<std::vector<std::uint64_t>>{}:values;
}
}
static GuardedTransitionsStats runStateCorridorsImpl(llvm::Module &,const GuardedTransitionsOptions &,bool,GuardedPartialCorridorsStats *,bool partialMasks=false,GuardedCheckedCorridorsStats *continuations=nullptr,bool bitSlices=false,bool storeExpressions=false);
GuardedTransitionsStats runGuardedStateCorridors(llvm::Module &module,const GuardedTransitionsOptions &options) {
  return runGuardedStateCorridors(module,options,false);
}
GuardedTransitionsStats runGuardedStateCorridors(llvm::Module &module,const GuardedTransitionsOptions &options,bool storePredictions) {
  return runStateCorridorsImpl(module,options,storePredictions,nullptr);
}
GuardedPartialCorridorsStats runGuardedPartialCorridors(llvm::Module &module,const GuardedTransitionsOptions &options) {
  GuardedPartialCorridorsStats result;
  result.base=runStateCorridorsImpl(module,options,false,&result);
  result.partialUnresolvedEdges=result.partialCandidateEdges-result.partialRewrittenEdges;
  if(result.partialCandidateEdges) {
    result.base.complete=false;
    if(result.base.message.empty())result.base.message="partial target predictions retain unknown outcomes in full fallback";
  }
  return result;
}
GuardedPartialCorridorsStats runGuardedPartialMaskCorridors(llvm::Module &module,const GuardedTransitionsOptions &options) {
  GuardedPartialCorridorsStats result;
  result.base=runStateCorridorsImpl(module,options,false,&result,true);
  result.partialUnresolvedEdges=result.partialCandidateEdges-result.partialRewrittenEdges;
  if(result.partialCandidateEdges) {
    result.base.complete=false;
    if(result.base.message.empty())result.base.message="partial Boolean-mask predictions retain unknown outcomes in full fallback";
  }
  return result;
}
GuardedCheckedCorridorsStats runGuardedCheckedCorridors(llvm::Module &module,const GuardedTransitionsOptions &options) {
  GuardedCheckedCorridorsStats result;
  auto &partial=result.partial;
  partial.base=runStateCorridorsImpl(module,options,false,&partial,true,&result);
  partial.partialUnresolvedEdges=partial.partialCandidateEdges-partial.partialRewrittenEdges;
  if(partial.base.candidateEdges) {
    partial.base.complete=false;
    if(partial.base.message.empty())partial.base.message="checked continuation contexts retain every actual read and full fallback";
  }
  return result;
}
GuardedCheckedCorridorsStats runGuardedBitSlices(llvm::Module &module,const GuardedTransitionsOptions &options) {
  GuardedCheckedCorridorsStats result;auto &partial=result.partial;
  partial.base=runStateCorridorsImpl(module,options,false,&partial,true,&result,true);
  partial.partialUnresolvedEdges=partial.partialCandidateEdges-partial.partialRewrittenEdges;
  if(partial.base.candidateEdges) {
    partial.base.complete=false;
    if(partial.base.message.empty())partial.base.message="bounded bit-slice predictions retain source operations and complete fallbacks";
  }
  return result;
}
GuardedCheckedCorridorsStats runGuardedStoreExpressions(llvm::Module &module,const GuardedTransitionsOptions &options) {
  GuardedCheckedCorridorsStats result;auto &partial=result.partial;
  partial.base=runStateCorridorsImpl(module,options,false,&partial,false,&result,false,true);
  partial.partialUnresolvedEdges=partial.partialCandidateEdges-partial.partialRewrittenEdges;
  if(partial.base.candidateEdges) {
    partial.base.complete=false;
    if(partial.base.message.empty())partial.base.message="STORE-expression predictions retain actual operations, reads and full fallbacks";
  }
  return result;
}
static GuardedTransitionsStats runStateCorridorsImpl(llvm::Module &module,const GuardedTransitionsOptions &options,bool storePredictions,GuardedPartialCorridorsStats *partialStats,bool partialMasks,GuardedCheckedCorridorsStats *continuations,bool bitSlices,bool storeExpressions) {
  GuardedTransitionsStats result;auto *metadata=module.getNamedMetadata(storeExpressions?"dragon.guard-predictions":"dragon.guarded-entry");if(!metadata)return result;result.available=true;
  const bool preferLoops=bitSlices || storeExpressions;
  const char *continuationMarker=storeExpressions?StoreExpressionGenerated:ContinuationGenerated;
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
  std::vector<signed char> seedAdmission(storeExpressions?initial.size():0,-1);
  auto eligible=[&](std::size_t n,bool seedRole=false){
    auto *b=initial[n];auto *term=b->getTerminator();if(!llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term))return false;
    auto &cache=storeExpressions && seedRole?seedAdmission:admission;
    if(cache[n]<0){if(!work.charge(instructionCounts[n]))return false;cache[n]=safeBlock(*b,llvm::isa<llvm::SwitchInst>(term),storeExpressions && seedRole,&work)?1:0;}
    return cache[n]!=0;
  };
  auto continuationSeed=[&](std::size_t n) {
    auto *block=initial[n];auto *term=block->getTerminator();
    if(!generated(*block) || term->getMetadata(continuationMarker))return false;
    if(llvm::isa<llvm::BranchInst>(term))return true;
    auto *jump=llvm::dyn_cast<llvm::SwitchInst>(term);
    auto *load=jump?llvm::dyn_cast<llvm::LoadInst>(jump->getCondition()):nullptr;
    return jump && jump->getNumCases() && jump->getNumCases()<=16 && load &&
           load->isSimple() && load->getParent()==block;
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
  std::vector<Edge> branchEdges,switchEdges;
  std::set<std::pair<std::size_t,unsigned>> selected,selectedLoops,selectedPartial;std::size_t searches=0;
  while(!queue.empty()) {
    auto q=std::move(queue.front());queue.pop_front();
    for(const auto &in:incoming[q.path.front()]) {
      if(++searches>32768){limited=true;queue.clear();break;}
      if(!work.charge(1+q.path.size()))return scanFailure();
      if(!reachable[in.block] || std::find(q.path.begin(),q.path.end(),in.block)!=q.path.end())continue;
      const bool checkedSeed=continuations &&
        (continuationSeed(in.block) || (preferLoops && !generated(*initial[in.block])));
      if(generated(*initial[in.block]) && !checkedSeed)continue;
      if(!eligible(in.block,true)){if(work.exhausted)return scanFailure();continue;}
      if(!work.charge(q.path.size()))return scanFailure();
      std::vector<llvm::BasicBlock *> path;for(auto n:q.path)path.push_back(initial[n]);
      bool rootPhisRetained=true;
      for(auto &phi:path.front()->phis()) {
        if(!work.charge())return scanFailure();
        if(phi.getNumIncomingValues()<=1) {
          if(!work.charge(path.size()))return scanFailure();
          if(!eliminableSingleIncomingRootPHI(phi,*initial[in.block],
              [&](llvm::BasicBlock *block){return std::find(path.begin(),path.end(),block)!=path.end();},dom))rootPhisRetained=false;
        }
      }
      bool usedPartial=false;
      auto predicted=rootPhisRetained && (!continuations || checkedSeed)?predict(*initial[in.block],path,hint,work,limited,storePredictions,dom,partialStats!=nullptr && !storeExpressions,usedPartial,partialMasks,bitSlices,storeExpressions):std::optional<std::vector<std::uint64_t>>{};if(work.exhausted)return scanFailure();
      if(predicted) {
        bool closesLoop=false;
        if(preferLoops) {
          auto *jump=llvm::cast<llvm::SwitchInst>(initial[q.terminal]->getTerminator());
          for(auto value:*predicted) {
            if(!work.charge())return scanFailure();
            auto item=jump->findCaseValue(llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),value));
            if(item!=jump->case_default() && item->getCaseSuccessor()==path.front())closesLoop=true;
          }
        }
        const auto key=std::pair{in.block,in.slot};
        const bool fresh=selected.emplace(key).second;
        const bool upgrade=preferLoops && closesLoop && selectedLoops.emplace(key).second;
        if(fresh || upgrade) {
          if(fresh)++result.candidateEdges;targets.insert(q.terminal);
          if(preferLoops) {
            if(usedPartial && selectedPartial.emplace(key).second)++partialStats->partialCandidateEdges;
            if(!usedPartial && selectedPartial.erase(key))--partialStats->partialCandidateEdges;
          } else if(usedPartial)++partialStats->partialCandidateEdges;
          const bool branch=llvm::isa<llvm::BranchInst>(initial[in.block]->getTerminator());
          auto &edges=branch?branchEdges:switchEdges;
          if(upgrade && !fresh)for(auto it=edges.begin();it!=edges.end();++it) {
            if(!work.charge())return scanFailure();
            if(it->seed==in.block && it->slot==in.slot){edges.erase(it);break;}
          }
          if(edges.size()==options.maxCandidates) {
            limited=true;
            if(closesLoop) {
              auto replace=edges.end();
              for(auto it=edges.begin();it!=edges.end();++it) {
                if(!work.charge())return scanFailure();
                if(!it->loop)replace=it;
              }
              if(replace!=edges.end())edges.erase(replace);
            }
          }
          if(edges.size()<options.maxCandidates)edges.push_back({in.block,in.slot,q.path,q.slots,*predicted,usedPartial,closesLoop});
        }
        // A fresh slice may be predictable outside the copied suffix without
        // depending on a root PHI. Keep that straight candidate, but continue
        // the bounded original search so it cannot hide a natural loop root.
        if(!preferLoops || closesLoop)continue;
      }
      // A pre-existing generated block is a seed only, never part of the
      // copied corridor. This pass's own appended blocks cannot be new seeds.
      if(continuations && generated(*initial[in.block]))continue;
      // A seed has already run. Recheck the copied-path role before extending
      // through it, so an admitted original math call is never cloned.
      if(storeExpressions && !eligible(in.block)){if(work.exhausted)return scanFailure();continue;}
      // A switch may be a selected seed edge, but is not traversed as an
      // intermediate dispatcher: that would duplicate a whole central region.
      if(!llvm::isa<llvm::BranchInst>(initial[in.block]->getTerminator()))continue;
      if(q.path.size()==32 || instructionCounts[in.block]>2048-q.instructions){limited=true;continue;}
      auto &seen=visited[q.terminal];if(seen.contains(in.block))continue;
      if(seen.size()==512){limited=true;continue;}seen.insert(in.block);
      auto next=q;next.path.insert(next.path.begin(),in.block);next.slots.insert(next.slots.begin(),in.slot);next.instructions+=instructionCounts[in.block];queue.push_back(std::move(next));
    }
  }
  std::vector<Edge> edges;
  if(preferLoops) {
    for(bool loop:{true,false})for(auto *more:{&branchEdges,&switchEdges})for(auto &edge:*more) {
      if(!work.charge())return scanFailure();
      if(edge.loop!=loop)continue;
      if(edges.size()==options.maxCandidates){limited=true;break;}edges.push_back(std::move(edge));
    }
  } else {
    edges=std::move(branchEdges);
    for(auto &edge:switchEdges){if(edges.size()==options.maxCandidates){limited=true;break;}edges.push_back(std::move(edge));}
  }
  initial.clear();ordinal.clear();
  if(limited){result.complete=false;++result.budgetExhausted;result.message="state-corridor expression or search budget exhausted";}
  for(const auto &edge:edges) {
    if(result.rewrittenEdges>=options.maxRewrites || result.clonedBlocks>=options.maxBlocks || result.clonedInstructions>=options.maxInstructions){result.complete=false;++result.budgetExhausted;result.message="state-corridor module budget exhausted";break;}
    auto current=blocks(*function);if(current.size()<originalCount){reject("core original block identity contract violated");break;}
    auto *seed=current[edge.seed];auto *term=seed->getTerminator();std::vector<llvm::BasicBlock *> path;for(auto n:edge.path)path.push_back(current[n]);
    ++result.attemptedEdges;if(edge.partial)++partialStats->partialAttemptedEdges;bool valid=(continuations?(preferLoops || generated(*seed)) && !term->getMetadata(continuationMarker):!generated(*seed)) && llvm::isa<llvm::BranchInst,llvm::SwitchInst>(term) && edge.slot<term->getNumSuccessors() && term->getSuccessor(edge.slot)==path.front() && indexOf(*path.back());
    for(std::size_t n=0;n<path.size();++n){if(generated(*path[n]))valid=false;if(n+1<path.size()){auto *t=path[n]->getTerminator();if(edge.slots[n]>=t->getNumSuccessors() || t->getSuccessor(edge.slots[n])!=path[n+1])valid=false;}}
    if(!valid){reject("frozen state-corridor edge no longer applicable");continue;}
    GuardedTransitionSetRequest request{seed,edge.slot,path.front(),path,edge.slots,{}};
    for(auto prediction:edge.predictions)request.candidates.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(module.getContext()),prediction));
    std::size_t loopSlots=0;
    if(continuations) {
      auto *jump=llvm::cast<llvm::SwitchInst>(path.back()->getTerminator());
      for(auto *candidate:request.candidates)
        if(jump->findCaseValue(candidate)->getCaseSuccessor()==path.front())++loopSlots;
    }
    const auto oldCount=current.size();auto changed=(continuations?cloneGuardedContinuationSet:cloneGuardedTransitionSet)(*function,request,
      {std::min<std::size_t>(32,options.maxBlocks-result.clonedBlocks),std::min<std::size_t>(2048,options.maxInstructions-result.clonedInstructions)});
    if(!changed.changed){result.complete=false;++result.rejected;result.budgetExhausted+=changed.budgetExhausted;result.message=changed.message;continue;}
    result.changed=true;++result.rewrittenEdges;if(edge.partial)++partialStats->partialRewrittenEdges;++result.fallbackSwitchesCreated;result.clonedBlocks+=changed.clonedBlocks;result.clonedInstructions+=changed.clonedInstructions;
    if(continuations && loopSlots){++continuations->loopContexts;continuations->loopBackedges+=loopSlots;}
    auto updated=blocks(*function);for(std::size_t n=oldCount;n<updated.size();++n) {
      auto *created=updated[n]->getTerminator();created->setMetadata(Generated,llvm::MDNode::get(module.getContext(),{}));
      if(continuations)created->setMetadata(continuationMarker,llvm::MDNode::get(module.getContext(),{}));
    }
  }
  auto final=blocks(*function);for(auto n:targets)if(llvm::isa<llvm::SwitchInst>(final[n]->getTerminator()))++result.originalSwitchesRetained;
  result.unresolvedEdges=result.candidateEdges-result.rewrittenEdges;return result;
}
}
