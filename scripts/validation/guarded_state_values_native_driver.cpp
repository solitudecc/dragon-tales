// Actual guarded-state-values public API with independent hint metadata.
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
#include <vector>
using namespace dragon;
using namespace dragon::llvmlifter;
static std::string text(const llvm::Value &v){std::string s;llvm::raw_string_ostream o(s);v.print(o);return s;}
static std::string moduleText(const llvm::Module &m){std::string s;llvm::raw_string_ostream o(s);m.print(o,nullptr);return s;}
static bool save(const std::string &s,const std::string &path){std::error_code ec;llvm::raw_fd_ostream o(path,ec);if(ec)return false;o<<s;o.close();return !o.has_error();}
int main(int argc,char **argv){
 if(argc!=5)return 2;
 const std::string input=argv[1],output=argv[2],reportPath=argv[3],mode=argv[4];
 if(llvm::sys::fs::exists(output)||llvm::sys::fs::exists(reportPath))return 2;
 auto buffer=llvm::MemoryBuffer::getFile(input);if(!buffer)return 2;
 LLVMLifter lifter(Architecture::X86_64,input);if(!lifter.setExecutionMode(ExecutionMode::StaticAnalysis))return 2;
 llvm::SMDiagnostic diagnostic;
 if(llvm::parseAssemblyInto((*buffer)->getMemBufferRef(),&lifter.module(),nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 auto &m=lifter.module();auto *f=m.getFunction("oracle");if(!f)return 2;
 const std::string state=mode=="wrongvalue"?"0x33":"0x2";
 const std::string target=mode=="wrongtarget"?"0x309":"0x2b";
 std::string manifest="{\"hints\":[{\"function\":\"oracle\",\"block\":\"seed\",\"state\":\""+state+"\",\"target\":\""+target+"\"}]}";
 if(mode=="bad")manifest="{bad";
 auto addMD=[&](const char *name,const std::string &value){
  m.getOrInsertNamedMetadata(name)->addOperand(llvm::MDNode::get(m.getContext(),llvm::MDString::get(m.getContext(),value)));
 };
 // Explicit untrusted hints and unrelated metadata retain their identities.
 addMD("dragon.guarded-state-values",manifest);
 addMD("oracle.untouched","native-target-set-sentinel");
 auto *entryMD=m.getNamedMetadata("dragon.guarded-state-values")->getOperand(0);
 auto *sentinelMD=m.getNamedMetadata("oracle.untouched")->getOperand(0);

 if(llvm::verifyModule(m,&llvm::errs()))return 2;
 const auto before=moduleText(m);if(!save(before,output+".parsed-before.ll"))return 2;
 auto *originalModule=&m;auto *originalFunction=f;
 std::vector<llvm::Argument *> arguments;for(auto &a:f->args())arguments.push_back(&a);
 std::vector<std::pair<llvm::GlobalValue *,std::string>> globals;for(auto &g:m.global_values())globals.push_back({&g,text(g)});
 auto attributes=f->getAttributes();auto linkage=f->getLinkage();
 passes::PassPipeline pipeline;passes::Options options;
 if(mode=="budget")options["max_blocks"]=std::int64_t(3);
 if(mode=="zero")options["max_rewrites"]=std::int64_t(0);
 if(mode=="resumed"||mode=="partial")options["max_rewrites"]=std::int64_t(1);
 const bool accepted=pipeline.add("guarded-state-values",options);
 const auto optionError=pipeline.lastError();passes::Report pipelineReport;
 llvm::json::Array rounds;
 if(accepted)for(unsigned round=0;round<(mode=="resumed"?3u:1u);++round){
  pipelineReport=pipeline.run(lifter);auto parsed=llvm::json::parse(pipelineReport.toJSON());if(!parsed)return 2;rounds.push_back(std::move(*parsed));
  if(llvm::verifyModule(m,&llvm::errs()))return 4;
  if(!save(moduleText(m),output+".round"+std::to_string(round)+".ll"))return 2;
 }

 bool identity=&lifter.module()==originalModule && m.getFunction("oracle")==originalFunction && f->getAttributes()==attributes && f->getLinkage()==linkage;
 for(unsigned n=0;n<arguments.size();++n)identity=identity&&f->getArg(n)==arguments[n];
 std::size_t n=0;for(auto &g:m.global_values()){
  if(n>=globals.size()){identity=false;break;}
  identity=identity&& &g==globals[n].first&&(&g==f||text(g)==globals[n].second);++n;
 }
 identity=identity&&n==globals.size();
 const bool metadataPreserved=m.getNamedMetadata("dragon.guarded-state-values")->getNumOperands()==1 &&
  m.getNamedMetadata("dragon.guarded-state-values")->getOperand(0)==entryMD &&
  m.getNamedMetadata("oracle.untouched")->getNumOperands()==1 &&
  m.getNamedMetadata("oracle.untouched")->getOperand(0)==sentinelMD;
 identity=identity&&metadataPreserved;
 const bool verified=!llvm::verifyModule(m,&llvm::errs());const auto after=moduleText(m);
 llvm::json::Object result{{"mode",mode},{"metadata_preserved",metadataPreserved},{"option_accepted",accepted},{"option_error",optionError},{"verified_output",verified},{"identity_preserved",identity},{"unchanged",before==after},{"full_cff_completion_claimed",false}};
 result["rounds"]=std::move(rounds);
 std::string report;llvm::raw_string_ostream reportStream(report);reportStream<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result)))<<"\n";
 if(!save(report,reportPath))return 2;
 if(!verified||!identity)return 4;
 if(!accepted)return before==after?3:4;
 if(!pipelineReport.ok)return 4;
 return save(after,output)?0:2;
}
