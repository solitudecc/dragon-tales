// Compile one complete, hash-bound original Image TU with narrow test overlays.
// All imported scalar helpers/LLVM come from the explicitly pinned SDK.
#define DRAGON_LOOP_OBSERVATION_TEST_ACCESS 1
#ifndef DRAGON_OBSERVATION_IMAGE_SOURCE
#define DRAGON_OBSERVATION_IMAGE_SOURCE "LLVMImageTableContext.overlay.cpp"
#endif
#include DRAGON_OBSERVATION_IMAGE_SOURCE

#include <llvm/AsmParser/Parser.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/SourceMgr.h>
#include <stdexcept>
#include <string>

namespace dragon::llvmlifter::detail {
namespace {

struct LoopRangesObservationTestAccess {
  using Cache = LoopRanges::ObservationLoadCache;
  struct Snapshot {
    Cache loads;
    std::vector<LoopRanges::ObservationTrial *> trials;
    std::set<LoadInst *> memoryActive;
    std::set<Value *> scalarActive;
    const Cache *closedBase;
    unsigned independentAnchors;
  };
  struct HookState {
    unsigned childPublications = 0;
    bool injectBudget = false, actualNested = true;
    unsigned childNodes = 0, childPoints = 0;
    std::size_t childWork = 0;
  };
  static inline HookState *hook = nullptr;
  static Snapshot snapshot(const LoopRanges &r) {
    return {r.loads, r.observationTrials, r.memoryActive, r.active,
            r.closedLoadsBeforeTrials, r.independentAnchors};
  }
  static bool sameCache(const Cache &a, const Cache &b) {
    if (a.size() != b.size()) return false;
    auto i=a.begin(), j=b.begin();
    for (;i!=a.end();++i,++j)
      if (i->first!=j->first || i->second.low!=j->second.low ||
          i->second.high!=j->second.high || i->second.bits!=j->second.bits) return false;
    return true;
  }
  static bool sameState(const LoopRanges &r, const Snapshot &s) {
    return sameCache(r.loads,s.loads) && r.observationTrials==s.trials &&
      r.memoryActive==s.memoryActive && r.active==s.scalarActive &&
      r.closedLoadsBeforeTrials==s.closedBase &&
      r.independentAnchors==s.independentAnchors;
  }
  static void publication(LoopRanges &r, LoadInst *load) {
    if (!hook || load->getName()!="child") return;
    ++hook->childPublications;
    hook->actualNested &= r.observationTrials.size()>=2 &&
      r.observationTrials[r.observationTrials.size()-2]->load->getName()=="parent" &&
      r.observationTrials.back()->load==load &&
      r.observationTrials.back()->phase==LoopRanges::ObservationPhase::Aliases &&
      r.loads.contains(load);
    hook->childNodes=r.nodes; hook->childPoints=r.points;
    hook->childWork=r.work.stats.work;
    if (hook->injectBudget) r.work.limit=r.work.stats.work; // Never limit=work-1 or zero-underflow.
  }
  static std::pair<unsigned,unsigned> debt(const LoopRanges &r) { return {r.nodes,r.points}; }
  static bool cached(const LoopRanges&r,LoadInst*l) { return r.loads.contains(l); }

