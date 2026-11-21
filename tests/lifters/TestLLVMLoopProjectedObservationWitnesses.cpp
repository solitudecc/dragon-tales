#include "LoopProjectedObservationFixture.hpp"
TEST_CASE("Expanded explicit low byte witnesses cross preserving corridors",
          "[llvm][image-loop-projected-witness]") {
 for(auto mode:{ProjectionFixture::Disjoint,ProjectionFixture::High32Only}) DYNAMIC_SECTION("mode "<<mode) {
  ProjectionFixture f(mode,true);auto *fn=f.module->getFunction("view");auto *arg=fn->getArg(0);
  auto r=runLoopImageTableContext(*f.module,f.request(),LoopImageProofProfile::Expanded);INFO(r.message);
  CHECK(r.closed);CHECK(r.changed);CHECK_FALSE(r.budgetExhausted);CHECK(r.work<=f.request().maxWork);
  CHECK(f.module->getFunction("view")==fn);CHECK(fn->getArg(0)==arg);
  CHECK(f.ir().find("projection.fresh = load i64")!=std::string::npos);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
TEST_CASE("Explicit witnesses reject missing observations and intermediate clobbers",
          "[llvm][image-loop-projected-witness]") {
 for(auto mode:{ProjectionFixture::Low32Clobber,ProjectionFixture::UnknownEffect,
    ProjectionFixture::AtomicEffect,ProjectionFixture::NonDominating,
    ProjectionFixture::OrphanArm,ProjectionFixture::OtherObservation,
    ProjectionFixture::BigEndian,ProjectionFixture::TemporalGuard}) DYNAMIC_SECTION("mode "<<mode) {
  ProjectionFixture f(mode,true);auto before=f.ir();auto *fn=f.module->getFunction("view");auto *arg=fn->getArg(0);auto *entry=&fn->getEntryBlock();
  auto r=runLoopImageTableContext(*f.module,f.request(),LoopImageProofProfile::Expanded);INFO(r.message);
  CHECK_FALSE(r.closed);CHECK_FALSE(r.changed);CHECK(f.ir()==before);
  CHECK(f.module->getFunction("view")==fn);CHECK(fn->getArg(0)==arg);CHECK(&fn->getEntryBlock()==entry);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
TEST_CASE("Default profile retains explicit witness observation refusal",
          "[llvm][image-loop-projected-witness]") {
 ProjectionFixture f(ProjectionFixture::Disjoint,true);auto before=f.ir();
 auto r=runLoopImageTableContext(*f.module,f.request());INFO(r.message);
 CHECK_FALSE(r.closed);CHECK_FALSE(r.changed);CHECK(f.ir()==before);
 REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
}
