// Reuse the frozen framework and ACTUAL Image TU, not a mirrored resolver.
#define main inheritedObservationCacheMain
#include DRAGON_OBSERVATION_FRAMEWORK
#undef main

int main() {
  namespace D=dragon::llvmlifter::detail;
  try {
    constexpr llvm::StringLiteral ir=R"IR(define void @fixture(ptr %descriptor, i1 %choose) {
entry:
  %guest = load ptr, ptr %descriptor, align 1
  %d.ptr = getelementptr i8, ptr %guest, i64 1024
  br label %seed
seed:
  store i64 7, ptr %d.ptr, align 1
  br label %query.block
orphan:
  br i1 %choose, label %orphan, label %query.block
query.block:
  %query = load i64, ptr %d.ptr, align 1
  ret void
})IR";
    D::Fixture fixture(ir);
    D::Checks checks;
    auto *entry=&fixture.function->getEntryBlock();
    llvm::BasicBlock *seed=nullptr,*orphan=nullptr,*query=nullptr;
    for(auto &b:*fixture.function) {
      if(b.getName()=="seed")seed=&b;
      if(b.getName()=="orphan")orphan=&b;
      if(b.getName()=="query.block")query=&b;
    }
    checks.require(seed && orphan && query,"all explicit CFG role blocks exist");
    checks.require(entry->getTerminator()->getNumSuccessors()==1 &&
      entry->getTerminator()->getSuccessor(0)==seed,"the only entry route reaches the complete seed");
    std::vector<llvm::BasicBlock*> orphanPreds(llvm::pred_begin(orphan),llvm::pred_end(orphan));
    checks.require(orphanPreds.size()==1 && orphanPreds.front()==orphan,
      "orphan is a closed self-SCC with no entry or seed predecessor");
    std::set<llvm::BasicBlock*> queryPreds(llvm::pred_begin(query),llvm::pred_end(query));
    checks.require(queryPreds==std::set<llvm::BasicBlock*>{seed,orphan},
      "query has both the anchored arm and the separate unanchored SCC");
    auto row=D::ordinaryRefusal("isolated-unanchored-orphan-SCC",ir,checks);
    llvm::json::Array failures;
    for(const auto &failure:checks.failures)failures.push_back(failure);
    llvm::json::Object report{{"schema","actual-D-isolated-orphan-coverage-v1"},
      {"source_inclusion",true},{"guest_native",0},{"installed_Image_execution",false},
      {"assertions",checks.assertions},{"failures",std::move(failures)},
      {"case",std::move(row)},{"entry_missing_is_not_this_control",true}};
    llvm::outs()<<llvm::formatv("{0:2}\n",llvm::json::Value(std::move(report)));
    return checks.failures.empty()?0:1;
  } catch(const std::exception &error) {
    llvm::errs()<<"supplement fixture/preparation exception: "<<error.what()<<"\n";
    return 2;
  }
}
