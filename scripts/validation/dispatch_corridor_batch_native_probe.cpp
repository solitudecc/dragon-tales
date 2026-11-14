// Parse-only and installed Batch API modes share the exact raw input filename.
// BEFORE_ONLY builds contain no cloner call/reference. Candidate labels select
// original handles only; they are never scalar/memory/definedness certificates.
#include <dragon/lifters/llvm/DispatchCorridor.hpp>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
using namespace dragon::llvmlifter::detail;
static std::string text(llvm::Module &module) {
  std::string value; llvm::raw_string_ostream out(value); module.print(out,nullptr);return value;
}
static bool save(const char *path,const std::string &value) {
  std::error_code error;llvm::raw_fd_ostream out(path,error);if(error)return false;
  out<<value;out.close();return !out.has_error();
}
static llvm::BasicBlock *block(llvm::Function &f,llvm::StringRef name) {
  for(auto &b:f)if(b.getName()==name)return &b;return nullptr;
}
int main(int argc,char **argv) {
  // mode raw.ll case.json parsed-before.ll output.ll report.json
  if(argc!=7)return 2;
  const std::string mode=argv[1];
  if(mode!="before"&&mode!="rewrite")return 2;
#ifdef DRAGON_DISPATCH_CORRIDOR_BEFORE_ONLY
  if(mode!="before")return 2;
#endif
  for(unsigned n=4;n!=7;++n)if(llvm::sys::fs::exists(argv[n]))return 2;
  llvm::LLVMContext context;llvm::SMDiagnostic diagnostic;
  auto input=llvm::MemoryBuffer::getFile(argv[2]);if(!input)return 3;
  auto module=llvm::parseAssembly((*input)->getMemBufferRef(),diagnostic,context);
  if(!module){diagnostic.print("corridor-batch-native",llvm::errs());return 3;}
  if(llvm::verifyModule(*module,&llvm::errs()))return 3;
  auto file=llvm::MemoryBuffer::getFile(argv[3]);if(!file)return 4;
  auto parsed=llvm::json::parse((*file)->getBuffer());
  if(!parsed){llvm::errs()<<llvm::toString(parsed.takeError())<<'\n';return 4;}
  auto *object=parsed->getAsObject();if(!object)return 4;
  auto functionName=object->getString("function");auto *array=object->getArray("candidates");
  if(!functionName||!array||array->size()>512)return 4;
  auto *function=module->getFunction(*functionName);if(!function||function->isDeclaration())return 4;
  std::vector<DispatchCorridorEdge> candidates;
  for(auto &item:*array){
    auto *edge=item.getAsObject();if(!edge)return 4;
    auto source=edge->getString("source");auto slot=edge->getInteger("successor_slot");auto *path=edge->getArray("path");
    if(!source||!slot||*slot<0||static_cast<uint64_t>(*slot)>std::numeric_limits<unsigned>::max()||!path)return 4;
    DispatchCorridorEdge candidate{block(*function,*source),static_cast<unsigned>(*slot),{}};
    if(!candidate.source)return 4;
    for(auto &element:*path){auto name=element.getAsString();if(!name)return 4;auto *b=block(*function,*name);if(!b)return 4;candidate.path.push_back(b);}
    candidates.push_back(std::move(candidate));
  }
  auto *originalFunction=function;std::vector<llvm::Argument *> arguments;
  for(auto &arg:function->args())arguments.push_back(&arg);
  const auto before=text(*module);bool called=false;DispatchCorridorStats stats;
#ifndef DRAGON_DISPATCH_CORRIDOR_BEFORE_ONLY
  if(mode=="rewrite"){called=true;stats=threadDispatchCorridorBatch(*function,candidates);}
#endif
  if(llvm::verifyModule(*module,&llvm::errs()))return 5;
  function=module->getFunction(*functionName);
  if(function!=originalFunction||function->arg_size()!=arguments.size())return 6;
  for(unsigned n=0;n!=arguments.size();++n)if(function->getArg(n)!=arguments[n])return 6;
  const auto after=text(*module);
  if(!save(argv[4],before)||!save(argv[5],after))return 7;
  llvm::json::Object report{{"mode",mode},{"raw_input",argv[2]},
    {"case_manifest",argv[3]},{"function",functionName->str()},
    {"original_candidates",static_cast<int64_t>(candidates.size())},
    {"cloner_calls",called?1:0},{"verified_before",true},{"verified_after",true},
    {"function_argument_identity",true},{"byte_equal",before==after},
    {"module_identifier",module->getModuleIdentifier()},
    {"scope","Installed opt-in atomic batch API; candidate labels carry no semantic facts"}};
  unsigned switches=0,cyclicSwitches=0;
  for(auto &b:*function) switches+=llvm::isa<llvm::SwitchInst>(b.getTerminator());
  for(auto it=llvm::scc_begin(function);!it.isAtEnd();++it){
    const auto &component=*it;bool cyclic=component.size()>1;
    if(!cyclic)for(auto *next:llvm::successors(component.front()))cyclic|=next==component.front();
    if(cyclic)for(auto *b:component)cyclicSwitches+=llvm::isa<llvm::SwitchInst>(b->getTerminator());
  }
  report["switches"]=switches;report["cyclic_switches"]=cyclicSwitches;
  const DispatchCorridorBatchOptions limits;
  report["batch_limits"]=llvm::json::Object{
    {"max_candidates",static_cast<int64_t>(limits.maxCandidates)},
    {"max_rewrites",static_cast<int64_t>(limits.maxRewrites)},
    {"max_cloned_blocks",static_cast<int64_t>(limits.maxClonedBlocks)},
    {"max_cloned_instructions",static_cast<int64_t>(limits.maxClonedInstructions)},
    {"max_repair_cells",static_cast<int64_t>(limits.maxRepairCells)},
    {"max_work",static_cast<int64_t>(limits.maxWork)}};
#ifdef DRAGON_DISPATCH_CORRIDOR_BEFORE_ONLY
  report["compiled_before_only"]=true;
#else
  report["compiled_before_only"]=false;
#endif
  if(called){
    llvm::json::Object values{{"changed",stats.changed},{"complete",stats.complete},
      {"budget_exhausted",stats.budgetExhausted},{"candidates_visited",static_cast<int64_t>(stats.candidatesVisited)},
      {"rejected",static_cast<int64_t>(stats.rejected)},{"rewritten_edges",static_cast<int64_t>(stats.rewrittenEdges)},
      {"cloned_blocks",static_cast<int64_t>(stats.clonedBlocks)},{"cloned_instructions",static_cast<int64_t>(stats.clonedInstructions)},
      {"snapshots",static_cast<int64_t>(stats.snapshots)},{"repair_phis",static_cast<int64_t>(stats.repairPhis)},
      {"repair_cells",static_cast<int64_t>(stats.repairCells)},{"work",static_cast<int64_t>(stats.work)},{"message",stats.message}};
    report["stats"]=std::move(values);
  }
  llvm::json::Array maps,solverMaps;std::ifstream mapped("/proc/self/maps");
  for(std::string line;std::getline(mapped,line);){
    if(line.find("libdragon.so")!=std::string::npos)maps.push_back(line);
    if(line.find("libz3.so")!=std::string::npos)solverMaps.push_back(line);
  }
  report["runtime_maps"]=std::move(maps);
  report["solver_maps"]=std::move(solverMaps);
  return save(argv[6],llvm::formatv("{0:2}\n",llvm::json::Value(std::move(report))).str())?0:7;
}
