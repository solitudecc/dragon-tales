#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <filesystem>
#include <string>

// Independent input IR, not a machine lifter or unit-test fixture. All target
// cells remain real Guest LOADs; host initialization is only an oracle input.
namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
enum Mode { Basic, TwoBridge, OuterGuard, EntryValue, UnknownCall, Store,
            Atomic, Volatile, Fence, Count };
constexpr const char *names[] = {"basic", "two_bridge", "outer_guard", "entry_value",
    "unknown_call", "store", "atomic", "volatile", "fence"};
constexpr uint64_t Payload = 0x1111, Alternate = 0x2222;
constexpr uint64_t ReenterB = 0x3333, ReenterJ = 0x4444;
struct View {
  llvm::Function *fn;
  llvm::BasicBlock *guard, *known, *unknown[3], *bridge, *bounds, *table, *payload;
  llvm::PHINode *state, *tailState;
  llvm::Instruction *bridgeFlags;
  llvm::BranchInst *guardBranch, *boundsBranch;
  llvm::SwitchInst *targetSwitch;
};
llvm::Value *byte(Builder &b, llvm::Value *base, unsigned index) {
  return b.CreateGEP(b.getInt8Ty(), base, b.getInt64(index));
}
llvm::LoadInst *read(Builder &b, llvm::Type *type, llvm::Value *address, const char *name) {
  auto *load = b.CreateLoad(type, address, name); load->setAlignment(llvm::Align(1)); return load;
}
void write(Builder &b, llvm::Value *value, llvm::Value *address) {
  b.CreateStore(value, address)->setAlignment(llvm::Align(1));
}
void tag(llvm::Instruction *op, const char *name) {
  op->setMetadata(name, llvm::MDNode::get(op->getContext(), {}));
}
View build(llvm::Module &m, Mode mode, const std::string &overrideName = "") {
  auto &c = m.getContext(); auto *ptr = llvm::PointerType::getUnqual(c);
  auto *i64 = llvm::Type::getInt64Ty(c); auto *i32 = llvm::Type::getInt32Ty(c);
  auto *fn = llvm::Function::Create(llvm::FunctionType::get(i64,
      {ptr, ptr, ptr, i64, i32, i32}, false), llvm::GlobalValue::ExternalLinkage,
      overrideName.empty() ? std::string("context_") + names[mode] : overrideName, m);
  for (auto &arg : fn->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto *regs=fn->getArg(0), *desc=fn->getArg(1), *exit=fn->getArg(2);
  auto *x=fn->getArg(3), *route=fn->getArg(4), *rounds=fn->getArg(5);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,fn);};
  auto *entry=block("entry"), *choose=block("choose"), *known=block("known.fifty");
  llvm::BasicBlock *unknown[]={block("unknown.one"),block("unknown.two"),block("unknown.three")};
  auto *guard=block("state.guard"), *stateFail=block("state.failure"), *bridge=block("bridge");
  auto *bridge2=mode==TwoBridge ? block("bridge.two") : nullptr;
  auto *bounds=block("bounds.join"), *memoryFail=block("memory.failure"), *table=block("table.load");
  auto *payload=block("payload.one"), *alternate=block("payload.two"), *targetFail=block("target.default");
  auto *outerFail=block("outer.failure"), *finish=block("finish");
  Builder b(entry);
  auto *data=read(b,ptr,desc,"guest.base");
  auto *old16=read(b,i64,byte(b,regs,16),"entry.reg16");
  auto *old32=read(b,i64,byte(b,regs,32),"entry.reg32");
  if(mode==OuterGuard) b.CreateCondBr(b.CreateICmpEQ(b.CreateAnd(x,b.getInt64(1)),b.getInt64(0)),choose,outerFail);
  else b.CreateBr(choose);
  b.SetInsertPoint(choose);
  auto *routing=b.CreateSwitch(route,unknown[2],3);
  routing->addCase(b.getInt32(0),known); routing->addCase(b.getInt32(1),unknown[0]); routing->addCase(b.getInt32(2),unknown[1]);
  b.SetInsertPoint(known); b.CreateBr(guard);
  llvm::Value *values[3];
  for(unsigned n=0;n!=3;++n){
    b.SetInsertPoint(unknown[n]); values[n]=n==0 ? x : n==1 ? b.CreateXor(x,b.getInt64(0x55)) : b.CreateAdd(x,b.getInt64(1));
    b.CreateBr(guard);
  }
  b.SetInsertPoint(guard);
  auto *state=b.CreatePHI(i64,5,"state"); state->addIncoming(b.getInt64(50),known);
  auto *iteration=b.CreatePHI(i64,5,"iteration"); iteration->addIncoming(b.getInt64(0),known);
  for(unsigned n=0;n!=3;++n){state->addIncoming(values[n],unknown[n]);iteration->addIncoming(b.getInt64(0),unknown[n]);}
  auto *baseFlags=b.CreateXor(b.CreateXor(state,old16),b.getInt64(0xf00dfacecafebeefULL),"base.flags");
  auto *guardBranch=b.CreateCondBr(b.CreateICmpUGT(state,b.getInt64(50)),stateFail,bridge);
  tag(guardBranch,"context.state.guard");
  b.SetInsertPoint(bridge);
  auto *bridgeState=b.CreatePHI(i64,1,"bridge.state");bridgeState->addIncoming(state,guard);
  auto *bridgeIteration=b.CreatePHI(i64,1,"bridge.iteration");bridgeIteration->addIncoming(iteration,guard);
  auto *outside=b.CreatePHI(i64,1,"outside.entry");outside->addIncoming(mode==EntryValue?old32:old16,guard);
  auto *size=read(b,i64,byte(b,desc,8),"guest.size"); tag(size,"context.size");
  auto *extra=read(b,i64,byte(b,data,0x100),"bridge.load");
  if(mode==Atomic){extra->setAtomic(llvm::AtomicOrdering::Acquire);extra->setAlignment(llvm::Align(8));}
  if(mode==Volatile)extra->setVolatile(true);
  if(mode==UnknownCall){auto callee=m.getOrInsertFunction("state_guard_unknown_writer",llvm::FunctionType::get(b.getVoidTy(),{ptr,ptr,i64},false));b.CreateCall(callee,{regs,data,x});}
  if(mode==Store)write(b,x,byte(b,data,0x708));
  if(mode==Fence)b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
  auto *bridgeFlags=llvm::cast<llvm::Instruction>(b.CreateXor(b.CreateXor(bridgeState,outside),
      b.CreateXor(b.getInt64(0xf00dfacecafebeefULL),extra),"bridge.flags"));
  b.CreateBr(bridge2 ? bridge2 : bounds);
  if(bridge2){b.SetInsertPoint(bridge2);auto *extra2=read(b,i64,byte(b,data,0x108),"bridge.two.load");bridgeFlags=llvm::cast<llvm::Instruction>(b.CreateXor(bridgeFlags,extra2,"bridge.two.flags"));b.CreateBr(bounds);}
  auto *lastBridge=bridge2 ? bridge2 : bridge;
  b.SetInsertPoint(bounds);
  auto *tailState=b.CreatePHI(i64,2,"tail.state");tailState->addIncoming(bridgeState,lastBridge);
  auto *tailIteration=b.CreatePHI(i64,2,"tail.iteration");tailIteration->addIncoming(bridgeIteration,lastBridge);
  auto *tailFlags=b.CreatePHI(i64,2,"tail.flags");tailFlags->addIncoming(bridgeFlags,lastBridge);
  auto *tailSize=b.CreatePHI(i64,2,"tail.size");tailSize->addIncoming(size,lastBridge);
  auto *index=b.CreateAdd(b.CreateShl(tailState,b.getInt64(3)),b.getInt64(0x400),"table.index");
  auto *fits=b.CreateAnd(b.CreateICmpUGT(tailSize,b.getInt64(7)),b.CreateICmpULE(index,b.CreateAdd(tailSize,b.getInt64(-8))));
  auto *boundsBranch=b.CreateCondBr(fits,table,memoryFail);tag(boundsBranch,"context.bounds");
  b.SetInsertPoint(table);
  auto *address=llvm::cast<llvm::Instruction>(b.CreateGEP(b.getInt8Ty(),data,index,"table.address"));tag(address,"context.address");
  auto *target=read(b,i64,address,"actual.target");tag(target,"context.table");
  auto *nextIteration=b.CreateAdd(tailIteration,b.getInt64(1),"next.iteration");
  auto *nextState=b.CreateXor(tailState,b.getInt64(1),"next.state");
  auto *effective=b.CreateSelect(b.CreateICmpULT(tailIteration,b.CreateZExt(rounds,i64)),target,b.getInt64(Payload),"bounded.target");
  auto *targetSwitch=b.CreateSwitch(effective,targetFail,4);tag(targetSwitch,"context.switch");
  targetSwitch->addCase(b.getInt64(Payload),payload);targetSwitch->addCase(b.getInt64(Alternate),alternate);
  targetSwitch->addCase(b.getInt64(ReenterB),guard);targetSwitch->addCase(b.getInt64(ReenterJ),bounds);
  state->addIncoming(x,table);iteration->addIncoming(nextIteration,table);
  tailState->addIncoming(nextState,table);tailIteration->addIncoming(nextIteration,table);
  tailFlags->addIncoming(tailFlags,table);tailSize->addIncoming(tailSize,table);
  for(auto *dest:{stateFail,memoryFail,payload,alternate,targetFail,outerFail}){b.SetInsertPoint(dest);b.CreateBr(finish);}
  b.SetInsertPoint(finish);
  auto phi=[&](const char *name,llvm::Value *a,llvm::Value *z,llvm::Value *p,llvm::Value *q,llvm::Value *d,llvm::Value *o){
    auto *node=b.CreatePHI(i64,6,name);node->addIncoming(a,stateFail);node->addIncoming(z,memoryFail);
    node->addIncoming(p,payload);node->addIncoming(q,alternate);node->addIncoming(d,targetFail);node->addIncoming(o,outerFail);return node;};
  auto *outState=phi("out.state",state,tailState,tailState,tailState,tailState,b.getInt64(0xabc));
  auto *outFlags=phi("out.flags",baseFlags,tailFlags,tailFlags,tailFlags,tailFlags,b.getInt64(0xdef));
  auto *outIteration=phi("out.iteration",iteration,tailIteration,tailIteration,tailIteration,tailIteration,b.getInt64(0));
  auto *outIndex=phi("out.index",b.getInt64(0),index,index,index,index,b.getInt64(0));
  auto *status=phi("status",b.getInt64(2),b.getInt64(3),b.getInt64(1),b.getInt64(5),b.getInt64(4),b.getInt64(6));
  // The alternate's expression is computed before the switch and used only
  // along that edge, making target live-outs observable outside the corridor.
  b.SetInsertPoint(targetSwitch);auto *altValue=b.CreateXor(target,b.getInt64(0x222));
  b.SetInsertPoint(finish);
  auto *value=phi("out.value",b.getInt64(17),b.getInt64(23),target,altValue,target,b.getInt64(29));
  auto *result=b.CreateXor(b.CreateXor(outState,outFlags),b.CreateXor(b.CreateXor(status,value),b.CreateXor(outIteration,b.CreateXor(outIndex,old32))));
  write(b,result,regs);write(b,outState,byte(b,regs,8));write(b,outFlags,byte(b,regs,16));
  write(b,outIteration,byte(b,regs,24));write(b,b.CreateXor(old32,result),byte(b,regs,32));write(b,outIndex,byte(b,regs,48));
  write(b,result,byte(b,data,0x600));write(b,status,byte(b,data,0x608));
  llvm::Value *fields[]={status,b.getInt64(0),b.getInt64(0xb000+mode*16),b.CreateAdd(result,b.getInt64(0x1000)),b.getInt64(1),b.CreateXor(x,b.getInt64(0xdeadbeef)),outIteration};
  for(unsigned n=0;n!=7;++n)write(b,fields[n],byte(b,exit,n*8));
  b.CreateRet(result);
  return {fn,guard,known,{unknown[0],unknown[1],unknown[2]},bridge,bounds,table,payload,state,tailState,bridgeFlags,guardBranch,boundsBranch,targetSwitch};
}
void dump(llvm::Module &m,const std::filesystem::path &path){std::error_code ec;llvm::raw_fd_ostream out(path.string(),ec,llvm::sys::fs::OF_Text);if(ec)std::exit(2);m.print(out,nullptr);}
unsigned marked(const llvm::Function &f,const char *name){unsigned n=0;for(auto &bb:f)for(auto &op:bb)n+=op.getMetadata(name)!=nullptr;return n;}
struct Precision { bool bounds=false,address=false,switchTargets=false; };
Precision measure(const llvm::Module &m,const std::string &name){
  auto copy=llvm::CloneModule(m);auto *f=copy->getFunction(name);
  for(unsigned round=0;round!=64;++round){bool changed=false;for(auto &bb:*f)for(auto it=bb.begin();it!=bb.end();){auto &op=*it++;if(op.isTerminator()||op.getType()->isVoidTy())continue;if(auto *v=llvm::ConstantFoldInstruction(&op,copy->getDataLayout())){op.replaceAllUsesWith(v);op.eraseFromParent();changed=true;}}if(!changed)break;}
  llvm::BasicBlock *known=nullptr,*originalTable=nullptr,*originalGuard=nullptr,*originalBounds=nullptr;
  for(auto &bb:*f){if(bb.getName()=="known.fifty")known=&bb;if(bb.getName()=="table.load")originalTable=&bb;if(bb.getName()=="state.guard")originalGuard=&bb;if(bb.getName()=="bounds.join")originalBounds=&bb;}
  Precision result;auto *current=known;
  for(unsigned step=0;current&&step!=12;++step){
    auto *branch=llvm::dyn_cast<llvm::BranchInst>(current->getTerminator());if(!branch)break;
    if(branch->isConditional()){
      if(!branch->getMetadata("context.bounds")||current==originalBounds)break;
      result.bounds=true;current=branch->getSuccessor(0);break;
    }
    current=branch->getSuccessor(0);
  }
  if(!current||current==originalTable)return result;
  for(auto &op:*current)if(op.getMetadata("context.address"))if(auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(&op))if(auto *index=llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1)))result.address=index->getValue()==llvm::APInt(64,0x590);
  if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(current->getTerminator())){
    bool b=false,j=false,p=false,q=false;for(auto &edge:sw->cases()){auto v=edge.getCaseValue()->getZExtValue();auto *d=edge.getCaseSuccessor();if(v==ReenterB)b=d==originalGuard;if(v==ReenterJ)j=d==originalBounds;if(v==Payload)p=d->getName()=="payload.one";if(v==Alternate)q=d->getName()=="payload.two";}
    result.switchTargets=b&&j&&p&&q&&sw->getDefaultDest()->getName()=="target.default";
  }
  return result;
}
}
int main(int argc,char **argv){
  if(argc!=2)return 2;const std::filesystem::path out(argv[1]);
  for(unsigned mode=0;mode!=Count;++mode){llvm::LLVMContext c;llvm::Module m("state-context-independent-native",c);m.setDataLayout("e-p:64:64");auto view=build(m,static_cast<Mode>(mode));if(llvm::verifyModule(m,&llvm::errs()))return 3;dump(m,out/(std::string(names[mode])+"-before.ll"));auto stats=dragon::llvmlifter::runDispatcherSpecialization(m);bool invalid=llvm::verifyModule(m,&llvm::errs());dump(m,out/(std::string(names[mode])+"-after.ll"));if(invalid)return 3;auto precision=measure(m,view.fn->getName().str());bool unknown=true;for(auto *pred:view.unknown)unknown&=pred->getTerminator()->getSuccessor(0)==view.guard;
    llvm::outs()<<"case="<<names[mode]<<" changed="<<stats.changed<<" complete="<<stats.complete<<" branches="<<stats.branchesRewritten<<" bounds="<<precision.bounds<<" address="<<precision.address<<" switch_targets="<<precision.switchTargets<<" unknown="<<unknown<<" table_loads="<<marked(*view.fn,"context.table")<<" bounds_checks="<<marked(*view.fn,"context.bounds")<<" invalid="<<invalid<<'\n';}
  for(unsigned control=0;control!=5;++control){llvm::LLVMContext c;llvm::Module m("state-context-fault-control",c);m.setDataLayout("e-p:64:64");const char *n[]={"unknown","flags","failure","reentry","default"};auto view=build(m,Basic,std::string("control_")+n[control]);if(control==0)view.guardBranch->setCondition(llvm::ConstantInt::getFalse(c));if(control==1)view.bridgeFlags->replaceAllUsesWith(llvm::ConstantInt::get(view.bridgeFlags->getType(),0));if(control==2)view.boundsBranch->setCondition(llvm::ConstantInt::getTrue(c));if(control==3){view.state->setIncomingValue(view.state->getBasicBlockIndex(view.table),llvm::ConstantInt::get(view.state->getType(),50));view.tailState->setIncomingValue(view.tailState->getBasicBlockIndex(view.table),llvm::ConstantInt::get(view.tailState->getType(),50));}if(control==4)view.targetSwitch->setDefaultDest(view.payload);if(llvm::verifyModule(m,&llvm::errs()))return 3;dump(m,out/(std::string("control_")+n[control]+".ll"));}
}
