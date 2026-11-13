#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <memory>
#include <chrono>
#include <llvm/IR/ModuleSlotTracker.h>
#include <map>
#include <optional>
#include <stdexcept>
using namespace dragon;
// A separate pure-output candidate. No frozen exporter or gate is changed.
std::unique_ptr<llvm::ModuleSlotTracker> slots;
std::uint64_t printedCalls=0, operandCalls=0, typedCalls=0;
llvm::ModuleSlotTracker &tracker(const llvm::Value &v) {
 const llvm::Function *f=nullptr;
 if(auto *i=llvm::dyn_cast<llvm::Instruction>(&v))f=i->getFunction();
 else if(auto *a=llvm::dyn_cast<llvm::Argument>(&v))f=a->getParent();
 else if(auto *b=llvm::dyn_cast<llvm::BasicBlock>(&v))f=b->getParent();
 if(f)slots->incorporateFunction(*f);
 return *slots;
}
std::string printed(const llvm::Value &v){++printedCalls;std::string s;llvm::raw_string_ostream out(s);v.print(out,tracker(v));return s;}
std::string operand(const llvm::Value &v){++operandCalls;std::string s;llvm::raw_string_ostream out(s);v.printAsOperand(out,false,tracker(v));return s;}
std::string typedOperand(const llvm::Value &v){++typedCalls;std::string s;llvm::raw_string_ostream out(s);v.printAsOperand(out,true,tracker(v));return s;}
std::string json(llvm::json::Value v){std::string s;llvm::raw_string_ostream out(s);out<<v;return s+"\n";}
void write(const std::string &p,const std::string &s){std::ofstream f(p,std::ios::binary);f<<s;f.close();if(!f)throw std::runtime_error("write failed: "+p);}
llvm::json::Value inventory(llvm::Module &module,const std::string &selected) {
 auto *f=module.getFunction(selected);if(!f)throw std::runtime_error("manifest view absent");
 std::map<llvm::BasicBlock *,std::size_t> ordinal;std::size_t n=0;for(auto &b:*f)ordinal[&b]=n++;
 llvm::json::Array blocks,switches,guards,checks;
 for(auto &b:*f){
  auto *term=b.getTerminator();llvm::json::Array edges,calls,callTargets,stores;
  for(unsigned slot=0;slot<term->getNumSuccessors();++slot)edges.push_back(llvm::json::Object{{"slot",slot},{"target",term->getSuccessor(slot)->getName().str()}});
  for(auto &i:b){
   if(auto *call=llvm::dyn_cast<llvm::CallBase>(&i)){calls.push_back(printed(*call));if(auto *callee=call->getCalledFunction())callTargets.push_back(callee->getName().str());}
   if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&i))stores.push_back(llvm::json::Object{{"instruction",printed(*store)},{"value",printed(*store->getValueOperand())},{"pointer",printed(*store->getPointerOperand())}});
  }
  blocks.push_back(llvm::json::Object{{"ordinal",ordinal[&b]},{"name",b.getName().str()},{"successors",std::move(edges)},{"calls",std::move(calls)},{"call_targets",std::move(callTargets)},{"stores",std::move(stores)},{"terminator",printed(*term)},{"generated",term->getMetadata("dragon.guarded-transitions.generated")!=nullptr}});
  if(auto *sw=llvm::dyn_cast<llvm::SwitchInst>(term)){
   llvm::json::Array cases,incoming;
   for(const auto &c:sw->cases())cases.push_back(llvm::json::Object{{"value",printed(*c.getCaseValue())},{"target",c.getCaseSuccessor()->getName().str()}});
   for(auto &pred:*f){auto *pt=pred.getTerminator();for(unsigned slot=0;slot<pt->getNumSuccessors();++slot)if(pt->getSuccessor(slot)==&b)incoming.push_back(llvm::json::Object{{"seed",pred.getName().str()},{"seed_ordinal",ordinal[&pred]},{"slot",slot}});}
   llvm::json::Object row{{"block",b.getName().str()},{"ordinal",ordinal[&b]},{"selector",operand(*sw->getCondition())},{"default",sw->getDefaultDest()->getName().str()},{"cases",std::move(cases)},{"incoming",std::move(incoming)}};
   if(auto *load=llvm::dyn_cast<llvm::LoadInst>(sw->getCondition())){
    row["load"]=printed(*load);row["load_block"]=load->getParent()->getName().str();row["same_block_load"]=load->getParent()==&b;row["simple_load"]=load->isSimple();row["pointer"]=printed(*load->getPointerOperand());
    if(auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand())){
     row["base"]=printed(*gep->getPointerOperand());if(gep->getNumIndices()==1){row["index"]=printed(*gep->getOperand(1));if(auto *c=llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1)))if(c->getBitWidth()==64)row["address_decimal"]=std::to_string(c->getZExtValue());}
    }
   }
   switches.push_back(std::move(row));
  }
  auto *br=llvm::dyn_cast<llvm::BranchInst>(term);if(!br||!br->isConditional())continue;
  auto *eq=llvm::dyn_cast<llvm::ICmpInst>(br->getCondition());if(!eq||eq->getPredicate()!=llvm::CmpInst::ICMP_EQ)continue;
  auto *load=llvm::dyn_cast<llvm::LoadInst>(eq->getOperand(0));auto *c=llvm::dyn_cast<llvm::ConstantInt>(eq->getOperand(1));
  if(!load||!c)continue;
  llvm::json::Object check{{"block",b.getName().str()},{"load",printed(*load)},{"constant",printed(*c)},{"success",br->getSuccessor(0)->getName().str()},{"failure",br->getSuccessor(1)->getName().str()}};
  if(auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand())) {
   if(gep->getNumIndices()==1 && gep->getSourceElementType()->isIntegerTy(8))if(auto *idx=llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1)))if(idx->getBitWidth()==64)check["address_decimal"]=std::to_string(idx->getZExtValue());
   auto *base=llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());check["canonical_guest_base"]=base && f->arg_size()>1 && base->getPointerOperand()==f->getArg(1);
  }
  checks.push_back(std::move(check));
  auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(br->getSuccessor(1)->getTerminator());
  if(!fallback || fallback->getCondition()!=load)continue;
  auto chosen=fallback->findCaseValue(c);
  const bool hasCase=chosen!=fallback->case_default();
  guards.push_back(llvm::json::Object{{"block",b.getName().str()},{"load",printed(*load)},{"load_block",load->getParent()->getName().str()},{"candidate",printed(*c)},{"success",br->getSuccessor(0)->getName().str()},{"fallback",br->getSuccessor(1)->getName().str()},{"same_value_full_switch",true},{"fallback_has_candidate_case",hasCase},{"fallback_candidate_matches_success",hasCase && chosen->getCaseSuccessor()==br->getSuccessor(0)},{"generated",term->getMetadata("dragon.guarded-transitions.generated")!=nullptr}});
 }
 return llvm::json::Object{{"function",selected},{"blocks",std::move(blocks)},{"switches",std::move(switches)},{"same_value_guards",std::move(guards)},{"equality_checks",std::move(checks)}};
}
bool verify(llvm::Module &m,std::string &error){llvm::raw_string_ostream out(error);return !llvm::verifyModule(m,&out);}
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
 if(argc!=4 && argc!=5)return 2;
 const bool generic=argc==5 && std::string(argv[4])=="base";
 if(argc==5 && !generic)return 2;
 auto start=std::chrono::steady_clock::now();
 auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)return 2;
 llvm::LLVMContext context;llvm::Module module("checked-context-inventory",context);llvm::SMDiagnostic diag;
 if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&module,nullptr,diag)){diag.print(argv[0],llvm::errs());return 3;}
 std::string error;if(!verify(module,error)){llvm::errs()<<error;return 4;}
 auto *function=module.getFunction(argv[2]);if(!function)throw std::runtime_error("manifest view absent");
 auto parseDone=std::chrono::steady_clock::now();
 slots=std::make_unique<llvm::ModuleSlotTracker>(&module);
 slots->incorporateFunction(*function);
 auto result=generic?inventory(module,argv[2]):stateInventory(module,argv[2]);
 auto rendered=std::chrono::steady_clock::now();
 write(argv[3],json(std::move(result)));
 auto end=std::chrono::steady_clock::now();
 auto seconds=[](auto a,auto b){return std::chrono::duration<double>(b-a).count();};
 llvm::errs()<<json(llvm::json::Object{{"scope","independent pure output candidate"},{"parse_verify_seconds",seconds(start,parseDone)},{"inventory_seconds",seconds(parseDone,rendered)},{"serialize_write_seconds",seconds(rendered,end)},{"printed_calls",printedCalls},{"operand_calls",operandCalls},{"typed_calls",typedCalls},{"passes_executed",0},{"native_executions",0}});
 slots.reset();return 0;
 }catch(const std::exception &e){llvm::errs()<<e.what();return 9;}}
