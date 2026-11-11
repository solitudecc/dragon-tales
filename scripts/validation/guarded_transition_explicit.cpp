// Explicit-IR driver: names are selectors, never production proof assumptions.
#include "GuardedTransition.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace dragon::llvmlifter::detail;
namespace {
std::vector<std::string> split(const std::string &s) {
  if(s=="-") return {};
  std::vector<std::string> out; std::stringstream input(s); std::string token;
  while(std::getline(input,token,',')) out.push_back(token);
  return out;
}
llvm::BasicBlock *findBlock(llvm::Function &f,const std::string &name) {
  for(auto &b:f) if(b.getName()==name) return &b;
  throw std::runtime_error("missing block: "+name);
}
void save(llvm::Module &module,const std::string &path) {
  std::error_code error; llvm::raw_fd_ostream out(path,error);
  if(error) throw std::runtime_error(error.message());
  module.print(out,nullptr);
}
}
int main(int argc,char **argv) {
  if(argc!=10 && argc!=12) {
    std::cerr<<"usage: guarded-transition-explicit INPUT OUTPUT REPORT FUNCTION SEED SLOT CANDIDATE BLOCKS_CSV FORWARD_SLOTS_CSV [MAX_BLOCKS MAX_INSTRUCTIONS]\n";
    return 2;
  }
  try {
    llvm::LLVMContext context; llvm::SMDiagnostic diagnostic;
    auto module=llvm::parseIRFile(argv[1],diagnostic,context);
    if(!module) {diagnostic.print(argv[0],llvm::errs());return 3;}
    if(llvm::verifyModule(*module,&llvm::errs())) return 4;
    auto *function=module->getFunction(argv[4]);
    if(!function) throw std::runtime_error("missing function");
    GuardedTransitionRequest request;
    request.seed=findBlock(*function,argv[5]);request.seedSuccessor=std::stoul(argv[6]);
    llvm::json::Array path,slots;
    for(const auto &name:split(argv[8])) {
      request.blocks.push_back(findBlock(*function,name));path.push_back(name);
    }
    for(const auto &slot:split(argv[9])) {
      const auto value=std::stoul(slot);
      request.forwardSuccessors.push_back(value);slots.push_back(static_cast<int64_t>(value));
    }
    if(request.blocks.empty()) throw std::runtime_error("empty path");
    request.seedTarget=request.blocks.front();
    auto *jump=llvm::dyn_cast<llvm::SwitchInst>(request.blocks.back()->getTerminator());
    if(!jump || !jump->getCondition()->getType()->isIntegerTy())
      throw std::runtime_error("terminal switch not integer");
    const auto candidate=std::stoull(argv[7],nullptr,0);
    auto *type=llvm::cast<llvm::IntegerType>(jump->getCondition()->getType());
    if(type->getBitWidth()<64 && (candidate>>type->getBitWidth()))
      throw std::runtime_error("candidate does not fit selector");
    request.candidate=llvm::ConstantInt::get(type,candidate);
    GuardedTransitionOptions options;
    if(argc==12) {options.maxBlocks=std::stoull(argv[10]);options.maxInstructions=std::stoull(argv[11]);}
    // Parsing can change textual use-list order. Preserve the parsed baseline.
    save(*module,std::string(argv[2])+".parsed-before.ll");
    const auto result=cloneGuardedTransition(*function,request,options);
    const bool valid=!llvm::verifyModule(*module,&llvm::errs());
    save(*module,argv[2]);
    llvm::json::Object report{
      {"schema","guarded-transition-explicit-v1"},
      {"input",argv[1]},{"output",argv[2]},{"function",argv[4]},
      {"seed",argv[5]},{"seed_successor",static_cast<int64_t>(std::stoul(argv[6]))},
      {"path",std::move(path)},{"forward_successors",std::move(slots)},
      {"candidate",argv[7]},{"candidate_is_prediction_only",true},
      {"max_blocks",static_cast<int64_t>(options.maxBlocks)},
      {"max_instructions",static_cast<int64_t>(options.maxInstructions)},
      {"changed",result.changed},{"complete",result.complete},
      {"rejected",result.rejected},{"budget_exhausted",result.budgetExhausted},
      {"cloned_blocks",static_cast<int64_t>(result.clonedBlocks)},
      {"cloned_instructions",static_cast<int64_t>(result.clonedInstructions)},
      {"snapshot_phis",static_cast<int64_t>(result.snapshotPhis)},
      {"repair_phis",static_cast<int64_t>(result.repairPhis)},
      {"original_exit_edges",static_cast<int64_t>(result.originalExitEdges)},
      {"message",result.message},{"verified_output",valid},
      {"native_validation_performed",false},{"full_cff_completion_claimed",false}
    };
    std::error_code error;llvm::raw_fd_ostream output(argv[3],error);
    if(error) throw std::runtime_error(error.message());
    output<<llvm::formatv("{0:2}",llvm::json::Value(std::move(report)))<<"\n";
    return !valid?5:result.changed?0:6;
  } catch(const std::exception &error) {
    std::cerr<<error.what()<<"\n";return 2;
  }
}
