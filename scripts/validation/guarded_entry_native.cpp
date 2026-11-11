// Independent LLVM oracle. Canonical prologue/default comes from the real
// lifter; the machine-body replacement below has a separate byte-array model.
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <memory>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;
using B=llvm::IRBuilder<llvm::NoFolder>;
const char *Modes[]={"plain","overwrite","partial","call","loop","reg_alias","descriptor_alias","exit_alias","recursive","base_reload"};
llvm::Value *at(B &b,llvm::Value *p,llvm::Value *n){return b.CreateGEP(b.getInt8Ty(),p,n);}
llvm::Value *at(B &b,llvm::Value *p,std::uint64_t n){return at(b,p,b.getInt64(n));}
llvm::LoadInst *read(B &b,llvm::Type *t,llvm::Value *p,const char *n=""){auto *v=b.CreateLoad(t,p,n);v->setAlignment(llvm::Align(1));return v;}
llvm::StoreInst *write(B &b,llvm::Value *v,llvm::Value *p){auto *s=b.CreateStore(v,p);s->setAlignment(llvm::Align(1));return s;}
void dump(LLVMLifter &l,const std::filesystem::path &path){std::error_code e;llvm::raw_fd_ostream f(path.string(),e);if(e)std::exit(2);f<<l.ir();}

