// Independent fresh-context parse/verify/render. It never constructs a lifter
// or runs a pass. Its helper source is the separately byte-validated pure printer.
#define main shared_inventory_output_main
#include "checked_context_inventory_shared.cpp"
#undef main
#include <filesystem>

int main(int argc,char **argv) {try {
 if(argc!=4)throw std::runtime_error("input.ll selected-function prefix required");
 const std::string prefix=argv[3];
 if(std::filesystem::exists(prefix+".ll") || std::filesystem::exists(prefix+".json"))throw std::runtime_error("refuse overwrite reference");
 auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)throw std::runtime_error("source read failed");
 llvm::LLVMContext context;llvm::Module module("partial-masks-values-real",context);llvm::SMDiagnostic diagnostic;
 if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&module,nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 std::string error;if(!verify(module,error))throw std::runtime_error(error);
 auto *function=module.getFunction(argv[2]);if(!function)throw std::runtime_error("selected function absent");
 std::string ir;llvm::raw_string_ostream out(ir);module.print(out,nullptr);out.flush();
 slots=std::make_unique<llvm::ModuleSlotTracker>(&module);slots->incorporateFunction(*function);
 const auto inventoryJSON=json(stateInventory(module,argv[2]));slots.reset();
 write(prefix+".ll",ir);write(prefix+".json",inventoryJSON);
 write(prefix+".scope.json",json(llvm::json::Object{{"full_module_parsed_and_verified",true},
   {"full_LL_rendered",true},{"full_selected_JSON_rendered",true},{"ModuleIdentifier","partial-masks-values-real"},
   {"passes_executed",0},{"target_native_executions",0},{"normalization",false}}));return 0;
 }catch(const std::exception &e){slots.reset();llvm::errs()<<e.what()<<'\n';return 9;}}
