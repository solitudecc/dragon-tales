#pragma once
#include <dragon/lifters/llvm/LLVMLifter.hpp>

namespace dragon::llvmlifter::detail {
MBASimplifyStats simplifyMBA(llvm::Module &, const MBASimplifyOptions &);
}
