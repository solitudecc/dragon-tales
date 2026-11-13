// Validation-only single-sample query. No production name/address special case.
#define main shared_inventory_output_main
#include "checked_context_inventory_shared.cpp"
#undef main
#include <llvm/IR/ValueHandle.h>
#include <filesystem>

namespace {
constexpr auto ModuleName="partial-masks-values-real";
std::uint64_t passCalls=0;
std::string readFile(const char *path) {
 auto file=llvm::MemoryBuffer::getFile(path);
 if(!file)throw std::runtime_error(std::string("read failed: ")+path);
 return (*file)->getBuffer().str();
}
void fresh(const std::string &path,const std::string &text) {
 if(std::filesystem::exists(path))throw std::runtime_error("refuse overwrite "+path);
 write(path,text);
}
struct Frame {std::string ir,inventory;};
Frame snapshot(dragon::llvmlifter::LLVMLifter &l,const std::string &selected,
               const std::string &prefix) {
 // Every snapshot gets a fresh tracker. It never crosses a Module mutation.
 slots=std::make_unique<llvm::ModuleSlotTracker>(&l.module());
 slots->incorporateFunction(*l.module().getFunction(selected));
 Frame result{l.ir(),json(stateInventory(l.module(),selected))};slots.reset();
 fresh(prefix+".ll",result.ir);fresh(prefix+".json",result.inventory);return result;
}
llvm::json::Object describe(llvm::Value *value,llvm::LoadInst *original) {
 if(!value)return llvm::json::Object{{"kind","deleted_without_live_successor"},{"numeric_value","Unknown"}};
 std::string kind=value==original?"same_original_load":llvm::isa<llvm::PHINode>(value)?"phi":
                  llvm::isa<llvm::ConstantInt>(value)?"constant_int":"other";
 llvm::json::Object result{{"kind",kind},{"same_original_object",value==original},
                          {"operand",operand(*value)},{"typed",typedOperand(*value)},
                          {"full_value",printed(*value)}};
 if(auto *i=llvm::dyn_cast<llvm::Instruction>(value)) {
  result["block"]=i->getParent()->getName().str();result["opcode"]=i->getOpcodeName();
 }
 if(auto *integer=llvm::dyn_cast<llvm::ConstantInt>(value);integer && integer->getBitWidth()==64) {
  result["literal_u64"]=std::to_string(integer->getZExtValue());
  const auto low=integer->getValue().trunc(32);
  const auto relation=llvm::APInt(32,20)-low;
  result["owner_supplied_modular_relation"]=llvm::json::Object{
    {"relation","20 - trunc32(tracked load)"},
    {"u32",std::to_string(relation.getZExtValue())},
    {"scope","arithmetic evaluation of the requested relation only; no claim of global PHI/SSA simplification or definedness"}};
 }else result["numeric_value"]="Unknown";
 return result;
}
}

