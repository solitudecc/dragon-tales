#include <dragon/passes/FiniteDispatch.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/analysis/IndirectTargets.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace dragon::passes {
namespace {
std::uint32_t number(const Options &o,const char *name,std::uint32_t fallback) {
  auto it=o.find(name);if(it==o.end())return fallback;
  const auto *value=std::get_if<std::int64_t>(&it->second);
  if(!value || *value<0 || std::uint64_t(*value)>UINT32_MAX)
    throw std::invalid_argument(std::string(name)+" must be a uint32 integer");
  return std::uint32_t(*value);
}
class Resolver final : public analysis::IndirectTargetResolver {
  using QueryKey=std::tuple<std::uint64_t,std::size_t,std::uint64_t,ignil::Width>;
  using MemoryKey=std::tuple<std::uintptr_t,std::set<std::uint64_t>,std::vector<QueryKey>>;
 public:
  analysis::FiniteValueLimits limits;
  std::uint32_t roundsLimit=256;
  std::uint64_t rounds=0,operations=0,memoryOperations=0;
  std::uint64_t memoryQueries=0,memoryCacheHits=0,maxRoundOperations=0,maxCachedFacts=0;
  bool aborted=false;
  std::string message;
  analysis::FiniteValueResult current;
  bool prepare(const Graph &graph,const std::set<std::uint64_t> &entries) override {
    memoryCache.clear();cachedFacts=0;roundOperations=0;
    if(aborted)return false;
    ++rounds;current=analysis::finiteValues(graph,entries,{},limits);
    operations+=current.operations;roundOperations=current.operations;
    maxRoundOperations=std::max(maxRoundOperations,roundOperations);
    if(!current.complete) {failBudget();return false;}
    return true;
  }
  std::optional<std::vector<std::uint64_t>> targets(std::uint64_t block) const override {
    if(aborted)return {};
    auto it=current.jumpTargets.find(block);
    if(it==current.jumpTargets.end() || it->second.kind!=analysis::ValueSetKind::Finite)return {};
    return it->second.values;
  }
  std::optional<analysis::FiniteValueResult> memoryFacts(const Graph &graph,
      const std::set<std::uint64_t> &entries,
      std::span<const analysis::FiniteMemoryQuery> queries) override {
    if(aborted)return failBudget();
    // The identity is only a cache key within one immutable recovery round;
    // no Graph pointer/view is retained or dereferenced after this call.
    // prepare() clears every identity before a new CFG can be analysed.
    if(queries.size()>limits.maxFacts || entries.size()>limits.maxFacts)
      return failBudget();
    const auto keyFacts=std::uint64_t(queries.size())+entries.size()+1;
    if(keyFacts>limits.maxFacts)return failBudget();
    MemoryKey key{reinterpret_cast<std::uintptr_t>(&graph),entries,{}};
    auto &group=std::get<2>(key);
    group.reserve(queries.size());
    for(const auto &query:queries)
      group.emplace_back(query.block,query.beforeOpIndex,query.address,query.width);
    if(const auto found=memoryCache.find(key);found!=memoryCache.end()) {
      ++memoryCacheHits;
      return found->second;
    }
    if(roundOperations>=limits.maxOperations || cachedFacts>limits.maxFacts-keyFacts ||
       cachedFacts+keyFacts==limits.maxFacts)return failBudget();
    auto remaining=limits;
    remaining.maxOperations=limits.maxOperations-roundOperations;
    remaining.maxFacts=limits.maxFacts-cachedFacts-keyFacts;
    ++memoryQueries;
    auto facts=analysis::finiteMemoryValues(graph,entries,queries,remaining);
    operations+=facts.operations;memoryOperations+=facts.operations;
    roundOperations+=facts.operations;
    maxRoundOperations=std::max(maxRoundOperations,roundOperations);
    if(!facts.complete)return failBudget();
    std::uint64_t resultFacts=facts.values.size()+facts.jumpTargets.size();
    for(const auto &value:facts.values)resultFacts+=value.values.size();
    for(const auto &[address,value]:facts.jumpTargets)resultFacts+=value.values.size();
    if(resultFacts>limits.maxFacts-cachedFacts-keyFacts)return failBudget();
    cachedFacts+=keyFacts+resultFacts;
    maxCachedFacts=std::max(maxCachedFacts,cachedFacts);
    memoryCache.emplace(std::move(key),facts);
    return facts;
  }
  std::uint32_t maxRounds() const override {return roundsLimit;}
  std::uint32_t maxTargets() const override {return limits.maxValues;}
  void stopped(std::string_view why) override {
    if(!aborted) {aborted=true;message=why;}
  }
 private:
  std::map<MemoryKey,analysis::FiniteValueResult> memoryCache;
  std::uint64_t cachedFacts=0,roundOperations=0;
  analysis::FiniteValueResult failBudget() {
    aborted=true;message="finite dispatch analysis budget exhausted";
    analysis::FiniteValueResult failure;
    failure.complete=false;failure.message=message;
    return failure; // Engaged failure prevents an unbudgeted default retry.
  }
};
std::string shape(const Graph &g) {
  std::ostringstream out;
  for(const auto &[at,i]:g.listing()) {
    out<<at<<':'<<unsigned(i.size())<<':'<<i.isBlockStart()<<':'<<i.isFunctionStart()<<':'<<i.edges()<<';';
    for(auto a:i.branches())out<<a<<',';
    out<<'/';for(auto a:i.callees())out<<a<<',';
    out<<'\n';
  }
  for(auto at:g.validBlockAddresses()) {
    out<<"b"<<at<<'\n';
    if(const auto *b=g.getIGNILBlock(at))out<<ignil::toString(*b);
  }
  for(auto at:g.validFunctionAddresses())out<<"f"<<at<<'\n';
  return out.str();
}
}
FiniteDispatchPass::FiniteDispatchPass(std::span<const std::uint8_t> code,std::uint64_t base,
    std::span<const std::uint64_t> entries) : FiniteDispatchPass(code,base,entries,{}) {}
