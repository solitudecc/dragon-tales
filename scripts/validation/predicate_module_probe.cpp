// Apply the public predicate pass to a frozen complete module, without lifting
// or changing its entry contract, for separate execution/precision validation.
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
using namespace dragon;
using namespace dragon::llvmlifter;
static bool save(const std::string &path, const std::string &text) {
  std::error_code error; llvm::raw_fd_ostream out(path, error);
  if (error) return false;
  out << text; out.close(); return !out.has_error();
}
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const std::string prefix = argv[2];
  for (const auto *suffix : {".before.ll", ".after.ll", ".json"})
    if (llvm::sys::fs::exists(prefix + suffix)) return 2;
  auto input = llvm::MemoryBuffer::getFile(argv[1]);
  if (!input) return 2;
  LLVMLifter lifter{Architecture::X86_64, "predicate-module-probe"};
  if (!lifter.setExecutionMode(ExecutionMode::StaticAnalysis)) return 2;
  llvm::SMDiagnostic error;
  if (llvm::parseAssemblyInto((*input)->getMemBufferRef(), &lifter.module(), nullptr, error)) {
    error.print(argv[0], llvm::errs()); return 2;
  }
  if (llvm::verifyModule(lifter.module(), &llvm::errs())) return 3;
  const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding();
  if (llvm::verifyModule(lifter.module(), &llvm::errs())) return 3;
  const auto after = lifter.ir();
  const PredicateFoldOptions options;
  llvm::json::Object report{{"public_API", true}, {"available", stats.available},
    {"visited", stats.conditionsVisited}, {"folded", stats.conditionsFolded},
    {"queries", stats.solverQueries}, {"unknown", stats.unknownQueries},
    {"unsupported", stats.unsupportedConditions}, {"budget_skipped", stats.budgetSkipped},
    {"timeout_ms", options.timeoutMs}, {"max_queries", options.maxQueries},
    {"max_expression_nodes", options.maxExpressionNodes}, {"resource_limit", options.resourceLimit},
    {"unchanged", before == after}, {"verified", true}, {"native_runs", 0}};
  std::string text; llvm::raw_string_ostream out(text);
  out << llvm::formatv("{0:2}", llvm::json::Value(std::move(report))) << '\n';
  return save(prefix + ".before.ll", before) && save(prefix + ".after.ll", after) &&
         save(prefix + ".json", text) ? 0 : 4;
}
