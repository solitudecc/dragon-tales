#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  llvm::LLVMContext context;
  llvm::SMDiagnostic error;
  auto module = llvm::parseIRFile(argv[1], error, context);
  if (!module) {error.print(argv[0], llvm::errs()); return 3;}
  if (llvm::verifyModule(*module, &llvm::errs())) return 4;
  dragon::llvmlifter::MemoryForwardOptions options;
  options.runMem2Reg = false;
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
  if (llvm::verifyModule(*module, &llvm::errs())) return 5;
  std::error_code ec;
  llvm::raw_fd_ostream out(argv[2], ec, llvm::sys::fs::OF_Text);
  if (ec) return 6;
  module->print(out, nullptr);
  llvm::outs() << "forwarded=" << stats.loadsForwarded << " reused=" << stats.loadsReused
      << " blocks=" << stats.blocksVisited << " loads=" << stats.guestLoads << " stores=" << stats.guestStores
      << " complete=" << stats.complete << " budget=" << stats.budgetExhausted
      << " alias=" << stats.aliasBarriers << " loop=" << stats.loopBarriers
      << " join=" << stats.joinBarriers << " phis=" << stats.phisInserted << '\n';
}