  // Deliberately establish a provisional parent context, not a proved fact.
  // This checks the actual child AnchorPhase cannot read a polluted conditional
  // cache or borrow that active parent. It runs real RAII and loadRange bodies.
  static bool contaminatedAnchorRefuses(LoopRanges &r, LoadInst *early,
                                        LoadInst *parent, LoadInst *query) {
    if (!r.work.spend(2*r.loads.size()+8)) return false;
    auto before=snapshot(r);
    bool refused=false, childStateRestored=false;
    {
      LoopRanges::ObservationTransaction outer(r,parent,0);
      if (!outer.enter()) return false;
      outer.trial.phase=LoopRanges::ObservationPhase::Aliases;
      outer.trial.candidate=Range{7,7,64};
      // Artificially hostile conditional cache: it is not an independent seed.
      r.loads[early]=Range{7,7,64};
      auto inside=snapshot(r);
      refused=!r.loadRange(query,0);
      childStateRestored=sameState(r,inside);
    }
    return refused && childStateRestored && sameState(r,before);
  }
};

#ifdef DRAGON_LOOP_OBSERVATION_PUBLICATION_TESTING
void observationLoadPublishedForTesting(LoopRanges &r, LoadInst *load) {
  LoopRangesObservationTestAccess::publication(r,load);
}
#endif

struct Checks {
  std::vector<std::string> failures;
  unsigned assertions=0;
  void require(bool good, StringRef why) {
    ++assertions;
    if (!good) failures.push_back(why.str());
  }
};
struct Fixture {
  LLVMContext context;
  std::unique_ptr<Module> module;
  Function *function;
  Value *guest;
  std::string original;
  explicit Fixture(StringRef ir) {
    SMDiagnostic error;
    module=parseAssemblyString(ir,error,context);
    if (!module) { std::string text; raw_string_ostream out(text);error.print("fixture",out);throw std::runtime_error(text); }
    std::string errors;
    raw_string_ostream out(errors);
    if (verifyModule(*module,&out)) throw std::runtime_error("fixture verify: "+errors);
    function=module->getFunction("fixture");
    guest=find("guest");
    raw_string_ostream initial(original);module->print(initial,nullptr);
  }
  Instruction *find(StringRef name) {
    for (auto &b:*function) for (auto&i:b) if(i.getName()==name) return &i;
    throw std::runtime_error("missing instruction "+name.str());
  }
  LoadInst *load(StringRef name) { return cast<LoadInst>(find(name)); }
  bool unchanged() {
    std::string now;raw_string_ostream out(now);module->print(out,nullptr);
    return original==now && !verifyModule(*module,&errs());
  }
};

std::string cacheFixture(bool overwrite) {
  std::string ir=R"IR(define void @fixture(ptr %descriptor, i64 %unknown) {
entry:
  %guest = load ptr, ptr %descriptor, align 1
  %cache.ptr = getelementptr i8, ptr %guest, i64 64
  store i64 5, ptr %cache.ptr, align 1
  %cache = load i64, ptr %cache.ptr, align 1
  %d.ptr = getelementptr i8, ptr %guest, i64 1024
  store i64 1, ptr %d.ptr, align 1
  %bad.ptr = getelementptr i8, ptr %guest, i64 %unknown
  store i64 0, ptr %bad.ptr, align 1
)IR";
  if(overwrite) ir+="  store i64 7, ptr %d.ptr, align 1\n";
  ir+=R"IR(  %c.ptr = getelementptr i8, ptr %guest, i64 2048
  store i64 2, ptr %c.ptr, align 1
  %child = load i64, ptr %c.ptr, align 1
  %scaled = shl i64 %child, 3
  %safe = add i64 %scaled, 8192
  %safe.ptr = getelementptr i8, ptr %guest, i64 %safe
  store i64 9, ptr %safe.ptr, align 1
  %parent = load i64, ptr %d.ptr, align 1
  ret void
}
)IR";
  return ir;
}

