#include "LoopRangeArithmeticFixture.hpp"
TEST_CASE("Loop range arithmetic closes bounded products and sparse masks",
          "[llvm][image-loop-range-arithmetic]") {
 for(auto mode:{ArithmeticFixture::Sparse32,ArithmeticFixture::Sparse64,
    ArithmeticFixture::SafeFlags32,ArithmeticFixture::SafeFlags64,
    ArithmeticFixture::ZeroUnknown,ArithmeticFixture::LowOnesSelf}) DYNAMIC_SECTION("mode "<<mode) {
  ArithmeticFixture f(mode);auto *view=f.module->getFunction("view");auto *arg=view->getArg(0);
  auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
  CHECK(result.closed);CHECK(result.changed);CHECK_FALSE(result.budgetExhausted);
  CHECK(result.work<=f.request().maxWork);CHECK(f.module->getFunction("view")==view);CHECK(view->getArg(0)==arg);
  CHECK(f.ir().find("call void @ordinary")!=std::string::npos);
  CHECK(f.ir().find("dragon.loop.image.private")==std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
TEST_CASE("Loop range products never infer nooverflow correlation or disjointness",
          "[llvm][image-loop-range-arithmetic]") {
 for(auto mode:{ArithmeticFixture::UnknownX,ArithmeticFixture::Overflow,
    ArithmeticFixture::ActualCellOverlap,ArithmeticFixture::PartialCellOverlap,
    ArithmeticFixture::WrappedAddress,ArithmeticFixture::UnknownCall,
    ArithmeticFixture::Poison,ArithmeticFixture::Undef,
    ArithmeticFixture::IndependentFreeze,ArithmeticFixture::FlaggedUnknown}) DYNAMIC_SECTION("mode "<<mode) {
  ArithmeticFixture f(mode);auto before=f.ir();auto *view=f.module->getFunction("view");
  auto *entry=&view->getEntryBlock();auto *arg=view->getArg(0);
  auto result=runLoopImageTableContext(*f.module,f.request());INFO(result.message);
  CHECK_FALSE(result.closed);CHECK_FALSE(result.changed);CHECK(f.ir()==before);
  CHECK(f.module->getFunction("view")==view);CHECK(&view->getEntryBlock()==entry);CHECK(view->getArg(0)==arg);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
