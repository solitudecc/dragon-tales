// Independent reporting oracle: identical generator binary loads old/new backend.
#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <string>

using B = llvm::IRBuilder<llvm::NoFolder>;
struct Scenario { const char *name, *shape; unsigned nodes; bool branch; };
const Scenario Cases[] = {
  {"zero", "freeze", 0, false},
  {"switch_short", "freeze", 1, false},
  {"switch_exact", "freeze", 2, false},
  {"switch_spare", "freeze", 3, false},
  {"branch_short", "freeze", 1, true},
  {"branch_exact", "freeze", 2, true},
  {"literal_exact", "literal", 1, false},
  {"unknown_exact", "unknown", 1, false},
  {"unknown_branch", "unknown", 1, true},
  {"unsupported_root", "icmp", 1, true},
  {"flag_exact", "flag", 3, false},
  {"flag_short", "flag", 2, false},
  {"undef_exact", "undef", 2, false},
  {"shared_short", "shared", 4, false},
  {"shared_exact", "shared", 5, false},
  {"unknown_deep", "unknown_deep", 2, false},
  {"cycle_exact", "cycle", 2, false},
  {"cycle_short", "cycle", 1, false},
  {"default_short", "deep", 128, false},
  {"default_exact", "deep", 131, false},
  {"mixed_continue", "mixed", 1, false},
};

void build(llvm::Module &m, std::string shape, bool branch, const char *name) {
  auto &c=m.getContext(); auto *i64=llvm::Type::getInt64Ty(c);
  auto *argTy=branch?llvm::Type::getInt1Ty(c):i64;
  auto *f=llvm::Function::Create(llvm::FunctionType::get(i64,{argTy},false),llvm::GlobalValue::ExternalLinkage,name,m);
  f->getArg(0)->addAttr(llvm::Attribute::NoUndef);
  auto *entry=llvm::BasicBlock::Create(c,"entry",f);
  auto *yes=llvm::BasicBlock::Create(c,"yes",f),*no=llvm::BasicBlock::Create(c,"no",f);
  B b(entry); llvm::Value *v=llvm::ConstantInt::get(argTy,1);
  if(shape=="freeze")v=b.CreateFreeze(v,"frozen");
  else if(shape=="unknown")v=f->getArg(0);
  else if(shape=="icmp")v=b.CreateICmpEQ(f->getArg(0),b.getTrue(),"comparison");
  else if(shape=="flag")v=b.CreateNSWAdd(b.getInt64(1),b.getInt64(0),"flagged");
  else if(shape=="undef")v=b.CreateFreeze(llvm::UndefValue::get(i64),"unknown.freeze");
  else if(shape=="shared") {auto *shared=b.CreateFreeze(b.getInt64(1));v=b.CreateAdd(shared,shared,"repeated.ssa");}
  else if(shape=="unknown_deep") {auto *deep=b.CreateFreeze(b.CreateFreeze(b.getInt64(1)));v=b.CreateAdd(f->getArg(0),deep,"unknown.plus.deep");}
  else if(shape=="deep")for(unsigned k=0;k<130;++k)v=b.CreateFreeze(v,"deep");
  if(branch)b.CreateCondBr(v,yes,no);
  else {auto *s=b.CreateSwitch(v,no,2);s->addCase(b.getInt64(1),yes);s->addCase(b.getInt64(2),yes);}
  b.SetInsertPoint(yes);b.CreateRet(b.getInt64(11));b.SetInsertPoint(no);b.CreateRet(b.getInt64(22));
  if(shape=="cycle") {
    // No concrete execution enters this closed block. It checks bounded IR
    // proof robustness only, never supplies an invented defined native input.
    auto *loop=llvm::BasicBlock::Create(c,"closed.loop",f);b.SetInsertPoint(loop);
    auto *p=b.CreatePHI(i64,1,"self");p->addIncoming(p,loop);
    b.CreateSwitch(p,loop,0);
    // Remove the entry switch to isolate the cycle's one condition.
    entry->getTerminator()->eraseFromParent();b.SetInsertPoint(entry);b.CreateBr(no);
  }
}
void dump(llvm::Module &m,const std::filesystem::path &p) {
  std::error_code ec;llvm::raw_fd_ostream out(p.string(),ec,llvm::sys::fs::OF_Text);
  if(ec)std::exit(2);m.print(out,nullptr);
}
int main(int argc,char **argv) {
  if(argc!=2)return 2;std::filesystem::path out(argv[1]);std::filesystem::create_directories(out);
  for(auto test:Cases) {
    llvm::LLVMContext c;llvm::Module m("dispatch-budget-independent",c);m.setDataLayout("e-p:64:64");
    if(std::string(test.shape)=="mixed") {
      build(m,"freeze",false,"first.exhausted");build(m,"literal",true,"second.proved");build(m,"unknown",false,"third.unknown");
    }else build(m,test.shape,test.branch,"probe");
    if(llvm::verifyModule(m,&llvm::errs()))return 3;
    dump(m,out/(std::string(test.name)+"-before.ll"));
    auto s=dragon::llvmlifter::runDispatcherSpecialization(m,{256,test.nodes});
    if(llvm::verifyModule(m,&llvm::errs()))return 4;
    dump(m,out/(std::string(test.name)+"-after.ll"));
    llvm::outs()<<"case="<<test.name<<" available="<<s.available<<" complete="<<s.complete<<" changed="<<s.changed
      <<" visited="<<s.conditionsVisited<<" unique="<<s.uniqueConditions<<" switches="<<s.switchesVisited
      <<" branches_rewritten="<<s.branchesRewritten<<" switches_rewritten="<<s.switchesRewritten
      <<" unresolved="<<s.unresolvedSwitches<<" budget="<<s.budgetExhausted<<" invalid=0\n";
  }
}
