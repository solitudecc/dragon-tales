#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <memory>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  const std::filesystem::path output(argv[2]);
  for (unsigned padding : {0u, 1u, 7u, 31u, 127u, 257u, 511u, 1023u}) {
    // Host allocation layout only: each run parses exactly the same IR bytes.
    std::vector<std::unique_ptr<char[]>> allocations;
    for (unsigned i = 0; i < padding; ++i)
      allocations.push_back(std::make_unique<char[]>(sizeof(llvm::BasicBlock)));
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto module = llvm::parseIRFile(argv[1], error, context);
    if (!module || llvm::verifyModule(*module, &llvm::errs())) return 3;
    auto dump = [&](const char *stage) {
      std::error_code error;
      llvm::raw_fd_ostream out((output / (std::to_string(padding) + "-" + stage + ".ll")).string(), error);
      if (error) std::exit(4);
      module->print(out, nullptr);
    };
    dump("before");
    dragon::llvmlifter::MemoryForwardOptions options;
    options.runMem2Reg = false;
    const auto s = dragon::llvmlifter::detail::runGuestMemorySSA(*module, options);
    if (llvm::verifyModule(*module, &llvm::errs())) return 5;
    dump("after");
    llvm::outs() << "padding=" << padding << " available=" << s.available
      << " complete=" << s.complete << " changed=" << s.changed
      << " blocks=" << s.blocksVisited << " loads=" << s.guestLoads
      << " stores=" << s.guestStores << " forwarded=" << s.loadsForwarded
      << " reused=" << s.loadsReused << " calls=" << s.callBarriers
      << " aliases=" << s.aliasBarriers << " partial=" << s.partialOverlaps
      << " phis=" << s.phisInserted << " joins=" << s.joinBarriers
      << " loops=" << s.loopBarriers << " budget=" << s.budgetExhausted
      << " bytes=" << s.byteRebuilds << " leaf=" << s.leafCallsPreserved
      << " summary_preserved=" << s.summaryCallsPreserved
      << " summary_rejected=" << s.summaryCallsRejected << '\n';
  }
}
