// Standalone prototype consumer. Predictions never certify Guest contents.
#include "GuardedStateCorridors.hpp"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
using namespace dragon::llvmlifter::detail;
static bool save(const std::string &path,const std::string &value) {
  std::error_code ec;llvm::raw_fd_ostream out(path,ec);if(ec)return false;
  out<<value;out.close();return !out.has_error();
}
static std::string moduleText(llvm::Module &m) {
  std::string out;llvm::raw_string_ostream stream(out);m.print(stream,nullptr);return out;
}
int main(int argc,char **argv) {
  if(argc!=5)return 2;
  for(unsigned i=3;i<5;++i)if(llvm::sys::fs::exists(argv[i]))return 2;
  llvm::LLVMContext context;llvm::SMDiagnostic error;
  auto m=llvm::parseAssemblyFile(argv[1],error,context);
  if(!m){error.print(argv[0],llvm::errs());return 2;}
  auto hints=llvm::MemoryBuffer::getFile(argv[2]);if(!hints)return 2;
  if(m->getNamedMetadata("dragon.guard-predictions"))return 2;
  m->getOrInsertNamedMetadata("dragon.guard-predictions")->addOperand(
    llvm::MDNode::get(context,llvm::MDString::get(context,(*hints)->getBuffer())));
  if(llvm::verifyModule(*m,&llvm::errs()))return 2;
  const auto before=moduleText(*m);
  if(!save(std::string(argv[3])+".parsed-before.ll",before))return 2;
  // Freeze the initial prototype budgets before any fixture or real pass run.
  const GuardedTransitionsOptions options{64,32,128,16384};
  const auto checked=runGuardedStoreExpressions(*m,options);
  const auto &partial=checked.partial;const auto &s=partial.base;
  bool verified=!llvm::verifyModule(*m,&llvm::errs());const auto after=moduleText(*m);
  llvm::json::Object result{{"available",s.available},{"complete",s.complete},
    {"changed",s.changed},{"verified",verified},{"unchanged",before==after},
    {"message",s.message},{"candidate_edges",s.candidateEdges},
    {"attempted_edges",s.attemptedEdges},{"rewritten_edges",s.rewrittenEdges},
    {"unresolved_edges",s.unresolvedEdges},{"rejected",s.rejected},
    {"budget_exhausted",s.budgetExhausted},{"cloned_blocks",s.clonedBlocks},
    {"cloned_instructions",s.clonedInstructions},
    {"loop_contexts",checked.loopContexts},{"loop_backedges",checked.loopBackedges},
    {"partial_candidate_edges",partial.partialCandidateEdges},
    {"full_cff_completion_claimed",false}};
  std::string report;llvm::raw_string_ostream stream(report);
  stream<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result)))<<"\n";
  if(!save(argv[4],report)||!save(argv[3],after))return 2;
  return verified?0:4;
}
