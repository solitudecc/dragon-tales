// Read-only provenance check: exact archived raw -> parser + hint metadata.
// This performs no optimization, pass run, semantic comparison or normalization.
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
int main(int argc, char **argv) {
  if (argc != 5) return 2;
  auto raw = llvm::MemoryBuffer::getFile(argv[1]);
  auto hints = llvm::MemoryBuffer::getFile(argv[2]);
  auto before = llvm::MemoryBuffer::getFile(argv[3]);
  if (!raw || !hints || !before) return 2;
  llvm::LLVMContext context; llvm::SMDiagnostic error;
  // parseAssemblyFile's original input identifier is explicit provenance. It
  // must come from the recorded probe invocation, not from a guessed filename.
  auto module = llvm::parseAssembly(llvm::MemoryBufferRef((*raw)->getBuffer(), argv[4]), error, context);
  if (!module) { error.print(argv[0], llvm::errs()); return 2; }
  if (module->getNamedMetadata("dragon.guard-predictions")) return 2;
  module->getOrInsertNamedMetadata("dragon.guard-predictions")->addOperand(
      llvm::MDNode::get(context, llvm::MDString::get(context, (*hints)->getBuffer())));
  if (llvm::verifyModule(*module, &llvm::errs())) return 2;
  std::string text; llvm::raw_string_ostream stream(text); module->print(stream, nullptr);
  if (llvm::StringRef(text) != (*before)->getBuffer()) { llvm::errs() << "parser + hint bytes differ from frozen parsed-before\n"; return 3; }
  llvm::outs() << "exact parser + hint binding matched\n";
  return 0;
}
