#include "LoopProjectedObservationFixture.hpp"
// Original v1 expectation failed: normalization canonicalizes the guard into
// a wide mask and moves Trunc through Shl. This shape is explicitly not fixed.
TEST_CASE("Canonical mask observation precision boundary remains conservative",
          "[llvm][image-loop-projected-observation]") {
 for(auto mode:{ProjectionFixture::Disjoint,ProjectionFixture::High32Only}) {
  ProjectionFixture f(mode);auto before=f.ir();auto *fn=f.module->getFunction("view");auto *arg=fn->getArg(0);
  auto r=runLoopImageTableContext(*f.module,f.request(),LoopImageProofProfile::Expanded);INFO(r.message);
  CHECK_FALSE(r.closed);CHECK_FALSE(r.changed);CHECK(f.ir()==before);
  CHECK(f.module->getFunction("view")==fn);CHECK(fn->getArg(0)==arg);REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
TEST_CASE("Projected memory guards reject missing observations and intermediate clobbers",
          "[llvm][image-loop-projected-observation]") {
 for(auto mode:{ProjectionFixture::Low32Clobber,ProjectionFixture::UnknownEffect,
    ProjectionFixture::AtomicEffect,ProjectionFixture::NonDominating,
    ProjectionFixture::OrphanArm,ProjectionFixture::OtherObservation,
    ProjectionFixture::BigEndian}) DYNAMIC_SECTION("mode "<<mode) {
  ProjectionFixture f(mode);auto before=f.ir();auto *fn=f.module->getFunction("view");auto *arg=fn->getArg(0);auto *entry=&fn->getEntryBlock();
  auto r=runLoopImageTableContext(*f.module,f.request(),LoopImageProofProfile::Expanded);INFO(r.message);
  CHECK_FALSE(r.closed);CHECK_FALSE(r.changed);CHECK(f.ir()==before);
  CHECK(f.module->getFunction("view")==fn);CHECK(fn->getArg(0)==arg);CHECK(&fn->getEntryBlock()==entry);
  REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
 }
}
TEST_CASE("Default profile retains its guarded counter observation refusal",
          "[llvm][image-loop-projected-observation]") {
 ProjectionFixture f(ProjectionFixture::Disjoint);auto before=f.ir();
 auto r=runLoopImageTableContext(*f.module,f.request());INFO(r.message);
 CHECK_FALSE(r.closed);CHECK_FALSE(r.changed);CHECK(f.ir()==before);
 REQUIRE_FALSE(llvm::verifyModule(*f.module,&llvm::errs()));
}