int main(int argc,char **argv) {try {
 // The independent reference is emitted by real61_inventory_reference.cpp.
 // --import-only is a zero-pass actual LLVMLifter import for post binding.
 if(argc!=6 && argc!=7)throw std::runtime_error("private.ll expected.ll expected.json source.json prefix [--import-only]");
 const bool importOnly=argc==7 && std::string(argv[6])=="--import-only";
 if(argc==7 && !importOnly)throw std::runtime_error("unknown mode");
 const std::string prefix=argv[5];
 if(!importOnly && std::filesystem::space(std::filesystem::path(prefix).parent_path()).available<536870912) {
  fresh(prefix+".refused.json",json(llvm::json::Object{{"reason","original512MiB-start-gate"},{"pass_calls",0}}));return 6;
 }
 const auto input=readFile(argv[1]),expectedIR=readFile(argv[2]);
 const auto expectedJSON=readFile(argv[3]),sourceJSON=readFile(argv[4]);
 dragon::llvmlifter::LLVMLifter lifter(dragon::Architecture::X86_64,ModuleName);
 if(!lifter.setExecutionMode(dragon::llvmlifter::ExecutionMode::StaticAnalysis))throw std::runtime_error("StaticAnalysis mode failed");
 llvm::SMDiagnostic diagnostic;
 if(llvm::parseAssemblyInto(llvm::MemoryBufferRef(input,argv[1]),&lifter.module(),nullptr,diagnostic)) {
  diagnostic.print(argv[0],llvm::errs());return 2;
 }
 auto &module=lifter.module();std::string error;
 if(!verify(module,error))throw std::runtime_error("before verification: "+error);
 if(module.getModuleIdentifier()!=ModuleName)throw std::runtime_error("ModuleIdentifier changed");
 const auto manifestText=lifter.guardedEntryManifest();auto manifest=llvm::json::parse(manifestText);
 if(!manifest)throw std::runtime_error("entry manifest invalid");
 auto *object=manifest->getAsObject();auto name=object?object->getString("view_symbol"):std::nullopt;
 if(!name)throw std::runtime_error("selected original view missing");
 const auto selected=name->str();auto *function=module.getFunction(selected);
 if(!function)throw std::runtime_error("selected function absent");
 const auto before=snapshot(lifter,selected,prefix+".before");
 const bool sameIR=before.ir==expectedIR,sameJSON=before.inventory==expectedJSON,
            sameSource=before.inventory==sourceJSON;
 fresh(prefix+".import.json",json(llvm::json::Object{{"actual_full_LL_equal_independent",sameIR},
   {"actual_full_JSON_equal_independent",sameJSON},{"actual_full_JSON_equal_source",sameSource},
   {"source_raw_LL_equal_actual_import",before.ir==input},{"pass_calls_at_check",0},
   {"normalization",false},{"mode",importOnly?"actual-import-only":"single-default-guest-memory-query"}}));
 if(!sameIR || !sameJSON || !sameSource)throw std::runtime_error("actual independent import mismatch; zero pass calls");
 if(importOnly) {
  fresh(prefix+".verify.json",json(llvm::json::Object{{"verified",true},{"pass_calls",0},{"target_native_executions",0}}));return 0;
 }
 llvm::LoadInst *original=nullptr;unsigned matches=0;
 for(auto &block:*function)if(block.getName()=="memory.valid322")for(auto &i:block)
  if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&i);load && load->getType()->isIntegerTy(64)){original=load;++matches;}
 if(matches!=1)throw std::runtime_error("requested block has no unique i64 LOAD; zero pass calls");
 llvm::WeakTrackingVH tracked(original);
 std::vector<llvm::WeakTrackingVH> users;
 for(auto *user:original->users())users.emplace_back(user);
 slots=std::make_unique<llvm::ModuleSlotTracker>(&module);slots->incorporateFunction(*function);
 llvm::json::Array beforeUses;
 for(auto &user:users)beforeUses.push_back(describe(user,original));
 auto beforeQuery=describe(original,original);beforeQuery["initial_direct_users"]=std::move(beforeUses);slots.reset();
 fresh(prefix+".query.before.json",json(std::move(beforeQuery)));
 // Use the public builtin with NO options: exactly the registered defaults.
 dragon::passes::PassPipeline pipeline;
 if(!pipeline.add("guest-memory"))throw std::runtime_error(pipeline.lastError());
 dragon::passes::RunOptions options;options.maxIterations=1;options.maxPassRuns=1;options.verifyLLVM=true;
 if(std::filesystem::space(std::filesystem::path(prefix).parent_path()).available<536870912) {
  fresh(prefix+".refused-before-pass.json",json(llvm::json::Object{{"reason","original512MiB-start-gate"},{"pass_calls",0}}));return 6;
 }
 passCalls=1;const auto report=pipeline.run(lifter,options);
 fresh(prefix+".guest-memory-report.json",report.toJSON());
 // Retain actual raw output even if the public pipeline reports failure.
 fresh(prefix+".raw-after.ll",lifter.ir());
 error.clear();const bool valid=verify(module,error);
 if(!valid){fresh(prefix+".invalid-after.ll",lifter.ir());throw std::runtime_error("after verification: "+error);}
 if(module.getModuleIdentifier()!=ModuleName || lifter.guardedEntryManifest()!=manifestText)throw std::runtime_error("ModuleIdentifier or entry metadata changed");
 snapshot(lifter,selected,prefix+".after");
 if(!report.ok || report.runs.size()!=1 || report.runs.front().name!="guest-memory")throw std::runtime_error("single default pass protocol failed");
 slots=std::make_unique<llvm::ModuleSlotTracker>(&module);slots->incorporateFunction(*module.getFunction(selected));
 auto afterQuery=describe(tracked,original);llvm::json::Array afterUses;
 for(auto &user:users)afterUses.push_back(describe(user,original));
 afterQuery["tracked_original_direct_users_after"]=std::move(afterUses);slots.reset();
 fresh(prefix+".query.after.json",json(std::move(afterQuery)));
 fresh(prefix+".verify.json",json(llvm::json::Object{{"before",true},{"after",true},
   {"pass_calls",1},{"pass_name","guest-memory"},{"options_provided",llvm::json::Object{}},
   {"registered_defaults",llvm::json::Object{{"max_rewrites",65536},{"run_mem2reg",1},
      {"enable_cross_block",1},{"enable_phi",1},{"consume_call_effect_summaries",0},{"edge_worklist",0}}},
   {"ModuleIdentifier",ModuleName},{"entry_manifest_unchanged",true},{"StaticAnalysis",true},
   {"lookup_scope","validation-only unique i64 LOAD in requested original-view block; no production matching"},
   {"target_native_executions",0},{"Bit_C59_reruns",0},{"fresh_full_replays",0}}));
 return 0;
 }catch(const std::exception &e){slots.reset();llvm::errs()<<e.what()<<"; pass_calls="<<passCalls<<'\n';return 9;}}
