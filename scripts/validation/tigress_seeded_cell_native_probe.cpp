// Source-only parser / installed Image API producer. Never executes target IR.
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <map>
#include <vector>
using namespace dragon::llvmlifter::detail;
static std::string text(llvm::Module &m) { std::string s;llvm::raw_string_ostream o(s);m.print(o,nullptr);return s; }
static std::string text(llvm::Function &f) { std::string s;llvm::raw_string_ostream o(s);f.print(o);return s; }
static bool save(const char *p,const std::string &s) {std::error_code e;llvm::raw_fd_ostream o(p,e);if(e)return false;o<<s;o.close();return !o.has_error();}
static unsigned namedLoads(llvm::Function &f, llvm::StringRef name){unsigned n=0;for(auto &b:f)for(auto &i:b)if(llvm::isa<llvm::LoadInst>(i)&&i.getName()==name)++n;return n;}
int main(int argc,char **argv){
 // mode raw.ll case.json parsed-before.ll output.ll report.json
 if(argc!=7)return 2;std::string mode=argv[1];if(mode!="before"&&mode!="rewrite")return 2;
#ifdef DRAGON_SEEDED_CELL_BEFORE_ONLY
 if(mode!="before")return 2;
#endif
 for(unsigned n=4;n!=7;++n)if(llvm::sys::fs::exists(argv[n]))return 2;
 llvm::LLVMContext ctx;llvm::SMDiagnostic err;
 auto raw=llvm::MemoryBuffer::getFile(argv[2]);auto cfg=llvm::MemoryBuffer::getFile(argv[3]);if(!raw||!cfg)return 3;
 auto m=llvm::parseAssembly((*raw)->getMemBufferRef(),err,ctx);if(!m){err.print("seeded-cell-native",llvm::errs());return 3;}
 if(llvm::verifyModule(*m,&llvm::errs()))return 3;
 auto j=llvm::json::parse((*cfg)->getBuffer());if(!j)return 4;auto *o=j->getAsObject();if(!o)return 4;
 auto symbol=o->getString("view_symbol");auto base=o->getInteger("table_offset");auto *rows=o->getArray("targets");if(!symbol||!base||*base<0||!rows||rows->empty()||rows->size()>512)return 4;
 ImageTableContextRequest request;request.viewSymbol=symbol->str();request.tableOffset=*base;
 for(auto &row:*rows){auto v=row.getAsInteger();if(!v||*v<0)return 4;request.targets.push_back(*v);}
 auto *view=m->getFunction(request.viewSymbol);if(!view||view->isDeclaration())return 4;
 std::map<std::string,llvm::Function*> identities;std::map<std::string,std::vector<llvm::Argument*>> args;std::map<std::string,std::string> ordinary;
 for(auto &f:*m){identities[f.getName().str()]=&f;for(auto &a:f.args())args[f.getName().str()].push_back(&a);if(!f.isDeclaration()&&&f!=view)ordinary[f.getName().str()]=text(f);}
 auto before=text(*m);unsigned beforeLoads=namedLoads(*view,"target"),beforeCellLoads=namedLoads(*view,"cell.reload");bool called=false;ImageTableContextStats s;
#ifndef DRAGON_SEEDED_CELL_BEFORE_ONLY
 if(mode=="rewrite"){called=true;s=runClosedImageTableContext(*m,request);}
#endif
 if(llvm::verifyModule(*m,&llvm::errs()))return 5;
 for(auto &[name,f]:identities){auto *current=m->getFunction(name);if(current!=f||current->arg_size()!=args[name].size())return 6;for(unsigned i=0;i!=args[name].size();++i)if(current->getArg(i)!=args[name][i])return 6;}
 for(auto &[name,old]:ordinary)if(text(*m->getFunction(name))!=old)return 6;
 auto after=text(*m);view=m->getFunction(request.viewSymbol);
 llvm::json::Object report{{"mode",mode},{"raw_input",argv[2]},{"case_manifest",argv[3]},
   {"verified_before",true},{"verified_after",true},{"function_argument_identity",true},
   {"ordinary_definitions_byte_equal",true},{"byte_equal",before==after},{"api_calls",called?1:0},
   {"target_loads_before",beforeLoads},{"target_loads_after",namedLoads(*view,"target")},
   {"cell_loads_before",beforeCellLoads},{"cell_loads_after",namedLoads(*view,"cell.reload")},
   {"scope","Installed Image API source-only producer; no target execution; joint table/cell closure; no arbitrary future-load fact"}};
#ifdef DRAGON_SEEDED_CELL_BEFORE_ONLY
 report["compiled_before_only"]=true;
#else
 report["compiled_before_only"]=false;
#endif
 if(called)report["stats"]=llvm::json::Object{{"changed",s.changed},{"closed",s.closed},{"budget_exhausted",s.budgetExhausted},
  {"guarded_bytes",static_cast<int64_t>(s.guardedBytes)},{"preserved_stores",static_cast<int64_t>(s.preservedStores)},
  {"constant_values",static_cast<int64_t>(s.constantValues)},{"work",static_cast<int64_t>(s.work)},{"message",s.message}};
 llvm::json::Array maps;std::ifstream proc("/proc/self/maps");for(std::string line;std::getline(proc,line);)if(line.find("libdragon.so")!=std::string::npos||line.find("libz3.so")!=std::string::npos)maps.push_back(line);report["runtime_maps"]=std::move(maps);
 if(!save(argv[4],before)||!save(argv[5],after))return 7;
 return save(argv[6],llvm::formatv("{0:2}\n",llvm::json::Value(std::move(report))).str())?0:7;
}
