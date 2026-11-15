// Read-only structural superset inventory. No address/value inference or passes.
#include <llvm/AsmParser/Parser.h>
#include <llvm/Config/llvm-config.h>
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
#include <stdexcept>
using namespace llvm;
void need(bool v,const Twine&w){if(!v)throw std::runtime_error(w.str());}
std::string sha(StringRef s){auto b=SHA256::hash(ArrayRef<std::uint8_t>((const std::uint8_t*)s.data(),s.size()));constexpr char h[]="0123456789abcdef";std::string x;for(auto v:b){x+=h[v>>4];x+=h[v&15];}return x;}
std::string render(Value&v){std::string s;raw_string_ostream o(s);v.print(o);return s;}
std::string render(Module&m){std::string s;raw_string_ostream o(s);m.print(o,nullptr);return s;}
int main(int argc,char**argv){try{
 need(argc==6,"usage: inventory SOURCE HASH VIEW ORDINARY NEW_JSON");
 auto raw=MemoryBuffer::getFile(argv[1]);need(bool(raw),"source missing");need(sha((*raw)->getBuffer())==argv[2],"source hash changed");
 LLVMContext context;SMDiagnostic error;auto m=parseAssemblyString((*raw)->getBuffer(),error,context);if(!m){error.print(argv[0],errs());throw std::runtime_error("source parse failed");}
 std::string why;raw_string_ostream errors(why);need(!verifyModule(*m,&errors),"invalid source module: "+errors.str());
 auto*f=m->getFunction(argv[3]);need(f&&!f->isDeclaration()&&m->getFunction(argv[4]),"view/ordinary absent");
 auto original=render(*m);json::Array rows,nonSimple;std::uint64_t bn=0,allI64=0,allInteger=0,instructions=0;
 for(auto&b:*f){std::uint64_t in=0;for(auto&i:b){++instructions;if(auto*l=dyn_cast<LoadInst>(&i)){
  allInteger+=l->getType()->isIntegerTy();if(l->getType()->isIntegerTy(64)){
   ++allI64;auto*t=b.getTerminator();auto load=render(*l),term=render(*t);
   json::Object row{{"block_ordinal",bn},{"instruction_ordinal",in},{"block_name",b.getName().str()},{"load_ir",load},{"load_ir_sha256",sha(load)},{"terminator_ir",term},{"terminator_ir_sha256",sha(term)},{"simple_i64",l->isSimple()}};
   if(l->isSimple())rows.push_back(std::move(row));else nonSimple.push_back(std::move(row));
  }
 }++in;}++bn;}
 need(!rows.empty()&&rows.size()<=512,"complete simple-i64 inventory exceeds unchanged emitter512 cap or empty");
 need(allI64==rows.size()+nonSimple.size(),"LOAD inventory incomplete");need(render(*m)==original,"readonly inventory changed module");
 json::Object out{{"schema","readonly-all-original-simple-i64-load-inventory-v1"},{"source_sha256",argv[2]},{"LLVM_version",LLVM_VERSION_STRING},{"view_symbol",argv[3]},{"ordinary_symbol",argv[4]},{"inventory_scope","all original simple i64 LOAD instructions of the selected view; structural superset, no address classification and no claim these are all table lookups"},{"exact_table_lookup_inventory",false},{"blocks",f->size()},{"instructions",instructions},{"all_integer_LOAD_count",allInteger},{"all_i64_LOAD_count",allI64},{"simple_i64_LOAD_count",rows.size()},{"non_simple_i64_LOAD_count",nonSimple.size()},{"lookups",std::move(rows)},{"excluded_non_simple_i64_LOADs",std::move(nonSimple)},{"module_unchanged",true},{"new_pass_runs",0},{"Guest_native_runs",0},{"proof_facts_published",false}};
 need(!std::filesystem::exists(argv[5]),"refuse existing output");std::error_code ec;raw_fd_ostream file(argv[5],ec);need(!ec,"output open failed");file<<formatv("{0:2}\n",json::Value(std::move(out)));file.close();need(!file.has_error(),"output failed");return 0;
 }catch(const std::exception&e){errs()<<e.what()<<"\n";return 1;}}
