#pragma once

#include <dragon/lifters/llvm/ImageTableContext.hpp>

#ifdef DRAGON_IMAGE_TABLE_CONTEXT_TESTING
namespace dragon::llvmlifter::detail {
// Validation-only forced rejection after staging (1) or body publication (2).
ImageTableContextStats runImageTableContextCommitFailureForTest(
    llvm::Module &, const ImageTableContextRequest &, bool consumeClosedCFG,
    unsigned failureStage);
} // namespace dragon::llvmlifter::detail
#endif
