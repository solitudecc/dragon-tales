// Binding-only validation entry. The included frozen1b7 implementation is unchanged.
#define main frozenObservedAdapterEntryNotCalled
#include DRAGON_LOOP_NATIVE_ADAPTER_SOURCE
#undef main
#include <llvm/AsmParser/Parser.h>

namespace {
int syntheticBindingUnit() {
  const std::string text=R"IR(@keep = global i64 7
define void @wrapper(ptr %s, ptr %m, ptr %r, ptr %e) {
  call void @ordinary(ptr %s, ptr %m, ptr %r, ptr %e), !dragon.guest.call.target !0
  ret void
}
define void @ordinary(ptr %s, ptr %m, ptr %r, ptr %e) {
  store i64 9, ptr %e, align 1
  ret void
}
define void @cleanview(ptr %s, ptr %m, ptr %r, ptr %e) {
  %ok = icmp eq ptr %s, %m
  br i1 %ok, label %body, label %fallback
body:
  store i64 9, ptr %e, align 1
  ret void
fallback:
  call void @ordinary(ptr %s, ptr %m, ptr %r, ptr %e)
  ret void
}
!0 = !{i64 4112}
)IR";
  unsigned checks=0;auto check=[&](bool good,const char *why){++checks;require(good,why);};
  auto make=[&](const std::string &input,LLVMContext &context){SMDiagnostic diagnostic;
    auto module=parseAssemblyString(input,diagnostic,context);require(bool(module),"toy parse failed");verify(*module);return module;};
  Roles roles{"wrapper","ordinary","cleanview","","",4112};
  LLVMContext context;auto module=make(text,context);json::Object binding;
  const auto ordinary=printed(*module->getFunction("ordinary")),view=printed(*module->getFunction("cleanview")),global=printed(*module->getGlobalVariable("keep"));
  auto *call=bind(*module,roles,binding);
  check(call->getCalledFunction()==module->getFunction("cleanview"),"binding did not select clean view");
  check(printed(*module->getFunction("ordinary"))==ordinary,"ordinary changed");
  check(printed(*module->getFunction("cleanview"))==view,"clean view changed");
  check(module->global_size()==1 && printed(*module->getGlobalVariable("keep"))==global,"binding added globals/observer");
  check(!module->getNamedValue("dragon_loop_native_accepted") && !module->getNamedValue("dragon_loop_native_fallback"),"observer was emitted");
  for(bool duplicate:{false,true}) {
    const std::string selected="  call void @ordinary(ptr %s, ptr %m, ptr %r, ptr %e), !dragon.guest.call.target !0\n";
    auto bad=duplicate ? replaceOnce(text,selected,selected+selected)
                       : replaceOnce(text,"!0 = !{i64 4112}","!0 = !{i64 4113}");
    LLVMContext badContext;auto original=make(bad,badContext);const auto before=printedModule(*original);bool refused=false;
    try{json::Object ignored;bind(*original,roles,ignored);}catch(const std::exception&){refused=true;}
    check(refused,"invalid binding was accepted");check(printedModule(*original)==before,"binding refusal mutated module");
  }
  outs()<<formatv("{0:2}\n",json::Value(json::Object{{"synthetic_only",true},{"checks",checks},{"passed",true},
    {"observer_calls",0},{"Guest_calls",0},{"product_pass_calls",0}}));return 0;
}
}
int main(int argc,char **argv) {
  try {
    if(argc==2 && StringRef(argv[1])=="--synthetic-binding-unit")return syntheticBindingUnit();
    require(argc==4,"usage: clean-binding input.ll sealed-roles.json NEW-OUT");
    std::filesystem::path input=argv[1],config=argv[2],out=argv[3];
    require(std::filesystem::is_regular_file(input) && !std::filesystem::is_symlink(input) &&
      std::filesystem::is_regular_file(config) && !std::filesystem::is_symlink(config),"regular input/roles required");
    auto inputBytes=read(input),roleBytes=read(config);auto value=json::parse(roleBytes);
    require(bool(value),"roles JSON invalid");auto *object=value->getAsObject();require(object,"roles object required");
    require(object->getString("schema")=="loop-clean-native-binding-roles-v1" && object->getBoolean("ready")==true &&
      object->getBoolean("eligible_actual_closed_cleanup")==true,"clean binding roles are not sealed/ready");
    Roles roles{string(*object,"wrapper"),string(*object,"ordinary_kernel"),string(*object,"view"),
      string(*object,"production_ir_sha256"),string(*object,"production_first_line"),0};
    auto pc=object->getInteger("kernel_pc");require(pc && *pc>=0,"kernel PC missing");roles.kernelPC=*pc;
    require(sha(inputBytes)==roles.sourceSHA && inputBytes.substr(0,inputBytes.find('\n'))==roles.sourceLiteral,"clean input pin/ModuleID differs");
    require(object->getString("cleanup_report_sha256") && object->getString("cleanup_report_sha256")->size()==64 &&
      object->getString("closed_source_sha256")=="714701bcb58d9d3b5298ed9393e7479006aa6417c1ec60cb7a0c840069e52f74", "sealed cleanup lineage absent");
    require(!std::filesystem::exists(out),"new output required");
    LLVMContext context;auto module=parse(input,context);const auto parsed=printedModule(*module);json::Object binding;
    bind(*module,roles,binding);verify(*module);const auto adapter=printedModule(*module);
    require(read(input)==inputBytes && read(config)==roleBytes,"immutable cleanup source/roles changed");
    std::filesystem::create_directories(out);save(out/"production.parsed-before.ll",parsed);save(out/"native-adapter.ll",adapter);
    save(out/"REPORT.json",formatv("{0:2}\n",json::Value(json::Object{{"schema","loop-clean-native-binding-emission-v1"},
      {"input_sha256",sha(inputBytes)},{"input_first_line",roles.sourceLiteral},{"actual_parsed_ModuleID",module->getModuleIdentifier()},
      {"roles_sha256",sha(roleBytes)},{"parsed_before_sha256",sha(parsed)},{"native_adapter_sha256",sha(adapter)},
      {"binding",std::move(binding)},{"binding_only",true},{"observer_calls",0},{"Guest_calls",0},{"product_pass_calls",0},
      {"production_input_unchanged",true},{"whole_module_verified",true},{"closure_proved_by_adapter",false},
      {"native_semantics_validated",false}})).str());return 0;
  }catch(const std::exception &error){errs()<<"clean binding refusal: "<<error.what()<<"\n";return 1;}
}