std::unique_ptr<LLVMLifter> source(unsigned mode,std::string &symbol) {
  const auto pc=0x1000+mode*0x100;
  Graph graph{Architecture::X86_64,{}};Disassembler dis{Architecture::X86_64,{}};
  const std::vector<std::uint8_t> bytes={0x48,0x89,0xc8,0x48,0x89,0xda,0xc3}; // mov rax,rcx; mov rdx,rbx; ret
  dis.disassemble(bytes,pc,graph);
  auto l=std::make_unique<LLVMLifter>(Architecture::X86_64,"guarded-entry-independent");
  if(!l->setExecutionMode(ExecutionMode::StaticAnalysis)||!l->setStaticEntryPolicy(StaticEntryPolicy::FunctionStart)||!l->lift(Function{&graph,pc},graph))std::exit(3);
  auto &m=l->module();m.setDataLayout("e-p:64:64");llvm::Function *f=nullptr;
  for(auto &candidate:m)if(!candidate.isDeclaration()){f=&candidate;break;}if(!f)std::exit(4);
  symbol=f->getName().str();auto &c=m.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);
  auto *entrySwitch=llvm::dyn_cast<llvm::SwitchInst>(f->getEntryBlock().getTerminator());if(!entrySwitch||entrySwitch->getNumCases()!=1)std::exit(5);
  llvm::Value *regs=nullptr,*base=nullptr;
  for(auto &op:f->getEntryBlock())if(auto *ld=llvm::dyn_cast<llvm::LoadInst>(&op))if(ld->getType()->isPointerTy()){
    std::int64_t off=0;auto *p=llvm::GetPointerBaseWithConstantOffset(ld->getPointerOperand(),off,m.getDataLayout());
    if(p==f->getArg(0)&&off==24)regs=ld;if(p==f->getArg(1)&&off==0)base=ld;
  }
  if(!regs||!base)std::exit(6);
  llvm::Value *initial[6]={};const unsigned offsets[]={0,8,16,24,32,128};
  for(auto &op:f->getEntryBlock())if(auto *ld=llvm::dyn_cast<llvm::LoadInst>(&op))if(ld->getType()->isIntegerTy(64)){
    std::int64_t off=0;auto *p=llvm::GetPointerBaseWithConstantOffset(ld->getPointerOperand(),off,m.getDataLayout());
    if(p==regs)for(unsigned k=0;k<6;++k)if(off==offsets[k])initial[k]=ld;
  }
  for(auto *v:initial)if(!v)std::exit(7);
  auto block=[&](const char *n){return llvm::BasicBlock::Create(c,n,f);};
  auto *body=block("oracle.machine.entry"),*check=block("oracle.check"),*valid=block("oracle.valid"),*fail=block("oracle.failure"),*done=block("oracle.done"),*latch=block("oracle.latch");
  entrySwitch->setSuccessor(1,body);
  B b(body);auto *iteration=b.CreatePHI(i64,2,"oracle.iteration");iteration->addIncoming(b.getInt64(0),&f->getEntryBlock());
  auto *index=initial[3],*x=initial[1];llvm::Value *currentBase=base;
  auto unknown=[&](){auto helper=m.getOrInsertFunction("guarded_unknown",llvm::FunctionType::get(b.getVoidTy(),{ptr,ptr,ptr,i64,i64},false));b.CreateCall(helper,{regs,base,f->getArg(1),index,x});};
  if(mode==1)write(b,b.CreateXor(x,b.getInt64(34)),at(b,base,index))->setMetadata("oracle.effect",llvm::MDNode::get(c,{}));
  if(mode==2)write(b,b.getInt8(0xa5),at(b,at(b,base,index),1));
  if(mode==3)unknown();
  if(mode==5)write(b,x,at(b,regs,std::uint64_t{0}));
  if(mode==6)write(b,b.getInt64(7),at(b,base,72));
  if(mode==7)write(b,x,at(b,f->getArg(3),16));
  if(mode==9){auto *next=at(b,base,4096);write(b,next,f->getArg(1));currentBase=read(b,ptr,f->getArg(1),"oracle.reloaded.base");}
  if(mode==8){
    auto *recurse=block("oracle.recursive"),*resume=block("oracle.resume");
    b.CreateCondBr(b.CreateICmpNE(initial[2],b.getInt64(0)),recurse,resume);
    b.SetInsertPoint(recurse);write(b,b.getInt64(0),at(b,regs,16));write(b,b.getInt64(34),at(b,base,index));
    b.CreateCall(f,{f->getArg(0),f->getArg(1),f->getArg(2),f->getArg(3)});b.CreateBr(resume);b.SetInsertPoint(resume);
  }
  b.CreateBr(check);b.SetInsertPoint(check);
  auto *size=read(b,i64,at(b,f->getArg(1),8),"oracle.actual.size");
  auto *fits=b.CreateAnd(b.CreateICmpUGE(size,b.getInt64(8)),b.CreateICmpULE(index,b.CreateSub(size,b.getInt64(8))));b.CreateCondBr(fits,valid,fail)->setMetadata("oracle.bounds",llvm::MDNode::get(c,{}));
  b.SetInsertPoint(valid);auto *value=read(b,i64,at(b,currentBase,index),"oracle.actual.target");value->setMetadata("oracle.target",llvm::MDNode::get(c,{}));
  if(mode==4)b.CreateCondBr(b.CreateICmpULT(iteration,b.CreateAnd(initial[2],b.getInt64(3))),latch,done);else b.CreateBr(done);
  b.SetInsertPoint(latch);auto *next=b.CreateAdd(iteration,b.getInt64(1));write(b,b.CreateAdd(b.getInt64(34),next),at(b,base,index));b.CreateBr(body);iteration->addIncoming(next,latch);
  // The unused latch remains a valid unreachable block for non-loop modes.
  auto finish=[&](llvm::BasicBlock *where,bool ok){
    b.SetInsertPoint(where);auto *result=ok?b.CreateXor(value,x):b.CreateXor(index,b.getInt64(0xbad0000000000000ULL));
    write(b,result,at(b,regs,std::uint64_t{0}));write(b,iteration,at(b,regs,16));write(b,b.getInt64(ok?0xdead:pc),at(b,regs,128));
    llvm::Value *fields[]={b.getInt64(ok?1:5),b.getInt64(0),b.getInt64(pc),b.getInt64(ok?0xdead:pc),b.getInt64(ok),b.getInt64(0),b.getInt64(ok?0:3)};
    for(unsigned k=0;k<7;++k)write(b,fields[k],at(b,f->getArg(3),k*8));b.CreateRetVoid();
  };finish(done,true);finish(fail,false);
  if(llvm::verifyModule(m,&llvm::errs()))std::exit(8);return l;
}
int main(int argc,char **argv){
  if(argc!=2)return 2;std::filesystem::path out(argv[1]);std::filesystem::create_directories(out);
  for(unsigned mode=0;mode<std::size(Modes);++mode){
    std::string symbol;auto original=source(mode,symbol);auto before=original->ir();dump(*original,out/(std::string(Modes[mode])+"-original.ll"));
#ifdef GUARDED_ENTRY_API
    GuardedEntryContext context;context.entry=0x1000+mode*0x100;context.contextId="oracle";context.provenance="independent frozen synthetic LLVM facts, never ELF initialization";
    context.registers={GuardedEntryBytes{24,{0,1,0,0,0,0,0,0}}};context.memory={GuardedEntryBytes{256,{14,0,0,0,0,0,0,0}}};
    std::string error;auto view=original->createGuardedEntryView(context,error);if(!view){llvm::errs()<<error<<"\n";return 9;}
    if(original->ir()!=before)return 10;auto manifest=view->guardedEntryManifest();
    context.memory[0].bytes[0]=99;context.registers.clear();original.reset();
    if(view->guardedEntryManifest()!=manifest||llvm::verifyModule(view->module(),&llvm::errs()))return 11;
    dump(*view,out/(std::string(Modes[mode])+"-guarded.ll"));
    {std::error_code e;llvm::raw_fd_ostream f((out/(std::string(Modes[mode])+"-manifest.json")).string(),e);if(e)return 12;f<<manifest;}
    passes::PassPipeline pipeline;for(auto name:{"mem2reg","instcombine","guest-memory","gvn","dispatch-specialize","control-flow","dce"})if(!pipeline.add(name))return 13;
    auto report=pipeline.run(*view,{2,64,true});if(!report.ok||llvm::verifyModule(view->module(),&llvm::errs()))return 14;
    dump(*view,out/(std::string(Modes[mode])+"-optimized.ll"));
    {std::error_code e;llvm::raw_fd_ostream f((out/(std::string(Modes[mode])+"-pipeline.json")).string(),e);if(e)return 15;f<<report.toJSON();}
#endif
    llvm::outs()<<"case="<<Modes[mode]<<" original="<<symbol<<" guarded="<<symbol<<".guarded.oracle invalid=0\n";
  }
  const char *faults[]={"constant","drop_store","drop_call","bounds","entry"};
  for(unsigned fault=0;fault<5;++fault){
    std::string symbol;auto l=source(fault<2?1:fault==2?3:0,symbol);auto *f=l->module().getFunction(symbol);
    for(auto &block:*f)for(auto it=block.begin();it!=block.end();){auto *op=&*it++;
      if(fault==0&&op->getMetadata("oracle.target")){op->replaceAllUsesWith(llvm::ConstantInt::get(op->getType(),14));op->eraseFromParent();}
      if(fault==1&&op->getMetadata("oracle.effect"))op->eraseFromParent();
      if(fault==2)if(auto *call=llvm::dyn_cast<llvm::CallInst>(op))if(call->getCalledFunction()&&call->getCalledFunction()->getName()=="guarded_unknown")call->eraseFromParent();
      if(fault==3&&op->getMetadata("oracle.bounds"))llvm::cast<llvm::BranchInst>(op)->setCondition(llvm::ConstantInt::getTrue(l->module().getContext()));
    }
    if(fault==4){auto *sw=llvm::cast<llvm::SwitchInst>(f->getEntryBlock().getTerminator());auto *target=sw->getSuccessor(1);llvm::BranchInst::Create(target,sw);sw->eraseFromParent();}
    f->setName(std::string("control_")+faults[fault]);if(llvm::verifyModule(l->module(),&llvm::errs()))return 16;
    dump(*l,out/(std::string("control_")+faults[fault]+".ll"));
  }
}
