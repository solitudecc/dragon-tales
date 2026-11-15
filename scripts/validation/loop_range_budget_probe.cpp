// The complete exact original TU is included; only friend access/public renames are overlaid.
#include DRAGON_BUDGET_IMAGE_SOURCE
#include <llvm/IR/NoFolder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace dragon::llvmlifter::detail {
namespace {
struct LoopRangeBudgetTestAccess {
  static unsigned nodes(const LoopRanges &r) { return r.nodes; }
  static unsigned points(const LoopRanges &r) { return r.points; }
  static std::optional<Range> direct(LoopRanges &r, Value *v, unsigned depth,
                                     bool memory, Instruction *at) {
    return r.visit(v, depth, memory, at);
  }
  static std::optional<Range> nestedStore(LoopRanges &r, Value *v, unsigned depth,
                                          StoreInst *at) {
    return r.rangeAtOriginalStore(v, depth, at);
  }
  static OpaqueIntegerAdmissionResult warmShape(LoopRanges &r, Value *v) {
    auto result = r.opaque.getPhiRegions(v,128,r.work.limit-r.work.stats.work);
    if (!r.work.spend(result.work)) throw std::runtime_error("warm shape Work exhausted");
    return result;
  }
};
struct Checks {
  unsigned count = 0;
  std::vector<std::string> failures;
  void require(bool value, StringRef reason) {
    ++count;
    if (!value) failures.push_back(reason.str());
  }
};
struct Fixture {
  LLVMContext context;
  Module module{"budget-fixture",context};
  Function *function;
  BasicBlock *entry;
  LoadInst *guest;
  IRBuilder<NoFolder> builder;
  Value *root = nullptr;
  StoreInst *at = nullptr;
  std::vector<LoadInst *> leaves;
  std::vector<PHINode *> roots;
  std::string original;

