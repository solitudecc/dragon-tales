#include "LLVMPhiLowBits.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
using namespace llvm;
using namespace dragon::llvmlifter::detail;
namespace {
std::string moduleText(Module &m) { std::string s;raw_string_ostream out(s);m.print(out,nullptr);return s; }
std::string instructionText(Instruction &i) { std::string s;raw_string_ostream out(s);i.print(out);return s; }
bool effect(Instruction &i) { return i.isTerminator() || isa<CallBase,AllocaInst>(i) || i.mayReadOrWriteMemory() || i.mayHaveSideEffects(); }
struct Saved {
  Instruction *i;BasicBlock *block;std::vector<Value *> operands;
  SmallVector<std::pair<unsigned,MDNode *>,4> metadata;
};
std::vector<Saved> save(Function &f) {
  std::vector<Saved> result;
  for(auto &b:f)for(auto &i:b)if(effect(i)) {
    Saved row{&i,&b,{},{}};for(auto &u:i.operands())row.operands.push_back(u.get());
    i.getAllMetadata(row.metadata);result.push_back(std::move(row));
  }
  return result;
}
bool sameEffects(Function &f,const std::vector<Saved> &old) {
  auto now=save(f);if(now.size()!=old.size())return false;
  for(unsigned n=0;n<now.size();++n)
    if(now[n].i!=old[n].i || now[n].block!=old[n].block || now[n].operands!=old[n].operands || now[n].metadata!=old[n].metadata)return false;
  return true;
}
struct Candidate {std::string function;unsigned ordinal;std::string printed;};
std::unique_ptr<Module> parse(const char *path,LLVMContext &context) {
  SMDiagnostic error;auto m=parseIRFile(path,error,context);
  if(!m)error.print("phi-low-real-probe",errs());return m;
}
}
int main(int argc,char **argv) {
  if(argc!=4)return 2;
  LLVMContext initialContext;auto initial=parse(argv[1],initialContext);if(!initial || verifyModule(*initial,&errs()))return 3;
  std::vector<Candidate> candidates;unsigned blocks=0;
  for(auto &f:*initial)for(auto &b:f)if(b.getName()==argv[2]) {
    ++blocks;unsigned ordinal=0;
    for(auto &i:b) {
      if(auto *t=dyn_cast<TruncInst>(&i);t && t->getSrcTy()->isIntegerTy(64) && t->getDestTy()->isIntegerTy(32))
        candidates.push_back({f.getName().str(),ordinal,instructionText(i)});
      ++ordinal;
    }
  }
  json::Array rows;bool gates=blocks==1 && !candidates.empty();
  for(const auto &candidate:candidates) {
    LLVMContext context;auto m=parse(argv[1],context);if(!m || verifyModule(*m,&errs()))return 4;
    auto *f=m->getFunction(candidate.function);if(!f)return 5;
    BasicBlock *block=nullptr;for(auto &b:*f)if(b.getName()==argv[2])block=&b;
    if(!block || block->size()<=candidate.ordinal)return 6;
    auto it=block->begin();std::advance(it,candidate.ordinal);auto *t=dyn_cast<TruncInst>(&*it);
    if(!t || instructionText(*t)!=candidate.printed)return 7;
    auto before=moduleText(*m);auto effects=save(*f);auto *originalFunction=f;auto *originalBlock=block;
    std::vector<Argument *> arguments;for(auto &a:f->args())arguments.push_back(&a);
    const bool beforeVerified=!verifyModule(*m,&errs());
    auto stats=replacePhiLowTruncs(*f,{t});
    const bool afterVerified=!verifyModule(*m,&errs());
    auto after=moduleText(*m);
    bool argumentsSame=true;unsigned n=0;for(auto &a:f->args())argumentsSame &= n<arguments.size() && arguments[n++]==&a;
    const bool identity=f==originalFunction && t->getParent()==originalBlock && block==originalBlock && argumentsSame;
    const bool effectsSame=sameEffects(*f,effects),refusalExact=stats.complete || before==after;
    json::Array phis;for(auto *phi:stats.provedLowPhis)phis.push_back(json::Object{{"block",phi->getParent()->getName().str()},{"instruction",instructionText(*phi)}});
    const bool rowGate=beforeVerified && afterVerified && identity && effectsSame && refusalExact;
    gates &= rowGate;
    rows.push_back(json::Object{{"function",candidate.function},{"block",std::string(argv[2])},{"instruction_ordinal",candidate.ordinal},
      {"candidate",candidate.printed},{"changed",stats.changed},{"complete",stats.complete},{"budget_exhausted",stats.budgetExhausted},
      {"candidates_visited",stats.candidatesVisited},{"pairs_visited",stats.pairsVisited},{"rewritten_truncs",stats.rewrittenTruncs},
      {"rewritten_uses",stats.rewrittenUses},{"nodes",stats.nodes},{"work",stats.work},{"message",stats.message},
      {"proved_low_phis",std::move(phis)},{"verify_before",beforeVerified},{"verify_after",afterVerified},
      {"original_objects_retained",identity},{"effect_nodes_operands_metadata_exact",effectsSame},{"refusal_byte_exact",refusalExact},
      {"full_module_byte_equal",before==after},{"original_trunc_retained",t->getParent()==block},{"gate",rowGate}});
  }
  json::Array maps;std::ifstream proc("/proc/self/maps");std::string line;while(std::getline(proc,line))
    if(line.find("libdragon.so")!=std::string::npos || line.find("libz3.so")!=std::string::npos)maps.push_back(line);
  json::Object report{{"schema","phi-low-bits-saved-hypothetical-diagnostic-v1"},{"input",std::string(argv[1])},
    {"selected_block",std::string(argv[2])},{"matching_blocks",blocks},{"candidate_count",candidates.size()},
    {"fresh_parse_per_candidate",true},{"default_caps",json::Object{{"max_candidates",128},{"max_nodes",256},{"max_depth",32},{"max_work",65536}}},
    {"rows",std::move(rows)},{"maps",std::move(maps)},{"gate",gates},{"publishable",false},{"guest_native_executions",0},
    {"scope","Installed helper only on frozen hypothetical postBatch IR; no table or C preservation proof"}};
  std::error_code error;raw_fd_ostream out(argv[3],error,sys::fs::OF_Text);if(error)return 8;
  out<<formatv("{0:2}\n",json::Value(std::move(report)));return gates?0:9;
}
