#include <catch2/catch_test_macros.hpp>
#include "../c_shim/dragon_c.h"
#include <memory>
#include <string>

namespace {
std::string takeLoopReport(char *raw) {
  REQUIRE(raw);std::unique_ptr<char,decltype(&dragon_cstr_free)> owner(raw,dragon_cstr_free);
  return owner.get();
}
}

TEST_CASE("C pipeline accepts full uint64 loop candidates without ABI additions",
          "[c-api][loop-pipeline]") {
  std::unique_ptr<dragon_pass_pipeline_t,decltype(&dragon_pass_pipeline_destroy)> pipeline(
      dragon_pass_pipeline_create(1),dragon_pass_pipeline_destroy);
  REQUIRE(pipeline);
  const auto description=takeLoopReport(dragon_pass_pipeline_describe(pipeline.get()));
  CHECK(description.find("loop-image-context")!=std::string::npos);
  CHECK(description.find("function-control-flow")!=std::string::npos);
  REQUIRE(dragon_pass_pipeline_add(pipeline.get(),"loop-image-context",
    R"({"view_symbol":"absent","table_offset":"0x8000000000000000","targets":"0xffffffffffffffff,0x1100,0xffffffffffffffff"})"));
  for(const auto *options:{
      R"({"view_symbol":"v","table_offset":true,"targets":"1"})",
      R"({"view_symbol":"v","table_offset":8192,"targets":[1,2]})",
      R"({"view_symbol":"v","table_offset":8192,"targets":"1,,2"})",
      R"({"view_symbol":"v","table_offset":"0xfffffffffffffff8","targets":"1"})",
      R"({"view_symbol":"v","table_offset":8192,"targets":"1","max_work":0})"}) {
    CHECK_FALSE(dragon_pass_pipeline_add(pipeline.get(),"loop-image-context",options));
    CHECK_FALSE(takeLoopReport(dragon_pass_pipeline_error(pipeline.get())).empty());
  }
  std::unique_ptr<dragon_llvm_lifter_t,decltype(&dragon_llvm_lifter_destroy)> lifter(
      dragon_llvm_lifter_create(DRAGON_ARCH_X86_64,"loop-c-api"),dragon_llvm_lifter_destroy);
  REQUIRE(lifter);const auto before=takeLoopReport(dragon_llvm_lifter_ir(lifter.get()));
  const auto unavailable=takeLoopReport(dragon_pass_pipeline_run_llvm(pipeline.get(),lifter.get(),1,64,1));
  CHECK(unavailable.find("\"available\":0")!=std::string::npos);
  CHECK(unavailable.find("\"closed\":0")!=std::string::npos);
  CHECK(unavailable.find("\"status\":1")!=std::string::npos);
  CHECK(takeLoopReport(dragon_llvm_lifter_ir(lifter.get()))==before);
  REQUIRE(dragon_llvm_lifter_set_execution_mode(lifter.get(),DRAGON_LLVM_STATIC_ANALYSIS));
  const auto incomplete=takeLoopReport(dragon_pass_pipeline_run_llvm(pipeline.get(),lifter.get(),1,64,1));
  CHECK(incomplete.find("\"available\":1")!=std::string::npos);
  CHECK(incomplete.find("\"closed\":0")!=std::string::npos);
  CHECK(incomplete.find("\"status\":2")!=std::string::npos);
  CHECK(incomplete.find("view symbol is absent")!=std::string::npos);
  CHECK(takeLoopReport(dragon_llvm_lifter_ir(lifter.get()))==before);
}

TEST_CASE("C function cleanup refuses missing functions and malformed options",
          "[c-api][loop-pipeline]") {
  std::unique_ptr<dragon_pass_pipeline_t,decltype(&dragon_pass_pipeline_destroy)> pipeline(
      dragon_pass_pipeline_create(1),dragon_pass_pipeline_destroy);
  REQUIRE(pipeline);
  CHECK_FALSE(dragon_pass_pipeline_add(pipeline.get(),"function-control-flow","{}"));
  CHECK_FALSE(dragon_pass_pipeline_add(pipeline.get(),"function-control-flow",R"({"function":true})"));
  REQUIRE(dragon_pass_pipeline_add(pipeline.get(),"function-control-flow",R"({"function":"absent"})"));
  std::unique_ptr<dragon_llvm_lifter_t,decltype(&dragon_llvm_lifter_destroy)> lifter(
      dragon_llvm_lifter_create(DRAGON_ARCH_X86_64,"cleanup-c-api"),dragon_llvm_lifter_destroy);
  REQUIRE(lifter);const auto before=takeLoopReport(dragon_llvm_lifter_ir(lifter.get()));
  const auto failed=takeLoopReport(dragon_pass_pipeline_run_llvm(pipeline.get(),lifter.get(),1,64,1));
  CHECK(failed.find("\"ok\":false")!=std::string::npos);
  CHECK(failed.find("\"status\":3")!=std::string::npos);
  CHECK(failed.find("requires a defined function")!=std::string::npos);
  CHECK(takeLoopReport(dragon_llvm_lifter_ir(lifter.get()))==before);
}