  Fixture() : builder(context) {
    auto *type=FunctionType::get(builder.getVoidTy(),
       {builder.getPtrTy(),builder.getInt1Ty(),builder.getInt32Ty(),builder.getInt1Ty()},false);
    function=Function::Create(type,GlobalValue::ExternalLinkage,"fixture",module);
    function->getArg(0)->setName("descriptor");
    function->getArg(1)->setName("choice");
    function->getArg(2)->setName("raw");
    function->getArg(3)->setName("again");
    entry=BasicBlock::Create(context,"entry",function);
    builder.SetInsertPoint(entry);
    guest=builder.CreateLoad(builder.getPtrTy(),function->getArg(0),"guest");
    guest->setAlignment(Align(1));
  }
  static Value *balanced(IRBuilder<NoFolder> &b, std::vector<Value *> values) {
    while (values.size()>1) {
      std::vector<Value *> next;
      for (std::size_t i=0;i<values.size();i+=2)
        next.push_back(i+1==values.size() ? values[i] : b.CreateAdd(values[i],values[i+1],"sum"));
      values=std::move(next);
    }
    return values.front();
  }
  void finish(Value *value, bool loop=false) {
    root=value;
    auto *pointer=builder.CreateGEP(builder.getInt8Ty(),guest,builder.getInt64(2048),"out.ptr");
    auto *wide=builder.CreateZExt(value,builder.getInt64Ty(),"wide");
    at=builder.CreateStore(wide,pointer); at->setAlignment(Align(1));
    if (loop) {
      auto *out=BasicBlock::Create(context,"exit",function);
      auto *current=builder.GetInsertBlock();
      builder.CreateCondBr(function->getArg(3),current,out);
      builder.SetInsertPoint(out);builder.CreateRetVoid();
    } else builder.CreateRetVoid();
    std::string error;raw_string_ostream diagnostic(error);
    if (verifyModule(module,&diagnostic)) throw std::runtime_error("fixture invalid: "+error);
    raw_string_ostream saved(original);module.print(saved,nullptr);saved.flush();
  }
  bool unchanged() {
    std::string printed;raw_string_ostream out(printed);module.print(out,nullptr);out.flush();
    return printed==original && !verifyModule(module,&errs());
  }
  void save(StringRef directory, StringRef name) {
    std::error_code error;
    raw_fd_ostream file((directory+"/"+name+".ll").str(),error);
    if (error) throw std::runtime_error(error.message());
    file<<original;
  }
  void selects(unsigned count, unsigned mode) {
    // mode0 literals; mode1 two shared LOADs; mode2 distinct SSA LOADs all mapped to1.
    if (mode) {
      unsigned loads=mode==1?2:count;
      for (unsigned i=0;i<loads;++i) {
        auto *ptr=builder.CreateGEP(builder.getInt8Ty(),guest,builder.getInt64(64+4*i),"leaf.ptr");
        auto *load=builder.CreateLoad(builder.getInt32Ty(),ptr,"leaf");
        load->setAlignment(Align(1));leaves.push_back(load);
      }
    }
    std::vector<Value *> values;
    for (unsigned i=0;i<count;++i) {
      Value *a=mode?leaves[mode==1?0:i]:static_cast<Value *>(builder.getInt32(1));
      Value *b=mode==1?leaves[1]:static_cast<Value *>(builder.getInt32(2));
      values.push_back(builder.CreateSelect(function->getArg(1),a,b,"pick"));
    }
    finish(balanced(builder,std::move(values)));
  }
  void plain(unsigned count) {
    std::vector<Value *> values(count,function->getArg(2));
    finish(balanced(builder,std::move(values)));
  }
  void opaqueRoots(unsigned count, bool distinct) {
    auto *derived=builder.CreateXor(function->getArg(2),builder.getInt32(123),"derive0");
    derived=builder.CreateAdd(derived,builder.getInt32(5),"derive1");
    derived=builder.CreateXor(derived,builder.getInt32(77),"derive2");
    auto *loop=BasicBlock::Create(context,"loop",function);builder.CreateBr(loop);
    builder.SetInsertPoint(loop);
    for (unsigned i=0;i<(distinct?count:1);++i) {
      auto *phi=builder.CreatePHI(builder.getInt32Ty(),2,"opaque");
      phi->addIncoming(derived,entry);phi->addIncoming(phi,loop);roots.push_back(phi);
    }
    std::vector<Value *> masks;
    for (unsigned i=0;i<count;++i)
      masks.push_back(builder.CreateAnd(roots[distinct?i:0],builder.getInt32(7),"masked"));
    finish(balanced(builder,std::move(masks)),true);
  }
};
json::Object observation(StringRef name, const std::optional<Range> &result,
                         const ImageTableContextStats &stats, LoopRanges &ranges) {
  json::Object out{{"case",name.str()},{"present",bool(result)},
      {"nodes",LoopRangeBudgetTestAccess::nodes(ranges)},
      {"points",LoopRangeBudgetTestAccess::points(ranges)},
      {"work",stats.work},{"budget_exhausted",stats.budgetExhausted},
      {"message",stats.message}};
  if (result) {
    out["low"]=result->low;out["high"]=result->high;out["bits"]=result->bits;
  }
  return out;
}
bool exact(const std::optional<Range> &range,std::uint64_t low,std::uint64_t high,unsigned bits=32) {
  return range && range->low==low && range->high==high && range->bits==bits;
}
void knownPositive(StringRef output, Checks &checks,json::Array &cases,bool hc) {
  Fixture f;f.selects(40,hc?1:0);f.save(output,hc?"shared-HC":"shared-literals");
  ConstantFacts facts;
  if(hc) { facts.emplace(f.leaves[0],f.builder.getInt32(1));facts.emplace(f.leaves[1],f.builder.getInt32(2)); }
  ImageTableContextStats stats;Work work{stats,1000000};LoopRanges ranges(work,f.guest,facts);
  auto first=ranges.get(f.root,f.at);cases.push_back(observation(hc?"HC-first":"literal-first",first,stats,ranges));
  checks.require(exact(first,40,80),hc?"same-SSA HC positive range":"shared literal positive range");
  if(first) checks.require(LoopRangeBudgetTestAccess::nodes(ranges)==121,"first root pays121 nodes");
  // A failed first query remains failed evidence. Do not clear its sticky flag or retry it.
  if(first) {
    auto beforeWork=stats.work;
    auto second=ranges.get(f.root,f.at);cases.push_back(observation(hc?"HC-second":"literal-second",second,stats,ranges));
    checks.require(exact(second,40,80) && LoopRangeBudgetTestAccess::nodes(ranges)==121,
                   "each outer get repays complete known leaf ledger");
    checks.require(stats.work-beforeWork==199,"repeated outer get still spends199 actual visits");
    auto written=ranges.getForWrite(f.at->getValueOperand(),f.at,4,{{1024,8}});
    cases.push_back(observation(hc?"HC-write-root":"literal-write-root",written,stats,ranges));
    checks.require(exact(written,40,80,64) && LoopRangeBudgetTestAccess::nodes(ranges)==122,
                   "outer getForWrite independently repays ledger and cast");
    checks.require(!stats.budgetExhausted,"successful outer roots no cap flag");
  }
  checks.require(f.unchanged(),"positive complete Module/IR/memory nodes unchanged");
  if(hc) {
    ConstantFacts other;
    other.emplace(f.leaves[0],f.builder.getInt32(7));other.emplace(f.leaves[1],f.builder.getInt32(9));
    ImageTableContextStats s;Work w{s,1000000};LoopRanges candidate(w,f.guest,other);
    auto result=candidate.get(f.root,f.at);cases.push_back(observation("independent-HC-candidate",result,s,candidate));
    checks.require(exact(result,280,360),"different candidate immutable H maps not reused");
    if(result) checks.require(LoopRangeBudgetTestAccess::nodes(candidate)==121,"new candidate pays fresh ledger");
    checks.require(f.unchanged(),"other candidate module unchanged");
  }
}
void controls(StringRef output,Checks &checks,json::Array &cases) {
  {
    Fixture f;f.selects(40,0);f.save(output,"work-limit");ConstantFacts facts;
    ImageTableContextStats s;Work w{s,198};LoopRanges r(w,f.guest,facts);
    auto result=r.get(f.root,f.at);cases.push_back(observation("198-work-reject",result,s,r));
    checks.require(!result && s.budgetExhausted && s.work<=198,"known reuse cannot evade Work198");
    checks.require(f.unchanged(),"work refusal unchanged");
  }
  {
    Fixture f;f.selects(1,0);f.save(output,"depth-limit");ConstantFacts facts;
    ImageTableContextStats s;Work w{s,1000000};LoopRanges r(w,f.guest,facts);
    auto *literal=f.builder.getInt32(1);
    auto warm=LoopRangeBudgetTestAccess::direct(r,literal,0,true,f.at);
    const auto before=s.work;
    auto result=LoopRangeBudgetTestAccess::direct(r,literal,33,true,f.at);
    cases.push_back(observation("known-depth33-reject",result,s,r));
    checks.require(exact(warm,1,1) && !result && s.budgetExhausted && s.work==before+1,
                   "already-paid literal still checks depth and spends Work");
    checks.require(f.unchanged(),"depth refusal unchanged");
  }
  {
    Fixture f;f.selects(40,2);f.save(output,"distinct-HC-keys");ConstantFacts facts;
    for(auto *load:f.leaves) facts.emplace(load,f.builder.getInt32(1));
    ImageTableContextStats s;Work w{s,1000000};LoopRanges r(w,f.guest,facts);
    auto result=r.get(f.root,f.at);cases.push_back(observation("distinct-HC-keys-reject",result,s,r));
    checks.require(!result && s.budgetExhausted,"same literal in distinct HC SSA keys never merges fees");
    checks.require(f.unchanged(),"distinct HC refusal unchanged");
  }
  {
    Fixture f;f.plain(70);f.save(output,"nonknown-repeated");ConstantFacts facts;
    ImageTableContextStats s;Work w{s,1000000};LoopRanges r(w,f.guest,facts);
    auto result=r.get(f.root,f.at);cases.push_back(observation("nonknown-reuse-reject",result,s,r));
    checks.require(!result && s.budgetExhausted,"nonknown same SSA still charged per visit");
    checks.require(f.unchanged(),"nonknown refusal unchanged");
  }
  {
    Fixture f;f.selects(40,0);f.save(output,"nested-store-debt");ConstantFacts facts;
    ImageTableContextStats s;Work w{s,1000000};LoopRanges r(w,f.guest,facts);
    bool warmed=true;
    for(unsigned i=0;i<100;++i)
      warmed &= bool(LoopRangeBudgetTestAccess::direct(r,f.function->getArg(2),0,true,f.at));
    auto beforeNodes=LoopRangeBudgetTestAccess::nodes(r);auto beforeWork=s.work;
    auto result=LoopRangeBudgetTestAccess::nestedStore(r,f.root,0,f.at);
    cases.push_back(observation("nested-store-no-reset",result,s,r));
    checks.require(warmed && beforeNodes==100 && !result && s.budgetExhausted &&
        LoopRangeBudgetTestAccess::nodes(r)>128 && s.work>beforeWork,
        "nested original STORE query preserves spent scalar nodes/Work");
    checks.require(f.unchanged(),"nested debt refusal unchanged");
  }
}
void opaquePositive(StringRef output,Checks &checks,json::Array &cases) {
  Fixture f;f.opaqueRoots(20,false);f.save(output,"repeated-opaque");ConstantFacts facts;
  ImageTableContextStats s;Work w{s,1000000};LoopRanges r(w,f.guest,facts);
  auto warm=LoopRangeBudgetTestAccess::warmShape(r,f.roots[0]);
  checks.require(warm.admitted && warm.uniqueNodes==8 && warm.work>1,
                 "independent complete shape warm reports8 nodes and actual Work");
  auto first=r.get(f.root,f.at);cases.push_back(observation("same-opaque-first",first,s,r));
  checks.require(exact(first,0,140),"repeated same opaque root fits whole root budget");
  if(first) {
    checks.require(LoopRangeBudgetTestAccess::nodes(r)==67,"first opaque footprint full8 paid once, root visits all charged");
    auto second=r.get(f.root,f.at);cases.push_back(observation("same-opaque-second",second,s,r));
    checks.require(exact(second,0,140) && LoopRangeBudgetTestAccess::nodes(r)==67,
                   "outer root clears opaque ledger despite helper cache hits");
    auto write=r.getForWrite(f.at->getValueOperand(),f.at,4,{{1024,8}});
    cases.push_back(observation("same-opaque-write-root",write,s,r));
    checks.require(exact(write,0,140,64) && LoopRangeBudgetTestAccess::nodes(r)==68,
                   "outer write root repays complete opaque footprint");
    checks.require(!s.budgetExhausted,"opaque successful roots have no budget flag");
  }
  checks.require(f.unchanged(),"opaque analysis leaves complete IR/CFG/memory nodes unchanged");
  Fixture many;many.opaqueRoots(20,true);many.save(output,"distinct-opaque-roots");
  ImageTableContextStats sm;Work wm{sm,1000000};LoopRanges rm(wm,many.guest,facts);
  auto rejected=rm.get(many.root,many.at);cases.push_back(observation("distinct-opaque-roots-reject",rejected,sm,rm));
  checks.require(!rejected && sm.budgetExhausted,"overlapping distinct opaque roots never merge footprint credit");
  checks.require(many.unchanged(),"distinct opaque refusal unchanged");
}
int run(StringRef output) {
  if(std::error_code error=sys::fs::create_directory(output)) throw std::runtime_error(error.message());
  Checks checks;json::Array cases;
  knownPositive(output,checks,cases,false);
  knownPositive(output,checks,cases,true);
  controls(output,checks,cases);
  opaquePositive(output,checks,cases);
  json::Array failures;for(auto &why:checks.failures) failures.push_back(why);
  json::Object report{{"schema","original-LoopRanges-budget-policy-tests-v1"},
    {"source_variant",DRAGON_BUDGET_VARIANT},{"source_inclusion",true},
    {"installed_Image_execution",false},{"guest_native",0},{"C_or_UC",0},
    {"assertions",checks.count},{"failures",std::move(failures)},
    {"cases",std::move(cases)}};
  outs()<<formatv("{0:2}\n",json::Value(std::move(report)));
  return checks.failures.empty()?0:1;
}
} // namespace
} // namespace dragon::llvmlifter::detail
int main(int argc,char **argv) {
  try {
    // Give the external observer one bounded interval to capture actual loaded DSOs.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if(argc!=2) throw std::runtime_error("one new fixture-output directory required");
    return dragon::llvmlifter::detail::run(argv[1]);
  } catch(const std::exception &error) {
    errs()<<"fixture/preparation exception: "<<error.what()<<"\n";return 2;
  }
}
