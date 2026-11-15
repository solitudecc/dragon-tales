#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
static std::string text(llvm::Function &f){std::string s;llvm::raw_string_ostream out(s);f.print(out);return s;}
int main(int argc,char **argv){
 if(argc!=5)return 2;
 llvm::LLVMContext context;llvm::SMDiagnostic error;
 auto module=llvm::parseAssemblyFile(argv[1],error,context);
 if(!module){error.print(argv[0],llvm::errs());return 3;}
 if(llvm::verifyModule(*module,&llvm::errs()))return 4;
 auto *view=module->getFunction("view");if(!view||view->isDeclaration())return 5;
 auto *ordinary=module->getFunction("ordinary");auto *unknown=module->getFunction("unknown_call");
 if(!ordinary||!unknown)return 5;
 const auto ordinaryText=text(*ordinary),unknownText=text(*unknown);
 std::vector<llvm::Argument*>args;for(auto &arg:view->args())args.push_back(&arg);
 std::error_code ec;llvm::raw_fd_ostream before(argv[2],ec);if(ec)return 6;module->print(before,nullptr);before.close();
 dragon::llvmlifter::detail::ImageTableContextRequest request{"view",8192,{4352,4608}};
 auto result=dragon::llvmlifter::detail::runLoopImageTableContext(*module,request);
 const bool verified=!llvm::verifyModule(*module,&llvm::errs());
 bool identity=module->getFunction("view")==view&&module->getFunction("ordinary")==ordinary&&module->getFunction("unknown_call")==unknown;
 for(unsigned n=0;n<args.size();++n)identity&=view->getArg(n)==args[n];
 bool outside=text(*ordinary)==ordinaryText&&text(*unknown)==unknownText;
 llvm::json::Array runtimeMaps,solverMaps;std::ifstream maps("/proc/self/maps");std::string line;
 while(std::getline(maps,line)){
  if(line.find("libdragon.so")!=std::string::npos)runtimeMaps.push_back(line);
  if(line.find("libz3.so")!=std::string::npos)solverMaps.push_back(line);
  if(line.find("libLLVM")!=std::string::npos)return 7;
 }
 llvm::json::Object report{{"api_calls",1},{"closed",result.closed},{"changed",result.changed},{"budget_exhausted",result.budgetExhausted},{"work",static_cast<int64_t>(result.work)},{"message",result.message},{"verified_before",true},{"verified_after",verified},{"identity",identity},{"outside_unchanged",outside},{"runtime_maps",std::move(runtimeMaps)},{"solver_maps",std::move(solverMaps)}};
 llvm::raw_fd_ostream json(argv[4],ec);if(ec)return 6;json<<llvm::formatv("{0:2}\n",llvm::json::Value(std::move(report)));json.close();
 if(!result.closed||!result.changed||result.budgetExhausted||!verified||!identity||!outside)return 8;
 llvm::raw_fd_ostream after(argv[3],ec);if(ec)return 6;module->print(after,nullptr);return 0;
}
