// Synthetic LLVM structure unit only. No Image call, Guest execution or B input.
#define main adapterDraftMainForSyntheticUnit
#include DRAGON_LOOP_NATIVE_ADAPTER_SOURCE
#undef main
#include <llvm/AsmParser/Parser.h>
#include <chrono>
#include <thread>

namespace {
struct UnitChecks {
  unsigned assertions=0;
  std::vector<std::string> failures;
  void check(bool value,const std::string &why){++assertions;if(!value)failures.push_back(why);}
};
enum class ToyMode { Normal, DuplicateCall, WrongTarget, DuplicateGuardEdge,
                     Reentry, GuardStore, Memory, NoSync, CounterCollision };
std::string toy(ToyMode mode) {
  std::string text=R"IR(; Synthetic LLVM only: no production result or closure claim.
@untouched = global i64 123, align 8
COUNTER
define void @ordinary1000(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {
entry:
  call void @ordinary1010(ptr %state, ptr %memory, ptr %runtime, ptr %exit), !dragon.guest.call.target !0
  EXTRA_CALL
  ret void
}
define void @ordinary1010(ptr %state, ptr %memory, ptr %runtime, ptr %exit) {
entry:
  store i64 9, ptr %exit, align 1
  ret void
}
define void @toyview(ptr %state, ptr %memory, ptr %runtime, ptr %exit) ATTRIBUTES {
entry:
  %pc = load i64, ptr %state, align 1
  switch i64 %pc, label %unsupported [ i64 4112, label %bounds ]
bounds:
  GUARD_EFFECT
  %size = load i64, ptr %memory, align 1
  %valid = icmp uge i64 %size, 65536
  FIRST_BRANCH
facts:
  %sp = load i64, ptr %runtime, align 1
  %spok = icmp eq i64 %sp, 51192
  br i1 %spok, label %accepted, label %fallback
accepted:
  br label %body
body:
  store i64 9, ptr %exit, align 1
  ret void
fallback:
  call void @ordinary1010(ptr %state, ptr %memory, ptr %runtime, ptr %exit)
  ret void
unsupported:
  ret void
REENTRY
}
!0 = !{i64 TARGET}
)IR";
  auto change=[&](const std::string &old,const std::string &value){text=replaceOnce(text,old,value);};
  change("COUNTER",mode==ToyMode::CounterCollision ? "@dragon_loop_native_accepted = global i64 0, align 8" : "");
  change("EXTRA_CALL",mode==ToyMode::DuplicateCall ?
    "call void @ordinary1010(ptr %state, ptr %memory, ptr %runtime, ptr %exit), !dragon.guest.call.target !0" : "");
  change("ATTRIBUTES",mode==ToyMode::Memory ? "memory(argmem: readwrite)" : mode==ToyMode::NoSync ? "nosync" : "");
  change("GUARD_EFFECT",mode==ToyMode::GuardStore ? "store i64 1, ptr %exit, align 1" : "");
  change("FIRST_BRANCH",mode==ToyMode::DuplicateGuardEdge ?
    "br i1 %valid, label %fallback, label %fallback" : "br i1 %valid, label %facts, label %fallback");
  change("REENTRY",mode==ToyMode::Reentry ? "reentry:\n  br label %facts" : "");
  change("TARGET",mode==ToyMode::WrongTarget ? "4113" : "4112");return text;
}
std::unique_ptr<Module> parseToy(StringRef text,LLVMContext &context) {
  SMDiagnostic diagnostic;auto module=parseAssemblyString(text,diagnostic,context);
  if(!module){diagnostic.print("synthetic-adapter-unit",errs());throw std::runtime_error("toy parse failed");}
  verify(*module);return module;
}
std::set<Instruction *> nodes(Module &module) {
  std::set<Instruction *> result;for(auto&f:module)for(auto&b:f)for(auto&i:b)result.insert(&i);return result;
}
bool bindingIsCorrect(Module &module) {
  auto *wrapper=module.getFunction("ordinary1000"),*root=module.getFunction("ordinary1010"),*view=module.getFunction("toyview");
  unsigned calls=0;
  for(auto&b:*wrapper)for(auto&i:b)if(auto*call=dyn_cast<CallBase>(&i)){
    ++calls;if(call->getCalledFunction()!=view)return false;
  }
  unsigned fallback=0;
  for(auto&b:*view)if(b.getName()=="fallback")for(auto&i:b)if(auto*call=dyn_cast<CallBase>(&i)){
    ++fallback;if(call->getCalledFunction()!=root)return false;
  }
  return calls==1 && fallback==1;
}
bool counterProtocol(Module &module,const std::set<Instruction *> &before) {
  auto *accepted=module.getGlobalVariable("dragon_loop_native_accepted"),*fallback=module.getGlobalVariable("dragon_loop_native_fallback");
  if(!accepted || !fallback || accepted==fallback)return false;
  std::map<BasicBlock *,unsigned> byBlock;unsigned added=0,loads=0,stores=0,adds=0;
  for(auto&f:module)for(auto&b:f)for(auto&i:b)if(!before.contains(&i)) {
    ++added;++byBlock[&b];
    if(auto*load=dyn_cast<LoadInst>(&i)) {
      if(!load->isVolatile() || load->isAtomic() || !load->getType()->isIntegerTy(64) || load->getAlign()!=Align(8) ||
          (load->getPointerOperand()!=accepted && load->getPointerOperand()!=fallback))return false;
      ++loads;
    } else if(auto*store=dyn_cast<StoreInst>(&i)) {
      if(!store->isVolatile() || store->isAtomic() || store->getAlign()!=Align(8) ||
          (store->getPointerOperand()!=accepted && store->getPointerOperand()!=fallback))return false;
      auto *add=dyn_cast<BinaryOperator>(store->getValueOperand());if(!add || add->getOpcode()!=Instruction::Add)return false;
      auto *load=dyn_cast<LoadInst>(add->getOperand(0));
      if(!load || load->getPointerOperand()!=store->getPointerOperand() || load->getParent()!=&b)return false;
      ++stores;
    } else if(auto*add=dyn_cast<BinaryOperator>(&i)) {
      auto *one=dyn_cast<ConstantInt>(add->getOperand(1));
      if(add->getOpcode()!=Instruction::Add || add->hasPoisonGeneratingAnnotations() || !one || !one->equalsInt(1) ||
          !add->getType()->isIntegerTy(64) || !isa<LoadInst>(add->getOperand(0)))return false;
      ++adds;
    } else return false;
  }
  auto *view=module.getFunction("toyview");BasicBlock *a=nullptr,*b=nullptr;
  for(auto&block:*view){if(block.getName()=="accepted")a=&block;if(block.getName()=="fallback")b=&block;}
  return added==6 && loads==2 && stores==2 && adds==2 && byBlock.size()==2 && byBlock[a]==3 && byBlock[b]==3;
}
json::Object normalCase(const std::filesystem::path &out,UnitChecks &checks) {
  const auto raw=toy(ToyMode::Normal);save(out/"toy-normal.raw.ll",raw);LLVMContext context;auto module=parseToy(raw,context);
  Roles roles{"ordinary1000","ordinary1010","toyview","","",4112};
  auto *root=module->getFunction("ordinary1010"),*view=module->getFunction("toyview"),*wrapper=module->getFunction("ordinary1000");
  const auto rootBefore=printed(*root),viewBefore=printed(*view),globalsBefore=printed(*module->getNamedGlobal("untouched"));
  const auto originalNodes=nodes(*module);CallInst *originalCall=nullptr;
  for(auto&b:*wrapper)for(auto&i:b)if(auto*call=dyn_cast<CallInst>(&i))originalCall=call;
  auto *metadata=originalCall->getMetadata("dragon.guest.call.target");json::Object binding;
  auto *selected=bind(*module,roles,binding);save(out/"toy-normal.adapter.ll",printedModule(*module));
  checks.check(selected==originalCall && bindingIsCorrect(*module),"one actual wrapper callee retarget with ordinary fallback retained");
  checks.check(selected->getMetadata("dragon.guest.call.target")==metadata,"call target attachment identity unchanged");
  checks.check(nodes(*module)==originalNodes,"binding adds/deletes no instructions");
  checks.check(printed(*root)==rootBefore && printed(*view)==viewBefore,"binding preserves ordinary/view bodies");
  auto selectedSites=sites(*view,*root,4112);const auto fallbackCall=printed(*selectedSites.ordinaryCall);
  auto beforeObserver=nodes(*module);auto observation=observe(*module,roles);verify(*module);
  checks.check(counterProtocol(*module,beforeObserver),"exact6 counter events with volatile i64/align8/+1 protocols");
  checks.check(printed(*root)==rootBefore,"observer preserves ordinary kernel");
  checks.check(printed(*selectedSites.ordinaryCall)==fallbackCall && selectedSites.ordinaryCall->getCalledFunction()==root,
               "observer keeps fallback call/arguments/metadata unchanged");
  checks.check(printed(*module->getNamedGlobal("untouched"))==globalsBefore,"observer preserves existing globals");
  checks.check(bindingIsCorrect(*module),"observer preserves direct-call and fallback routing");
  save(out/"toy-normal.observer.ll",printedModule(*module));
  // Deliberate valid-IR wrong outputs prove independent mandatory gates detect errors.
  selected->setCalledFunction(root);verify(*module);
  checks.check(!bindingIsCorrect(*module),"wrong-callee output control detected despite valid IR");
  save(out/"fault-wrong-callee.ll",printedModule(*module));selected->setCalledFunction(view);
  BinaryOperator *increment=nullptr;StoreInst *counterStore=nullptr;
  for(auto&i:*selectedSites.accepted)if(!beforeObserver.contains(&i)) {
    if(auto*add=dyn_cast<BinaryOperator>(&i))increment=add;
    if(auto*store=dyn_cast<StoreInst>(&i))counterStore=store;
  }
  require(increment && counterStore,"counter mutation sites absent");
  increment->setOperand(1,ConstantInt::get(increment->getType(),2));verify(*module);
  checks.check(!counterProtocol(*module,beforeObserver),"wrong +2 counter output detected with six events still present");
  save(out/"fault-double-count.ll",printedModule(*module));increment->setOperand(1,ConstantInt::get(increment->getType(),1));
  counterStore->eraseFromParent();verify(*module);
  checks.check(!counterProtocol(*module,beforeObserver),"omitted volatile counter-store output detected");
  save(out/"fault-omitted-store.ll",printedModule(*module));
  return json::Object{{"case","normal-and-three-wrong-output-controls"},{"binding",std::move(binding)},
                     {"observation",std::move(observation)},{"fault_outputs_verified",3},{"Guest_calls",0}};
}
json::Object refusal(const std::filesystem::path &out,ToyMode mode,StringRef name,StringRef expected,UnitChecks &checks) {
  const auto raw=toy(mode);save(out/(name.str()+".raw.ll"),raw);LLVMContext context;auto module=parseToy(raw,context);
  Roles roles{"ordinary1000","ordinary1010","toyview","","",4112};std::string message;bool rejected=false;
  auto before=printedModule(*module);bool bindOnly=mode==ToyMode::DuplicateCall || mode==ToyMode::WrongTarget;
  try {
    json::Object record;bind(*module,roles,record);
    if(!bindOnly) {before=printedModule(*module);observe(*module,roles);}
  } catch(const std::exception &error) {rejected=true;message=error.what();}
  checks.check(rejected,name.str()+" rejected by actual helper");
  checks.check(message.find(expected.str())!=std::string::npos,name.str()+" expected refusal reason");
  checks.check(printedModule(*module)==before,name.str()+" no unexpected mutation on rejected operation");verify(*module);
  return json::Object{{"case",name.str()},{"rejected",rejected},{"message",message},{"valid_input_IR",true},
                     {"Guest_calls",0},{"Image_calls",0}};
}
int unit(const std::filesystem::path &out) {
  require(!std::filesystem::exists(out),"new synthetic unit output required");std::filesystem::create_directories(out);
  UnitChecks checks;json::Array cases;cases.push_back(normalCase(out,checks));
  struct Negative {ToyMode mode;StringRef name,expected;};
  for(auto item:{Negative{ToyMode::DuplicateCall,"duplicate-direct-call","exactly one direct"},
      Negative{ToyMode::WrongTarget,"wrong-target-attachment","target attachment"},
      Negative{ToyMode::DuplicateGuardEdge,"duplicate-failure-successors","two ordinary-fallback successors"},
      Negative{ToyMode::Reentry,"guard-reentry","reentry/duplicate predecessor"},
      Negative{ToyMode::GuardStore,"effect-in-guard","effect in guard"},
      Negative{ToyMode::Memory,"memory-promise","inherited function promise"},
      Negative{ToyMode::NoSync,"nosync-promise","inherited function promise"},
      Negative{ToyMode::CounterCollision,"counter-collision","counter name collides"}})
    cases.push_back(refusal(out,item.mode,item.name,item.expected,checks));
  json::Array failures;for(auto&why:checks.failures)failures.push_back(why);
  json::Object report{{"schema","synthetic-loop-native-adapter-unit-v1"},{"synthetic_only",true},
    {"actual_bind_observe_bodies_included",true},{"production_main_eligibility_bypassed",false},
    {"B_IR_used",false},{"Image_calls",0},{"Guest_calls",0},{"C_or_Unicorn_calls",0},
    {"assertions",checks.assertions},{"failures",std::move(failures)},{"cases",std::move(cases)}};
  outs()<<formatv("{0:2}\n",json::Value(std::move(report)));return checks.failures.empty()?0:1;
}
}
int main(int argc,char **argv) {
  try {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(argc==2,"one new toy output directory required");return unit(argv[1]);
  } catch(const std::exception &error) {errs()<<"synthetic adapter unit exception: "<<error.what()<<"\n";return 2;}
}
