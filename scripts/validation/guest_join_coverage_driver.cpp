// Actual production GuestSSA adapter. Link only libdragon, never a second LLVM.
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
using namespace dragon;
using namespace dragon::llvmlifter;
static std::string quote(const std::string &s) {
  std::string r="\""; const char *hex="0123456789abcdef";
  for(unsigned char c:s) { if(c=='"'||c=='\\') {r+='\\';r+=char(c);} else if(c<32) {r+="\\u00";r+=hex[c>>4];r+=hex[c&15];} else r+=char(c); }
  return r+'"';
}
static bool save(const llvm::Module &m,const std::string &path) {
 std::error_code ec;llvm::raw_fd_ostream out(path,ec,llvm::sys::fs::OF_Text);
 if(ec)return false;m.print(out,nullptr);out.close();return !out.has_error();
}
int main(int argc,char **argv) {
 if(argc!=3 || llvm::sys::fs::exists(argv[2]) || llvm::sys::fs::exists(std::string(argv[2])+".invalid.ll")) return 2;
 auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)return 2;
 LLVMLifter lifter(Architecture::X86_64,argv[1]);if(!lifter.setExecutionMode(ExecutionMode::StaticAnalysis))return 2;
 llvm::SMDiagnostic diag;
 if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&lifter.module(),nullptr,diag)){diag.print(argv[0],llvm::errs());return 2;}
 std::string error;{llvm::raw_string_ostream out(error);if(llvm::verifyModule(lifter.module(),&out)){llvm::errs()<<error;return 2;}}
 MemoryForwardOptions options;options.runMem2Reg=false;
 const auto stats=dragon::llvmlifter::detail::runGuestMemorySSA(lifter.module(),options);
 bool valid;{llvm::raw_string_ostream out(error);valid=!llvm::verifyModule(lifter.module(),&out);}
 const auto path=std::string(argv[2])+(valid?"":".invalid.ll");
 const bool saved=save(lifter.module(),path);
 llvm::errs()<<"{\"input_verified\":true,\"output_verified\":"<<(valid?"true":"false")
  <<",\"available\":"<<(stats.available?"true":"false")<<",\"complete\":"<<(stats.complete?"true":"false")
  <<",\"changed\":"<<(stats.changed?"true":"false")<<",\"verify_error\":"<<quote(error)<<",\"saved\":"<<(saved?"true":"false");
#define FIELD(f) llvm::errs()<<",\"" #f "\":"<<stats.f
 FIELD(blocksVisited);FIELD(guestLoads);FIELD(guestStores);FIELD(loadsForwarded);FIELD(loadsReused);FIELD(callBarriers);FIELD(aliasBarriers);FIELD(partialOverlaps);FIELD(phisInserted);FIELD(joinBarriers);FIELD(loopBarriers);FIELD(budgetExhausted);FIELD(byteRebuilds);FIELD(leafCallsPreserved);FIELD(summaryCallsPreserved);FIELD(summaryCallsRejected);
#undef FIELD
 llvm::errs()<<"}\n";
 return !saved?2:valid?0:4;
}
