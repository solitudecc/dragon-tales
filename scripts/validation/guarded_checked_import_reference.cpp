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
int main(int argc,char **argv){try{
 if(argc!=4&&argc!=5)return 2;const std::string inputPath=argv[1],actualPrefix=argv[2],reportPrefix=argv[3];
 auto input=llvm::MemoryBuffer::getFile(inputPath);if(!input)return 2;
 llvm::LLVMContext context;llvm::Module module("partial-masks-values-real",context);llvm::SMDiagnostic diag;
 if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&module,nullptr,diag)){diag.print(argv[0],llvm::errs());return 3;}
 std::string error;if(!verify(module,error)){llvm::errs()<<error;return 4;}
 auto *metadata=module.getNamedMetadata("dragon.guarded-entry");if(!metadata||metadata->getNumOperands()!=1)return 4;
 auto *text=llvm::dyn_cast<llvm::MDString>(metadata->getOperand(0)->getOperand(0));if(!text)return 4;
 auto manifest=llvm::json::parse(text->getString());if(!manifest)return 4;auto *object=manifest->getAsObject();auto selected=object?object->getString("view_symbol"):std::nullopt;if(!selected)return 4;
 std::string expectedIR;llvm::raw_string_ostream out(expectedIR);module.print(out,nullptr);out.flush();auto expectedJSON=json(stateInventory(module,selected->str()));
 auto actualIR=llvm::MemoryBuffer::getFile(actualPrefix+".ll"),actualJSON=llvm::MemoryBuffer::getFile(actualPrefix+".json");if(!actualIR||!actualJSON)return 4;
 std::string ir=(*actualIR)->getBuffer().str(),inventory=(*actualJSON)->getBuffer().str();if(argc==5){if(std::string(argv[4])=="damage-ir")ir.at(ir.size()/2)^=1;else if(std::string(argv[4])=="damage-json")inventory.at(inventory.size()/2)^=1;else return 2;}
 const bool sameIR=ir==expectedIR,sameJSON=inventory==expectedJSON;
 write(reportPrefix+".json",json(llvm::json::Object{{"source_input",inputPath},{"actual_prefix",actualPrefix},{"IR_exact_independent_parse_render",sameIR},{"JSON_exact_independent_parse_render",sameJSON},{"expected_IR_bytes",expectedIR.size()},{"expected_JSON_bytes",expectedJSON.size()},{"passes_executed",0},{"target_native_executions",0},{"mutation_control",argc==5?argv[4]:"none"}}));
 if(!sameIR||!sameJSON){if(argc==4){write(reportPrefix+".expected.ll",expectedIR);write(reportPrefix+".expected.json",expectedJSON);}return 5;}return 0;
 }catch(const std::exception &e){llvm::errs()<<e.what();return 9;}}
