#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <string>
int main(int argc,char **argv){
 if(argc!=4)return 2;
 dragon::llvmlifter::LLVMLifter lifter(dragon::Architecture::X86_64);
 auto buffer=llvm::MemoryBuffer::getFile(argv[1]);if(!buffer)return 2;
 llvm::SMDiagnostic diagnostic;auto &m=lifter.module();
 if(llvm::parseAssemblyInto((*buffer)->getMemBufferRef(),&m,nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 if(llvm::verifyModule(m,&llvm::errs()))return 2;
 auto *function=m.getFunction("oracle");if(!function)return 2;auto *arg=function->getArg(0);
 dragon::passes::PassPipeline pipeline;
 if(!pipeline.add("invariant-self-loops")){std::ofstream(argv[3])<<"{\"accepted\":false,\"error\":\"missing builtin\"}\n";return 3;}
 auto report=pipeline.run(lifter);std::ofstream(argv[3])<<report.toJSON()<<'\n';
 if(!report.ok || report.runs.size()!=1 || !report.runs[0].result.changed || report.runs[0].result.counters.at("isolated_loops")!=1 || m.getFunction("oracle")!=function || function->getArg(0)!=arg || llvm::verifyModule(m,&llvm::errs()))return 4;
 std::string text;llvm::raw_string_ostream out(text);m.print(out,nullptr);std::ofstream(argv[2])<<text;return 0;
}
