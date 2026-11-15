// Validation-only native binding/observation copies. Never edits the production IR.
// No product pass, Guest execution, memory fact or closure proof is performed here.
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

using namespace llvm;
namespace {
void require(bool value,const Twine &message) {
  if(!value) throw std::runtime_error(message.str());
}
std::string read(const std::filesystem::path &path) {
  auto file=MemoryBuffer::getFile(path.string());
  require(bool(file),"cannot read "+path.string());return (*file)->getBuffer().str();
}
std::string sha(StringRef value) {
  auto bytes=SHA256::hash(ArrayRef<std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(value.data()),value.size()));
  static constexpr char digits[]="0123456789abcdef";
  std::string result;for(auto byte:bytes){result+=digits[byte>>4];result+=digits[byte&15];}return result;
}
template<class T> std::string printed(T &value) {
  std::string result;raw_string_ostream out(result);value.print(out);return result;
}
std::string printedModule(Module &module) {
  std::string result;raw_string_ostream out(result);module.print(out,nullptr);return result;
}
void save(const std::filesystem::path &path,StringRef value) {
  require(!std::filesystem::exists(path),"new output required: "+path.string());
  std::error_code error;raw_fd_ostream out(path.string(),error);
  require(!error,"cannot create "+path.string());out<<value;
}
void verify(Module &module) {
  std::string error;raw_string_ostream out(error);
  require(!verifyModule(module,&out),"module verify failed: "+error);
}
std::string replaceOnce(std::string value,const std::string &before,const std::string &after) {
  require(!before.empty(),"empty exact difference");auto at=value.find(before);
  require(at!=std::string::npos && value.find(before,at+before.size())==std::string::npos,
          "exact difference is absent or ambiguous");value.replace(at,before.size(),after);return value;
}
std::string string(const json::Object &object,StringRef name) {
  auto value=object.getString(name);require(bool(value),"missing string "+name);return value->str();
}
struct Roles {
  std::string wrapper,kernel,view,sourceSHA,sourceLiteral;
  std::uint64_t kernelPC;
};
std::unique_ptr<Module> parse(const std::filesystem::path &path,LLVMContext &context) {
  SMDiagnostic diagnostic;auto result=parseIRFile(path.string(),diagnostic,context);
  if(!result){diagnostic.print("loop-native-adapter",errs());throw std::runtime_error("cannot parse production input");}
  verify(*result);return result;
}
void descriptorABI(Function &function) {
  require(!function.isDeclaration() && !function.isVarArg() &&
          function.arg_size()==4 && function.getReturnType()->isVoidTy(),"unsupported descriptor function");
  for(auto &arg:function.args()) require(arg.getType()->isPointerTy() &&
      arg.getType()->getPointerAddressSpace()==0,"unsupported descriptor argument");
}
CallInst *bind(Module &module,const Roles &roles,json::Object &record) {
  auto *wrapper=module.getFunction(roles.wrapper),*kernel=module.getFunction(roles.kernel),
       *view=module.getFunction(roles.view);
  require(wrapper && kernel && view && wrapper!=kernel && wrapper!=view && kernel!=view,"missing/distinct binding symbols");
  descriptorABI(*wrapper);descriptorABI(*kernel);descriptorABI(*view);
  require(kernel->getFunctionType()==view->getFunctionType() &&
          kernel->getCallingConv()==view->getCallingConv(),"kernel/view ABI differs");
  CallInst *selected=nullptr;
  for(auto &block:*wrapper)for(auto &i:block)if(auto *call=dyn_cast<CallBase>(&i)) {
    if(call->getCalledFunction()!=kernel)continue;
    require(!selected && isa<CallInst>(call),"expected exactly one direct ordinary-kernel CallInst in wrapper");
    require(call->getFunctionType()==view->getFunctionType() &&
            call->getCallingConv()==view->getCallingConv() && !call->getNumOperandBundles(),"wrapper call ABI differs");
    auto *tag=call->getMetadata("dragon.guest.call.target");
    auto *value=tag && tag->getNumOperands()==1 ? mdconst::dyn_extract<ConstantInt>(tag->getOperand(0)) : nullptr;
    require(value && value->getBitWidth()==64 && value->equalsInt(roles.kernelPC),"wrapper target attachment differs");
    selected=cast<CallInst>(call);
  }
  require(selected,"wrapper ordinary-kernel call absent");
  const auto moduleBefore=printedModule(module),wrapperBefore=printed(*wrapper),callBefore=printed(*selected);
  std::map<Function *,std::string> functions;
  for(auto &f:module)functions.emplace(&f,printed(f));
  selected->setCalledFunction(view);verify(module);
  const auto callAfter=printed(*selected),wrapperAfter=printed(*wrapper);
  require(replaceOnce(wrapperBefore,callBefore,callAfter)==wrapperAfter,"binding changed more than the selected call");
  require(replaceOnce(moduleBefore,wrapperBefore,wrapperAfter)==printedModule(module),"binding changed more than wrapper body");
  for(const auto &[f,body]:functions)if(f!=wrapper)require(printed(*f)==body,"binding changed another function");
  record=json::Object{{"wrapper",roles.wrapper},{"ordinary_kernel",roles.kernel},{"closed_view",roles.view},
    {"retargeted_direct_calls",1},{"kernel_PC",roles.kernelPC},{"call_before",callBefore},{"call_after",callAfter},
    {"ordinary_kernel_unchanged",true},{"view_unchanged",true},{"other_functions_unchanged",true},
    {"only_one_callee_operand_changed",true}};
  return selected;
}
struct Sites { BasicBlock *accepted,*fallback;CallInst *ordinaryCall;json::Array chain; };
bool ordinaryFallback(BasicBlock *block,Function &view,Function &kernel) {
  if(block->size()!=2 || !isa<ReturnInst>(block->back()))return false;
  auto *call=dyn_cast<CallInst>(&block->front());
  if(!call || call->getCalledFunction()!=&kernel || call->arg_size()!=4 ||
      call->getFunctionType()!=view.getFunctionType() ||
      call->getCallingConv()!=kernel.getCallingConv() || call->getNumOperandBundles())return false;
  for(unsigned n=0;n<4;++n)if(call->getArgOperand(n)!=view.getArg(n))return false;
  return true;
}
Sites sites(Function &view,Function &kernel,std::uint64_t pc) {
  auto *sw=dyn_cast<SwitchInst>(view.getEntryBlock().getTerminator());
  require(sw && sw->getNumCases()==1 && sw->getCondition()->getType()->isIntegerTy(64),"view entry is not the frozen single-case descriptor entry");
  auto item=*sw->case_begin();require(item.getCaseValue()->equalsInt(pc),"view entry case differs");
  auto *block=item.getCaseSuccessor(),*previous=&view.getEntryBlock();
  require(block!=sw->getDefaultDest(),"entry case/default are identical");
  BasicBlock *fallback=nullptr;unsigned checks=0;std::set<BasicBlock *> seen;json::Array chain;
  for(;;) {
    require(seen.size()<65536 && seen.insert(block).second && block->getSinglePredecessor()==previous,
            "entry observation chain has reentry/duplicate predecessor or exceeds cap");
    auto *branch=dyn_cast<BranchInst>(block->getTerminator());require(branch,"entry observation chain is not branches");
    for(auto &i:*block)if(!i.isTerminator()) {
      auto *load=dyn_cast<LoadInst>(&i);
      require(!isa<PHINode,CallBase,StoreInst,FenceInst>(i) && !i.mayHaveSideEffects() &&
              (!i.mayReadOrWriteMemory() || (load && load->isSimple())),"effect in guard observation chain");
    }
    chain.push_back(json::Object{{"block",block->getName().str()},{"terminator",printed(*branch)}});
    if(branch->isUnconditional()) {
      require(checks>0 && fallback && block->size()==1,"accepted site is not an empty post-check edge");
      require(ordinaryFallback(fallback,view,kernel),"ordinary fallback changed shape");
      return {block,fallback,cast<CallInst>(&fallback->front()),std::move(chain)};
    }
    unsigned failed=2;
    for(unsigned slot=0;slot<2;++slot)if(ordinaryFallback(branch->getSuccessor(slot),view,kernel)) {
      require(failed==2,"guard has two ordinary-fallback successors");failed=slot;
    }
    require(failed<2,"guard failure does not call ordinary kernel");
    require(!fallback || fallback==branch->getSuccessor(failed),"guard failures use different fallback sites");
    fallback=branch->getSuccessor(failed);++checks;previous=block;block=branch->getSuccessor(1-failed);
  }
}
void countBefore(Instruction *point,GlobalVariable *counter,StringRef role) {
  IRBuilder<> builder(point);
  auto *old=builder.CreateAlignedLoad(builder.getInt64Ty(),counter,Align(8),"native.observe."+role+".old");old->setVolatile(true);
  auto *next=builder.CreateAdd(old,builder.getInt64(1),"native.observe."+role+".next");
  auto *store=builder.CreateAlignedStore(next,counter,Align(8));store->setVolatile(true);
}
json::Object observe(Module &module,const Roles &roles) {
  auto *view=module.getFunction(roles.view),*kernel=module.getFunction(roles.kernel);
  require(view && kernel,"observer symbols missing");auto selected=sites(*view,*kernel,roles.kernelPC);
  // The observer adds global volatile effects; do not rely on inherited restricted effects.
  require(!view->hasFnAttribute(Attribute::Memory) && !view->hasFnAttribute(Attribute::NoSync) &&
          !view->hasFnAttribute(Attribute::Speculatable),"observer would violate an inherited function promise");
  std::map<Function *,std::string> functions;for(auto &f:module)functions.emplace(&f,printed(f));
  std::map<GlobalVariable *,std::string> globals;for(auto &g:module.globals())globals.emplace(&g,printed(g));
  const auto acceptedBefore=printed(*selected.accepted),fallbackBefore=printed(*selected.fallback),
             callBefore=printed(*selected.ordinaryCall),viewBefore=printed(*view);
  auto makeCounter=[&](StringRef name) {
    require(!module.getNamedValue(name),"observer counter name collides");
    auto *global=new GlobalVariable(module,Type::getInt64Ty(module.getContext()),false,
        GlobalValue::ExternalLinkage,ConstantInt::get(Type::getInt64Ty(module.getContext()),0),name);
    global->setAlignment(Align(8));return global;
  };
  auto *acceptedCounter=makeCounter("dragon_loop_native_accepted"),*fallbackCounter=makeCounter("dragon_loop_native_fallback");
  countBefore(selected.accepted->getTerminator(),acceptedCounter,"accepted");
  countBefore(selected.ordinaryCall,fallbackCounter,"fallback");
  verify(module);
  require(printed(*selected.ordinaryCall)==callBefore,"observer changed ordinary fallback call");
  auto expected=replaceOnce(viewBefore,acceptedBefore,printed(*selected.accepted));
  expected=replaceOnce(expected,fallbackBefore,printed(*selected.fallback));
  require(printed(*view)==expected,"observer changed outside the two exact blocks");
  for(const auto &[f,body]:functions)if(f!=view)require(printed(*f)==body,"observer changed another function");
  for(const auto &[g,body]:globals)require(printed(*g)==body,"observer changed existing global");
  return json::Object{{"accepted_block",selected.accepted->getName().str()},
      {"fallback_block",selected.fallback->getName().str()},{"chain",std::move(selected.chain)},
      {"accepted_before",acceptedBefore},{"accepted_after",printed(*selected.accepted)},
      {"fallback_before",fallbackBefore},{"fallback_after",printed(*selected.fallback)},
      {"counter_globals",json::Array{acceptedCounter->getName().str(),fallbackCounter->getName().str()}},
      {"inserted_volatile_loads",2},{"inserted_volatile_stores",2},{"inserted_flagfree_adds",2},
      {"ordinary_fallback_call_unchanged",true},{"other_functions_unchanged",true},{"existing_globals_unchanged",true}};
}
} // namespace
int main(int argc,char **argv) {
  try {
    require(argc==4,"usage: adapter production.ll sealed-roles.json NEW-OUT");
    std::filesystem::path input=argv[1],config=argv[2],out=argv[3];
    require(std::filesystem::is_regular_file(input) && !std::filesystem::is_symlink(input) &&
            std::filesystem::is_regular_file(config) && !std::filesystem::is_symlink(config),
            "production input and sealed roles must be regular files");
    auto roleBytes=read(config),inputBytes=read(input);auto value=json::parse(roleBytes);
    require(bool(value),"roles JSON invalid");auto *object=value->getAsObject();require(object,"roles object required");
    require(object->getString("schema")=="loop-real-native-adapter-roles-v1" && object->getBoolean("ready")==true,
            "adapter roles are not sealed/ready; draft cannot emit or execute");
    Roles roles{string(*object,"wrapper"),string(*object,"ordinary_kernel"),string(*object,"view"),
                string(*object,"production_ir_sha256"),string(*object,"production_first_line"),0};
    auto pc=object->getInteger("kernel_pc");require(pc && *pc>=0,"kernel PC required");roles.kernelPC=*pc;
    require(sha(inputBytes)==roles.sourceSHA && inputBytes.substr(0,inputBytes.find('\n'))==roles.sourceLiteral,
            "production input/first-line pin differs; never strip ModuleID");
    // Eligibility is sealed externally against actual producer/library reports.
    // This mechanical adapter itself proves no closure or guarded-context fact.
    require(object->getBoolean("eligible_actual_closed_result")==true && object->getString("closure_evidence_sha256") &&
            object->getString("closure_evidence_sha256")->size()==64,"missing externally sealed actual-closure provenance");
    require(!std::filesystem::exists(out),"new output namespace required");std::filesystem::create_directories(out);
    LLVMContext context;auto module=parse(input,context);const auto parsed=printedModule(*module);
    save(out/"production.parsed-before.ll",parsed);json::Object binding;bind(*module,roles,binding);
    const auto adapter=printedModule(*module);save(out/"native-adapter.ll",adapter);
    LLVMContext observedContext;auto observed=parse(input,observedContext);json::Object repeatBinding;bind(*observed,roles,repeatBinding);
    require(printedModule(*observed)==adapter,"independent adapter parse differs");auto observation=observe(*observed,roles);
    const auto observer=printedModule(*observed);save(out/"native-observer.ll",observer);
    require(read(input)==inputBytes && read(config)==roleBytes,"immutable production input/roles changed");
    json::Object report{{"schema","loop-real-native-adapter-emission-v1"},{"input",input.string()},
      {"input_sha256",sha(inputBytes)},{"input_first_line",roles.sourceLiteral},
      {"actual_parsed_ModuleID",module->getModuleIdentifier()},{"roles_sha256",sha(roleBytes)},
      {"parsed_before_sha256",sha(parsed)},{"native_adapter_sha256",sha(adapter)},{"native_observer_sha256",sha(observer)},
      {"binding",std::move(binding)},{"observation",std::move(observation)},
      {"production_input_unchanged",true},{"product_pass_calls",0},{"Guest_native_calls",0},
      {"C_calls",0},{"Unicorn_calls",0},{"closure_proved_by_adapter",false},
      {"native_semantics_validated",false},{"whole_modules_verified",true}};
    save(out/"REPORT.json",formatv("{0:2}\n",json::Value(std::move(report))).str());return 0;
  } catch(const std::exception &error) {errs()<<"adapter preparation/refusal: "<<error.what()<<"\n";return 1;}
}
