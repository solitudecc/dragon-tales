#include <dragon/lifters/llvm/DispatchCorridor.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "fixture.hpp"
using namespace dragon::llvmlifter::detail;
std::string print(llvm::Module &m){std::string s;llvm::raw_string_ostream out(s);m.print(out,nullptr);return s;}
int main(int argc,char **argv){
 if(argc!=2||std::filesystem::exists(argv[1]))return 2;
 std::filesystem::create_directories(argv[1]);std::filesystem::path out=argv[1];std::ofstream rows(out/"rows.tsv");
 for(unsigned count:{16u,70u}){
  llvm::LLVMContext context;llvm::SMDiagnostic error;auto module=llvm::parseAssemblyString(carriedBatchLoop(count),error,context);
  if(!module||llvm::verifyModule(*module,&llvm::errs()))return 3;
  const auto stem="n"+std::to_string(count);const auto before=print(*module);std::ofstream(out/(stem+"-before.ll"))<<before;
  auto high=llvm::CloneModule(*module);
  for(auto [m,work]:{std::pair(module.get(),180000u),std::pair(high.get(),1000000u)}){
   auto *f=m->getFunction("f");auto block=[&](const std::string &name){for(auto &b:*f)if(b.getName()==name)return &b;return (llvm::BasicBlock*)nullptr;};
   std::vector<DispatchCorridorEdge> edges;
   for(unsigned n=count;n!=0;--n)edges.push_back({block("seed"+std::to_string(n-1)),0,{block("head"),block("dispatch")}});
   DispatchCorridorBatchOptions options;options.maxWork=work;auto stats=threadDispatchCorridorBatch(*f,edges,options);
   if(llvm::verifyModule(*m,&llvm::errs()))return 4;
   auto after=print(*m);if(!stats.changed&&before!=after)return 5;
   std::ofstream(out/(stem+(work==180000?"-after.ll":"-high.ll")))<<after;
   rows<<count<<'\t'<<work<<'\t'<<stats.changed<<'\t'<<stats.complete<<'\t'<<stats.budgetExhausted<<'\t'<<stats.work<<'\t'<<stats.message<<'\n';
  }
 }
}