json::Object rollbackCase(bool exhaust, bool ordinary, Checks &checks) {
  Fixture fixture(cacheFixture(false));
  ImageTableContextStats stats;
  Work work{stats,1000000}; ConstantFacts facts;
  LoopRanges ranges(work,fixture.guest,facts);
  auto cache=ranges.get(fixture.load("cache"),fixture.find("cache"));
  checks.require(cache && cache->low==5 && cache->high==5,"baseline exact cache populated by real query");
  auto before=LoopRangesObservationTestAccess::snapshot(ranges);
  checks.require(before.loads.size()==1,"entry cache is nonempty and exact");
  const auto workBefore=stats.work;
  LoopRangesObservationTestAccess::HookState trace;
  trace.injectBudget=exhaust;
  LoopRangesObservationTestAccess::hook=&trace;
  auto result=ranges.get(fixture.load("parent"),fixture.find("parent"));
  LoopRangesObservationTestAccess::hook=nullptr;
  auto debt=LoopRangesObservationTestAccess::debt(ranges);
  checks.require(!result,"parent refused");
  checks.require(LoopRangesObservationTestAccess::sameState(ranges,before),"entry exact cache/trials/memory/anchor/base/scalar states restored");
  checks.require(!LoopRangesObservationTestAccess::cached(ranges,fixture.load("child")),"successful conditional child cache did not escape");
  checks.require(stats.work>workBefore && debt.first>0 && debt.second>0,"spent work/nodes/points did not roll back");
  if(!ordinary) {
    checks.require(trace.childPublications==1 && trace.actualNested,"actual nested child completed once before parent failure");
    checks.require(debt.first>=trace.childNodes && debt.second>=trace.childPoints && stats.work>=trace.childWork,"debt retained after child publication");
  }
  if(exhaust) {
    checks.require(stats.budgetExhausted && work.limit==stats.work && work.limit==trace.childWork,"budget injected at actual spent work without underflow");
  } else checks.require(!stats.budgetExhausted,"alias refusal was not budget exhaustion");
  auto failedWork=stats.work;
  // End only the artificial budget injection. Retain spent Work and the
  // exhaustion flag; never grant more than the original frozen 1M ceiling.
  work.limit=1000000;
  auto independent=ranges.get(fixture.load("child"),fixture.find("child"));
  checks.require(independent && independent->low==2 && independent->high==2,"subsequent independent real query succeeds");
  checks.require(stats.work>failedWork,"subsequent query retains shared spent work");
  checks.require(fixture.unchanged(),"complete original IR unchanged and verified");
  return json::Object{{"case",exhaust?"child-success-parent-budget":"child-success-parent-alias-failure"},
    {"child_publications",trace.childPublications},{"nested",trace.actualNested},
    {"work_before",workBefore},{"work_at_child",trace.childWork},{"work_after_failure",failedWork},
    {"nodes_after_failure",debt.first},{"points_after_failure",debt.second},
    {"subsequent_work",stats.work},{"parent_result",static_cast<bool>(result)}};
}

json::Object laterOverwrite(Checks &checks) {
  Fixture fixture(cacheFixture(true));ImageTableContextStats stats;
  Work work{stats,1000000};ConstantFacts facts;LoopRanges ranges(work,fixture.guest,facts);
  auto value=ranges.get(fixture.load("parent"),fixture.find("parent"));
  checks.require(value && value->low==7 && value->high==7,"complete later overwrite legitimately cuts earlier unknown clobber");
  auto s=LoopRangesObservationTestAccess::snapshot(ranges);
  checks.require(s.trials.empty() && s.memoryActive.empty() && !s.closedBase && s.independentAnchors==0,"successful outer state fully retired");
  checks.require(LoopRangesObservationTestAccess::cached(ranges,fixture.load("child")),"successful jointly closed child retained");
  checks.require(fixture.unchanged(),"later overwrite source remains exact");
  return json::Object{{"case","complete-overwrite"},{"value",value?value->low:UINT64_MAX},{"work",stats.work}};
}

constexpr StringLiteral EarlyIR=R"IR(define void @fixture(ptr %descriptor) {
entry:
  %guest = load ptr, ptr %descriptor, align 1
  %d.ptr = getelementptr i8, ptr %guest, i64 1024
  %early = load i64, ptr %d.ptr, align 1
  store i64 %early, ptr %d.ptr, align 1
  %query = load i64, ptr %d.ptr, align 1
  %parent.ptr = getelementptr i8, ptr %guest, i64 2048
  %parent = load i64, ptr %parent.ptr, align 1
  ret void
})IR";

