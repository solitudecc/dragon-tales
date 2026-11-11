#define main frozen_inventory_main
#include "guarded_business_inventory.cpp"
#undef main
llvm::json::Object loopRow(llvm::BasicBlock &b){
 llvm::json::Array phis,instructions;
 for(auto &i:b){instructions.push_back(printed(i));if(auto *p=llvm::dyn_cast<llvm::PHINode>(&i)){
  llvm::json::Array incoming;for(unsigned n=0;n<p->getNumIncomingValues();++n)incoming.push_back(llvm::json::Object{{"block",p->getIncomingBlock(n)->getName().str()},{"value",operand(*p->getIncomingValue(n))}});
  phis.push_back(llvm::json::Object{{"value",operand(*p)},{"incoming",std::move(incoming)}});
 }}
 auto *br=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());llvm::json::Object row{{"block",b.getName().str()},{"phis",std::move(phis)},{"instructions",std::move(instructions)},{"branch",br!=nullptr}};
 if(br){row["conditional"]=br->isConditional();llvm::json::Array targets;for(unsigned n=0;n<br->getNumSuccessors();++n)targets.push_back(br->getSuccessor(n)->getName().str());row["targets"]=std::move(targets);if(br->isConditional())row["condition"]=printed(*br->getCondition());}
 return row;
}
int main(int argc,char **argv){try{
 if(argc==2&&std::string(argv[1])=="--smoke"){llvmlifter::LLVMLifter l(Architecture::X86_64,"smoke");passes::PassPipeline p;if(!p.add("invariant-self-loops"))throw std::runtime_error(p.lastError());return llvm::verifyModule(l.module())?8:0;}
 if(argc!=3)throw std::runtime_error("input.ll output-prefix required");std::string prefix=argv[2];auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)throw std::runtime_error("read failed");
 llvmlifter::LLVMLifter l(Architecture::X86_64,"invariant-self-loop-real");if(!l.setExecutionMode(llvmlifter::ExecutionMode::StaticAnalysis))throw std::runtime_error("mode failed");llvm::SMDiagnostic d;if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&l.module(),nullptr,d)){d.print(argv[0],llvm::errs());return 2;}
 auto manifestText=l.guardedEntryManifest();auto m=llvm::json::parse(manifestText);if(!m)throw std::runtime_error(llvm::toString(m.takeError()));auto *o=m->getAsObject();auto view=o?o->getString("view_symbol"):std::nullopt;if(!view)throw std::runtime_error("view missing");std::string selected=view->str(),error;auto &module=l.module();if(!verify(module,error))throw std::runtime_error(error);
 write(prefix+".manifest.json",manifestText);write(prefix+".before.ll",l.ir());write(prefix+".before.json",json(inventory(module,selected)));
 std::vector<std::pair<std::string,std::string>> candidates;llvm::json::Array before;
 for(auto &f:module)for(auto &b:f)if(auto *br=llvm::dyn_cast<llvm::BranchInst>(b.getTerminator());br&&br->isConditional()&&(br->getSuccessor(0)==&b||br->getSuccessor(1)==&b)){
  auto row=loopRow(b);row["function"]=f.getName().str();before.push_back(std::move(row));candidates.emplace_back(f.getName().str(),b.getName().str());
 }
 write(prefix+".loops-before.json",json(std::move(before)));passes::PassPipeline p;if(!p.add("invariant-self-loops",{{"max_loops",std::int64_t(64)},{"max_work",std::int64_t(1000000)},{"max_blocks",std::int64_t(65536)}}))throw std::runtime_error(p.lastError());write(prefix+".description.json",p.describeJSON());passes::RunOptions options;options.maxIterations=1;options.maxPassRuns=1;options.verifyLLVM=true;auto report=p.run(l,options);write(prefix+".report.json",report.toJSON());
 llvm::json::Array after;
 for(auto &[fn,bn]:candidates){auto *f=module.getFunction(fn);if(!f)throw std::runtime_error("function removed");llvm::BasicBlock *b=nullptr;for(auto &bb:*f)if(bb.getName()==bn)b=&bb;if(!b)throw std::runtime_error("original block removed");auto row=loopRow(*b);row["function"]=fn;auto *br=llvm::dyn_cast<llvm::BranchInst>(b->getTerminator());if(br&&br->isConditional()&&br->getSuccessor(0)!=b){row["true_target_body"]=loopRow(*br->getSuccessor(0));}after.push_back(std::move(row));}
 write(prefix+".loops-after.json",json(std::move(after)));write(prefix+".after.ll",l.ir());write(prefix+".after.json",json(inventory(module,selected)));bool valid=verify(module,error);write(prefix+".verify.json",json(llvm::json::Object{{"after",valid},{"entry_manifest_unchanged",manifestText==l.guardedEntryManifest()},{"error",error},{"native_executions",0}}));return report.ok&&valid&&manifestText==l.guardedEntryManifest()?0:3;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 9;}}
