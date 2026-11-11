#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>

namespace {
using Builder = llvm::IRBuilder<llvm::NoFolder>;
llvm::Value *at(Builder &b, llvm::Value *base, unsigned offset) {
  return b.CreateGEP(b.getInt8Ty(), base, b.getInt64(offset));
}
void store(Builder &b, llvm::Value *v, llvm::Value *p) {
  b.CreateStore(v, p)->setAlignment(llvm::Align(1));
}
void make(llvm::Module &m, bool reverse, unsigned fault) {
  auto &c = m.getContext();
  auto *ptr = llvm::PointerType::getUnqual(c);
  auto *i64 = llvm::Type::getInt64Ty(c);
  auto *f = llvm::Function::Create(llvm::FunctionType::get(
      i64, {ptr, ptr, ptr, i64, i64, i64}, false),
      llvm::GlobalValue::ExternalLinkage, "join_memory", m);
  for (auto &arg : f->args()) arg.addAttr(llvm::Attribute::NoUndef);
  auto *entry = llvm::BasicBlock::Create(c, "entry", f);
  auto *choose = llvm::BasicBlock::Create(c, "choose", f);
  auto *one = llvm::BasicBlock::Create(c);
  auto *two = llvm::BasicBlock::Create(c);
  if (std::less<llvm::BasicBlock *>{}(two, one)) std::swap(one, two);
  auto *left = reverse ? two : one;
  auto *right = reverse ? one : two;
  left->setName("left"); right->setName("right");
  left->insertInto(f); right->insertInto(f);
  auto *join = llvm::BasicBlock::Create(c, "join", f);
  auto *invalid = llvm::BasicBlock::Create(c, "invalid", f);
  Builder b(entry);
  auto *regs=f->getArg(0), *desc=f->getArg(1), *ex=f->getArg(2);
  auto *index=f->getArg(3), *condition=f->getArg(4), *seed=f->getArg(5);
  auto *base=b.CreateLoad(ptr, desc, "guest.base"); base->setAlignment(llvm::Align(1));
  auto *slot=b.CreateGEP(b.getInt8Ty(), base, index, "guest.slot");
  auto *odd=b.CreateICmpNE(b.CreateAnd(condition,b.getInt64(1)),b.getInt64(0));
  b.CreateCondBr(b.CreateICmpULE(index,b.getInt64(65528)),choose,invalid);
  b.SetInsertPoint(choose); b.CreateCondBr(odd,left,right);
  b.SetInsertPoint(left); if(fault!=1)store(b,b.getInt64(10),slot); b.CreateBr(join);
  b.SetInsertPoint(right); if(fault!=1)store(b,b.getInt64(20),slot); b.CreateBr(join);
  b.SetInsertPoint(join);
  auto *loaded=b.CreateLoad(i64,slot,"observed"); loaded->setAlignment(llvm::Align(1));
  llvm::Value *seen=loaded;
  if(fault==2)seen=b.CreateSelect(odd,b.getInt64(20),b.getInt64(10));
  auto *result=b.CreateXor(seen,seed);
  store(b,result,at(b,regs,0)); store(b,seen,at(b,regs,8));
  store(b,condition,at(b,regs,16)); store(b,index,at(b,regs,24));
  // The output may alias the selected slot; the model preserves this order.
  store(b,result,at(b,base,0x300));
  llvm::Value *ok[]={b.getInt64(1),b.getInt64(0),b.getInt64(0x6000),seen,
                    b.getInt64(1),index,condition};
  for(unsigned n=0;n!=7;++n)store(b,ok[n],at(b,ex,n*8));
  b.CreateRet(result);
  b.SetInsertPoint(invalid);
  llvm::Value *bad[]={b.getInt64(5),b.getInt64(0),b.getInt64(0x6000),
                     b.getInt64(0x6000),b.getInt64(1),index,b.getInt64(3)};
  for(unsigned n=0;n!=7;++n)store(b,bad[n],at(b,ex,n*8));
  b.CreateRet(b.getInt64(0));
}
void dump(llvm::Module &m,const std::filesystem::path &path) {
  std::error_code error; llvm::raw_fd_ostream out(path.string(),error);
  if(error)std::exit(2); m.print(out,nullptr);
}
}
int main(int argc,char **argv) {
  if(argc!=2)return 2; const std::filesystem::path out(argv[1]);
  for(unsigned layout=0;layout!=2;++layout) {
    llvm::LLVMContext c; llvm::Module m("join-independent",c);
    m.setDataLayout("e-p:64:64"); make(m,layout,0);
    if(llvm::verifyModule(m,&llvm::errs()))return 3;
    auto name="join_"+std::to_string(layout); dump(m,out/(name+"-before.ll"));
    dragon::llvmlifter::MemoryForwardOptions options; options.runMem2Reg=false;
    auto stats=dragon::llvmlifter::detail::runGuestMemorySSA(m,options);
    if(llvm::verifyModule(m,&llvm::errs()))return 4;
    dump(m,out/(name+"-after.ll"));
    llvm::outs()<<"case="<<name<<" available="<<stats.available
      <<" complete="<<stats.complete<<" forwarded="<<stats.loadsForwarded
      <<" phis="<<stats.phisInserted<<" joins="<<stats.joinBarriers<<'\n';
  }
  for(unsigned fault=1;fault!=3;++fault) {
    llvm::LLVMContext c; llvm::Module m("join-independent-fault",c);
    m.setDataLayout("e-p:64:64"); make(m,false,fault);
    if(llvm::verifyModule(m,&llvm::errs()))return 5;
    dump(m,out/((fault==1?"drop-store":"wrong-value")+std::string(".ll")));
  }
}
