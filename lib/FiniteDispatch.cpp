#include <dragon/passes/FiniteDispatch.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/analysis/IndirectTargets.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <algorithm>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

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
 public:
  analysis::FiniteValueLimits limits;
  std::uint32_t roundsLimit=256;
  std::uint64_t rounds=0,operations=0;
  bool aborted=false;
  std::string message;
  analysis::FiniteValueResult current;
  bool prepare(const Graph &graph,const std::set<std::uint64_t> &entries) override {
    ++rounds;current=analysis::finiteValues(graph,entries,{},limits);
    operations+=current.operations;return current.complete;
  }
  std::optional<std::vector<std::uint64_t>> targets(std::uint64_t block) const override {
    auto it=current.jumpTargets.find(block);
    if(it==current.jumpTargets.end() || it->second.kind!=analysis::ValueSetKind::Finite)return {};
    return it->second.values;
  }
  std::uint32_t maxRounds() const override {return roundsLimit;}
  std::uint32_t maxTargets() const override {return limits.maxValues;}
  void stopped(std::string_view why) override {aborted=true;message=why;}
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
    std::span<const std::uint64_t> entries) :
    code_(code.begin(),code.end()),base_(base),entries_(entries.begin(),entries.end()) {
  if(code_.empty() || code_.size()>UINT64_MAX-base_)
    throw std::invalid_argument("finite dispatch needs a nonempty nonwrapping code range");
}
PassPipeline::Validator FiniteDispatchPass::validator() {
  return [](const Options &options) {
    try {
      const std::set<std::string> allowed{"max_values","max_blocks","max_operations","max_facts","max_rounds"};
      for(const auto &[name,v]:options) {
        if(!allowed.contains(name))return "unknown option: "+name;
        (void)number(options,name.c_str(),0);
      }
      if(number(options,"max_values",64)>256 || number(options,"max_rounds",256)>4096)
        return std::string{"max_values must be <=256 and max_rounds <=4096"};
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
    number(options,"max_operations",1000000),number(options,"max_facts",262144)};
  resolver->roundsLimit=number(options,"max_rounds",256);
  Result report;report.counters={{"rounds",0},{"analysis_operations",0},{"new_instructions",0},{"new_edges",0},{"unresolved_jumps",0}};
  for(const auto &[at,i]:source.listing())
    if(i.isJump() && i.hasIndirectTarget() && i.branches().empty())++report.counters["unresolved_jumps"];
  if(!resolver->limits.maxValues || !resolver->limits.maxBlocks || !resolver->limits.maxOperations ||
     !resolver->limits.maxFacts || !resolver->roundsLimit ||
     source.validBlockAddresses().size()>resolver->limits.maxBlocks)
    return {Status::Incomplete,false,"finite dispatch analysis budget is insufficient",report.counters};
  Graph work=source.clone();
  Disassembler::AnalysisHints hints;
  hints.entryPoints.assign(entries_.begin(),entries_.end());
  if(hints.entryPoints.empty())
    hints.entryPoints.assign(source.validFunctionAddresses().begin(),source.validFunctionAddresses().end());
  hints.indirectTargets=resolver;
  Disassembler dis{source.architecture(),source.configuration()};
  dis.disassemble(code_,base_,work,hints);
  report.counters["rounds"]=resolver->rounds;
  report.counters["analysis_operations"]=resolver->operations;
  if(resolver->aborted)
    return {Status::Incomplete,false,resolver->message,report.counters};
  report.counters["unresolved_jumps"]=0;
  for(const auto &[at,i]:work.listing())if(i.isJump() && i.hasIndirectTarget()) {
    if(i.branches().empty())++report.counters["unresolved_jumps"];
    const auto *old=source.get(at);
    for(auto target:i.branches())if(!old || !old->branches().contains(target))++report.counters["new_edges"];
  }
  report.counters["new_instructions"]=work.size()-source.size();
  report.status=report.counters["unresolved_jumps"]?Status::Incomplete:Status::Success;
  report.changed=shape(source)!=shape(work);
  if(report.changed) {
    context.analyses.invalidate();
    source=std::move(work);
  }
  return report;
}
} // namespace dragon::passes
