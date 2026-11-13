// Independent bit-slice-only step60 v1 I/O. Sealed profile59 is never rerun.
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
 if(argc!=6)throw std::runtime_error("input59.ll independent.expected.ll independent.expected.json source59.inventory.json prefix required");
 const std::string prefix=argv[5];auto input=llvm::MemoryBuffer::getFile(argv[1]);
 auto expectedIR=llvm::MemoryBuffer::getFile(argv[2]),expectedJSON=llvm::MemoryBuffer::getFile(argv[3]),sourceJSON=llvm::MemoryBuffer::getFile(argv[4]);
 if(!input||!expectedIR||!expectedJSON||!sourceJSON)throw std::runtime_error("input or independently rendered import frame missing");
 // Keep the inherited producer's ModuleIdentifier. No instruction, comment,
 // metadata ID or source text is normalized to make an import check pass.
 llvmlifter::LLVMLifter l(Architecture::X86_64,"partial-masks-values-real");
 if(!l.setExecutionMode(llvmlifter::ExecutionMode::StaticAnalysis))throw std::runtime_error("mode failed");
 llvm::SMDiagnostic diagnostic;if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&l.module(),nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 auto &module=l.module();std::string error;if(!verify(module,error))throw std::runtime_error(error);
 const auto manifestText=l.guardedEntryManifest();auto manifest=llvm::json::parse(manifestText);
 if(!manifest)throw std::runtime_error(llvm::toString(manifest.takeError()));auto *object=manifest->getAsObject();auto view=object?object->getString("view_symbol"):std::nullopt;
 if(!view)throw std::runtime_error("view missing");const auto selected=view->str();
 if(!module.getNamedMetadata("dragon.indirect-target-hints"))throw std::runtime_error("inherited target hints missing");
 freshWrite(prefix+".manifest.json",manifestText);llvm::json::Array links;
 const Snapshot before{prefix+".bits.before",l.ir(),json(stateInventory(module,selected))};
 const bool expectedIRMatch=before.ir==(*expectedIR)->getBuffer();
 const bool expectedJSONMatch=before.inventory==(*expectedJSON)->getBuffer();
 const bool sourceJSONMatch=before.inventory==(*sourceJSON)->getBuffer();
 // Actual complete renders are compared first. A cached hash or copied
 // reference inventory never substitutes for rendering the actual Module.
 if(expectedIRMatch&&expectedJSONMatch&&sourceJSONMatch){
  hardLink(argv[2],before.stem+".ll",links);hardLink(argv[4],before.stem+".json",links);
 }else{
  // A failed import retains BOTH unmodified actual renders before any pass.
  freshWrite(before.stem+".ll",before.ir);freshWrite(before.stem+".json",before.inventory);
 }
 aliases(before,prefix+".before",links);
 freshWrite(prefix+".import.json",json(llvm::json::Object{{"source_input",argv[1]},{"independent_expected_IR",argv[2]},{"independent_expected_inventory",argv[3]},{"source59_inventory",argv[4]},
   {"actual_before",before.stem},{"IR_matches_independent_parse_render",expectedIRMatch},{"inventory_matches_independent_parse_render",expectedJSONMatch},{"inventory_byte_equal_source59",sourceJSONMatch},
   {"source_IR_byte_equal_actual_before",before.ir==(*input)->getBuffer()},{"normalization_performed",false},{"candidate_pass_runs_at_import_check",0}}));
 if(!expectedIRMatch||!expectedJSONMatch||!sourceJSONMatch)throw std::runtime_error("bit-slice import frame differs; candidate pass not executed");
 const passes::Options bounded{{"max_candidates",std::int64_t(64)},{"max_rewrites",std::int64_t(32)},{"max_blocks",std::int64_t(128)},{"max_instructions",std::int64_t(16384)}};
 passes::PassPipeline pipeline;if(!pipeline.add("guarded-bit-slices",bounded))throw std::runtime_error(pipeline.lastError());
 passes::RunOptions options;options.maxIterations=1;options.maxPassRuns=1;options.verifyLLVM=true;
 const auto report=pipeline.run(l,options);freshWrite(prefix+".bits-report.json",report.toJSON());
 if(!report.ok||!verify(module,error)||l.guardedEntryManifest()!=manifestText)throw std::runtime_error("bit-slice stage failed: "+error);
 const auto after=snapshot(l,selected,prefix+".bits.after",&before,links);aliases(after,prefix+".BS",links);aliases(after,prefix+".after",links);
 freshWrite(prefix+".bindings.json",json(llvm::json::Object{{"schema","guarded-bit-slices-step60-v1"},{"source_input",argv[1]},{"source59_inventory",argv[4]},
   {"independent_expected_IR",argv[2]},{"independent_expected_inventory",argv[3]},{"bits_before",before.stem},{"bits_after",after.stem},{"final",prefix+".after"},
   {"view_symbol",selected},{"hint_injection","none; inherited M/V/C/table predictions remain untrusted"},{"exact_byte_hardlinks",std::move(links)},
   {"archive_dereference_required",true},{"alias_inode_length_sha256_receipt_required",true},{"import_deduplication_basis","all three actual full-render equality checks before any import-frame hardlink"},{"inventory_scope","same selected function with complete instruction operands PHIs and actual successor slots"}}));
 freshWrite(prefix+".verify.json",json(llvm::json::Object{{"mode","sealed59-plus-bit-slices1"},{"declared_inherited_runs",59},{"additional_runs",1},{"total_runs",60},
   {"after",true},{"entry_manifest_unchanged",l.guardedEntryManifest()==manifestText},{"import_checked_before_candidate",true},{"native_executions",0}}));
 return 0;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 9;}}
