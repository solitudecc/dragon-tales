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
#include <map>
#include <optional>
#include <stdexcept>
using namespace dragon;
std::string printed(const llvm::Value &v){std::string s;llvm::raw_string_ostream out(s);v.print(out);return s;}
std::string operand(const llvm::Value &v){std::string s;llvm::raw_string_ostream out(s);v.printAsOperand(out,false);return s;}
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
int main(int argc,char **argv){try {
 if(argc!=3)throw std::runtime_error("input.ll output.json required");
 llvmlifter::LLVMLifter lifter(Architecture::X86_64,"inventory-only");
 if(!lifter.setExecutionMode(llvmlifter::ExecutionMode::StaticAnalysis))throw std::runtime_error("mode failed");
 auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)throw std::runtime_error("input read failed");
 llvm::SMDiagnostic diagnostic;if(llvm::parseAssemblyInto((*input)->getMemBufferRef(),&lifter.module(),nullptr,diagnostic)){diagnostic.print(argv[0],llvm::errs());return 2;}
 std::string error;if(!verify(lifter.module(),error))throw std::runtime_error(error);
 auto manifest=llvm::json::parse(lifter.guardedEntryManifest());if(!manifest)throw std::runtime_error(llvm::toString(manifest.takeError()));
 auto *object=manifest->getAsObject();auto name=object?object->getString("view_symbol"):std::nullopt;
 if(!name)throw std::runtime_error("manifest view absent");
 write(argv[2],json(inventory(lifter.module(),name->str())));return 0;
 }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 9;}}
