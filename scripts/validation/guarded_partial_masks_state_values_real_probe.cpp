// Independent profile58: base56 + partial-mask predictions + state-value guards.
#define main frozen_inventory_main
#include "guarded_business_inventory.cpp"
#undef main
#include <llvm/IR/Metadata.h>
#include <filesystem>
std::string typedOperand(const llvm::Value &v){std::string s;llvm::raw_string_ostream out(s);v.printAsOperand(out,true);return s;}

llvm::json::Value stateInventory(llvm::Module &module,const std::string &selected) {
 auto result=inventory(module,selected);auto *f=module.getFunction(selected);
 llvm::json::Array rows;
 for(auto &block:*f){llvm::json::Array instructions;
  for(auto &i:block){llvm::json::Array operands;
   for(auto &v:i.operands())operands.push_back(llvm::json::Object{{"operand",operand(*v)},{"typed",typedOperand(*v)}});
   llvm::json::Object item{{"operand",operand(i)},{"opcode",i.getOpcodeName()},{"instruction",printed(i)},{"operands",std::move(operands)}};
   if(auto *cmp=llvm::dyn_cast<llvm::ICmpInst>(&i))item["predicate"]=llvm::ICmpInst::getPredicateName(cmp->getPredicate()).str();
   if(auto *phi=llvm::dyn_cast<llvm::PHINode>(&i)){llvm::json::Array incoming;
    for(unsigned n=0;n<phi->getNumIncomingValues();++n)incoming.push_back(llvm::json::Object{{"block",phi->getIncomingBlock(n)->getName().str()},{"value",operand(*phi->getIncomingValue(n))},{"typed",typedOperand(*phi->getIncomingValue(n))}});
    item["incoming"]=std::move(incoming);
   }
   if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&i))item["simple"]=load->isSimple();
   if(auto *branch=llvm::dyn_cast<llvm::BranchInst>(&i))if(branch->isConditional())item["condition"]=operand(*branch->getCondition());
   instructions.push_back(std::move(item));
  }
  rows.push_back(llvm::json::Object{{"block",block.getName().str()},{"instructions",std::move(instructions)}});
 }
 (*result.getAsObject())["state_value_structure"]=std::move(rows);return result;
}
namespace {
struct Snapshot {std::string stem,ir,inventory;};
void freshWrite(const std::string &path,const std::string &text) {
 if(std::filesystem::exists(path))throw std::runtime_error("refuse to overwrite "+path);
 write(path,text);
}
void hardLink(const std::string &source,const std::string &target,
              llvm::json::Array &links) {
 if(std::filesystem::exists(target))throw std::runtime_error("refuse to overwrite "+target);
 std::filesystem::create_hard_link(source,target);
 links.push_back(llvm::json::Object{{"source",source},{"target",target},
   {"basis","exact immutable snapshot bytes"},{"archive_dereference_required",true}});
}
Snapshot snapshot(llvmlifter::LLVMLifter &l,const std::string &selected,
                  const std::string &stem,const Snapshot *previous,
                  llvm::json::Array &links) {
 Snapshot result{stem,l.ir(),json(stateInventory(l.module(),selected))};
 if(previous && result.ir==previous->ir)hardLink(previous->stem+".ll",stem+".ll",links);
 else freshWrite(stem+".ll",result.ir);
 if(previous && result.inventory==previous->inventory)hardLink(previous->stem+".json",stem+".json",links);
 else freshWrite(stem+".json",result.inventory);
 return result;
}
void aliases(const Snapshot &source,const std::string &stem,llvm::json::Array &links) {
 hardLink(source.stem+".ll",stem+".ll",links);hardLink(source.stem+".json",stem+".json",links);
}
}
int main(int argc,char **argv){try {
 if(argc!=4)throw std::runtime_error("base56.ll value-hints.json-or-null prefix required");
 const std::string prefix=argv[3];auto input=llvm::MemoryBuffer::getFile(argv[1]);auto hints=llvm::MemoryBuffer::getFile(argv[2]);
 if(!input||!hints)throw std::runtime_error("input read failed");
 llvmlifter::LLVMLifter l(Architecture::X86_64,"partial-masks-values-real");
 if(!l.setExecutionMode(llvmlifter::ExecutionMode::StaticAnalysis))throw std::runtime_error("mode failed");
 llvm::SMDiagnostic diagnostic;if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&l.module(),nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 auto &module=l.module();std::string error;if(!verify(module,error))throw std::runtime_error(error);
 const auto manifestText=l.guardedEntryManifest();auto manifest=llvm::json::parse(manifestText);
 if(!manifest)throw std::runtime_error(llvm::toString(manifest.takeError()));auto *object=manifest->getAsObject();auto view=object?object->getString("view_symbol"):std::nullopt;
 if(!view)throw std::runtime_error("view missing");const auto selected=view->str();
 if(module.getNamedMetadata("dragon.guarded-state-values"))throw std::runtime_error("base56 already contains state-value hints");
 if(!module.getNamedMetadata("dragon.indirect-target-hints"))throw std::runtime_error("base56 target hints missing");
 auto parsed=llvm::json::parse((*hints)->getBuffer());if(!parsed)throw std::runtime_error(llvm::toString(parsed.takeError()));
 if(!parsed->getAsNull()){
  auto *o=parsed->getAsObject();auto *rows=o?o->getArray("hints"):nullptr;if(!rows||rows->empty())throw std::runtime_error("invalid injection hints");
  for(const auto &row:*rows){auto *r=row.getAsObject();auto name=r?r->getString("function"):std::nullopt;if(!name||*name!=selected)throw std::runtime_error("hint view mismatch");}
 }
 freshWrite(prefix+".manifest.json",manifestText);llvm::json::Array links;
 const auto maskBefore=snapshot(l,selected,prefix+".mask.before",nullptr,links);aliases(maskBefore,prefix+".before",links);
 const passes::Options bounded{{"max_candidates",std::int64_t(64)},{"max_rewrites",std::int64_t(32)},{"max_blocks",std::int64_t(128)},{"max_instructions",std::int64_t(16384)}};
 auto stage=[&](const char *pass,const std::string &label){passes::PassPipeline p;if(!p.add(pass,bounded))throw std::runtime_error(p.lastError());passes::RunOptions opts;opts.maxIterations=1;opts.maxPassRuns=1;opts.verifyLLVM=true;
  const auto result=p.run(l,opts);freshWrite(prefix+"."+label+"-report.json",result.toJSON());std::string e;
  if(!result.ok||!verify(module,e)||l.guardedEntryManifest()!=manifestText)throw std::runtime_error("stage "+label+" failed: "+e);
 };
 stage("guarded-partial-masks","M");
 const auto maskAfter=snapshot(l,selected,prefix+".mask.after",&maskBefore,links);aliases(maskAfter,prefix+".M",links);
 // Metadata is injected after the mask stage, so each component has its own
 // real input frame. Snapshot reuse below requires exact rendered byte equality.
 if(!parsed->getAsNull())module.getOrInsertNamedMetadata("dragon.guarded-state-values")->addOperand(llvm::MDNode::get(module.getContext(),llvm::MDString::get(module.getContext(),(*hints)->getBuffer())));
 if(l.guardedEntryManifest()!=manifestText)throw std::runtime_error("entry metadata changed during hint injection");
 const auto valueBefore=snapshot(l,selected,prefix+".value.before",&maskAfter,links);
 stage("guarded-state-values","V");
 const auto valueAfter=snapshot(l,selected,prefix+".value.after",&valueBefore,links);aliases(valueAfter,prefix+".V",links);aliases(valueAfter,prefix+".after",links);
 // No transformation or second dump occurs between the V output and final IR.
 error.clear();const bool valid=verify(module,error);
 freshWrite(prefix+".bindings.json",json(llvm::json::Object{
   {"schema","partial-masks-state-values-profile58-v1"},{"source_input",argv[1]},{"value_hint_input",argv[2]},
   {"mask_before",maskBefore.stem},{"mask_after",maskAfter.stem},{"value_before",valueBefore.stem},{"value_after",valueAfter.stem},{"final",prefix+".after"},
   {"hint_injection","only after M, before V; untrusted predictions"},{"exact_byte_hardlinks",std::move(links)},
   {"archive_dereference_required",true},{"inventory_scope","same selected function; every pass has its actual before/after frame"}}));
 freshWrite(prefix+".verify.json",json(llvm::json::Object{{"mode","base56-plus-mask1-value1"},{"inherited_runs",56},{"additional_runs",2},{"total_runs",58},{"after",valid},{"entry_manifest_unchanged",l.guardedEntryManifest()==manifestText},{"error",error},{"native_executions",0}}));
 return valid&&l.guardedEntryManifest()==manifestText?0:3;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 9;}}
