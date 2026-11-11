// Measurement-only cleanup of a saved IR copy. Never used as analysis input.
// Only constant-integer arithmetic, casts, comparisons and selects are folded.
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  llvm::LLVMContext context;
  llvm::SMDiagnostic error;
  auto module = llvm::parseIRFile(argv[1], error, context);
  if (!module) { error.print(argv[0], llvm::errs()); return 3; }
  if (llvm::verifyModule(*module, &llvm::errs())) return 4;
  unsigned folded = 0;
  for (unsigned round = 0; round != 8; ++round) {
    unsigned changed = 0;
    for (auto &function : *module)
      for (auto &block : function)
        for (auto it = block.begin(); it != block.end();) {
          auto &inst = *it++;
          if (!llvm::isa<llvm::BinaryOperator, llvm::CastInst,
                         llvm::ICmpInst, llvm::SelectInst>(inst)) continue;
          bool integers = true;
          for (auto &operand : inst.operands())
            integers &= llvm::isa<llvm::ConstantInt>(operand.get());
          if (!integers) continue;
          auto *value = llvm::ConstantFoldInstruction(&inst,
                                                     module->getDataLayout());
          if (!llvm::isa_and_nonnull<llvm::ConstantInt>(value)) continue;
          inst.replaceAllUsesWith(value);
          inst.eraseFromParent();
          ++changed;
        }
    folded += changed;
    if (!changed) break;
  }
  if (llvm::verifyModule(*module, &llvm::errs())) return 5;
  std::error_code ec;
  llvm::raw_fd_ostream out(argv[2], ec, llvm::sys::fs::OF_Text);
  if (ec) return 6;
  module->print(out, nullptr);
  llvm::outs() << "constant_integer_instructions_folded=" << folded << '\n';
}