FiniteDispatchPass::FiniteDispatchPass(std::span<const std::uint8_t> code,std::uint64_t base,
    std::span<const std::uint64_t> entries,const Disassembler::AnalysisHints &hints) :
    code_(code.begin(),code.end()),base_(base),entries_(entries.begin(),entries.end()),hints_(hints) {
  if(code_.empty() || code_.size()>UINT64_MAX-base_)
    throw std::invalid_argument("finite dispatch needs a nonempty nonwrapping code range");
  entries_.insert(hints.entryPoints.begin(),hints.entryPoints.end());
  hints_.indirectTargets.reset(); // The pass supplies its own bounded resolver.
}
PassPipeline::Validator FiniteDispatchPass::validator() {
  return [](const Options &options) {
    try {
      const std::set<std::string> allowed{"max_values","max_blocks","max_operations","max_facts","max_rounds","prove_leaf_calls"};
      for(const auto &[name,v]:options) {
        if(!allowed.contains(name))return "unknown option: "+name;
        (void)number(options,name.c_str(),0);
      }
      if(number(options,"max_values",64)>256 || number(options,"max_rounds",256)>4096)
        return std::string{"max_values must be <=256 and max_rounds <=4096"};
      if(number(options,"prove_leaf_calls",0)>1)return std::string{"prove_leaf_calls must be 0 or 1"};
    } catch(const std::exception &e) {return std::string{e.what()};}
    return std::string{};
  };
}
Result FiniteDispatchPass::operator()(GraphContext &context,const Options &options) const {
  auto problem=validator()(options);if(!problem.empty())return {Status::Error,false,problem,{}};
  auto &source=context.graph;
  if(source.architecture()!=Architecture::X86_32 && source.architecture()!=Architecture::X86_64)
    return {Status::Unavailable,false,"finite dispatch currently supports x86 CFG recovery",{}};
  auto resolver=std::make_shared<Resolver>();
  resolver->limits={number(options,"max_values",64),number(options,"max_blocks",4096),
    number(options,"max_operations",1000000),number(options,"max_facts",262144),
    number(options,"prove_leaf_calls",0)!=0};
  resolver->roundsLimit=number(options,"max_rounds",256);
  Result report;report.counters={{"rounds",0},{"analysis_operations",0},{"memory_analysis_operations",0},
    {"memory_analysis_queries",0},{"memory_analysis_cache_hits",0},{"memory_analysis_cached_facts_peak",0},
    {"max_round_analysis_operations",0},{"new_instructions",0},{"new_edges",0},
    {"removed_edges",0},{"unresolved_jumps",0},{"proven_calls",0},{"rejected_calls",0}};
  auto incomplete=[&](std::string message) {
    // No new speculative object is committed on failure. Old inference cannot
    // serve as proof for this invocation's changed entries/content contract,
    // however, even when a zero budget prevents us from attempting recovery.
    for(auto *instruction:source.instructions()) {
      report.counters["removed_edges"]+=instruction->inferredBranches().size();
      instruction->clearInferredBranches();
    }
    report.changed=report.counters["removed_edges"]!=0;
    if(report.changed)context.analyses.invalidate();
    report.counters["unresolved_jumps"]=0;
    for(const auto &[at,i]:source.listing())
      if(i.isJump() && i.hasIndirectTarget() && i.branches().empty())++report.counters["unresolved_jumps"];
    report.status=Status::Incomplete;report.message=std::move(message);
    return report;
  };
  for(const auto &[at,i]:source.listing())
    if(i.isJump() && i.hasIndirectTarget() && i.branches().empty())++report.counters["unresolved_jumps"];
  if(!resolver->limits.maxValues || !resolver->limits.maxBlocks || !resolver->limits.maxOperations ||
     !resolver->limits.maxFacts || !resolver->roundsLimit ||
     source.validBlockAddresses().size()>resolver->limits.maxBlocks)
    return incomplete("finite dispatch analysis budget is insufficient");
  Graph work=source.clone();
  auto hints=hints_;
  hints.entryPoints.assign(entries_.begin(),entries_.end());
  if(hints.entryPoints.empty())
    hints.entryPoints.assign(source.validFunctionAddresses().begin(),source.validFunctionAddresses().end());
  hints.indirectTargets=resolver;
  Disassembler dis{source.architecture(),source.configuration()};
  dis.disassemble(code_,base_,work,hints);
  report.counters["rounds"]=resolver->rounds;
  report.counters["analysis_operations"]=resolver->operations;
  report.counters["memory_analysis_operations"]=resolver->memoryOperations;
  report.counters["memory_analysis_queries"]=resolver->memoryQueries;
  report.counters["memory_analysis_cache_hits"]=resolver->memoryCacheHits;
  report.counters["memory_analysis_cached_facts_peak"]=resolver->maxCachedFacts;
  report.counters["max_round_analysis_operations"]=resolver->maxRoundOperations;
  // Unique sites in the final analysis, rather than repeated visits/sweeps.
  report.counters["proven_calls"]=resolver->current.provenCalls;
  report.counters["rejected_calls"]=resolver->current.rejectedCalls;
  if(resolver->aborted)
    return incomplete(resolver->message);
  report.counters["unresolved_jumps"]=0;
  report.counters["removed_edges"]=0;
  for(const auto &[at,i]:work.listing())if(i.isJump() && i.hasIndirectTarget()) {
    if(i.branches().empty())++report.counters["unresolved_jumps"];
    const auto *old=source.get(at);
    for(auto target:i.branches())if(!old || !old->branches().contains(target))++report.counters["new_edges"];
  }
  report.counters["new_instructions"]=work.size()-source.size();
  for(const auto &[at,i]:source.listing()) {
    const auto *current=work.get(at);
    for(auto target:i.branches())
      if(!current || !current->branches().contains(target))++report.counters["removed_edges"];
  }
  report.status=report.counters["unresolved_jumps"]?Status::Incomplete:Status::Success;
  report.changed=shape(source)!=shape(work);
  if(report.changed) {
    context.analyses.invalidate();
    source=std::move(work);
  }
  return report;
}
} // namespace dragon::passes
