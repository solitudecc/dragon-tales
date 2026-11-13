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
 if(argc!=4)return 2;
 auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)return 2;
 llvm::LLVMContext context;llvm::Module module("checked-context-inventory",context);llvm::SMDiagnostic diag;
 if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&module,nullptr,diag)){diag.print(argv[0],llvm::errs());return 3;}
 std::string error;if(!verify(module,error)){llvm::errs()<<error;return 4;}
 write(argv[3],json(stateInventory(module,argv[2])));return 0;
 }catch(const std::exception &e){llvm::errs()<<e.what();return 9;}}
