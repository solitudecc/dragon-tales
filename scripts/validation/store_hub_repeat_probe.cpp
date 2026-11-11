#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <string>

void dump(const llvm::Module &module, const std::filesystem::path &path) {
  std::error_code error;
  llvm::raw_fd_ostream out(path.string(), error, llvm::sys::fs::OF_Text);
  if (error) std::exit(2);
  module.print(out, nullptr);
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  llvm::LLVMContext context;
  llvm::SMDiagnostic error;
  auto module = llvm::parseIRFile(argv[1], error, context);
  if (!module) {error.print(argv[0], llvm::errs()); return 3;}
  if (llvm::verifyModule(*module, &llvm::errs())) return 4;
  const std::filesystem::path out(argv[2]);
  for (unsigned round = 1; round != 4; ++round) {
    dragon::llvmlifter::MemoryForwardOptions options;
    options.runMem2Reg = false;
    const auto memory = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    if (llvm::verifyModule(*module, &llvm::errs())) return 5;
    const auto state = dragon::llvmlifter::runDispatcherSpecialization(*module, {}, true);
    if (llvm::verifyModule(*module, &llvm::errs())) return 6;
    dump(*module, out / ("round-" + std::to_string(round) + ".ll"));
    llvm::outs() << "round=" << round
        << " memory_forwarded=" << memory.loadsForwarded
        << " memory_reused=" << memory.loadsReused
        << " memory_blocks=" << memory.blocksVisited
        << " memory_complete=" << memory.complete
        << " memory_budget=" << memory.budgetExhausted
        << " memory_alias=" << memory.aliasBarriers
        << " memory_loop=" << memory.loopBarriers
        << " memory_join=" << memory.joinBarriers
        << " state_branches=" << state.branchesRewritten
        << " state_switches=" << state.switchesRewritten
        << " state_complete=" << state.complete
        << " state_budget=" << state.budgetExhausted << '\n';
  }
}