json::Object anchorLeak(Checks &checks) {
  Fixture fixture(EarlyIR);ImageTableContextStats stats;Work work{stats,1000000};
  ConstantFacts facts;LoopRanges ranges(work,fixture.guest,facts);
  auto early=ranges.get(fixture.load("early"),fixture.find("early"));
  checks.require(!early,"earlier observation cannot use a future store");
  auto before=LoopRangesObservationTestAccess::snapshot(ranges);
  checks.require(LoopRangesObservationTestAccess::contaminatedAnchorRefuses(ranges,
      fixture.load("early"),fixture.load("parent"),fixture.load("query")),
      "independent anchor rejects hostile conditional early cache and restores both scopes");
  checks.require(LoopRangesObservationTestAccess::sameState(ranges,before),"anchor rejection keeps exact entry map");
  checks.require(fixture.unchanged(),"anchor leakage fixture unchanged");
  return json::Object{{"case","anchor-phase-hostile-conditional-cache"},{"work",stats.work}};
}

constexpr StringLiteral IncrementIR=R"IR(define void @fixture(ptr %descriptor) {
entry:
  %guest = load ptr, ptr %descriptor, align 1
  %d.ptr = getelementptr i8, ptr %guest, i64 1024
  store i64 0, ptr %d.ptr, align 1
  br label %loop
loop:
  %query = load i64, ptr %d.ptr, align 1
  %next = add i64 %query, 1
  store i64 %next, ptr %d.ptr, align 1
  br label %loop
})IR";
constexpr StringLiteral NoAnchorIR=R"IR(define void @fixture(ptr %descriptor, i1 %choose) {
entry:
  %guest = load ptr, ptr %descriptor, align 1
  %d.ptr = getelementptr i8, ptr %guest, i64 1024
  br i1 %choose, label %seed, label %unseeded
seed:
  store i64 7, ptr %d.ptr, align 1
  br label %query.block
unseeded:
  br i1 %choose, label %unseeded, label %query.block
query.block:
  %query = load i64, ptr %d.ptr, align 1
  ret void
})IR";
json::Object ordinaryRefusal(StringRef name, StringRef ir, Checks &checks) {
  Fixture fixture(ir);ImageTableContextStats stats;Work work{stats,1000000};
  ConstantFacts facts;LoopRanges ranges(work,fixture.guest,facts);
  auto before=LoopRangesObservationTestAccess::snapshot(ranges);
  auto result=ranges.get(fixture.load("query"),fixture.find("query"));
  checks.require(!result,"unsupported anchor obligation refused");
  checks.require(LoopRangesObservationTestAccess::sameState(ranges,before),"refusal restores exact numeric/phase state");
  checks.require(!stats.budgetExhausted,"refusal is semantic rather than budget");
  checks.require(fixture.unchanged(),"refusal input unchanged");
  return json::Object{{"case",name.str()},{"work",stats.work},{"message",stats.message}};
}

int runObservationCacheTests(bool ordinary) {
  Checks checks;json::Array cases;
  cases.push_back(rollbackCase(false,ordinary,checks));
  if(!ordinary) cases.push_back(rollbackCase(true,false,checks));
  cases.push_back(laterOverwrite(checks));
  cases.push_back(anchorLeak(checks));
  cases.push_back(ordinaryRefusal("unmasked-L-plus-one",IncrementIR,checks));
  cases.push_back(ordinaryRefusal("unanchored-arm-SCC",NoAnchorIR,checks));
  json::Array failures;for(auto&f:checks.failures) failures.push_back(f);
  json::Object report{{"schema","actual-D-cache-transaction-tests-v1"},
    {"source_inclusion",true},{"installed_Image_execution",false},{"guest_native",0},
    {"ordinary_hook_off",ordinary},{"assertions",checks.assertions},
    {"failures",std::move(failures)},{"cases",std::move(cases)}};
  outs()<<formatv("{0:2}\n",json::Value(std::move(report)));
  return checks.failures.empty()?0:1;
}
} // anonymous namespace
} // namespace dragon::llvmlifter::detail

int main(int argc,char **argv) {
  try {
    bool ordinary=argc==2 && std::string(argv[1])=="--ordinary";
    return dragon::llvmlifter::detail::runObservationCacheTests(ordinary);
  } catch(const std::exception &error) {
    errs()<<"fixture/preparation exception: "<<error.what()<<"\n";
    return 2;
  }
}
