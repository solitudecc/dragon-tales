// Independent pure-LLVM binder for the original D37 generic inventory schema.
#define main frozen_business_inventory_main
#include "guarded_business_inventory.cpp"
#undef main
int main(int argc,char **argv) { try {
  if(argc!=4)return 2;
  auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)return 2;
  llvm::LLVMContext context;llvm::Module module("D37-anchor-reference",context);llvm::SMDiagnostic diagnostic;
  if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&module,nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 3;}
  std::string error;if(!verify(module,error)){llvm::errs()<<error;return 4;}
  if(!module.getFunction(argv[2]))return 5;
  write(argv[3],json(inventory(module,argv[2])));return 0;
} catch(const std::exception &e) { llvm::errs()<<e.what();return 9; } }
