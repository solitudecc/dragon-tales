#define main history_fixture_main
#include "guest_memory_history_native.cpp"
#undef main
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

namespace worklist_oracle {
const char *Names[] = {"different", "same", "hub", "history"};
const char *Errors[] = {"none", "wrong_store", "drop_store", "drop_alias", "drop_call", "bounds", "fresh_history", "stale_fresh", "reentry", "liveout"};
struct Edge { llvm::BasicBlock *from, *to; unsigned successor; bool known; };
struct Fixture { llvm::Function *fn; std::vector<Edge> edges; };
Fixture make(llvm::Module &m, unsigned mode, unsigned fault=0) {
  if(mode==3) {
    unsigned mapped = fault==2?3:fault==5?4:fault==6?1:fault==7?2:fault==8?5:fault==9?6:0;
    auto v=build(m,2,0,mapped);
    v.function->setName(fault?std::string("control_")+Errors[fault]:"worklist_history");
    auto *valid=v.state->getParent(); auto *term=llvm::cast<llvm::BranchInst>(valid->getTerminator());
    llvm::ValueToValueMapTy map;
    auto *second=llvm::CloneBasicBlock(v.body,map,".second",v.function);
    for(auto &i:*second) llvm::RemapInstruction(&i,map,llvm::RF_IgnoreMissingLocals);
    auto *secondTerm=second->getTerminator();
    for(auto &phi:v.hub->phis()) { auto *old=phi.getIncomingValueForBlock(v.body); auto found=map.find(old); phi.addIncoming(found==map.end()?old:static_cast<llvm::Value*>(found->second),second); }
    Builder s(secondTerm); s.CreateBr(v.hub); secondTerm->eraseFromParent();
    for(auto &i:*second) if(auto *st=llvm::dyn_cast<llvm::StoreInst>(&i))
      if(auto *n=llvm::dyn_cast<llvm::ConstantInt>(st->getValueOperand());n&&n->equalsInt(14)) st->setOperand(0,s.getInt64(fault==1?14:34));
    auto *route=llvm::BasicBlock::Create(m.getContext(),"latch.route",v.function);
    Builder r(route); llvm::Value *count=nullptr;
    for(auto &bb:*v.function) for(auto &i:bb) if(i.getName()=="current.count") count=&i;
    r.CreateCondBr(r.CreateICmpEQ(r.CreateAnd(count,r.getInt64(1)),r.getInt64(0)),v.body,second);
    term->setSuccessor(0,route);
    return {v.function,{{v.body,v.hub,1,true},{second,v.hub,0,true},{v.seed,v.hub,0,false}}};
  }
  auto &c=m.getContext(); auto *ptr=llvm::PointerType::getUnqual(c); auto *i64=llvm::Type::getInt64Ty(c);
  auto *fn=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,ptr,i64,i64,i64,ptr},false),llvm::GlobalValue::ExternalLinkage,
      fault?std::string("control_")+Errors[fault]:std::string("worklist_")+Names[mode],m);
  for(auto &arg:fn->args())arg.addAttr(llvm::Attribute::NoUndef);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,fn);};
  auto *entry=block("entry"); std::vector<llvm::BasicBlock*> sources;
  for(auto n:{"store14","store34","alias.after","call.after","unknown"})sources.push_back(block(n));
  auto *hub=mode==2?block("hub"):nullptr, *seed=mode==2?block("unknown.hub"):nullptr;
  auto *checked=block("checked"),*valid=block("valid"),*output=block("output"),*failure=block("failure");
  Builder b(entry); auto *regs=fn->getArg(0),*desc=fn->getArg(1),*ex=fn->getArg(2),*path=fn->getArg(3),*sp=fn->getArg(4),*x=fn->getArg(5);
  auto *base=load(b,ptr,desc,"guest.base"); auto *sw=b.CreateSwitch(path,mode==2?seed:sources[4],5);
  for(unsigned k=0;k<5;++k)sw->addCase(b.getInt64(k),sources[k]);
  std::vector<llvm::Value*> frames; Fixture view{fn,{}};
  for(unsigned k=0;k<5;++k) {
    b.SetInsertPoint(sources[k]); auto *frame=mode==1?static_cast<llvm::Value*>(sp):b.CreateAdd(sp,b.getInt64(k*32)); frames.push_back(frame);
    auto *slot=address(b,base,b.CreateSub(frame,b.getInt64(128)));
    if(k!=4&&fault!=2)store(b,b.getInt64(k==1?(fault==1?14:34):14),slot);
    if(k==2&&fault!=3)store(b,b.CreateXor(x,b.getInt64(0x5555)),fn->getArg(6));
    if(k==3&&fault!=4) {
      auto callee=m.getOrInsertFunction("edge_unknown_writer",llvm::FunctionType::get(b.getVoidTy(),{ptr,ptr,ptr,i64},false));
      b.CreateCall(callee,{regs,base,slot,x});
    }
    b.CreateBr(hub?hub:checked);view.edges.push_back({sources[k],hub?hub:checked,0,k<2});
  }
  llvm::Value *selected=nullptr;
  if(hub) {
    b.SetInsertPoint(hub); auto *f=b.CreatePHI(i64,5,"hub.frame");for(unsigned k=0;k<5;++k)f->addIncoming(frames[k],sources[k]);selected=f;b.CreateBr(checked);
    b.SetInsertPoint(seed);auto *other=b.CreateAdd(sp,b.getInt64(160));b.CreateBr(checked);
    b.SetInsertPoint(checked);auto *f2=b.CreatePHI(i64,2,"frame");f2->addIncoming(selected,hub);f2->addIncoming(other,seed);selected=f2;
    view.edges.push_back({seed,checked,0,false});
  }else {
    b.SetInsertPoint(checked);auto *f=b.CreatePHI(i64,5,"frame");for(unsigned k=0;k<5;++k)f->addIncoming(frames[k],sources[k]);selected=f;
  }
  auto *index=b.CreateSub(selected,b.getInt64(128),"index");auto *size=load(b,i64,at(b,desc,8),"guest.size");
  auto *fits=b.CreateAnd(b.CreateICmpUGE(size,b.getInt64(8)),b.CreateICmpULE(index,b.CreateSub(size,b.getInt64(8))));
  b.CreateCondBr(fault==5?static_cast<llvm::Value*>(b.getTrue()):fits,valid,failure);
  b.SetInsertPoint(valid);auto *state=load(b,i64,address(b,base,index),"actual.state");state->setMetadata("history.target.load",llvm::MDNode::get(c,{}));
  auto *result=b.CreateXor(state,x);b.CreateBr(output);
  auto finish=[&](llvm::BasicBlock *bb,bool success){
    b.SetInsertPoint(bb);auto *value=success?result:b.CreateXor(index,b.getInt64(0xbad0000000000000ULL));
    store(b,value,at(b,regs,0));store(b,selected,at(b,regs,8));store(b,index,at(b,regs,16));store(b,path,at(b,regs,24));store(b,value,at(b,base,0x500));
    llvm::Value *fields[]={b.getInt64(success?1:5),b.getInt64(0),b.getInt64(0x8000+mode*0x100),index,b.getInt64(success),x,b.getInt64(success?0:3)};
    for(unsigned n=0;n<7;++n)store(b,fields[n],at(b,ex,n*8));b.CreateRet(value);
  };finish(output,true);finish(failure,false);return view;
}
}
int main(int argc,char **argv) {
  if(argc!=2)return 2;std::filesystem::path out(argv[1]);
  for(unsigned mode=0;mode<4;++mode) {
    llvm::LLVMContext c;llvm::Module m("edge-worklist-independent",c);m.setDataLayout("e-p:64:64");
    auto v=worklist_oracle::make(m,mode);std::string name=worklist_oracle::Names[mode];
    if(llvm::verifyModule(m,&llvm::errs()))return 3;dump(m,out/(name+"-before.ll"));
#ifdef EDGE_WORKLIST
    const auto stats=dragon::llvmlifter::detail::runGuestMemorySSA(m,{},nullptr,true);
#else
    const auto stats=dragon::llvmlifter::detail::runGuestMemorySSA(m,{});
#endif
    if(llvm::verifyModule(m,&llvm::errs()))return 4;dump(m,out/(name+"-after.ll"));
    unsigned known=0,negative=0,targets=0;
    for(auto e:v.edges){bool redirected=e.from->getTerminator()->getSuccessor(e.successor)!=e.to;if(e.known)known+=redirected;else negative+=redirected;}
    for(auto &bb:*v.fn)for(auto &i:bb)if(llvm::isa<llvm::LoadInst>(i)&&i.getMetadata("history.target.load"))++targets;
    llvm::outs()<<"case="<<name<<" available="<<stats.available<<" forwarded="<<stats.loadsForwarded<<" known_redirected="<<known<<" negative_redirected="<<negative<<" target_loads="<<targets<<" complete="<<stats.complete<<" budget="<<stats.budgetExhausted<<" invalid=0\n";
  }
  for(unsigned fault=1;fault<10;++fault){
    llvm::LLVMContext c;llvm::Module m("edge-worklist-error-control",c);m.setDataLayout("e-p:64:64");
    worklist_oracle::make(m,fault>=6?3:0,fault);if(llvm::verifyModule(m,&llvm::errs()))return 5;
    dump(m,out/(std::string("control_")+worklist_oracle::Errors[fault]+".ll"));
  }
}
