// Read-only validation inventory. Does not run a pass or any Guest code.
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <optional>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
using namespace llvm;
namespace {
void need(bool v,const Twine&w){if(!v)throw std::runtime_error(w.str());}
std::string sha(StringRef s){auto b=SHA256::hash(ArrayRef<std::uint8_t>((const std::uint8_t*)s.data(),s.size()));constexpr char h[]="0123456789abcdef";std::string x;for(auto v:b){x+=h[v>>4];x+=h[v&15];}return x;}
std::string render(Value&v){std::string s;raw_string_ostream o(s);v.print(o);return s;}
std::string render(Module&m){std::string s;raw_string_ostream o(s);m.print(o,nullptr);return s;}
struct Constants {
 const DataLayout&dl;std::map<Value*,Constant*>cache;std::set<Value*>active;unsigned work=0;
 Constant *get(Value*v,unsigned depth=0){
  if(depth>128||++work>65536||!v->getType()->isIntegerTy()||v->getType()->getIntegerBitWidth()>64)return nullptr;
  if(auto*c=dyn_cast<ConstantInt>(v))return c;
  if(auto it=cache.find(v);it!=cache.end())return it->second;
  auto*i=dyn_cast<Instruction>(v);if(!i||(!isa<BinaryOperator>(i)&&!isa<CastInst>(i))||!active.insert(v).second)return nullptr;
  SmallVector<Constant*,4>ops;bool all=true;for(auto&o:i->operands()){auto*c=get(o.get(),depth+1);if(!c){all=false;break;}ops.push_back(c);}active.erase(v);
  auto*c=all?ConstantFoldInstOperands(i,ops,dl):nullptr;if(!isa_and_nonnull<ConstantInt>(c))c=nullptr;cache.emplace(v,c);return c;
 }
 std::optional<std::uint64_t> offset(Value*p,Value*base,unsigned depth=0){
  if(p==base)return 0;if(depth>16)return {};
  auto*g=dyn_cast<GetElementPtrInst>(p);if(!g||!g->getSourceElementType()->isIntegerTy(8)||g->getNumIndices()!=1)return {};
  auto parent=offset(g->getPointerOperand(),base,depth+1);auto*c=dyn_cast_or_null<ConstantInt>(get(g->idx_begin()->get()));
  if(!parent||!c||c->getBitWidth()!=64)return {};return *parent+c->getZExtValue();
 }
};
std::uint64_t number(StringRef s){std::uint64_t v=0;need(!s.getAsInteger(0,v),"invalid integer");return v;}
}
int main(int argc,char**argv){
 try{
  need(argc==9,"usage: inventory SOURCE HASH VIEW ORDINARY TABLE_LO TABLE_LAST_BYTE REQUIRED_COUNT NEW_JSON");
  auto raw=MemoryBuffer::getFile(argv[1]);need(bool(raw),"source missing");need(sha((*raw)->getBuffer())==argv[2],"source hash changed");
  LLVMContext context;SMDiagnostic error;auto m=parseAssemblyString((*raw)->getBuffer(),error,context);if(!m){error.print(argv[0],errs());throw std::runtime_error("source parse failed");}
  std::string why;raw_string_ostream errors(why);need(!verifyModule(*m,&errors),"invalid source module: "+errors.str());
  auto*f=m->getFunction(argv[3]);auto*ordinary=m->getFunction(argv[4]);need(f&&ordinary&&f!=ordinary&&!f->isDeclaration(),"view/ordinary missing");need(f->arg_size()==4,"unexpected function interface");
  auto original=render(*m);Value*guest=nullptr;
  for(auto&i:f->getEntryBlock())if(auto*l=dyn_cast<LoadInst>(&i);l&&l->isSimple()&&l->getType()->isPointerTy()&&l->getPointerOperand()==f->getArg(1)){need(!guest,"ambiguous Guest snapshot");guest=l;}
  need(guest,"actual Guest base snapshot absent");BasicBlock*fallback=nullptr;
  for(auto&b:*f)for(auto&i:b)if(auto*c=dyn_cast<CallInst>(&i);c&&c->getCalledFunction()==ordinary){need(!fallback,"ambiguous ordinary fallback");fallback=&b;}
  need(fallback,"ordinary fallback absent");const auto lo=number(argv[5]),last=number(argv[6]),required=number(argv[7]);need(lo<=last&&last-lo>=7,"invalid table interval");
  Constants constants{m->getDataLayout()};json::Array rows,guards;std::uint64_t bn=0,total=0;
  for(auto&b:*f){std::uint64_t in=0;for(auto&i:b){auto*l=dyn_cast<LoadInst>(&i);if(l&&l->isSimple()&&l->getType()->isIntegerTy(64)){
    auto at=constants.offset(l->getPointerOperand(),guest);if(at&&*at>=lo&&*at<=last-7){
     ++total;need((*at-lo)%8==0,"table-region i64 read is not a word slot");auto*t=b.getTerminator();auto load=render(*l),term=render(*t);
     json::Object row{{"block_ordinal",bn},{"instruction_ordinal",in},{"block_name",b.getName().str()},{"address_offset",*at},{"load_ir",load},{"load_ir_sha256",sha(load)},{"terminator_ir",term},{"terminator_ir_sha256",sha(term)},{"simple_i64_Guest_base_exact_SSA",true}};
     if(l->use_empty())rows.push_back(std::move(row));else{
      need(l->hasOneUse(),"used table LOAD is not one original entry comparison");auto*cmp=dyn_cast<ICmpInst>(*l->user_begin());need(cmp&&cmp->getPredicate()==ICmpInst::ICMP_EQ&&cmp->hasOneUse(),"used table LOAD is not entry equality");
      auto*other=cmp->getOperand(cmp->getOperand(0)==l?1:0);need(isa<ConstantInt>(other),"entry equality lacks literal");auto*br=dyn_cast<BranchInst>(t);need(br&&br->isConditional()&&br->getCondition()==cmp&&cmp->getParent()==&b&&(br->getSuccessor(0)==fallback||br->getSuccessor(1)==fallback),"used table LOAD has no actual ordinary-miss branch");
      row["comparison_ir"]=render(*cmp);row["ordinary_miss_block"]=fallback->getName().str();guards.push_back(std::move(row));
     }
    }
   }++in;}++bn;}
  need(rows.size()==required,"actual original unused table lookup count differs");need(total==rows.size()+guards.size(),"table read partition incomplete");need(render(*m)==original,"readonly inventory mutated module");need(sha((*raw)->getBuffer())==argv[2],"buffer changed");
  json::Object out{{"schema","readonly-actual-table-lookup-inventory-v1"},{"source_sha256",argv[2]},{"LLVM_version",LLVM_VERSION_STRING},{"view_symbol",argv[3]},{"ordinary_symbol",argv[4]},{"table_first_byte",lo},{"table_last_byte",last},{"lookup_count",rows.size()},{"all_exact_table_i64_LOADs",total},{"entry_equality_guard_reads_excluded",guards.size()},{"lookups",std::move(rows)},{"entry_equality_guard_reads",std::move(guards)},{"selection_rule","actual simple i64 read, exact entry Guest base SSA, readonly pure-constant byte offset within interval; unused original reads selected; all other table reads separately validated as literal-equality entry guards to actual ordinary fallback"},{"module_unchanged",true},{"new_pass_runs",0},{"Guest_native_runs",0},{"proof_facts_published",false}};
  need(!std::filesystem::exists(argv[8]),"refuse existing output");std::error_code ec;raw_fd_ostream file(argv[8],ec);need(!ec,"output open failed");file<<formatv("{0:2}\n",json::Value(std::move(out)));file.close();need(!file.has_error(),"output failed");return 0;
 }catch(const std::exception&e){errs()<<e.what()<<"\n";return 1;}
}
