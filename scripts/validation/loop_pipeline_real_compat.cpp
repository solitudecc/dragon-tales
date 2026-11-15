// Existing public API/LLVM-only compatibility check. No private product source or Guest execution.
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include DRAGON_LOOP_PIPELINE_IDENTITY_HEADER
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/DFAJumpThreading.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <filesystem>
#include <stdexcept>

using namespace llvm;
using dragon::llvmlifter::LLVMLifter;
namespace {
void need(bool good,const Twine &why){if(!good)throw std::runtime_error(why.str());}
std::string read(const std::filesystem::path &path){auto b=MemoryBuffer::getFile(path.string());need(bool(b),"cannot read "+path.string());return (*b)->getBuffer().str();}
std::string digest(StringRef text){auto h=SHA256::hash(ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));static const char *d="0123456789abcdef";std::string s;for(auto b:h){s+=d[b>>4];s+=d[b&15];}return s;}
void save(const std::filesystem::path &path,StringRef bytes){need(!std::filesystem::exists(path),"new output required");std::error_code ec;raw_fd_ostream stream(path.string(),ec);need(!ec,"output creation failed");stream<<bytes;}
void verify(Module &m){std::string text;raw_string_ostream out(text);need(!verifyModule(m,&out),"LLVM verify: "+text);}
std::string field(const json::Object &o,StringRef k){auto s=o.getString(k);need(bool(s),"missing string "+k);return s->str();}
const json::Object &object(const json::Object &o,StringRef k){auto *v=o.getObject(k);need(v,"missing object "+k);return *v;}
std::uint64_t number(const json::Value &v){
  if(auto n=v.getAsInteger()){need(*n>=0,"negative unsigned number");return *n;}
  auto s=v.getAsString();need(bool(s),"uint64 integer/string required");std::uint64_t n=0;auto text=*s;unsigned radix=10;
  if(text.starts_with("0x")||text.starts_with("0X")){text=text.drop_front(2);radix=16;}
  need(!text.empty()&&!text.getAsInteger(radix,n),"invalid uint64 string");return n;
}
std::uint64_t number(const json::Object &o,StringRef k){auto *v=o.get(k);need(v,"missing uint64 "+k);return number(*v);}
std::string pinned(const json::Object &o){const auto bytes=read(field(o,"path"));need(digest(bytes)==field(o,"sha256"),"input SHA changed");if(auto size=o.getInteger("size"))need(*size>=0&&bytes.size()==static_cast<std::size_t>(*size),"input size changed");return bytes;}
void parseInto(LLVMLifter &l,StringRef text,StringRef identifier){
  need(l.setExecutionMode(dragon::llvmlifter::ExecutionMode::StaticAnalysis)&&l.setStaticEntryPolicy(dragon::llvmlifter::StaticEntryPolicy::FunctionStart),"static mode refused");
  SMDiagnostic error;if(parseAssemblyInto(MemoryBufferRef(text,identifier),&l.module(),nullptr,error)){error.print("loop-pipeline-real",errs());throw std::runtime_error("parseAssemblyInto failed");}verify(l.module());
}
dragon::passes::Counters counters(const dragon::llvmlifter::detail::ImageTableContextStats &s){return {
  {"available",1},{"complete",s.closed},{"closed",s.closed},{"budget_exhausted",s.budgetExhausted},
  {"guarded_bytes",s.guardedBytes},{"preserved_stores",s.preservedStores},{"constant_values",s.constantValues},{"work",s.work}};}
