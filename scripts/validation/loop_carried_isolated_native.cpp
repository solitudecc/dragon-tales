// Preserve the original producer verbatim. This variant isolates the old-B
// snapshot domain from the separately conservative future-S dependency.
#define main original_loop_carried_main
#include "loop_carried_context_native.cpp"
#undef main

namespace {
View isolated(llvm::Module &m,Mode mode,Fault fault=None) {
  auto view=build(m,mode,fault);
  llvm::PHINode *counter=nullptr;
  for(auto &phi:view.head->phis())if(phi.getName()=="iteration")counter=&phi;
  if(!counter)std::exit(7);
  auto *oldStep=llvm::dyn_cast<llvm::BinaryOperator>(counter->getIncomingValueForBlock(view.body));
  if(!oldStep||oldStep->getOpcode()!=llvm::Instruction::Add||oldStep->hasPoisonGeneratingFlags())std::exit(8);
  Builder b(view.body->getTerminator());
  // Same pure add, computed in the body. The table-block add is retained for
  // direct table->join reentry. This changes no byte-model behavior.
  auto *bodyStep=b.CreateAdd(oldStep->getOperand(0),oldStep->getOperand(1),"existing.body.counter");
  counter->setIncomingValue(counter->getBasicBlockIndex(view.body),bodyStep);
  return view;
}
}
int main(int argc,char **argv) {
  if(argc!=2)return 2;
  const std::filesystem::path out(argv[1]);
  for(unsigned mode=Phi;mode<=Freeze;++mode) {
    llvm::LLVMContext c;llvm::Module m("old-B-isolated-input",c);m.setDataLayout("e-p:64:64");
    auto view=isolated(m,static_cast<Mode>(mode));
    if(llvm::verifyModule(m,&llvm::errs()))return 3;
    dump(m,out/(std::string(Names[mode])+"-before.ll"));
    auto stats=dragon::llvmlifter::runDispatcherSpecialization(m);
    const bool invalid=llvm::verifyModule(m,&llvm::errs());
    dump(m,out/(std::string(Names[mode])+"-after.ll"));if(invalid)return 3;
    const auto known=indices(m,view.fn->getName().str());
    llvm::outs()<<"case="<<Names[mode]<<" changed="<<stats.changed<<" complete="<<stats.complete
      <<" folds="<<stats.branchesRewritten<<" redirected="<<redirects(view)
      <<" body_redirected="<<(view.body->getTerminator()->getSuccessor(0)!=view.head)
      <<" unknown_retained="<<(view.entry[3]->getTerminator()->getSuccessor(0)==view.head)
      <<" index16="<<known.contains(0x480)<<" invalid="<<invalid<<'\n';
  }
  for(unsigned fault=OldState;fault<=Flags;++fault) {
    llvm::LLVMContext c;llvm::Module m("old-B-isolated-fault",c);m.setDataLayout("e-p:64:64");
    isolated(m,Phi,static_cast<Fault>(fault));if(llvm::verifyModule(m,&llvm::errs()))return 3;
    dump(m,out/(std::string("control_")+FaultNames[fault]+".ll"));
  }
}
