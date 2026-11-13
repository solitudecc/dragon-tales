// Independent LLVM-only definedness evidence. No Dragon linkage or native undef execution.
#include <llvm/AsmParser/Parser.h>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/KnownBits.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cassert>
#include <cstdint>
static constexpr const char *IR=R"(
target datalayout = "e-p:64:64"
define i64 @source() {
entry:
 %p = alloca i64, align 8
 %raw = load i64, ptr %p, align 8
 %state = and i64 %raw, 3
 %original = icmp ugt i64 %state, 50
 br i1 %original, label %bad, label %good
bad:
 ret i64 9
good:
 ret i64 7
}
define i64 @unsafe_extra_branch() {
entry:
 %p = alloca i64, align 8
 %raw = load i64, ptr %p, align 8
 %state = and i64 %raw, 3
 %original = icmp ugt i64 %state, 50
 %extra = icmp eq i64 %state, 2
 br i1 %extra, label %fast, label %fallback
fast:
 br label %good
fallback:
 br i1 %original, label %bad, label %good
bad:
 ret i64 9
good:
 ret i64 7
}
define i64 @freeze_extra_condition() {
entry:
 %p = alloca i64, align 8
 %raw = load i64, ptr %p, align 8
 %state = and i64 %raw, 3
 %original = icmp ugt i64 %state, 50
 %extra = icmp eq i64 %state, 2
 %extra.defined = freeze i1 %extra
 br i1 %extra.defined, label %fast, label %fallback
fast:
 br label %good
fallback:
 br i1 %original, label %bad, label %good
bad:
 ret i64 9
good:
 ret i64 7
}
)";
int main(int argc,char **argv){
 llvm::LLVMContext context;llvm::SMDiagnostic diagnostic;auto module=llvm::parseAssemblyString(IR,diagnostic,context);if(!module){diagnostic.print(argv[0],llvm::errs());return 2;}if(llvm::verifyModule(*module,&llvm::errs()))return 2;
 if(argc==2){std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);if(ec)return 2;module->print(out,nullptr);}
 llvm::json::Array rows;
 for(auto &function:*module){llvm::Instruction *state=nullptr,*original=nullptr,*extra=nullptr,*frozen=nullptr;for(auto &block:function)for(auto &i:block){if(i.getName()=="state")state=&i;if(i.getName()=="original")original=&i;if(i.getName()=="extra")extra=&i;if(i.getName()=="extra.defined")frozen=&i;}
  assert(state&&original);llvm::SimplifyQuery query(module->getDataLayout());query.CanUseUndef=false;auto *simplified=llvm::dyn_cast_or_null<llvm::ConstantInt>(llvm::simplifyInstruction(original,query));assert(simplified&&simplified->isZero());auto known=llvm::computeKnownBits(state,module->getDataLayout());assert(known.Zero.getZExtValue()==~std::uint64_t(3));
  auto *branch=llvm::cast<llvm::BranchInst>(function.getEntryBlock().getTerminator());auto *condition=branch->getCondition();bool branchDefined=llvm::isGuaranteedNotToBeUndefOrPoison(condition);bool eqDefined=extra&&llvm::isGuaranteedNotToBeUndefOrPoison(extra);bool beforeUse=false;
  if(extra){branch->setCondition(original);beforeUse=llvm::isGuaranteedNotToBeUndefOrPoison(extra);branch->setCondition(condition);assert(!beforeUse);}
  // Querying an already-UB-consuming use can assume defined execution. This is
  // NOT a certificate that introducing that use was a refinement.
  if(function.getName()=="unsafe_extra_branch")assert(branchDefined&&eqDefined&&!beforeUse);
  if(function.getName()=="freeze_extra_condition")assert(branchDefined&&!eqDefined&&!beforeUse&&frozen&&condition==frozen&&frozen->getOperand(0)==extra);
  rows.push_back(llvm::json::Object{{"function",function.getName().str()},{"source_cmp_simplifies_false_without_using_undef",true},{"known_zero_upper62",true},{"entry_branch_condition_guaranteed_defined",branchDefined},{"raw_extra_EQ_guaranteed_defined_after_branch_attachment",eqDefined},{"raw_extra_EQ_guaranteed_defined_without_added_branch_use",beforeUse},{"state_guaranteed_defined",llvm::isGuaranteedNotToBeUndefOrPoison(state)},{"counterexample_source_return",7}});
 }
 llvm::outs()<<llvm::formatv("{0:2}\n",llvm::json::Value(llvm::json::Object{{"rows",std::move(rows)},{"proof_scope","LLVM21 definedness/knownbits diagnostics plus LangRef; not Alive2 or whole-pass proof"},{"native_undef_executions",0}}));return 0;
}
