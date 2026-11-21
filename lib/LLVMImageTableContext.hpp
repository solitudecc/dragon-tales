#pragma once

#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <string_view>

namespace dragon::llvmlifter::detail {
// Independent opt-in consumer of actual guarded-entry byte equalities.
// Every original continuing write must preserve the checked byte span before
// any reload uses can become constants. No section permissions or metadata
// certificate supplies facts. Rejection leaves the public module unchanged.
// Like loop-image-context, the larger Expanded range/work ceiling is opt-in:
// maxWork must not exceed loopImageMaxWork(profile). Unknown profiles refuse.
ImageTableContextStats propagateGuardedMemory(
    llvm::Module &, std::string_view viewSymbol,
    std::size_t maxWork=loopImageMaxWork(LoopImageProofProfile::Default),
    LoopImageProofProfile profile=LoopImageProofProfile::Default);
}

#ifdef DRAGON_IMAGE_TABLE_CONTEXT_TESTING
namespace dragon::llvmlifter::detail {
// Validation-only forced rejection after staging (1) or body publication (2).
ImageTableContextStats runImageTableContextCommitFailureForTest(
    llvm::Module &, const ImageTableContextRequest &, bool consumeClosedCFG,
    unsigned failureStage);
} // namespace dragon::llvmlifter::detail
#endif
