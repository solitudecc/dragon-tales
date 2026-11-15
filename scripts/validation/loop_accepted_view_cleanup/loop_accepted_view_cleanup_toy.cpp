// Mechanical tests only. Includes the unchanged frozen validation draft.
// No producer REPORT is forged and no production input is read.
#define main frozen_cleanup_readiness_entry
#include "loop_accepted_view_cleanup.cpp"
#undef main
namespace {
std::unique_ptr<Module> toyParse(StringRef raw,LLVMContext&context){SMDiagnostic error;auto m=parseAssemblyString(raw,error,context);if(!m){error.print("cleanup-toy",errs());throw std::runtime_error("invalid toy source");}verifyWhole(*m);return m;}
json::Object lookupPlan(Function&f){auto *load=dyn_cast<LoadInst>(&f.getEntryBlock().front());need(load&&load->getName()=="old.lookup","toy lookup shape differs");json::Array rows;rows.push_back(json::Object{{"block_ordinal",0},{"instruction_ordinal",0},{"load_ir_sha256",sha(render(*load))},{"terminator_ir_sha256",sha(render(*f.getEntryBlock().getTerminator()))}});return json::Object{{"lookups",std::move(rows)}};}
}
int main(int argc,char**argv){
  fs::path output;json::Array stages,controls;json::Object report{{"schema","accepted-view-cleanup-mechanical-toy-v1"},{"ok",false},{"production_inputs_read",0},{"Guest_native_runs",0},{"Unicorn_runs",0},{"production_cleanup_runs",0}};
  try{
    need(argc==3,"usage: toy ABS-PLAN ABS-NEW-OUT");output=argv[2];need(output.is_absolute()&&!fs::exists(output),"fresh toy output required");fs::create_directories(output);auto buf=MemoryBuffer::getFile(argv[1]);need(bool(buf),"toy plan missing");auto plan=parsedObject((*buf)->getBuffer());need(textField(plan,"schema")=="accepted-view-cleanup-mechanical-plan-v1","wrong toy plan");
    auto source=pin(objectField(plan,"toy_ir")),emitter=pin(objectField(plan,"emitter_source")),test=pin(objectField(plan,"test_source")),runtime=pin(objectField(plan,"runtime")),solver=pin(objectField(plan,"solver"));const auto raw=read(source),draft=read(emitter);check(test);check(runtime);check(solver);need(emitter.hash=="0ffa1459a03ad2526b02dd1538326a6f7974895dde469d8c47cb9b061bc947b2","frozen emitter changed");
    const std::string recipe="run(\"01-sccp\",SCCPPass());run(\"02-dfa-jump-threading\",DFAJumpThreadingPass());run(\"03-jump-threading\",JumpThreadingPass());run(\"04-simplify-cfg\",SimplifyCFGPass());run(\"05-instcombine\",InstCombinePass());run(\"06-dce\",DCEPass());";
    need(draft.find(recipe)!=std::string::npos&&draft.find("fpm.run(*view,fam);")!=std::string::npos,"draft recipe/selected-function call differs");report["runtime_binding_before"]=runtimeBinding(runtime,solver);save(output/"toy-source.ll",raw);
    {
      LLVMContext context;auto m=toyParse(raw,context);auto *view=m->getFunction("view");need(view,"toy view missing");Outside outside(*m,*view);auto inventory=lookupPlan(*view);auto old=lookups(*view,inventory);const auto before=render(*m),viewBefore=render(*view);save(output/"toy-before.ll",before);stages.push_back(json::Object{{"stage","00-before"},{"structure",structure(*view,old)}});
      LoopAnalysisManager lam;FunctionAnalysisManager fam;CGSCCAnalysisManager cgam;ModuleAnalysisManager mam;PassBuilder pb;pb.registerModuleAnalyses(mam);pb.registerCGSCCAnalyses(cgam);pb.registerFunctionAnalyses(fam);pb.registerLoopAnalyses(lam);pb.crossRegisterProxies(lam,fam,cgam,mam);
      unsigned passes=0;
      auto run=[&](StringRef label,auto pass){++passes;FunctionPassManager fpm;fpm.addPass(std::move(pass));fpm.run(*view,fam);verifyWhole(*m);outside.check(*m,*view);save(output/(label.str()+".ll"),render(*m));stages.push_back(json::Object{{"stage",label.str()},{"whole_verified",true},{"outside_text_and_identity_unchanged",true},{"structure",structure(*view,old)}});};
      // Exact same six default passes and selected-function mechanism as draft.
      run("01-sccp",SCCPPass());run("02-dfa-jump-threading",DFAJumpThreadingPass());run("03-jump-threading",JumpThreadingPass());run("04-simplify-cfg",SimplifyCFGPass());run("05-instcombine",InstCombinePass());run("06-dce",DCEPass());
      need(passes==6&&render(*view)!=viewBefore,"toy selected function did not simplify");need(static_cast<Value*>(old.front().load)==nullptr,"dead original lookup not tracked as deleted");report["recipe_runs"]=1;report["passes_verified"]=passes;report["view_simplified"]=true;report["original_lookup_deleted_safely"]=true;save(output/"toy-after.ll",render(*m));
    }
    for(const std::string kind:{"ordinary-body","global-value","named-metadata","invalid-IR"}){
      LLVMContext context;auto m=toyParse(raw,context);auto *view=m->getFunction("view");Outside outside(*m,*view);const auto before=render(*m);std::string expected;
      if(kind=="ordinary-body"){auto *i=dyn_cast<BinaryOperator>(&m->getFunction("ordinary")->getEntryBlock().front());need(i,"toy ordinary shape");i->setOperand(1,ConstantInt::get(Type::getInt64Ty(context),1));expected="ordinary Function text changed";}
      else if(kind=="global-value"){m->getGlobalVariable("global")->setInitializer(ConstantInt::get(Type::getInt64Ty(context),8));expected="module globals/declarations/attributes/metadata or view interface changed";}
      else if(kind=="named-metadata"){auto *n=m->getNamedMetadata("toy.stable");need(n,"toy metadata absent");n->clearOperands();n->addOperand(MDNode::get(context,{MDString::get(context,"changed")}));expected="module globals/declarations/attributes/metadata or view interface changed";}
      else{ReturnInst *ret=nullptr;for(auto&b:*view)if(auto*r=dyn_cast<ReturnInst>(b.getTerminator())){ret=r;break;}need(ret,"toy return absent");ret->setOperand(0,ConstantInt::get(Type::getInt32Ty(context),0));expected="whole-Module verify failed";}
      need(render(*m)!=before,"control failed to mutate toy");save(output/(kind+".ll"),render(*m));bool caught=false;std::string message;
      try{verifyWhole(*m);outside.check(*m,*view);}catch(const std::exception&e){message=e.what();caught=message.find(expected)!=std::string::npos;}
      need(caught,"control not detected by required checker: "+kind);controls.push_back(json::Object{{"control",kind},{"detected",true},{"message",message},{"passes_run",0}});
    }
    // A negative readiness plan only. Never invent a positive closed report.
    const auto rejectedPlan=output/"not-ready-plan.json";saveJSON(rejectedPlan,json::Object{{"schema","dragon-accepted-view-cleanup-plan-v1"},{"ready",false}});std::string program="cleanup-not-ready",planPath=rejectedPlan.string(),outPath=(output/"not-ready").string();char *args[]={program.data(),planPath.data(),outPath.data()};need(frozen_cleanup_readiness_entry(3,args)==1,"not-ready input accepted");auto rejectedBuf=MemoryBuffer::getFile(outPath+"/REPORT.json");need(bool(rejectedBuf),"not-ready receipt absent");auto rejected=parsedObject((*rejectedBuf)->getBuffer());need(integerField(rejected,"passes_attempted")==0,"not-ready input executed passes");controls.push_back(json::Object{{"control","not-ready-plan"},{"detected",true},{"passes_run",0}});
    for(auto&p:{source,emitter,test,runtime,solver})check(p);report["runtime_binding_after"]=runtimeBinding(runtime,solver);report["source_unchanged"]=true;report["controls_detected"]=controls.size();report["stages"]=std::move(stages);report["controls"]=std::move(controls);report["ok"]=true;saveJSON(output/"REPORT.json",std::move(report));outs()<<"toy: six selected-function passes and five rejection controls passed\n";return 0;
  }catch(const std::exception&e){report["failure"]=e.what();report["stages"]=std::move(stages);report["controls"]=std::move(controls);if(!output.empty()&&fs::exists(output)&&!fs::exists(output/"REPORT.json"))saveJSON(output/"REPORT.json",std::move(report));errs()<<e.what()<<"\n";return 1;}
}
