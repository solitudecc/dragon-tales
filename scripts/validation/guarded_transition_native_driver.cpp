// Actual production core/controller adapter, linked only to frozen libdragon.
#include "GuardedTransition.hpp"
#include "GuardedTransitions.hpp"
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>
using namespace dragon;
using namespace dragon::llvmlifter;
static std::string text(const llvm::Value &v){std::string s;llvm::raw_string_ostream o(s);v.print(o);return s;}
static std::string moduleText(const llvm::Module &m){std::string s;llvm::raw_string_ostream o(s);m.print(o,nullptr);return s;}
static llvm::BasicBlock *block(llvm::Function &f,const char *name){for(auto &b:f)if(b.getName()==name)return &b;return nullptr;}
static bool save(const std::string &s,const std::string &path){std::error_code ec;llvm::raw_fd_ostream o(path,ec);if(ec)return false;o<<s;o.close();return !o.has_error();}
int main(int argc,char **argv){
 if(argc!=6)return 2;
 const std::string input=argv[1],output=argv[2],reportPath=argv[3],method=argv[4];
 if(llvm::sys::fs::exists(output)||llvm::sys::fs::exists(reportPath))return 2;
 std::uint64_t hint=0;try{hint=std::stoull(argv[5]);}catch(...){return 2;}
 auto buffer=llvm::MemoryBuffer::getFile(input);if(!buffer)return 2;
 LLVMLifter lifter(Architecture::X86_64,input);if(!lifter.setExecutionMode(ExecutionMode::StaticAnalysis))return 2;
 llvm::SMDiagnostic diagnostic;
 if(llvm::parseAssemblyInto((*buffer)->getMemBufferRef(),&lifter.module(),nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 auto &m=lifter.module();auto *f=m.getFunction("oracle");if(!f)return 2;
 const bool automatic=method.starts_with("auto");
 if(automatic && method!="auto_none"){
  std::string bytes;const char *hex="0123456789abcdef";
  for(unsigned i=0;i<8;++i){auto b=(hint>>(8*i))&255;bytes+=hex[b>>4];bytes+=hex[b&15];}
  std::string manifest=method=="auto_bad"?"{bad":"{\"view_symbol\":\"oracle\",\"memory\":[{\"offset\":\"0x200\",\"bytes\":\""+bytes+"\"}]}";
  m.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(m.getContext(),llvm::MDString::get(m.getContext(),manifest)));
 }
 if(method=="auto_32")m.setDataLayout("e-p:32:32");
 if(llvm::verifyModule(m,&llvm::errs()))return 2;
 const auto before=moduleText(m);if(!save(before,output+".parsed-before.ll"))return 2;
 auto *originalModule=&m;auto *originalFunction=f;
 std::vector<llvm::Argument *> arguments;for(auto &a:f->args())arguments.push_back(&a);
 std::vector<std::pair<llvm::GlobalValue *,std::string>> globals;for(auto &g:m.global_values())globals.push_back({&g,text(g)});
 auto attributes=f->getAttributes();auto linkage=f->getLinkage();
 bool changed=false,complete=false,rejected=false;std::string message;
 llvm::json::Object result{{"method",method},{"candidate",argv[5]},{"input_verified",true},{"candidate_is_prediction_only",true}};
 if(automatic){
  auto s=dragon::llvmlifter::detail::runGuardedTransitions(m);
  changed=s.changed;complete=s.complete;rejected=s.rejected!=0;message=s.message;
  result["available"]=s.available;result["candidate_edges"]=static_cast<int64_t>(s.candidateEdges);
  result["rewritten_edges"]=static_cast<int64_t>(s.rewrittenEdges);result["unresolved_edges"]=static_cast<int64_t>(s.unresolvedEdges);
  result["fallback_switches"]=static_cast<int64_t>(s.fallbackSwitchesCreated);result["original_switches_retained"]=static_cast<int64_t>(s.originalSwitchesRetained);
  result["budget_exhausted"]=static_cast<int64_t>(s.budgetExhausted);
 }else{
  auto *head=block(*f,"head"),*seed=block(*f,"seed"),*check=block(*f,"check"),*write=block(*f,"write"),*dispatch=block(*f,"dispatch");
  if(!head||!seed||!check||!write||!dispatch)return 2;
  dragon::llvmlifter::detail::GuardedTransitionRequest q{seed,0,head,{head,check,write,dispatch},{0,0,0},llvm::ConstantInt::get(llvm::Type::getInt64Ty(m.getContext()),hint)};
  dragon::llvmlifter::detail::GuardedTransitionOptions opts;
  if(method=="core_budget")opts.maxBlocks=1;
  auto s=dragon::llvmlifter::detail::cloneGuardedTransition(*f,q,opts);
  changed=s.changed;complete=s.complete;rejected=s.rejected;message=s.message;
  result["cloned_blocks"]=static_cast<int64_t>(s.clonedBlocks);result["snapshot_phis"]=static_cast<int64_t>(s.snapshotPhis);
  result["repair_phis"]=static_cast<int64_t>(s.repairPhis);result["original_exit_edges"]=static_cast<int64_t>(s.originalExitEdges);result["budget_exhausted"]=s.budgetExhausted;
 }
 bool identity=&lifter.module()==originalModule && m.getFunction("oracle")==originalFunction && f->getAttributes()==attributes && f->getLinkage()==linkage;
 for(unsigned n=0;n<arguments.size();++n)identity=identity&&f->getArg(n)==arguments[n];
 std::size_t n=0;for(auto &g:m.global_values()){
  if(n>=globals.size()){identity=false;break;}
  identity=identity && &g==globals[n].first&&( &g==f || text(g)==globals[n].second);++n;
 }
 identity=identity&&n==globals.size();
 const bool verified=!llvm::verifyModule(m,&llvm::errs());const auto after=moduleText(m);
 result["changed"]=changed;result["complete"]=complete;result["rejected"]=rejected;result["message"]=message;
 result["verified_output"]=verified;result["identity_preserved"]=identity;result["unchanged_on_rejection"]=!changed&&before==after;
 result["full_cff_completion_claimed"]=false;
 std::string report;llvm::raw_string_ostream reportStream(report);reportStream<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result)))<<"\n";
 if(!save(report,reportPath))return 2;
 if(!verified||!identity)return 4;
 if(!changed)return before==after?3:4;
 return save(after,output)?0:2;
}
