#include <dragon/lifters/llvm/LLVMDeflattening.hpp>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include "same_function_select_fixtures.hpp"

#include <cstdlib>

namespace {
void dump(const llvm::Module &module, const std::string &path) {
  std::error_code error;
  llvm::raw_fd_ostream stream(path, error, llvm::sys::fs::OF_Text);
  if (error) std::exit(2);
  module.print(stream, nullptr);
}
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  llvm::LLVMContext context;
  llvm::Module module("same-function-select-native", context);
  for (const auto &config : dragon::validation::select_fixture::nativeConfigs())
    dragon::validation::select_fixture::build(module, config);
  dragon::validation::select_fixture::buildLoop(module);
  if (llvm::verifyModule(module, &llvm::errs())) return 3;
  dump(module, std::string(argv[1]) + "/before.ll");
  const auto stats = dragon::llvmlifter::runProvableDeflattening(module);
  const bool invalid = llvm::verifyModule(module, &llvm::errs());
  dump(module, std::string(argv[1]) + "/after.ll");
  llvm::outs() << "visited=" << stats.dispatchersVisited
               << " rewritten=" << stats.dispatchersRewritten
               << " edges=" << stats.edgesRewritten
               << " unresolved=" << stats.unresolvedDispatchers
               << " cross=" << stats.crossCallDispatchersRewritten
               << " invalid=" << invalid << '\n';
  return invalid ? 3 : 0;
}
