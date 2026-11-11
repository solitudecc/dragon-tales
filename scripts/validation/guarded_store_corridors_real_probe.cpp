// Independent read-only inventory and a public PassPipeline transformation.
// Keep the established verifier/selector inventory exactly as frozen for C/D.
#define main frozen_inventory_main
#include "guarded_business_inventory.cpp"
#undef main
#include <llvm/IR/Metadata.h>
int main(int argc,char **argv){try {
 if(argc==2 && std::string(argv[1])=="--smoke"){
  llvmlifter::LLVMLifter empty(Architecture::X86_64,"smoke");
  passes::PassPipeline p;return llvm::verifyModule(empty.module())?8:0;
 }
 if(argc!=4 && argc!=5)throw std::runtime_error("input.ll hint.json output-prefix [normal|missing|invalid] required");
 const std::string mode=argc==5?argv[4]:"normal";if(mode!="normal"&&mode!="missing"&&mode!="invalid")throw std::runtime_error("bad mode");
 const std::string prefix=argv[3];auto input=llvm::MemoryBuffer::getFile(argv[1]);
 auto hints=llvm::MemoryBuffer::getFile(argv[2]);if(!input||!hints)throw std::runtime_error("read failed");
 llvmlifter::LLVMLifter lifter(Architecture::X86_64,"target-set-real-probe");
 if(!lifter.setExecutionMode(llvmlifter::ExecutionMode::StaticAnalysis))throw std::runtime_error("mode failed");
 llvm::SMDiagnostic diagnostic;if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&lifter.module(),nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 auto manifestText=lifter.guardedEntryManifest();auto manifest=llvm::json::parse(manifestText);
 if(!manifest)throw std::runtime_error(llvm::toString(manifest.takeError()));
 auto *object=manifest->getAsObject();auto name=object?object->getString("view_symbol"):std::nullopt;
 if(!name)throw std::runtime_error("view missing");const auto selected=name->str();
 auto parsed=llvm::json::parse((*hints)->getBuffer());if(!parsed)throw std::runtime_error(llvm::toString(parsed.takeError()));
 auto *hintObject=parsed->getAsObject();auto function=hintObject?hintObject->getString("function"):std::nullopt;
 if(!function||*function!=selected)throw std::runtime_error("hint function mismatch");
 auto &module=lifter.module();if(module.getNamedMetadata("dragon.indirect-target-hints"))throw std::runtime_error("preexisting hints refused");
 std::string error;if(!verify(module,error))throw std::runtime_error(error);
 write(prefix+".manifest.json",manifestText);write(prefix+".before.ll",lifter.ir());write(prefix+".before.json",json(inventory(module,selected)));
 std::map<std::string,std::string> bodies;for(auto &f:module)bodies.emplace(f.getName().str(),printed(f));
 if(mode!="missing"){
  auto *node=module.getOrInsertNamedMetadata("dragon.indirect-target-hints");
  node->addOperand(llvm::MDNode::get(module.getContext(),llvm::MDString::get(module.getContext(),mode=="invalid"?llvm::StringRef("{"):(*hints)->getBuffer())));
 }
 if(lifter.guardedEntryManifest()!=manifestText)throw std::runtime_error("entry memory manifest changed");
 const auto passInputIR=lifter.ir();write(prefix+".with-hints.ll",passInputIR);
 auto runStage=[&](const std::string &stage,const std::vector<std::pair<std::string,passes::Options>> &steps){
  passes::PassPipeline stagePipeline;
  for(const auto &[name,opts]:steps)if(!stagePipeline.add(name,opts))throw std::runtime_error(stagePipeline.lastError());
  passes::RunOptions stageOptions;stageOptions.maxIterations=1;stageOptions.maxPassRuns=steps.size();stageOptions.verifyLLVM=true;
  auto stageReport=stagePipeline.run(lifter,stageOptions);write(prefix+"."+stage+"-report.json",stageReport.toJSON());
  write(prefix+"."+stage+".ll",lifter.ir());write(prefix+"."+stage+".json",json(inventory(module,selected)));
  std::string stageError;if(!stageReport.ok||!verify(module,stageError))throw std::runtime_error("stage failed "+stage+": "+stageError);
 };
 runStage("F",{{"guarded-state-corridors",{{"max_candidates",std::int64_t(64)},{"max_rewrites",std::int64_t(32)},{"max_blocks",std::int64_t(128)},{"max_instructions",std::int64_t(16384)}}}});
 runStage("H",{{"guarded-store-corridors",{{"max_candidates",std::int64_t(64)},{"max_rewrites",std::int64_t(32)},{"max_blocks",std::int64_t(128)},{"max_instructions",std::int64_t(16384)}}}});
 runStage("tiny",{{"mem2reg",{}},{"instcombine",{}},{"gvn",{}},{"instcombine",{}},{"dce",{}}});
 runStage("constant",{{"constant-branches",{{"max_folds",std::int64_t(4096)},{"max_instructions",std::int64_t(1000000)},{"max_blocks",std::int64_t(65536)}}}});
 passes::PassPipeline pipeline;
 if(!pipeline.add("guarded-target-sets",{{"max_candidates",std::int64_t(64)},{"max_rewrites",std::int64_t(32)},{"max_blocks",std::int64_t(128)},{"max_instructions",std::int64_t(16384)}}))throw std::runtime_error(pipeline.lastError());
 write(prefix+".description.json",pipeline.describeJSON());
 passes::RunOptions options;options.maxIterations=1;options.maxPassRuns=1;options.verifyLLVM=true;
 const auto report=pipeline.run(lifter,options);write(prefix+".report.json",report.toJSON());
 write(prefix+".after.ll",lifter.ir());write(prefix+".after.json",json(inventory(module,selected)));
 error.clear();bool valid=verify(module,error);std::map<std::string,std::string> afterBodies;for(auto &f:module)afterBodies.emplace(f.getName().str(),printed(f));write(prefix+".verify.json",json(llvm::json::Object{{"mode",mode},{"function_printing_equal_before_metadata",bodies==afterBodies},{"module_after_hint_unchanged",passInputIR==lifter.ir()},{"input",true},{"after",valid},{"entry_manifest_unchanged",lifter.guardedEntryManifest()==manifestText},{"error",error},{"native_executions",0}}));
 return valid&&(report.ok||mode=="missing")&&lifter.guardedEntryManifest()==manifestText&&(mode=="normal"||passInputIR==lifter.ir())?0:3;
 }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 9;}}
