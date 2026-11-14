// Diagnostic on an explicitly hypothetical table CFG. This does not prove
// table preservation and must never publish this module as a deobfuscated view.
#include <dragon/lifters/llvm/DispatchCorridor.hpp>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <map>
#include <set>
using namespace llvm;
using namespace dragon::llvmlifter::detail;
static std::string render(Module &module) {std::string s;raw_string_ostream o(s);module.print(o,nullptr);return s;}
static bool save(StringRef path,StringRef data) {std::error_code e;raw_fd_ostream f(path,e);if(e)return false;f<<data;return true;}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 LLVMContext context;SMDiagnostic error;auto module=parseIRFile(argv[1],error,context);
 if(!module){error.print(argv[0],errs());return 3;}
 if(verifyModule(*module,&errs()))return 4;
 auto *function=module->getFunction("dragon_static_entry_x86_64_1010.guarded.tigress_sp_table_rsi_v1");if(!function)return 5;
 std::map<std::string,BasicBlock *> names;for(auto &b:*function)names.emplace(b.getName().str(),&b);
 std::vector<BasicBlock *> path;
 for(auto name:{"bb_4384","bb_4390","memory.valid57"}){auto it=names.find(name);if(it==names.end())return 6;path.push_back(it->second);}
 std::set<BasicBlock *> copied(path.begin(),path.end());
 std::vector<DispatchCorridorEdge> candidates;json::Array edges;
 for(auto &b:*function)if(!copied.contains(&b)){
  auto *term=b.getTerminator();if(!isa<BranchInst,SwitchInst>(term))continue;
  for(unsigned slot=0;slot<term->getNumSuccessors();++slot)if(term->getSuccessor(slot)==path.front()){
   candidates.push_back({&b,slot,path});edges.push_back(json::Object{{"source",b.getName().str()},{"slot",slot}});
  }
 }
 if(candidates.empty())return 7;
 auto before=render(*module);if(!save(std::string(argv[2])+".before.ll",before))return 8;
 auto stats=threadDispatchCorridorBatch(*function,candidates);
 bool valid=!verifyModule(*module,&errs());auto after=render(*module);
 json::Object report{{"schema","dragon-hypothetical-dispatch-loop-candidate-v1"},
  {"table_preservation_proved",false},{"publishable_deobfuscated_view",false},{"native_runs",0},
  {"api_calls",1},{"changed",stats.changed},{"complete",stats.complete},{"budget_exhausted",stats.budgetExhausted},
  {"rewritten_edges",static_cast<int64_t>(stats.rewrittenEdges)},
  {"cloned_blocks",static_cast<int64_t>(stats.clonedBlocks)},
  {"cloned_instructions",static_cast<int64_t>(stats.clonedInstructions)},
  {"snapshots",static_cast<int64_t>(stats.snapshots)},
  {"repair_phis",static_cast<int64_t>(stats.repairPhis)},
  {"repair_cells",static_cast<int64_t>(stats.repairCells)},
  {"work",static_cast<int64_t>(stats.work)},{"message",stats.message},{"valid",valid},
  {"byte_equal",before==after},{"candidates",std::move(edges)}};
 std::string data;raw_string_ostream stream(data);stream<<formatv("{0:2}",json::Value(std::move(report)))<<"\n";
 if(!save(std::string(argv[2])+".after.ll",after)||!save(std::string(argv[2])+".json",data))return 9;
 outs()<<data;return valid && (stats.changed || before==after)?0:10;
}
