#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dragon {
class Graph;
namespace llvmlifter { class LLVMLifter; }
namespace passes {

enum class Stage { Graph = 0, LLVM = 1 };
enum class Status { Success = 0, Unavailable = 1, Incomplete = 2, Error = 3 };
using Parameter = std::variant<std::int64_t, bool, std::string>;
using Options = std::map<std::string, Parameter>;
using Counters = std::map<std::string, std::uint64_t>;

/// Per-run analysis storage. Mutating passes conservatively invalidate all
/// entries, even if they make no change. A pass modifying its target before
/// another analysis query must also call invalidate() itself.
class AnalysisCache {
 public:
  std::uint64_t revision() const { return revision_; }
  void invalidate() { values_.clear(); ++revision_; }
  template<class T> const T *get(std::string_view key) const {
    auto it=values_.find(std::string(key));
    return it==values_.end()?nullptr:std::any_cast<T>(&it->second);
  }
  template<class T> void put(std::string key, T value) {
    values_.insert_or_assign(std::move(key),std::any(std::move(value)));
  }
 private:
  std::uint64_t revision_=0;
  std::map<std::string,std::any> values_;
};

struct GraphContext { Graph &graph; AnalysisCache &analyses; void *bindingTarget=nullptr; };
struct LLVMContext { llvmlifter::LLVMLifter &lifter; AnalysisCache &analyses; void *bindingTarget=nullptr; };
struct Result {
  Status status=Status::Success;
  bool changed=false;
  std::string message;
  Counters counters;
};
struct PassInfo {
  std::string name;
  Stage stage;
  bool readOnly=false;
  std::vector<std::string> requiresPasses;
};
struct Step { std::string name; Options options; };
struct RunOptions {
  std::uint32_t maxIterations=1; ///< 1 preserves the historical single sweep.
  std::uint32_t maxPassRuns=64;
  bool verifyLLVM=true;
};
struct Invocation {
  std::string name;
  std::uint32_t iteration=0;
  std::uint64_t revision=0;
  Result result;
};
struct Report {
  bool ok=true;
  bool changed=false;
  bool converged=false; ///< No change in a complete sweep; not deobfuscation completeness.
  bool budgetExhausted=false;
  std::uint32_t iterations=0;
  std::string error;
  std::vector<Invocation> runs;
  std::string toJSON() const;
};

/// Trusted, in-process extensions. Callbacks must prove their transformations,
/// report mutations, and honor readOnly. Errors stop the pipeline; changes made
/// before an error are not rolled back, so discard that run's target.
/// Objects/targets are not thread-safe. Recursive runs/registration are rejected.
class PassPipeline {
 public:
  using GraphPass=std::function<Result(GraphContext &,const Options &)>;
  using LLVMPass=std::function<Result(LLVMContext &,const Options &)>;
  using Validator=std::function<std::string(const Options &)>;
  explicit PassPipeline(bool builtins=true);
  bool registerGraph(std::string name,GraphPass pass,bool readOnly=false,
                     std::vector<std::string> requiresPasses={},Validator validate={});
  bool registerLLVM(std::string name,LLVMPass pass,bool readOnly=false,
                    std::vector<std::string> requiresPasses={},Validator validate={});
  /// Replace only the executor; preserve stage, dependency and option validation.
  bool overrideGraph(std::string_view name,GraphPass pass);
  bool overrideLLVM(std::string_view name,LLVMPass pass);
  bool add(std::string name,Options options={});
  bool clear();
  std::vector<PassInfo> passes() const;
  const std::string &lastError() const { return error_; }
  bool isRunning() const { return active_; }
  std::string describeJSON() const;
  Report run(Graph &graph,const RunOptions &options={},void *bindingTarget=nullptr);
  Report run(llvmlifter::LLVMLifter &lifter,const RunOptions &options={},void *bindingTarget=nullptr);
  static const std::vector<std::string> &defaultLLVM();
 private:
  struct Entry { PassInfo info; GraphPass graph; LLVMPass llvm; Validator validate; };
  std::map<std::string,Entry> entries_;
  std::vector<Step> steps_;
  std::string error_;
  bool active_=false;
  bool fail(std::string message);
  Report runImpl(Stage,Graph *,llvmlifter::LLVMLifter *,const RunOptions &,void *);
};

/// Flat JSON objects of integer/bool/string options; counters are nonnegative integers.
Options parseOptionsJSON(std::string_view json);
Counters parseCountersJSON(std::string_view json);
std::vector<std::string> parseNamesJSON(std::string_view json);
std::string optionsJSON(const Options &options);
} // namespace passes
} // namespace dragon