dragon::passes::Counters structure(Function &f,const std::string &suffix){
  dragon::passes::Counters result;for(auto key:{"blocks","instructions","conditional_branches","switches"})result[std::string(key)+suffix]=0;
  for(auto &b:f){++result["blocks"+suffix];result["instructions"+suffix]+=b.size();if(auto *br=dyn_cast<BranchInst>(b.getTerminator()))result["conditional_branches"+suffix]+=br->isConditional();result["switches"+suffix]+=isa<SwitchInst>(b.getTerminator());}return result;
}
void cleanupReference(Module &m,Function &f){
  LoopAnalysisManager lam;FunctionAnalysisManager fam;CGSCCAnalysisManager cgam;ModuleAnalysisManager mam;PassBuilder pb;
  pb.registerModuleAnalyses(mam);pb.registerCGSCCAnalyses(cgam);pb.registerFunctionAnalyses(fam);pb.registerLoopAnalyses(lam);pb.crossRegisterProxies(lam,fam,cgam,mam);
  FunctionPassManager fpm;fpm.addPass(SCCPPass());fpm.addPass(DFAJumpThreadingPass());fpm.addPass(JumpThreadingPass());
  fpm.addPass(SimplifyCFGPass());fpm.addPass(InstCombinePass());fpm.addPass(DCEPass());fpm.run(f,fam);verify(m);
}
json::Object counterJSON(const dragon::passes::Counters &c){json::Object o;for(auto &[k,v]:c)o[k]=v;return o;}
}
int main(int argc,char **argv){
  std::filesystem::path output;json::Object result{{"schema","loop-pipeline-real-compat-v1"},{"ok",false},{"Guest_runs",0},{"C_runs",0},{"Unicorn_runs",0},{"observer_runs",0}};
  try {
    need(argc==3,"usage: driver request.json NEW-OUT");output=argv[2];need(!std::filesystem::exists(output),"new namespace required");std::filesystem::create_directories(output);
    const auto requestBytes=read(argv[1]);auto parsed=json::parse(requestBytes);need(bool(parsed),"request JSON invalid");auto *config=parsed->getAsObject();need(config,"request object required");
    need(field(*config,"schema")=="loop-pipeline-real-compat-request-v1","request schema differs");
    const auto &input=object(*config,"before");const auto bytes=pinned(input);save(output/"source.raw.ll",bytes);
    result["request_sha256"]=digest(requestBytes);result["input_sha256"]=digest(bytes);result["raw_ModuleID_literal"]=bytes.substr(0,bytes.find('\n'));
    const auto symbol=field(*config,"view_symbol");const auto &table=object(*config,"table");auto *targetList=table.getArray("targets");need(targetList&&!targetList->empty()&&targetList->size()<=512,"bounded ordered targets required");
    dragon::llvmlifter::detail::ImageTableContextRequest request;request.viewSymbol=symbol;request.tableOffset=number(table,"offset");for(auto &v:*targetList)request.targets.push_back(number(v));
    const auto &limits=object(*config,"limits");request.maxBlocks=number(limits,"max_blocks");request.maxInstructions=number(limits,"max_instructions");request.maxWork=number(limits,"max_work");
    need(request.maxBlocks&&request.maxBlocks<=4096&&request.maxInstructions&&request.maxInstructions<=65536&&request.maxWork&&request.maxWork<=1000000,"request caps changed");
    LLVMLifter direct{dragon::Architecture::X86_64,"guarded-source"},managed{dragon::Architecture::X86_64,"guarded-source"};
    parseInto(direct,bytes,field(input,"path"));parseInto(managed,bytes,field(input,"path"));
    const auto before=direct.ir();save(output/"direct.before.ll",before);save(output/"pipeline.before.ll",managed.ir());
    need(before==managed.ir(),"fresh parsed states differ");result["raw_byte_equal_actual_parse"]=bytes==before;result["actual_parsed_ModuleID"]=direct.module().getModuleIdentifier();
    dragon::passes::PassPipeline pipeline;
    std::string csv;for(auto n:request.targets){if(!csv.empty())csv+=',';csv+=std::to_string(n);}
    dragon::passes::Options options{{"view_symbol",symbol},{"table_offset",std::to_string(request.tableOffset)},{"targets",csv},
      {"max_blocks",static_cast<std::int64_t>(request.maxBlocks)},{"max_instructions",static_cast<std::int64_t>(request.maxInstructions)},{"max_work",static_cast<std::int64_t>(request.maxWork)}};
    if(!pipeline.add("loop-image-context",options)){
      result["status"]="builtin-API-absent-or-configuration-rejected";result["message"]=pipeline.lastError();result["direct_Loop_calls"]=0;result["pipeline_Loop_calls"]=0;
      save(output/"REPORT.json",formatv("{0:2}\n",json::Value(std::move(result))).str());return 3;
    }
    probe_identity::Identity directIdentity(direct.module(),symbol),managedIdentity(managed.module(),symbol);
    const auto stats=dragon::llvmlifter::detail::runLoopImageTableContext(direct.module(),request);save(output/"direct.after.ll",direct.ir());verify(direct.module());
    dragon::passes::RunOptions runOptions;runOptions.maxIterations=1;runOptions.maxPassRuns=1;runOptions.verifyLLVM=true;
    const auto report=pipeline.run(managed,runOptions);save(output/"loop-pipeline-report.json",report.toJSON());save(output/"pipeline.after.ll",managed.ir());verify(managed.module());
    need(report.ok&&report.runs.size()==1,"pipeline Loop invocation failed");const auto &r=report.runs.front().result;
    need(r.status==(stats.closed?dragon::passes::Status::Success:dragon::passes::Status::Incomplete)&&r.changed==stats.changed&&r.message==stats.message&&r.counters==counters(stats),"direct/builtin full result/stats differ");
    need(direct.ir()==managed.ir(),"direct/builtin complete IR differs");
    result["direct_identity"]=directIdentity.check(direct.module(),symbol,stats.closed&&stats.changed);
    result["pipeline_identity"]=managedIdentity.check(managed.module(),symbol,stats.closed&&stats.changed);
    result["Loop_full_IR_equal"]=true;result["Loop_full_stats_equal"]=true;result["loop_counters"]=counterJSON(counters(stats));result["loop_message"]=stats.message;
    result["direct_Loop_calls"]=1;result["pipeline_Loop_calls"]=1;
    if(config->getBoolean("expected_closed")==false){
      need(!stats.closed&&!stats.changed&&!stats.budgetExhausted&&direct.ir()==before&&managed.ir()==before,"expected atomic precision refusal not preserved");
      need(pinned(input)==bytes&&read(argv[1])==requestBytes,"refusal input/request changed");
      result["ok"]=true;result["precision_closed"]=false;result["expected_refusal"]=true;result["cleanup_calls"]=0;
      result["input_unchanged"]=true;result["normalizations"]=0;
      save(output/"REPORT.json",formatv("{0:2}\n",json::Value(std::move(result))).str());return 0;
    }
    need(stats.closed&&stats.changed&&!stats.budgetExhausted,"real B did not close under unchanged budgets");
    auto *selected=direct.module().getFunction(symbol);need(selected,"reference function absent");auto expectedCounts=structure(*selected,"_before");
    probe_identity::Identity referenceOutside(direct.module(),symbol),pipelineOutside(managed.module(),symbol);
    cleanupReference(direct.module(),*selected);expectedCounts.merge(structure(*direct.module().getFunction(symbol),"_after"));
    save(output/"reference.clean.ll",direct.ir());
    dragon::passes::PassPipeline clean;need(clean.add("function-control-flow",{{"function",symbol}}),clean.lastError());
    const auto cleaned=clean.run(managed,runOptions);save(output/"cleanup-pipeline-report.json",cleaned.toJSON());save(output/"pipeline.clean.ll",managed.ir());verify(managed.module());
    need(cleaned.ok&&cleaned.runs.size()==1&&cleaned.runs[0].result.status==dragon::passes::Status::Success&&cleaned.runs[0].result.counters==expectedCounts,"cleanup full stats differ");
    need(direct.ir()==managed.ir(),"official-reference/builtin complete cleanup IR differs");
    result["cleanup_reference_identity"]=referenceOutside.check(direct.module(),symbol);
    result["cleanup_pipeline_identity"]=pipelineOutside.check(managed.module(),symbol);
    need(pinned(input)==bytes&&read(argv[1])==requestBytes,"input/request changed");
    result["cleanup_full_IR_equal"]=true;result["cleanup_full_stats_equal"]=true;result["cleanup_counters"]=counterJSON(expectedCounts);
    result["before_parsed_states_equal"]=true;result["input_unchanged"]=true;result["normalizations"]=0;result["ok"]=true;
    save(output/"REPORT.json",formatv("{0:2}\n",json::Value(std::move(result))).str());return 0;
  }catch(const std::exception &error){
    result["failure"]=error.what();if(!output.empty()&&std::filesystem::is_directory(output)&&!std::filesystem::exists(output/"REPORT.json"))save(output/"REPORT.json",formatv("{0:2}\n",json::Value(std::move(result))).str());
    errs()<<error.what()<<"\n";return 1;
  }
}
