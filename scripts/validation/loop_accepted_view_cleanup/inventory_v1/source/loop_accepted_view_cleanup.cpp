// Validation-only, fixed-recipe emitter. Not registered in CMake or product APIs.
// A sealed producer report is a readiness/provenance gate, never an LLVM fact.
#include <llvm/AsmParser/Parser.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/DFAJumpThreading.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace llvm;
namespace fs=std::filesystem;
namespace {
void need(bool ok,const Twine &why){if(!ok)throw std::runtime_error(why.str());}
std::string hex(ArrayRef<std::uint8_t> bytes){constexpr char h[]="0123456789abcdef";std::string s;for(auto b:bytes){s+=h[b>>4];s+=h[b&15];}return s;}
std::string sha(StringRef text){return hex(SHA256::hash(ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),text.size())));}
std::string digestFile(const fs::path &p){
  std::ifstream in(p,std::ios::binary);need(bool(in),"cannot open pinned file: "+p.string());
  SHA256 hash;std::array<char,65536> b;
  while(in){in.read(b.data(),b.size());auto n=in.gcount();if(n)hash.update(ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(b.data()),std::size_t(n)));}
  need(in.eof()&&!in.bad(),"cannot read pinned file: "+p.string());return hex(hash.final());
}
std::string textField(const json::Object&o,StringRef key){auto v=o.getString(key);need(v.has_value(),"missing string: "+key);return v->str();}
const json::Object &objectField(const json::Object&o,StringRef key){auto *v=o.getObject(key);need(v,"missing object: "+key);return *v;}
std::uint64_t integerField(const json::Object&o,StringRef key){auto v=o.getInteger(key);need(v.has_value()&&*v>=0,"missing unsigned integer: "+key);return std::uint64_t(*v);}
void booleanField(const json::Object&o,StringRef key,bool expected){auto v=o.getBoolean(key);need(v.has_value()&&*v==expected,"unexpected boolean: "+key);}
struct Pin{fs::path path;std::string hash;std::uint64_t size;};
Pin pin(const json::Object&o){
  Pin p{textField(o,"path"),textField(o,"sha256"),integerField(o,"size")};
  need(p.path.is_absolute()&&p.hash.size()==64,"pin requires absolute path and SHA256");
  need(std::all_of(p.hash.begin(),p.hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}),"pin SHA256 must be lowercase hex");return p;
}
void check(const Pin&p){need(fs::is_regular_file(p.path)&&fs::file_size(p.path)==p.size&&digestFile(p.path)==p.hash,"changed/missing pin: "+p.path.string());}
std::string read(const Pin&p){check(p);auto b=MemoryBuffer::getFile(p.path.string());need(bool(b),"cannot read pin");auto bytes=(*b)->getBuffer();need(bytes.size()==p.size&&sha(bytes)==p.hash,"pin changed during read");return bytes.str();}
json::Object parsedObject(StringRef text){auto v=json::parse(text);need(bool(v),"invalid JSON");auto *o=v->getAsObject();need(o,"JSON object required");return std::move(*o);}
std::string render(Module&m){std::string s;raw_string_ostream o(s);m.print(o,nullptr);return s;}
std::string render(Function&f){std::string s;raw_string_ostream o(s);f.print(o);return s;}
std::string render(Value&v){std::string s;raw_string_ostream o(s);v.print(o);return s;}
void verifyWhole(Module&m){std::string s;raw_string_ostream o(s);need(!verifyModule(m,&o),"whole-Module verify failed: "+o.str());}
void save(const fs::path&p,StringRef text){need(!fs::exists(p),"refuse existing output: "+p.string());std::error_code ec;raw_fd_ostream o(p.string(),ec);need(!ec,"output open failed");o<<text;o.close();need(!o.has_error(),"output write failed");}
void saveJSON(const fs::path&p,json::Value v){save(p,formatv("{0:2}\n",v).str());}
void sealedMember(const json::Object&ledger,const fs::path&root,StringRef member,const Pin&p){
  fs::path rel(member.str());need(!rel.empty()&&rel.is_relative(),"seal member must be relative");for(auto&part:rel)need(part!="..","seal member escapes root");
  need(fs::canonical(root/rel)==fs::canonical(p.path),"seal member path differs from pinned input");
  auto *files=ledger.getArray("files");need(files,"seal FILESET has no files[]");std::size_t found=0;
  for(auto&v:*files){auto *o=v.getAsObject();need(o,"invalid seal member");if(textField(*o,"path")==member){++found;need(integerField(*o,"size")==p.size&&textField(*o,"sha256")==p.hash,"seal member digest/size differs");}}
  need(found==1,"seal member absent/duplicate");
}
void validateLedger(const json::Object&ledger){
  need(textField(ledger,"schema")=="exact-fileset-v1","unsupported FILESET schema; adapt explicitly before execution");
  auto *files=ledger.getArray("files");need(files,"seal has no files[]");std::set<std::string> names;std::uint64_t bytes=0;
  for(auto&v:*files){auto *o=v.getAsObject();need(o,"invalid files[] member");const auto n=textField(*o,"path");fs::path rel(n);need(!rel.empty()&&rel.is_relative()&&names.insert(n).second,"invalid/duplicate seal member");for(auto&part:rel)need(part!="..","seal member escapes root");auto size=integerField(*o,"size");need(size<=UINT64_MAX-bytes,"seal size overflow");bytes+=size;need(textField(*o,"sha256").size()==64,"seal member has no full digest");}
  need(integerField(ledger,"payload_count")==files->size()&&integerField(ledger,"payload_bytes")==bytes,"seal totals differ");
}
void closureGate(const json::Object&report,const json::Object&plan,const Pin&source){
  const auto schema=textField(report,"schema");need(schema==textField(plan,"closed_report_schema")&&schema.find("hypothetical")==std::string::npos,"wrong/hypothetical producer report");booleanField(report,"ok",true);
  auto *stages=report.getArray("stages");need(stages,"closed report stages missing");const json::Object *chosen=nullptr;
  for(auto&v:*stages){auto *o=v.getAsObject();need(o,"invalid producer stage");if(textField(*o,"case")==textField(plan,"case")){need(!chosen,"ambiguous closed stage");chosen=o;}}
  need(chosen,"selected producer stage absent");for(auto k:{"verified","closed","changed","ordinary_bodies_and_identity_unchanged","guard_miss_calls_ordinary_root"})booleanField(*chosen,k,true);
  booleanField(*chosen,"budget_exhausted",false);need(textField(*chosen,"after_sha256")==source.hash,"closed report does not bind exact source bytes");
}
std::string ordinarySymbol(Module&m,StringRef view){
  auto *md=m.getNamedMetadata("dragon.guarded-entry");need(md&&md->getNumOperands()==1,"independent GuardedEntry manifest required for scope identity");auto *n=md->getOperand(0);need(n->getNumOperands()==1,"guarded manifest shape differs");auto *s=dyn_cast<MDString>(n->getOperand(0));need(s,"guarded manifest is not a string");auto o=parsedObject(s->getString());need(textField(o,"view_symbol")==view,"selected symbol is not the manifest view");auto root=textField(o,"root_symbol");need(root!=view&&m.getFunction(root),"ordinary root absent/same as view");return root;
}
// Canonical whole-module skeleton on a separate owned clone: only the selected
// body is removed. All declarations/interfaces, globals, aliases, ifuncs,
// inline asm, module flags, attributes and named metadata remain printable.
std::string skeleton(Module&m,StringRef view){auto c=CloneModule(m);auto *f=c->getFunction(view);need(f,"view missing from skeleton clone");f->deleteBody();return render(*c);}
struct Outside {
  Module *module;std::string view,bodyless;std::vector<Function*>functions;
  std::vector<std::vector<Argument*>>arguments;std::map<Function*,std::string>ordinary;
  Outside(Module&m,Function&f):module(&m),view(f.getName().str()),bodyless(skeleton(m,view)){
    for(auto&fn:m){functions.push_back(&fn);std::vector<Argument*>args;for(auto&a:fn.args())args.push_back(&a);arguments.push_back(std::move(args));if(&fn!=&f)ordinary.emplace(&fn,render(fn));}
  }
  void check(Module&m,Function&f)const{
    need(&m==module&&m.size()==functions.size(),"Module/Function set changed");std::size_t n=0;
    for(auto&fn:m){need(&fn==functions[n]&&fn.arg_size()==arguments[n].size(),"Function identity/order/signature changed");std::size_t a=0;for(auto&arg:fn.args())need(&arg==arguments[n][a++],"Argument identity changed");if(&fn!=&f)need(render(fn)==ordinary.at(&fn),"ordinary Function text changed");++n;}
    need(f.getName()==view&&m.getFunction(view)==&f,"view Function identity changed");const auto actual=render(m);need(skeleton(m,view)==bodyless,"module globals/declarations/attributes/metadata or view interface changed");need(render(m)==actual,"validation skeleton cloning mutated the actual Module");
  }
};
struct Lookup {
  std::size_t id;WeakVH load,block,terminator;std::vector<WeakVH>successors;
  std::string originalLoad,originalTerminator,originalBlock;
};
std::vector<Lookup> lookups(Function&f,const json::Object&plan){
  auto *rows=plan.getArray("lookups");need(rows&&!rows->empty()&&rows->size()<=512,"explicit nonempty original lookup inventory required");std::vector<Lookup>out;std::set<Instruction*>seen;
  for(auto&v:*rows){auto *o=v.getAsObject();need(o,"invalid lookup row");auto bn=integerField(*o,"block_ordinal"),in=integerField(*o,"instruction_ordinal");need(bn<f.size(),"lookup block ordinal absent");auto b=f.begin();std::advance(b,bn);need(in<b->size(),"lookup instruction ordinal absent");auto i=b->begin();std::advance(i,in);auto *l=dyn_cast<LoadInst>(&*i);need(l&&l->isSimple()&&l->getType()->isIntegerTy(64)&&seen.insert(l).second,"lookup is not a distinct simple i64 LOAD");auto *t=b->getTerminator();need(sha(render(*l))==textField(*o,"load_ir_sha256")&&sha(render(*t))==textField(*o,"terminator_ir_sha256"),"lookup actual instruction/terminator pin differs");Lookup row{out.size(),l,&*b,t,{},render(*l),render(*t),b->getName().str()};for(auto *next:successors(&*b))row.successors.emplace_back(next);out.push_back(std::move(row));}
  return out;
}
json::Array edgeRows(BasicBlock&b,const std::map<BasicBlock*,std::size_t>&ord){json::Array edges;auto *t=b.getTerminator();for(unsigned slot=0;slot<t->getNumSuccessors();++slot)edges.push_back(json::Object{{"slot",slot},{"target",t->getSuccessor(slot)->getName().str()},{"target_ordinal",ord.at(t->getSuccessor(slot))}});return edges;}
json::Object structure(Function&f,const std::vector<Lookup>&original){
  need(!f.isDeclaration()&&!f.empty()&&f.size()<=4096,"view absent or exceeds producer block cap after cleanup");std::vector<BasicBlock*>blocks;std::map<BasicBlock*,std::size_t>ord;
  std::size_t instructions=0,switches=0,edges=0;for(auto&b:f){ord.emplace(&b,blocks.size());blocks.push_back(&b);instructions+=b.size();switches+=isa<SwitchInst>(b.getTerminator());edges+=b.getTerminator()->getNumSuccessors();}need(instructions<=65536,"view exceeds producer instruction cap after cleanup");
  std::vector<std::vector<std::size_t>>adj(blocks.size());for(std::size_t n=0;n<blocks.size();++n)for(auto *s:successors(blocks[n]))adj[n].push_back(ord.at(s));
  std::vector<bool>reachable(blocks.size());std::vector<std::size_t>todo{0};while(!todo.empty()){auto n=todo.back();todo.pop_back();if(reachable[n])continue;reachable[n]=true;for(auto s:adj[n])todo.push_back(s);}
  std::vector<int>index(blocks.size(),-1),low(blocks.size());std::vector<bool>on(blocks.size());std::vector<std::size_t>stack;std::vector<std::vector<std::size_t>>groups;int serial=0;
  std::function<void(std::size_t)>visit=[&](std::size_t v){index[v]=low[v]=serial++;stack.push_back(v);on[v]=true;for(auto w:adj[v]){if(index[w]<0){visit(w);low[v]=std::min(low[v],low[w]);}else if(on[w])low[v]=std::min(low[v],index[w]);}if(low[v]==index[v]){std::vector<std::size_t>group;for(;;){auto w=stack.back();stack.pop_back();on[w]=false;group.push_back(w);if(w==v)break;}std::sort(group.begin(),group.end());groups.push_back(std::move(group));}};
  for(std::size_t n=0;n<blocks.size();++n)if(index[n]<0)visit(n);std::sort(groups.begin(),groups.end(),[](auto&a,auto&b){return a.front()<b.front();});
  json::Array sccs;std::vector<bool>cyclic(blocks.size());std::size_t cyclicSwitches=0,reachableCyclicSwitches=0,reachableBlocks=0;
  for(auto&g:groups){bool cycle=g.size()>1||std::find(adj[g.front()].begin(),adj[g.front()].end(),g.front())!=adj[g.front()].end();json::Array members;for(auto n:g){cyclic[n]=cycle;reachableBlocks+=reachable[n];auto sw=isa<SwitchInst>(blocks[n]->getTerminator());cyclicSwitches+=cycle&&sw;reachableCyclicSwitches+=cycle&&sw&&reachable[n];members.push_back(json::Object{{"ordinal",n},{"name",blocks[n]->getName().str()}});}sccs.push_back(json::Object{{"cyclic",cycle},{"reachable",bool(reachable[g.front()])},{"blocks",std::move(members)}});}
  json::Array tracked;std::size_t trackedCyclic=0;
  for(auto&l:original){auto *load=dyn_cast_or_null<LoadInst>(static_cast<Value*>(l.load));auto *block=dyn_cast_or_null<BasicBlock>(static_cast<Value*>(l.block));auto *term=dyn_cast_or_null<Instruction>(static_cast<Value*>(l.terminator));json::Object row{{"id",l.id},{"original_block",l.originalBlock},{"original_load",l.originalLoad},{"original_terminator",l.originalTerminator},{"load_alive",load!=nullptr},{"original_block_alive",block!=nullptr},{"original_terminator_alive",term!=nullptr}};
    if(load){auto n=ord.at(load->getParent());trackedCyclic+=reachable[n]&&cyclic[n];row["actual_load"]=render(*load);row["load_uses"]=std::uint64_t(load->getNumUses());row["actual_parent"]=load->getParent()->getName().str();row["reachable_cyclic"]=reachable[n]&&cyclic[n];row["actual_next_terminator"]=render(*load->getParent()->getTerminator());row["actual_successor_slots"]=edgeRows(*load->getParent(),ord);}
    if(block){row["original_block_now_terminator"]=render(*block->getTerminator());row["original_block_now_successor_slots"]=edgeRows(*block,ord);}json::Array old;for(std::size_t slot=0;slot<l.successors.size();++slot){auto *b=dyn_cast_or_null<BasicBlock>(static_cast<Value*>(l.successors[slot]));json::Object edge{{"slot",slot},{"original_successor_alive",b!=nullptr},{"current_name",b?b->getName().str():""}};if(b)edge["current_ordinal"]=ord.at(b);old.push_back(std::move(edge));}row["original_successor_identities"]=std::move(old);tracked.push_back(std::move(row));}
  return json::Object{{"blocks",blocks.size()},{"instructions",instructions},{"switches",switches},{"edges_including_duplicates",edges},{"reachable_blocks",reachableBlocks},{"cyclic_switches_all_CFG",cyclicSwitches},{"cyclic_switches_reachable",reachableCyclicSwitches},{"tracked_original_lookups_in_reachable_SCC",trackedCyclic},{"SCCs_all_CFG",std::move(sccs)},{"original_lookup_cutpoints",std::move(tracked)},{"zero_reachable_cyclic_switches_observed",reachableCyclicSwitches==0},{"deobfuscation_complete",false}};
}
json::Object runtimeBinding(const Pin&runtime,const std::optional<Pin>&solver){
#if defined(__linux__)
  std::ifstream in("/proc/self/maps");need(bool(in),"cannot read own /proc/self/maps");std::string line;std::set<std::string>providers,solvers,sharedLLVM;json::Array matches;
  while(std::getline(in,line)){std::istringstream row(line);std::string address,perms,offset,device,inode,path;row>>address>>perms>>offset>>device>>inode;std::getline(row,path);auto start=path.find_first_not_of(' ');if(start==std::string::npos)continue;path.erase(0,start);if(path.empty()||path.front()!='/')continue;auto basename=fs::path(path).filename().string();if(basename.find("libdragon.so")!=std::string::npos){providers.insert(path);matches.push_back(line);}if(basename.find("libz3.so")!=std::string::npos){solvers.insert(path);matches.push_back(line);}if(basename.find("libLLVM")!=std::string::npos)sharedLLVM.insert(path);}
  need(sharedLLVM.empty(),"unexpected shared LLVM mapping");need(providers.size()==1&&fs::canonical(*providers.begin())==fs::canonical(runtime.path),"actual LLVM provider is not pinned SDK");check(runtime);
  if(solver){need(solvers.size()==1&&fs::canonical(*solvers.begin())==fs::canonical(solver->path),"actual solver mapping differs");check(*solver);}else need(solvers.empty(),"zero-Z3 plan has solver mapping");return json::Object{{"maps",std::move(matches)},{"runtime_sha256",runtime.hash},{"solver_mapped",!solvers.empty()},{"shared_LLVM_mapped",false}};
#else
  (void)runtime;(void)solver;throw std::runtime_error("this readiness/binding draft requires Linux /proc/self/maps");
#endif
}
} // namespace
int main(int argc,char**argv){
  fs::path output;LLVMContext context;std::unique_ptr<Module>module;json::Array stages;unsigned attempted=0;json::Object report{{"schema","dragon-accepted-view-cleanup-report-v1"},{"ok",false},{"passes_attempted",0},{"Guest_native_runs",0},{"Unicorn_runs",0},{"semantic_validation",false},{"deobfuscation_complete",false}};
  try{
    need(argc==3,"usage: emitter ABS-READY-PLAN.json ABS-NEW-OUT");need(fs::path(argv[1]).is_absolute(),"absolute plan required");output=argv[2];need(output.is_absolute()&&!fs::exists(output),"new absolute output required");fs::create_directories(output);
    auto planBuffer=MemoryBuffer::getFile(argv[1]);need(bool(planBuffer),"plan missing");const auto planBytes=(*planBuffer)->getBuffer().str();auto plan=parsedObject(planBytes);need(textField(plan,"schema")=="dragon-accepted-view-cleanup-plan-v1","wrong plan schema");booleanField(plan,"ready",true);booleanField(plan,"sealed",true);need(textField(plan,"recipe")=="existing-control-flow-six-v1","unsupported cleanup recipe");need(textField(plan,"LLVM_version")==LLVM_VERSION_STRING,"compiled LLVM version differs from pinned plan");
    auto source=pin(objectField(plan,"source")),producer=pin(objectField(plan,"closed_report")),ledgerPin=pin(objectField(plan,"seal_ledger")),statusPin=pin(objectField(plan,"seal_status")),emitterSource=pin(objectField(plan,"emitter_source")),runtime=pin(objectField(plan,"runtime"));std::optional<Pin>solver;if(auto *v=plan.get("solver");v&&v->getAsObject())solver=pin(*v->getAsObject());else need(plan.get("solver")&&plan.get("solver")->getAsNull().has_value(),"solver must be pinned object or explicit null");
    std::vector<Pin>pins{source,producer,ledgerPin,statusPin,emitterSource,runtime};if(solver)pins.push_back(*solver);const auto raw=read(source);auto closed=parsedObject(read(producer));auto ledger=parsedObject(read(ledgerPin));auto status=parsedObject(read(statusPin));check(emitterSource);check(runtime);if(solver)check(*solver);
    const fs::path sealRoot=textField(plan,"seal_root");need(sealRoot.is_absolute(),"absolute seal_root required");validateLedger(ledger);sealedMember(ledger,sealRoot,textField(plan,"source_member"),source);sealedMember(ledger,sealRoot,textField(plan,"closed_report_member"),producer);sealedMember(ledger,sealRoot,textField(plan,"seal_status_member"),statusPin);auto statusKey=textField(plan,"seal_status_key");need(statusKey=="no_more_writes"||statusKey=="no_more_writes_after_fileset","unsupported actual seal-status key");booleanField(status,statusKey,true);closureGate(closed,plan,source);
    report["plan_sha256"]=sha(planBytes);report["source_sha256"]=source.hash;report["closed_report_sha256"]=producer.hash;report["seal_ledger_sha256"]=ledgerPin.hash;report["seal_status_sha256"]=statusPin.hash;report["ready_and_closed_and_sealed_gates_passed"]=true;report["runtime_binding_before"]=runtimeBinding(runtime,solver);
    SMDiagnostic error;module=parseAssemblyString(raw,error,context);if(!module){error.print(argv[0],errs());throw std::runtime_error("source parse failed");}verifyWhole(*module);save(output/"00-source.ll",raw);const auto canonical=render(*module);save(output/"00-parsed.ll",canonical);report["raw_first_line"]=StringRef(raw).take_front(StringRef(raw).find('\n')).str();report["actual_parsed_ModuleID"]=module->getModuleIdentifier();report["actual_parsed_sha256"]=sha(canonical);report["raw_and_parsed_byte_equal"]=canonical==raw;
    const auto viewName=textField(plan,"view_symbol");auto *view=module->getFunction(viewName);need(view&&!view->isDeclaration(),"selected view definition absent");const auto ordinary=ordinarySymbol(*module,viewName);need(!view->hasPersonalityFn()&&!view->getSubprogram(),"unsupported view EH/debug identity in skeleton snapshot");for(auto&a:view->args())need(!a.isUsedByMetadata(),"selected local argument metadata identity unsupported");for(auto&b:*view){need(!b.hasAddressTaken()&&!b.isUsedByMetadata(),"address-taken or metadata-local selected block unsupported by bodyless snapshot");for(auto&i:b)need(!i.getDebugLoc()&&!i.hasDbgRecords()&&!i.isUsedByMetadata(),"selected debug/local metadata identity unsupported");}Outside outside(*module,*view);need(render(*module)==canonical,"validation snapshot cloning mutated source Module");auto original=lookups(*view,plan);report["ordinary_root_symbol"]=ordinary;report["view_symbol"]=viewName;report["recipe"]=textField(plan,"recipe");stages.push_back(json::Object{{"stage","00-source"},{"whole_verified",true},{"structure",structure(*view,original)}});
    LoopAnalysisManager lam;FunctionAnalysisManager fam;CGSCCAnalysisManager cgam;ModuleAnalysisManager mam;PassBuilder pb;pb.registerModuleAnalyses(mam);pb.registerCGSCCAnalyses(cgam);pb.registerFunctionAnalyses(fam);pb.registerLoopAnalyses(lam);pb.crossRegisterProxies(lam,fam,cgam,mam);
    auto run=[&](StringRef label,auto pass){++attempted;FunctionPassManager fpm;fpm.addPass(std::move(pass));fpm.run(*view,fam);verifyWhole(*module);outside.check(*module,*view);auto bytes=render(*module);save(output/(label.str()+".ll"),bytes);stages.push_back(json::Object{{"stage",label.str()},{"whole_verified",true},{"outside_text_and_identity_unchanged",true},{"module_sha256",sha(bytes)},{"structure",structure(*view,original)}});};
    // Exactly lib/LLVMLifter.cpp::runControlFlowSimplification, once, defaults.
    run("01-sccp",SCCPPass());run("02-dfa-jump-threading",DFAJumpThreadingPass());run("03-jump-threading",JumpThreadingPass());run("04-simplify-cfg",SimplifyCFGPass());run("05-instcombine",InstCombinePass());run("06-dce",DCEPass());
    for(auto&p:pins)check(p);need(MemoryBuffer::getFile(argv[1])&&digestFile(argv[1])==sha(planBytes),"plan changed during cleanup");report["runtime_binding_after"]=runtimeBinding(runtime,solver);outside.check(*module,*view);verifyWhole(*module);const auto final=render(*module);save(output/"after.ll",final);report["after_sha256"]=sha(final);report["ok"]=true;report["cleanup_emitted"]=true;report["source_and_all_pins_unchanged"]=true;report["passes_attempted"]=attempted;report["stages"]=std::move(stages);saveJSON(output/"REPORT.json",std::move(report));return 0;
  }catch(const std::exception&e){report["failure"]=e.what();report["passes_attempted"]=attempted;report["stages"]=std::move(stages);if(!output.empty()&&fs::exists(output)){if(module&&!fs::exists(output/"rejected.ll"))save(output/"rejected.ll",render(*module));if(!fs::exists(output/"REPORT.json"))saveJSON(output/"REPORT.json",std::move(report));}errs()<<e.what()<<"\n";return 1;}
}
