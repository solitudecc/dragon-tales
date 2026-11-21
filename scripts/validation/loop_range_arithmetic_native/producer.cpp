#ifndef BEFORE_ONLY
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#endif
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
#include <map>
#include <string>
#include <vector>
static std::string text(llvm::Function &f){std::string s;llvm::raw_string_ostream o(s);f.print(o);return s;}
int main(int argc,char **argv){
 if(argc!=5)return 2;llvm::LLVMContext c;llvm::SMDiagnostic e;auto m=llvm::parseAssemblyFile(argv[1],e,c);
 if(!m){e.print(argv[0],llvm::errs());return 3;}if(llvm::verifyModule(*m,&llvm::errs()))return 4;
 auto*f=m->getFunction("view");if(!f||f->isDeclaration())return 5;
 std::vector<llvm::Argument*>args;for(auto&a:f->args())args.push_back(&a);
 std::map<llvm::Function*,std::string>outside;for(auto&fn:*m)if(&fn!=f)outside[&fn]=text(fn);
 auto fnCount=m->size();auto globalCount=m->global_size();auto mdCount=m->named_metadata_size();
 bool closed=false,changed=false,budget=false;std::size_t work=0;std::string message;int api=0;
#ifndef BEFORE_ONLY
 dragon::llvmlifter::detail::ImageTableContextRequest q{"view",8192,{4352,4608}};
 auto r=std::string(argv[4])=="wide64" ? dragon::llvmlifter::detail::runLoopImageTableContext(*m,q,dragon::llvmlifter::detail::LoopImageProofProfile::Expanded) : dragon::llvmlifter::detail::runLoopImageTableContext(*m,q);
 closed=r.closed;changed=r.changed;budget=r.budgetExhausted;work=r.work;message=r.message;api=1;
#endif
 bool verified=!llvm::verifyModule(*m,&llvm::errs());bool identity=m->getFunction("view")==f&&m->size()==fnCount&&m->global_size()==globalCount&&m->named_metadata_size()==mdCount;
 for(unsigned i=0;i<args.size();++i)identity&=f->getArg(i)==args[i];for(auto&[fn,s]:outside)identity&=fn->getParent()==m.get()&&text(*fn)==s;
 llvm::json::Array runtime,solver;std::ifstream maps("/proc/self/maps");std::string line;
 while(std::getline(maps,line)){if(line.find("libdragon.so")!=std::string::npos)runtime.push_back(line);if(line.find("libz3.so")!=std::string::npos)solver.push_back(line);if(line.find("libLLVM")!=std::string::npos)return 7;}
 llvm::json::Object report{{"api_calls",api},{"closed",closed},{"changed",changed},{"budget_exhausted",budget},{"work",static_cast<int64_t>(work)},{"message",message},{"verified",verified},{"identity_outside_exact",identity},{"runtime_maps",std::move(runtime)},{"solver_maps",std::move(solver)}};
 std::error_code ec;llvm::raw_fd_ostream out(argv[3],ec);if(ec)return 6;out<<llvm::formatv("{0:2}\n",llvm::json::Value(std::move(report)));out.close();
 if(!verified||!identity)return 8;
#ifndef BEFORE_ONLY
 if(!closed||!changed||budget)return 9;
#endif
 llvm::raw_fd_ostream ir(argv[2],ec);if(ec)return 6;m->print(ir,nullptr);return 0;
}
