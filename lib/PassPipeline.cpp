#include <dragon/passes/PassPipeline.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace dragon::passes {
namespace {
std::string dump(llvm::json::Value value) {
  std::string text;llvm::raw_string_ostream out(text);out<<value;return text;
}
llvm::json::Value parse(std::string_view text) {
  auto value=llvm::json::parse(llvm::StringRef(text.data(),text.size()));
  if (!value) throw std::invalid_argument(llvm::toString(value.takeError()));
  return std::move(*value);
}
std::uint32_t number(const Options &o,const char *key,std::uint32_t fallback) {
  auto it=o.find(key);if(it==o.end()) return fallback;
  auto *n=std::get_if<std::int64_t>(&it->second);
  if (!n || *n<0 || static_cast<std::uint64_t>(*n)>UINT32_MAX)
    throw std::invalid_argument(std::string(key)+" must be a uint32 integer");
  return static_cast<std::uint32_t>(*n);
}
PassPipeline::Validator numeric(std::set<std::string> names) {
  return [names=std::move(names)](const Options &o) {
    try {for(const auto &[key,value]:o) {
      if(!names.contains(key)) return "unknown option: "+key;
      (void)number(o,key.c_str(),0);
    }} catch(const std::exception &e) {return std::string(e.what());}
    return std::string{};
  };
}
std::string invalid(llvmlifter::LLVMLifter &lifter) {
  std::string error;llvm::raw_string_ostream out(error);
  if(llvm::verifyModule(lifter.module(),&out)) return error;
  return {};
}
Result mba(LLVMContext &ctx,const Options &o) {
  const auto s=ctx.lifter.runMBASimplification({number(o,"timeout_ms",100),number(o,"max_queries",16),
      number(o,"max_expression_nodes",128),number(o,"max_variables",3),number(o,"resource_limit",1000000)});
  return {s.available?(s.unknownQueries?Status::Incomplete:Status::Success):Status::Unavailable,
      s.expressionsSimplified!=0,s.available?"":"Z3 is unavailable",
      {{"expressions_visited",s.expressionsVisited},{"candidates",s.candidates},
       {"expressions_simplified",s.expressionsSimplified},{"solver_queries",s.solverQueries},
       {"unknown_queries",s.unknownQueries},{"rejected_candidates",s.rejectedCandidates},{"budget_skipped",s.budgetSkipped}}};
}
Result predicates(LLVMContext &ctx,const Options &o) {
  const auto s=ctx.lifter.runPredicateFolding({number(o,"timeout_ms",25),number(o,"max_queries",64),
      number(o,"max_expression_nodes",256),number(o,"resource_limit",200000)});
  return {s.available?(s.unknownQueries?Status::Incomplete:Status::Success):Status::Unavailable,
      s.conditionsFolded!=0,s.available?"":"Z3 is unavailable",
      {{"conditions_visited",s.conditionsVisited},{"conditions_folded",s.conditionsFolded},
       {"solver_queries",s.solverQueries},{"unknown_queries",s.unknownQueries},
       {"unsupported_conditions",s.unsupportedConditions},{"budget_skipped",s.budgetSkipped}}};
}
Counters controlFlowCounts(llvm::Module &module,const std::string &suffix) {
  Counters counts;
  for(auto name:{"blocks","instructions","conditional_branches","switches"})counts[name+suffix]=0;
  for(auto &f:module)for(auto &b:f) {
    ++counts["blocks"+suffix];counts["instructions"+suffix]+=b.size();
    if(auto *branch=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator()))
      counts["conditional_branches"+suffix]+=branch->isConditional();
    counts["switches"+suffix]+=llvm::isa<llvm::SwitchInst>(b.getTerminator());
  }
  return counts;
}
}
Options parseOptionsJSON(std::string_view text) {
  auto value=parse(text.empty()?"{}":text);auto *object=value.getAsObject();
  if(!object) throw std::invalid_argument("options must be a JSON object");
  Options result;
  for(const auto &[key,v]:*object) {
    if(auto b=v.getAsBoolean()) result.emplace(key.str(),*b);
    else if(auto n=v.getAsInteger()) result.emplace(key.str(),*n);
    else if(auto s=v.getAsString()) result.emplace(key.str(),s->str());
    else throw std::invalid_argument("option values must be integer, bool or string");
  }
  return result;
}
Counters parseCountersJSON(std::string_view text) {
  Counters result;
  for(const auto &[key,value]:parseOptionsJSON(text)) {
    auto *n=std::get_if<std::int64_t>(&value);
    if(!n || *n<0) throw std::invalid_argument("counters must be nonnegative integers");
    result.emplace(key,static_cast<std::uint64_t>(*n));
  }
  return result;
}
std::vector<std::string> parseNamesJSON(std::string_view text) {
  auto value=parse(text.empty()?"[]":text);auto *array=value.getAsArray();
  if(!array) throw std::invalid_argument("dependencies must be a JSON array");
  std::vector<std::string> result;
  for(const auto &v:*array) {auto s=v.getAsString();if(!s) throw std::invalid_argument("dependency must be a string");result.push_back(s->str());}
  return result;
}
std::string optionsJSON(const Options &options) {
  llvm::json::Object object;
  for(const auto &[key,v]:options) std::visit([&](const auto &x){object[key]=x;},v);
  return dump(std::move(object));
}
const std::vector<std::string> &PassPipeline::defaultLLVM() {
  static const std::vector<std::string> names{"mem2reg","instcombine","gvn","mba","predicates","dce"};return names;
}
PassPipeline::PassPipeline(bool builtins) {
  if(!builtins) return;
  auto none=numeric({});
  registerLLVM("mem2reg",[](LLVMContext &c,const Options &){c.lifter.runMem2Reg();return Result{};},false,{},none);
  registerLLVM("instcombine",[](LLVMContext &c,const Options &){c.lifter.runInstCombine();return Result{};},false,{},none);
  registerLLVM("gvn",[](LLVMContext &c,const Options &){c.lifter.runGVN();return Result{};},false,{},none);
  registerLLVM("dce",[](LLVMContext &c,const Options &){c.lifter.runDCE();return Result{};},false,{},none);
  registerLLVM("guest-memory",[](LLVMContext &c,const Options &o) {
    llvmlifter::MemoryForwardOptions options;
    options.maxRewrites=number(o,"max_rewrites",65536);
    options.runMem2Reg=number(o,"run_mem2reg",1)!=0;
    options.enableCrossBlock=number(o,"enable_cross_block",1)!=0;
    options.enablePhi=number(o,"enable_phi",1)!=0;
    options.consumeCallEffectSummaries=number(o,"consume_call_effect_summaries",0)!=0;
    const auto s=c.lifter.runGuestMemorySSA(options,number(o,"edge_worklist",0)!=0);
    Result r;
    r.status=!s.available?Status::Unavailable:(s.complete?Status::Success:Status::Incomplete);
    r.changed=s.changed;
    if(!s.available)r.message="Guest memory analysis requires a nonempty StaticAnalysis or StatefulV2 module";
    r.counters={{"available",s.available},{"complete",s.complete},{"changed",s.changed},
      {"blocks_visited",s.blocksVisited},{"guest_loads",s.guestLoads},
      {"guest_stores",s.guestStores},{"loads_forwarded",s.loadsForwarded},
      {"loads_reused",s.loadsReused},{"call_barriers",s.callBarriers},
      {"alias_barriers",s.aliasBarriers},{"partial_overlaps",s.partialOverlaps},
      {"phis_inserted",s.phisInserted},{"join_barriers",s.joinBarriers},
      {"loop_barriers",s.loopBarriers},{"budget_exhausted",s.budgetExhausted},
      {"byte_rebuilds",s.byteRebuilds},{"leaf_calls_preserved",s.leafCallsPreserved},
      {"summary_calls_preserved",s.summaryCallsPreserved},
      {"summary_calls_rejected",s.summaryCallsRejected}};
    return r;
  },false,{},[](const Options &o) {
    auto error=numeric({"max_rewrites","run_mem2reg","enable_cross_block","enable_phi",
      "consume_call_effect_summaries","edge_worklist"})(o);
    if(!error.empty())return error;
    for(const auto *name:{"run_mem2reg","enable_cross_block","enable_phi",
                         "consume_call_effect_summaries","edge_worklist"})
      if(number(o,name,0)>1)return std::string{name}+" must be 0 or 1";
    return std::string{};
  });
  registerLLVM("control-flow",[](LLVMContext &c,const Options &) {
    Result r;r.counters=controlFlowCounts(c.lifter.module(),"_before");
    c.lifter.runControlFlowSimplification();
    r.counters.merge(controlFlowCounts(c.lifter.module(),"_after"));return r;
  },false,{},none);
  registerLLVM("dispatch-specialize",[](LLVMContext &c,const Options &o) {
    const auto stats=c.lifter.runDispatcherSpecialization({
        number(o,"max_conditions",256), number(o,"max_expression_nodes",128)},
        number(o,"allow_plain_stores",0)!=0);
    Result r;
    r.status=!stats.available?Status::Unavailable:
        (stats.complete?Status::Success:Status::Incomplete);
    r.changed=stats.changed;
    r.counters={{"conditions_visited",stats.conditionsVisited},
      {"unique_conditions",stats.uniqueConditions},
      {"switches_visited",stats.switchesVisited},
      {"branches_rewritten",stats.branchesRewritten},
      {"switches_rewritten",stats.switchesRewritten},
      {"unresolved_switches",stats.unresolvedSwitches},
      {"budget_exhausted",stats.budgetExhausted}};
    return r;
  },false,{},[](const Options &o) {
    auto error=numeric({"max_conditions","max_expression_nodes","allow_plain_stores"})(o);
    if(!error.empty())return error;
    return number(o,"allow_plain_stores",0)>1?std::string{"allow_plain_stores must be 0 or 1"}:std::string{};
  });
  registerLLVM("dispatch-deflatten",[](LLVMContext &c,const Options &o) {
    const auto stats=c.lifter.runProvableDeflattening({
        number(o,"max_dispatchers",64), number(o,"max_incoming_edges",512),
        number(o,"max_expression_nodes",128), number(o,"max_values",16)});
    Result r;
    r.status=!stats.available?Status::Unavailable:
        (stats.complete?Status::Success:Status::Incomplete);
    r.changed=stats.changed;
    r.counters={{"dispatchers_visited",stats.dispatchersVisited},
      {"dispatchers_rewritten",stats.dispatchersRewritten},
      {"edges_rewritten",stats.edgesRewritten},
      {"cross_call_dispatchers_rewritten",stats.crossCallDispatchersRewritten},
      {"cross_call_summaries_consumed",stats.crossCallSummariesConsumed},
      {"cross_call_direct_stores_consumed",stats.crossCallDirectStoresConsumed},
      {"unresolved_dispatchers",stats.unresolvedDispatchers},
      {"budget_exhausted",stats.budgetExhausted}};
    return r;
  },false,{},numeric({"max_dispatchers","max_incoming_edges","max_expression_nodes","max_values"}));
  registerLLVM("mba",mba,false,{},numeric({"timeout_ms","max_queries","max_expression_nodes","max_variables","resource_limit"}));
  registerLLVM("predicates",predicates,false,{},numeric({"timeout_ms","max_queries","max_expression_nodes","resource_limit"}));
  registerGraph("graph-audit",[](GraphContext &c,const Options &) {
    Result r;r.counters={{"instructions",c.graph.size()},{"blocks",c.graph.validBlockAddresses().size()},
        {"functions",c.graph.validFunctionAddresses().size()},{"missing_ignil",0},{"unlifted",0}};
    for(auto address:c.graph.validBlockAddresses()) {
      auto *b=c.graph.getIGNILBlock(address);
      if(!b) ++r.counters["missing_ignil"];else r.counters["unlifted"]+=b->unlifted().size();
    }
    return r;
  },true,{},none);
  registerGraph("finite-values",[](GraphContext &c,const Options &o) {
    auto facts=analysis::finiteValues(c.graph,{}, {},
      {number(o,"max_values",64),number(o,"max_blocks",4096),
       number(o,"max_operations",1000000),number(o,"max_facts",262144),number(o,"prove_leaf_calls",0)!=0});
    Result r{facts.complete?Status::Success:Status::Incomplete,false,facts.message,
      {{"block_visits",facts.blockVisits},{"operations",facts.operations},
       {"proven_calls",facts.provenCalls},{"rejected_calls",facts.rejectedCalls},
       {"finite_jumps",0},{"unknown_jumps",0},{"unreachable_jumps",0}}};
    for(const auto &[at,values]:facts.jumpTargets) {
      if(values.kind==analysis::ValueSetKind::Finite)++r.counters["finite_jumps"];
      else if(values.kind==analysis::ValueSetKind::Unknown)++r.counters["unknown_jumps"];
      else ++r.counters["unreachable_jumps"];
    }
    c.analyses.put("finite-values",std::move(facts));
    return r;
  },true,{},[](const Options &o) {
    auto error=numeric({"max_values","max_blocks","max_operations","max_facts","prove_leaf_calls"})(o);
    if(!error.empty())return error;
    return number(o,"prove_leaf_calls",0)>1?std::string{"prove_leaf_calls must be 0 or 1"}:std::string{};
  });
}
bool PassPipeline::fail(std::string message) {error_=std::move(message);return false;}
bool PassPipeline::registerGraph(std::string name,GraphPass pass,bool readOnly,std::vector<std::string> dependencies,Validator validate) {
  if(active_) return fail("pipeline is running");
  if(name.empty() || !pass || entries_.contains(name)) return fail("empty or duplicate pass name or callback");
  Entry e{{name,Stage::Graph,readOnly,std::move(dependencies)},std::move(pass),{},std::move(validate)};
  entries_.emplace(std::move(name),std::move(e));error_.clear();return true;
}
bool PassPipeline::registerLLVM(std::string name,LLVMPass pass,bool readOnly,std::vector<std::string> dependencies,Validator validate) {
  if(active_) return fail("pipeline is running");
  if(name.empty() || !pass || entries_.contains(name)) return fail("empty or duplicate pass name or callback");
  Entry e{{name,Stage::LLVM,readOnly,std::move(dependencies)},{},std::move(pass),std::move(validate)};
  entries_.emplace(std::move(name),std::move(e));error_.clear();return true;
}
bool PassPipeline::overrideGraph(std::string_view name,GraphPass pass) {
  auto it=entries_.find(std::string(name));
  if(active_ || !pass || it==entries_.end() || it->second.info.stage!=Stage::Graph) return fail("cannot override Graph pass");
  it->second.graph=std::move(pass);error_.clear();return true;
}
bool PassPipeline::overrideLLVM(std::string_view name,LLVMPass pass) {
  auto it=entries_.find(std::string(name));
  if(active_ || !pass || it==entries_.end() || it->second.info.stage!=Stage::LLVM) return fail("cannot override LLVM pass");
  it->second.llvm=std::move(pass);error_.clear();return true;
}
bool PassPipeline::add(std::string name,Options options) {
  if(active_) return fail("pipeline is running");
  auto it=entries_.find(name);if(it==entries_.end()) return fail("unknown pass: "+name);
  if(it->second.validate) {auto error=it->second.validate(options);if(!error.empty()) return fail(name+": "+error);}
  steps_.push_back({std::move(name),std::move(options)});error_.clear();return true;
}
bool PassPipeline::clear() {if(active_) return fail("pipeline is running");steps_.clear();error_.clear();return true;}
std::vector<PassInfo> PassPipeline::passes() const {std::vector<PassInfo> result;for(const auto &[name,e]:entries_) result.push_back(e.info);return result;}
Report PassPipeline::run(Graph &graph,const RunOptions &o,void *binding) {return runImpl(Stage::Graph,&graph,nullptr,o,binding);}
Report PassPipeline::run(llvmlifter::LLVMLifter &lifter,const RunOptions &o,void *binding) {return runImpl(Stage::LLVM,nullptr,&lifter,o,binding);}
Report PassPipeline::runImpl(Stage stage,Graph *graph,llvmlifter::LLVMLifter *lifter,const RunOptions &o,void *binding) {
  Report report;
  auto error=[&](std::string message){report.ok=false;report.error=std::move(message);return report;};
  if(active_) return error("recursive pipeline execution");
  if(o.maxIterations<1 || o.maxIterations>64 || o.maxPassRuns>4096) return error("limits: iterations 1..64 and pass runs 0..4096");
  std::set<std::string> seen;
  for(const auto &step:steps_) {
    const auto &e=entries_.at(step.name);
    if(e.info.stage!=stage) return error("wrong stage: "+step.name);
    if(e.validate) {auto problem=e.validate(step.options);if(!problem.empty()) return error(step.name+": "+problem);}
    for(const auto &dependency:e.info.requiresPasses) if(!seen.contains(dependency)) return error(step.name+": missing prior pass "+dependency);
    seen.insert(step.name);
  }
  if(lifter && o.verifyLLVM) {auto problem=invalid(*lifter);if(!problem.empty()) return error("invalid input IR: "+problem);}
  active_=true;struct Reset {bool &active;~Reset(){active=false;}} reset{active_};
  AnalysisCache cache;
  if(steps_.empty()) {report.converged=true;return report;}
  for(std::uint32_t iteration=0;iteration<o.maxIterations;++iteration) {
    bool changed=false;
    for(const auto &step:steps_) {
      if(report.runs.size()>=o.maxPassRuns) {report.budgetExhausted=true;return report;}
      report.iterations=iteration+1;
      const auto &e=entries_.at(step.name);Result result;
      // Exact text comparison also observes globals, attributes and metadata.
      // Keep snapshots local to a single invocation; never cache IR pointers.
      const auto before=lifter?lifter->ir():std::string{};
      try {
        if(lifter) {LLVMContext context{*lifter,cache,binding};result=e.llvm(context,step.options);}
        else {GraphContext context{*graph,cache,binding};result=e.graph(context,step.options);}
      } catch(const std::exception &ex) {result={Status::Error,false,ex.what(),{}};}
        catch(...) {result={Status::Error,false,"pass threw an unknown exception",{}};}
      if(lifter) result.changed=result.changed || before!=lifter->ir();
      if(result.status==Status::Error && !e.info.readOnly) result.changed=true;
      if(static_cast<unsigned>(result.status)>static_cast<unsigned>(Status::Error)) result={Status::Error,result.changed,"invalid pass status",{}};
      for(const auto &[key,value]:result.counters) if(value>static_cast<std::uint64_t>(INT64_MAX)) {
        result={Status::Error,result.changed,"counter exceeds int64 range: "+key,{}};break;
      }
      if(e.info.readOnly && result.changed) result={Status::Error,true,"read-only pass reported or performed a mutation",{}};
      if(result.status==Status::Unavailable && result.changed) result={Status::Error,true,"unavailable pass mutated its target",{}};
      if(!e.info.readOnly || result.changed || result.status==Status::Error) cache.invalidate();
      if(lifter && o.verifyLLVM) {auto problem=invalid(*lifter);if(!problem.empty()) result={Status::Error,result.changed,"invalid output IR: "+problem,{}};}
      changed|=result.changed;report.changed|=result.changed;
      report.runs.push_back({step.name,iteration+1,cache.revision(),std::move(result)});
      if(report.runs.back().result.status==Status::Error) return error(step.name+": "+report.runs.back().result.message);
    }
    if(!changed) {report.converged=true;return report;}
  }
  report.budgetExhausted=o.maxIterations>1;return report;
}
std::string PassPipeline::describeJSON() const {
  llvm::json::Array array;
  for(const auto &p:passes()) {
    llvm::json::Array dependencies;for(const auto &d:p.requiresPasses) dependencies.push_back(d);
    array.push_back(llvm::json::Object{{"name",p.name},{"stage",static_cast<int>(p.stage)},
        {"read_only",p.readOnly},{"requires",std::move(dependencies)}});
  }
  return dump(std::move(array));
}
std::string Report::toJSON() const {
  llvm::json::Array records;
  for(const auto &r:runs) {
    llvm::json::Object counters;for(const auto &[name,value]:r.result.counters) counters[name]=static_cast<std::int64_t>(value);
    records.push_back(llvm::json::Object{{"name",r.name},{"iteration",r.iteration},{"revision",r.revision},
      {"status",static_cast<int>(r.result.status)},{"changed",r.result.changed},{"message",r.result.message},{"counters",std::move(counters)}});
  }
  return dump(llvm::json::Object{{"ok",ok},{"changed",changed},{"converged",converged},
      {"budget_exhausted",budgetExhausted},{"iterations",iterations},{"error",error},{"runs",std::move(records)}});
}
} // namespace dragon::passes
