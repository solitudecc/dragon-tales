#pragma once
#include <dragon/disassembler/Graph.hpp>
#include <string>
namespace llvm { class Module; class Function; }
namespace dragon::detail {
// Internal loader-to-lifter bridge. Lives in the existing Graph pimpl; it adds
// no public methods, object fields, execution ABI or callee symbol names.
struct GraphLLVMMetadata {
  static void setImports(Graph&,const std::map<std::uint64_t,std::string>&);
  static const std::map<std::uint64_t,std::string>& imports(const Graph&);
};
void recordLLVMImports(llvm::Module&,llvm::Function&,const Function&,const Graph&);
}
