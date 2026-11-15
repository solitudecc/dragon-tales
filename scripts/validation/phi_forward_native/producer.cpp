#include "LLVMPhiLowBits.hpp"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <fstream>
#include "fixture.hpp"
using namespace dragon::llvmlifter::detail;
std::string print(llvm::Module &m){std::string s;llvm::raw_string_ostream out(s);m.print(out,nullptr);return s;}
int main(int argc,char **argv){
 if(argc!=2||std::filesystem::exists(argv[1]))return 2;
 std::filesystem::create_directories(argv[1]);std::filesystem::path out=argv[1];std::ofstream rows(out/"rows.tsv");
 for(unsigned count:{40u,80u})for(bool relation:{false,true}){
  llvm::LLVMContext context;llvm::SMDiagnostic diagnostic;auto module=llvm::parseAssemblyString(relation?relationForwardedLoop(count):forwardedLoop(count),diagnostic,context);
  if(!module||llvm::verifyModule(*module,&llvm::errs()))return 3;
  auto before=print(*module);auto stem=std::string(relation?"r":"d")+std::to_string(count);std::ofstream(out/(stem+"-before.ll"))<<before;
  auto *f=module->getFunction("f");llvm::TruncInst *trunc=nullptr;
  for(auto &b:*f)for(auto &i:b)if(i.getName()=="low")trunc=llvm::dyn_cast<llvm::TruncInst>(&i);
  if(!trunc)return 4;auto result=replacePhiLowTruncs(*f,{trunc});
  if(llvm::verifyModule(*module,&llvm::errs()))return 5;
  auto after=print(*module);if(!result.changed&&before!=after)return 6;
  std::ofstream(out/(stem+"-after.ll"))<<after;
  rows<<stem<<'\t'<<result.changed<<'\t'<<result.complete<<'\t'<<result.budgetExhausted<<'\t'<<result.nodes<<'\t'<<result.work<<'\t'<<result.message<<'\n';
 }
}
