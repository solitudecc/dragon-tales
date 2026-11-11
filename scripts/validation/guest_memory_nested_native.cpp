#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace {
using B=llvm::IRBuilder<llvm::NoFolder>;
const char *names[]={"nested","multi","self"};
llvm::Value *at(B &b,llvm::Value *base,unsigned offset){return b.CreateGEP(b.getInt8Ty(),base,b.getInt64(offset));}
void store(B &b,llvm::Value *v,llvm::Value *p){b.CreateStore(v,p)->setAlignment(llvm::Align(1));}
llvm::Value *load(B &b,llvm::Type *t,llvm::Value *p,const char *name=""){auto *v=b.CreateLoad(t,p,name);v->setAlignment(llvm::Align(1));return v;}
void make(llvm::Module &m,unsigned shape,unsigned layout,unsigned fault=0){
 auto &c=m.getContext();auto *ptr=llvm::PointerType::getUnqual(c);auto *i64=llvm::Type::getInt64Ty(c);
 const auto name=fault?std::string("control_")+names[shape]+(fault==1?"_drop_store":"_stale_load"):std::string("nested_")+names[shape]+"_"+std::to_string(layout);
 auto *f=llvm::Function::Create(llvm::FunctionType::get(i64,{ptr,ptr,ptr,i64,i64,i64},false),llvm::GlobalValue::ExternalLinkage,name,m);
 for(auto &a:f->args())a.addAttr(llvm::Attribute::NoUndef);
 auto *e=llvm::BasicBlock::Create(c,"entry",f);
 std::array<llvm::BasicBlock*,8> blocks{};std::array<unsigned,8> order{0,1,2,3,4,5,6,7};
 std::rotate(order.begin(),order.begin()+layout,order.end());if(layout&1)std::reverse(order.begin(),order.end());
 const char *blocknames[]={"outer","prefix","inner","body","latch","latch.one","latch.two","exit"};
 for(auto n:order)blocks[n]=llvm::BasicBlock::Create(c,blocknames[n],f);
 auto *o=blocks[0],*p=blocks[1],*i=blocks[2],*body=blocks[3],*l=blocks[4],*l1=blocks[5],*l2=blocks[6],*exit=blocks[7];
 B b(e);auto *regs=f->getArg(0),*desc=f->getArg(1),*ex=f->getArg(2),*x=f->getArg(3);
 auto *data=load(b,ptr,desc,"guest.base");auto *slot=at(b,data,0x100);
 auto *outerLimit=b.CreateAnd(f->getArg(4),b.getInt64(3));auto *innerLimit=b.CreateURem(f->getArg(5),b.getInt64(3));
 store(b,x,slot);b.CreateBr(o);
 b.SetInsertPoint(o);auto *outer=b.CreatePHI(i64,shape==1?3:2,"outer.count");auto *sum=b.CreatePHI(i64,shape==1?3:2,"sum");auto *total=b.CreatePHI(i64,shape==1?3:2,"total");
 outer->addIncoming(b.getInt64(0),e);sum->addIncoming(b.getInt64(0),e);total->addIncoming(b.getInt64(0),e);
 b.CreateCondBr(b.CreateICmpULT(outer,outerLimit),p,exit);
 b.SetInsertPoint(p);if(shape==1)store(b,b.CreateAdd(x,outer),slot);
 auto *seen=load(b,i64,slot,"observed.prefix");if(fault==2)seen=x;
 auto *nextSum=b.CreateAdd(sum,seen);auto *nextOuter=b.CreateAdd(outer,b.getInt64(1));
 if(shape==2)b.CreateCondBr(b.CreateICmpEQ(innerLimit,b.getInt64(0)),l,i);else b.CreateBr(i);
 b.SetInsertPoint(i);auto *inner=b.CreatePHI(i64,2,"inner.count");inner->addIncoming(b.getInt64(0),p);
 auto *currentTotal=b.CreatePHI(i64,2,"inner.total");currentTotal->addIncoming(total,p);
 llvm::Value *afterTotal=nullptr;
 if(shape==2){
  auto *value=b.CreateXor(x,b.CreateAdd(b.CreateMul(outer,b.getInt64(0x101)),b.CreateAdd(b.CreateMul(inner,b.getInt64(0x10001)),b.getInt64(0x55))));
  if(fault!=1)store(b,value,slot);
  auto *nextInner=b.CreateAdd(inner,b.getInt64(1));afterTotal=b.CreateAdd(currentTotal,b.getInt64(1));
  inner->addIncoming(nextInner,i);currentTotal->addIncoming(afterTotal,i);
  b.CreateCondBr(b.CreateICmpULT(nextInner,innerLimit),i,l);
  b.SetInsertPoint(body);b.CreateUnreachable();
 }else{
  b.CreateCondBr(b.CreateICmpULT(inner,innerLimit),body,l);
  b.SetInsertPoint(body);auto *value=b.CreateXor(x,b.CreateAdd(b.CreateMul(outer,b.getInt64(0x101)),b.CreateAdd(b.CreateMul(inner,b.getInt64(0x10001)),b.getInt64(0x55))));
  if(fault!=1)store(b,value,slot);
  auto *nextInner=b.CreateAdd(inner,b.getInt64(1));auto *nextTotal=b.CreateAdd(currentTotal,b.getInt64(1));inner->addIncoming(nextInner,body);currentTotal->addIncoming(nextTotal,body);b.CreateBr(i);afterTotal=currentTotal;
 }
 b.SetInsertPoint(l);
 if(shape==2){auto *joined=b.CreatePHI(i64,2,"completed.inner");joined->addIncoming(total,p);joined->addIncoming(afterTotal,i);afterTotal=joined;}
 if(shape==1)b.CreateCondBr(b.CreateICmpEQ(b.CreateAnd(outer,b.getInt64(1)),b.getInt64(0)),l1,l2);else b.CreateBr(o);
 if(shape!=1){outer->addIncoming(nextOuter,l);sum->addIncoming(nextSum,l);total->addIncoming(afterTotal,l);}
 for(auto *latch:{l1,l2}){
  b.SetInsertPoint(latch);if(shape!=1){b.CreateUnreachable();continue;}
  if(latch==l2)store(b,nextSum,at(b,data,0x110));
  b.CreateBr(o);outer->addIncoming(nextOuter,latch);sum->addIncoming(nextSum,latch);total->addIncoming(afterTotal,latch);
 }
 b.SetInsertPoint(exit);auto *final=load(b,i64,slot,"final.state");auto *result=b.CreateXor(sum,final);
 store(b,result,at(b,regs,0));store(b,outer,at(b,regs,8));store(b,total,at(b,regs,16));store(b,final,at(b,regs,24));
 store(b,result,at(b,data,0x200));store(b,total,at(b,data,0x208));
 llvm::Value *fields[]={b.getInt64(1),b.getInt64(0),b.getInt64(0x5000+shape*0x100),final,b.getInt64(1),x,total};
 for(unsigned n=0;n!=7;++n)store(b,fields[n],at(b,ex,n*8));b.CreateRet(result);
}
void dump(llvm::Module &m,const std::filesystem::path &p){std::error_code ec;llvm::raw_fd_ostream f(p.string(),ec,llvm::sys::fs::OF_Text);if(ec)std::exit(2);m.print(f,nullptr);}
}
int main(int argc,char **argv){
 if(argc!=2)return 2;const std::filesystem::path out(argv[1]);
 for(unsigned shape=0;shape!=3;++shape)for(unsigned layout=0;layout!=8;++layout){
  llvm::LLVMContext c;std::vector<std::unique_ptr<llvm::Module>> pads;
  for(unsigned n=0;n<layout*7;++n){auto m=std::make_unique<llvm::Module>("padding",c);auto *f=llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(c),false),llvm::GlobalValue::PrivateLinkage,"pad",*m);B b(llvm::BasicBlock::Create(c,"entry",f));b.CreateRetVoid();pads.push_back(std::move(m));}
  llvm::Module m("nested-independent",c);m.setDataLayout("e-p:64:64");make(m,shape,layout);
  if(llvm::verifyModule(m,&llvm::errs()))return 3;
  const auto n=std::string(names[shape])+"_"+std::to_string(layout);dump(m,out/(n+"-before.ll"));
  auto s=dragon::llvmlifter::detail::runGuestMemorySSA(m,{});if(llvm::verifyModule(m,&llvm::errs()))return 4;dump(m,out/(n+"-after.ll"));
  llvm::outs()<<"case="<<n<<" available="<<s.available<<" forwarded="<<s.loadsForwarded<<" loop="<<s.loopBarriers<<" join="<<s.joinBarriers<<" complete="<<s.complete<<" budget="<<s.budgetExhausted<<" invalid=0\n";
 }
 for(unsigned shape=0;shape!=3;++shape)for(unsigned fault=1;fault!=3;++fault){llvm::LLVMContext c;llvm::Module m("nested-fault",c);m.setDataLayout("e-p:64:64");make(m,shape,0,fault);if(llvm::verifyModule(m,&llvm::errs()))return 5;dump(m,out/(std::string("control_")+names[shape]+(fault==1?"_drop_store":"_stale_load")+".ll"));}
}
