#pragma once
#include <dragon/lifters/llvm/LLVMLifter.hpp>

namespace dragon::llvmlifter::detail {
PredicateFoldStats foldPredicates(llvm::Module &module,
                                 const PredicateFoldOptions &options);
}
