#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <filesystem>
#include <set>
#include <string>

// Independent input IR. Body writes are outside the copied guard corridor.
// The current LOAD/freeze must differ from the old value carried by the edge.
namespace {
using Builder=llvm::IRBuilder<llvm::NoFolder>;
enum Mode { Multi, Phi, Load, Freeze, Call, Store, Atomic, Volatile, Fence, Count };
constexpr const char *Names[]={"multi","phi","load","freeze","call","store","atomic","volatile","fence"};
enum Fault { None, OldState, NewLoad, NewFreeze, Unknown, Bounds, Default, Flags };
constexpr const char *FaultNames[]={"none","old_state","new_load","new_freeze","unknown","bounds","default","flags"};
struct View {llvm::Function *fn;llvm::BasicBlock *head,*entry[4],*body,*bounds,*table;};
llvm::Value *byte(Builder &b,llvm::Value *base,unsigned index){return b.CreateGEP(b.getInt8Ty(),base,b.getInt64(index));}
llvm::LoadInst *read(Builder &b,llvm::Type *type,llvm::Value *ptr,const char *name){auto *v=b.CreateLoad(type,ptr,name);v->setAlignment(llvm::Align(1));return v;}
void write(Builder &b,llvm::Value *v,llvm::Value *p){b.CreateStore(v,p)->setAlignment(llvm::Align(1));}
void mark(llvm::Instruction *v,const char *name){v->setMetadata(name,llvm::MDNode::get(v->getContext(),{}));}
View build(llvm::Module &m,Mode mode,Fault fault=None){
  auto &c=m.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);auto *i32=llvm::Type::getInt32Ty(c);
  std::string name=fault==None?std::string("carried_")+Names[mode]:std::string("control_")+FaultNames[fault];
  auto *f=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,ptr,i64,i32,i32},false),llvm::GlobalValue::ExternalLinkage,name,m);
  for(auto &a:f->args())a.addAttr(llvm::Attribute::NoUndef);
  auto *regs=f->getArg(0),*desc=f->getArg(1),*exit=f->getArg(2),*x=f->getArg(3),*route=f->getArg(4),*rounds=f->getArg(5);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,f);};
  auto *entry=block("entry");llvm::BasicBlock *seeds[]={block("input.fifty"),block("input.sixteen"),block("input.twenty_three"),block("input.unknown")};
  auto *head=block("guard.head"),*bridge=block("shared.bridge"),*bounds=block("bounds.join"),*table=block("actual.table");
  auto *headFail=block("state.failure"),*memoryFail=block("memory.failure"),*hit=block("payload.one"),*alt=block("payload.two"),*bad=block("target.default"),*body=block("repeat.body"),*done=block("finish");
  Builder b(entry);auto *data=read(b,ptr,desc,"guest.base");
  auto *initialState=read(b,i64,byte(b,regs,8),"initial.carried.state");
  auto *initialLoad=read(b,i64,byte(b,regs,24),"initial.carried.load");
  auto *initialFreeze=read(b,i64,byte(b,regs,32),"initial.carried.freeze");
  auto *routing=b.CreateSwitch(route,seeds[3],3);for(unsigned n=0;n!=3;++n)routing->addCase(b.getInt32(n),seeds[n]);
  llvm::Value *inputs[4];
  for(unsigned n=0;n!=4;++n){b.SetInsertPoint(seeds[n]);inputs[n]=mode==Multi&&n<3?static_cast<llvm::Value *>(b.getInt64(n==0?50:n==1?16:23)):n==1?b.CreateXor(x,b.getInt64(0x55)):n==2?b.CreateAdd(x,b.getInt64(1)):x;b.CreateBr(head);}
  b.SetInsertPoint(head);auto makePhi=[&](const char *n){return b.CreatePHI(i64,5,n);};
  auto *state=makePhi("state");auto *iteration=makePhi("iteration");auto *carriedState=makePhi("carried.old.phi");auto *carriedLoad=makePhi("carried.old.load");auto *carriedFreeze=makePhi("carried.old.freeze");
  for(unsigned n=0;n!=4;++n){state->addIncoming(inputs[n],seeds[n]);iteration->addIncoming(b.getInt64(0),seeds[n]);carriedState->addIncoming(initialState,seeds[n]);carriedLoad->addIncoming(initialLoad,seeds[n]);carriedFreeze->addIncoming(initialFreeze,seeds[n]);}
  auto *observed=read(b,i64,byte(b,data,0x100),"observed.this.execution");mark(observed,"carried.observe");
  if(mode==Atomic){observed->setAtomic(llvm::AtomicOrdering::Acquire);observed->setAlignment(llvm::Align(8));}
  if(mode==Volatile)observed->setVolatile(true);
  if(mode==Call){auto callee=m.getOrInsertFunction("carried_unknown_writer",llvm::FunctionType::get(b.getVoidTy(),{ptr,ptr,i64},false));b.CreateCall(callee,{regs,data,x});}
  if(mode==Store)write(b,x,byte(b,data,0x308));if(mode==Fence)b.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
  auto *frozen=b.CreateFreeze(observed,"frozen.this.execution");mark(llvm::cast<llvm::Instruction>(frozen),"carried.freeze");
  auto *flags=fault==Flags?static_cast<llvm::Value *>(b.getInt64(0)):b.CreateXor(b.CreateAdd(b.CreateMul(state,b.getInt64(17)),b.CreateMul(carriedState,b.getInt64(49))),b.CreateXor(carriedLoad,b.CreateXor(b.CreateShl(carriedFreeze,b.getInt64(5)),observed)),"flags");
  auto *headCondition=fault==Unknown?static_cast<llvm::Value *>(b.getFalse()):b.CreateICmpUGT(state,b.getInt64(50));
  b.CreateCondBr(headCondition,headFail,bridge);
  b.SetInsertPoint(bridge);auto *size=read(b,i64,byte(b,desc,8),"logical.size");b.CreateBr(bounds);
  b.SetInsertPoint(bounds);
  auto tailPhi=[&](llvm::Value *v,const char *n){auto *p=b.CreatePHI(i64,2,n);p->addIncoming(v,bridge);return p;};
  auto *s=tailPhi(state,"tail.state"),*it=tailPhi(iteration,"tail.iteration"),*cs=tailPhi(carriedState,"tail.old.phi"),*cl=tailPhi(carriedLoad,"tail.old.load"),*cf=tailPhi(carriedFreeze,"tail.old.freeze"),*obs=tailPhi(observed,"tail.observed"),*fr=tailPhi(frozen,"tail.frozen"),*fl=tailPhi(flags,"tail.flags"),*sz=tailPhi(size,"tail.size");
  auto *index=b.CreateAdd(b.CreateShl(s,b.getInt64(3)),b.getInt64(0x400),"table.index");
  auto *fits=fault==Bounds?static_cast<llvm::Value *>(b.getTrue()):b.CreateAnd(b.CreateICmpUGT(sz,b.getInt64(7)),b.CreateICmpULE(index,b.CreateAdd(sz,b.getInt64(-8))));
  auto *boundBranch=b.CreateCondBr(fits,table,memoryFail);mark(boundBranch,"carried.bounds");
  b.SetInsertPoint(table);auto *address=b.CreateGEP(b.getInt8Ty(),data,index,"table.address");mark(llvm::cast<llvm::Instruction>(address),"carried.address");
  auto *target=read(b,i64,address,"actual.target");mark(target,"carried.table");
  auto *nextIt=b.CreateAdd(it,b.getInt64(1));auto *nextJ=b.CreateXor(s,b.getInt64(1));
  auto *effective=b.CreateSelect(b.CreateICmpULT(it,b.CreateZExt(rounds,i64)),target,b.getInt64(0x1111));
  auto *sw=b.CreateSwitch(effective,fault==Default?hit:bad,4);
  sw->addCase(b.getInt64(0x1111),hit);sw->addCase(b.getInt64(0x2222),alt);sw->addCase(b.getInt64(0x3333),body);sw->addCase(b.getInt64(0x4444),bounds);
  s->addIncoming(nextJ,table);it->addIncoming(nextIt,table);cs->addIncoming(cs,table);cl->addIncoming(cl,table);cf->addIncoming(cf,table);obs->addIncoming(obs,table);fr->addIncoming(fr,table);fl->addIncoming(fl,table);sz->addIncoming(sz,table);
  b.SetInsertPoint(body);
  auto *changed=b.CreateAdd(obs,b.CreateAdd(b.getInt64(0x101),b.CreateAdd(x,it)),"changed.memory");write(b,changed,byte(b,data,0x100));
  auto *badRead=(fault==NewLoad||fault==NewFreeze)?static_cast<llvm::Value *>(read(b,i64,byte(b,data,0x100),"bad.new.execution.read")):nullptr;
  auto *nextState=mode==Multi?b.CreateSelect(b.CreateICmpEQ(b.CreateAnd(x,b.getInt64(1)),b.getInt64(0)),b.getInt64(16),b.getInt64(23)):static_cast<llvm::Value *>(b.getInt64(16));
  state->addIncoming(nextState,body);iteration->addIncoming(nextIt,body);
  auto *opaqueState=b.CreateAdd(s,b.getInt64(0),"existing.old.state");auto *opaqueLoad=b.CreateXor(obs,b.getInt64(0),"existing.old.load");auto *opaqueFreeze=b.CreateXor(fr,b.getInt64(0),"existing.old.freeze");
  carriedState->addIncoming(fault==OldState?static_cast<llvm::Value *>(b.getInt64(16)):mode==Phi?static_cast<llvm::Value *>(state):opaqueState,body);
  carriedLoad->addIncoming(fault==NewLoad?badRead:mode==Load?static_cast<llvm::Value *>(observed):opaqueLoad,body);
  carriedFreeze->addIncoming(fault==NewFreeze?b.CreateFreeze(badRead):mode==Freeze?frozen:opaqueFreeze,body);
  b.CreateBr(head);
  for(auto *dest:{headFail,memoryFail,hit,alt,bad}){b.SetInsertPoint(dest);b.CreateBr(done);}
  b.SetInsertPoint(done);
  auto output=[&](const char *n,llvm::Value *h,llvm::Value *t){auto *p=b.CreatePHI(i64,5,n);p->addIncoming(h,headFail);for(auto *dest:{memoryFail,hit,alt,bad})p->addIncoming(t,dest);return p;};
  auto *outState=output("out.state",state,s),*outIt=output("out.iteration",iteration,it),*outCS=output("out.old.phi",carriedState,cs),*outCL=output("out.old.load",carriedLoad,cl),*outCF=output("out.old.freeze",carriedFreeze,cf),*outObs=output("out.observed",observed,obs),*outFl=output("out.flags",flags,fl),*outIndex=output("out.index",b.getInt64(0),index);
  auto *status=b.CreatePHI(i64,5,"status");auto *value=b.CreatePHI(i64,5,"value");
  status->addIncoming(b.getInt64(2),headFail);value->addIncoming(b.getInt64(17),headFail);
  status->addIncoming(b.getInt64(3),memoryFail);value->addIncoming(b.getInt64(23),memoryFail);
  status->addIncoming(b.getInt64(1),hit);value->addIncoming(target,hit);
  status->addIncoming(b.getInt64(5),alt);value->addIncoming(target,alt);
  status->addIncoming(b.getInt64(4),bad);value->addIncoming(target,bad);
  auto *result=b.CreateXor(b.CreateAdd(outFl,b.CreateAdd(outState,b.CreateAdd(outCS,b.CreateAdd(outCL,b.CreateAdd(outCF,outObs))))),b.CreateXor(status,b.CreateXor(value,b.CreateXor(outIndex,outIt))));
  llvm::Value *outputs[]={result,outState,outCS,outCL,outCF,outObs,outFl,outIt,outIndex};for(unsigned n=0;n!=9;++n)write(b,outputs[n],byte(b,regs,n*8));
  write(b,result,byte(b,data,0x200));write(b,status,byte(b,data,0x208));write(b,outCF,byte(b,data,0x210));
  llvm::Value *fields[]={status,b.getInt64(0),b.getInt64(0xc000+mode*16),b.CreateAdd(result,b.getInt64(0x1000)),b.getInt64(1),b.CreateXor(x,b.getInt64(0xdeadbeef)),outIt};for(unsigned n=0;n!=7;++n)write(b,fields[n],byte(b,exit,n*8));b.CreateRet(result);
  return {f,head,{seeds[0],seeds[1],seeds[2],seeds[3]},body,bounds,table};
}
void dump(llvm::Module &m,const std::filesystem::path &p){std::error_code ec;llvm::raw_fd_ostream out(p.string(),ec,llvm::sys::fs::OF_Text);if(ec)std::exit(2);m.print(out,nullptr);}
unsigned redirects(const View &v){unsigned count=0;for(auto *seed:v.entry)count+=seed->getTerminator()->getSuccessor(0)!=v.head;count+=v.body->getTerminator()->getSuccessor(0)!=v.head;return count;}
std::set<uint64_t> indices(const llvm::Module &m,const std::string &name){auto clone=llvm::CloneModule(m);auto *f=clone->getFunction(name);for(unsigned round=0;round!=8;++round){bool changed=false;for(auto &bb:*f)for(auto it=bb.begin();it!=bb.end();){auto &op=*it++;if(op.isTerminator()||op.getType()->isVoidTy())continue;if(auto *c=llvm::ConstantFoldInstruction(&op,clone->getDataLayout())){op.replaceAllUsesWith(c);op.eraseFromParent();changed=true;}}if(!changed)break;}std::set<uint64_t> values;for(auto &bb:*f)for(auto &op:bb)if(op.getMetadata("carried.table"))if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&op))if(auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand()))if(auto *v=llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1)))values.insert(v->getZExtValue());return values;}
}
int main(int argc,char **argv){if(argc!=2)return 2;std::filesystem::path out(argv[1]);for(unsigned n=0;n!=Count;++n){llvm::LLVMContext c;llvm::Module m("loop-carried-independent",c);m.setDataLayout("e-p:64:64");auto v=build(m,static_cast<Mode>(n));if(llvm::verifyModule(m,&llvm::errs()))return 3;dump(m,out/(std::string(Names[n])+"-before.ll"));auto stats=dragon::llvmlifter::runDispatcherSpecialization(m);bool invalid=llvm::verifyModule(m,&llvm::errs());dump(m,out/(std::string(Names[n])+"-after.ll"));if(invalid)return 3;auto known=indices(m,v.fn->getName().str());llvm::outs()<<"case="<<Names[n]<<" changed="<<stats.changed<<" complete="<<stats.complete<<" folds="<<stats.branchesRewritten<<" redirected="<<redirects(v)<<" body_redirected="<<(v.body->getTerminator()->getSuccessor(0)!=v.head)<<" unknown_retained="<<(v.entry[3]->getTerminator()->getSuccessor(0)==v.head)<<" index50="<<known.contains(0x590)<<" index16="<<known.contains(0x480)<<" index23="<<known.contains(0x4b8)<<" invalid="<<invalid<<'\n';}for(unsigned fault=OldState;fault<=Flags;++fault){llvm::LLVMContext c;llvm::Module m("loop-carried-fault",c);m.setDataLayout("e-p:64:64");build(m,Phi,static_cast<Fault>(fault));if(llvm::verifyModule(m,&llvm::errs()))return 3;dump(m,out/(std::string("control_")+FaultNames[fault]+".ll"));}}
