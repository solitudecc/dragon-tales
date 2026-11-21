#include "LoopRangeArithmeticFixture.hpp"
// New explicit profile API is intentionally separate from the plain same-API
// old-SDK regression TU. Missing profile API is not an old execution error.
TEST_CASE("Expanded loop masks close wide low ones without global memory facts",
          "[llvm][image-loop-range-arithmetic-expanded]") {
 ArithmeticFixture f(ArithmeticFixture::WideLowOnes);
 auto *view=f.module->getFunction("view");auto *arg=view->getArg(0);
 auto result=runLoopImageTableContext(*f.module,f.request(),LoopImageProofProfile::Expanded);
 INFO(result.message);CHECK(result.closed);CHECK(result.changed);CHECK_FALSE(result.budgetExhausted);
 CHECK(result.work<=f.request().maxWork);CHECK(f.module->getFunction("view")==view);CHECK(view->getArg(0)==arg);
 CHECK(f.ir().find("call void @ordinary")!=std::string::npos);
 REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
}
TEST_CASE("Both loop profiles retain low ones history cycle compatibility",
          "[llvm][image-loop-range-arithmetic-expanded]") {
 for(auto profile:{LoopImageProofProfile::Default,LoopImageProofProfile::Expanded}) {
  ArithmeticFixture f(ArithmeticFixture::LowOnesSelf);
  auto result=runLoopImageTableContext(*f.module,f.request(),profile);
  INFO(result.message);CHECK(result.closed);CHECK(result.changed);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.work<=f.request().maxWork);REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
TEST_CASE("Expanded sparse and low ones range retries preserve rejection rollback",
          "[llvm][image-loop-range-arithmetic-expanded]") {
 for(auto mode:{ArithmeticFixture::ActualCellOverlap,ArithmeticFixture::PartialCellOverlap,
     ArithmeticFixture::WrappedAddress,ArithmeticFixture::UnknownCall,
     ArithmeticFixture::IndependentFreeze,ArithmeticFixture::FlaggedUnknown}) {
  ArithmeticFixture f(mode);auto before=f.ir();auto *view=f.module->getFunction("view");auto *arg=view->getArg(0);
  auto result=runLoopImageTableContext(*f.module,f.request(),LoopImageProofProfile::Expanded);
  INFO(result.message);CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(f.ir()==before);
  CHECK(f.module->getFunction("view")==view);CHECK(view->getArg(0)==arg);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
