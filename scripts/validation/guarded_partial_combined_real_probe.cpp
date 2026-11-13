#define main frozen_inventory_main
#include "guarded_business_inventory.cpp"
#undef main
#include <llvm/IR/Metadata.h>
int main(int argc,char **argv){try {
 if(argc==2&&std::string(argv[1])=="--smoke"){
  llvmlifter::LLVMLifter l(Architecture::X86_64,"smoke");passes::PassPipeline p;
  for(const auto *name:{"invariant-self-loops","guarded-partial-corridors"})if(!p.add(name))throw std::runtime_error(p.lastError());return llvm::verifyModule(l.module())?8:0;
 }
 bool full=argc==5&&std::string(argv[1])=="--full";bool tail=argc==4&&std::string(argv[1])=="--tail";
 if(!full&&!tail)throw std::runtime_error("--tail input.ll prefix OR --full D37.ll hints.json prefix required");
 std::string prefix=argv[argc-1];auto input=llvm::MemoryBuffer::getFile(argv[2]);if(!input)throw std::runtime_error("input read failed");
 llvmlifter::LLVMLifter l(Architecture::X86_64,"target-set-real-probe");if(!l.setExecutionMode(llvmlifter::ExecutionMode::StaticAnalysis))throw std::runtime_error("mode failed");llvm::SMDiagnostic diagnostic;if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&l.module(),nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 auto &module=l.module();std::string error;if(!verify(module,error))throw std::runtime_error(error);const auto manifestText=l.guardedEntryManifest();auto manifest=llvm::json::parse(manifestText);if(!manifest)throw std::runtime_error(llvm::toString(manifest.takeError()));auto *object=manifest->getAsObject();auto view=object?object->getString("view_symbol"):std::nullopt;if(!view)throw std::runtime_error("view missing");std::string selected=view->str();
 write(prefix+".manifest.json",manifestText);write(prefix+".before.ll",l.ir());write(prefix+".before.json",json(inventory(module,selected)));
 if(full){
  if(module.getNamedMetadata("dragon.indirect-target-hints"))throw std::runtime_error("preexisting D37 hints");auto hints=llvm::MemoryBuffer::getFile(argv[3]);if(!hints)throw std::runtime_error("hints read failed");auto parsed=llvm::json::parse((*hints)->getBuffer());if(!parsed)throw std::runtime_error(llvm::toString(parsed.takeError()));auto *hint=parsed->getAsObject();auto function=hint?hint->getString("function"):std::nullopt;if(!function||*function!=selected)throw std::runtime_error("hint view mismatch");auto *node=module.getOrInsertNamedMetadata("dragon.indirect-target-hints");node->addOperand(llvm::MDNode::get(module.getContext(),llvm::MDString::get(module.getContext(),(*hints)->getBuffer())));
 }else if(!module.getNamedMetadata("dragon.indirect-target-hints"))throw std::runtime_error("frozen tail input lacks existing hints");
 if(l.guardedEntryManifest()!=manifestText)throw std::runtime_error("entry metadata changed");write(prefix+".with-hints.ll",l.ir());
 auto stage=[&](const std::string &name,const std::vector<std::pair<std::string,passes::Options>> &steps){passes::PassPipeline p;for(const auto &[n,o]:steps)if(!p.add(n,o))throw std::runtime_error(p.lastError());passes::RunOptions opts;opts.maxIterations=1;opts.maxPassRuns=steps.size();opts.verifyLLVM=true;auto report=p.run(l,opts);write(prefix+"."+name+"-report.json",report.toJSON());write(prefix+"."+name+".ll",l.ir());write(prefix+"."+name+".json",json(inventory(module,selected)));std::string e;if(!report.ok||!verify(module,e)||l.guardedEntryManifest()!=manifestText)throw std::runtime_error("stage "+name+" failed: "+e);};
 const passes::Options bounded{{"max_candidates",std::int64_t(64)},{"max_rewrites",std::int64_t(32)},{"max_blocks",std::int64_t(128)},{"max_instructions",std::int64_t(16384)}};
 const passes::Options fold{{"max_folds",std::int64_t(4096)},{"max_instructions",std::int64_t(1000000)},{"max_blocks",std::int64_t(65536)}};
 const std::vector<std::pair<std::string,passes::Options>> tiny{{"mem2reg",{}},{"instcombine",{}},{"gvn",{}},{"instcombine",{}},{"dce",{}}};
 if(full){stage("F",{{"guarded-state-corridors",bounded}});stage("H",{{"guarded-store-corridors",bounded}});stage("H-resume",{{"guarded-store-corridors",bounded}});stage("tiny",tiny);stage("constant",{{"constant-branches",fold}});stage("base-E",{{"guarded-target-sets",bounded}});}
 stage("K",{{"invariant-self-loops",{{"max_loops",std::int64_t(64)},{"max_work",std::int64_t(1000000)},{"max_blocks",std::int64_t(65536)}}}});
 stage("J",{{"guarded-partial-corridors",bounded}});stage("tail-tiny",tiny);stage("tail-constant",{{"constant-branches",fold}});stage("tail-E",{{"guarded-target-sets",bounded}});
 write(prefix+".after.ll",l.ir());write(prefix+".after.json",json(inventory(module,selected)));error.clear();bool valid=verify(module,error);write(prefix+".verify.json",json(llvm::json::Object{{"mode",full?"full56":"tail9"},{"after",valid},{"entry_manifest_unchanged",l.guardedEntryManifest()==manifestText},{"error",error},{"native_executions",0}}));return valid&&l.guardedEntryManifest()==manifestText?0:3;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 9;}}
