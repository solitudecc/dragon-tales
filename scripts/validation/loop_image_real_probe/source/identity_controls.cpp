#include "probe_identity.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FormatVariadic.h>
#include <iostream>
using namespace llvm;
struct Toy {LLVMContext c;Module m{"identity-controls",c};Function *view,*ordinary;GlobalVariable *global;MDNode *md;
 Toy(){auto *i32=Type::getInt32Ty(c);auto *t=FunctionType::get(i32,{i32},false);ordinary=Function::Create(t,GlobalValue::ExternalLinkage,"ordinary",m);view=Function::Create(t,GlobalValue::ExternalLinkage,"view",m);for(auto *f:{ordinary,view}){auto *bb=BasicBlock::Create(c,"entry",f);IRBuilder<> b(bb);b.CreateRet(f->getArg(0));}global=new GlobalVariable(m,i32,false,GlobalValue::InternalLinkage,ConstantInt::get(i32,9),"global");md=MDNode::getDistinct(c,{ConstantAsMetadata::get(ConstantInt::get(i32,9))});m.getOrInsertNamedMetadata("kept")->addOperand(md);}
 Function *fshl32(bool use=true,Function *target=nullptr){auto *f=Intrinsic::getDeclaration(&m,Intrinsic::fshl,{Type::getInt32Ty(c)});if(use){auto *t=target?target:view;IRBuilder<> b(t->getEntryBlock().getTerminator());auto *call=b.CreateCall(f,{t->getArg(0),t->getArg(0),b.getInt32(13)});if(t==view)cast<ReturnInst>(t->getEntryBlock().getTerminator())->setOperand(0,call);}return f;}
 void verify(){std::string why;raw_string_ostream o(why);if(verifyModule(m,&o))throw std::runtime_error("invalid control IR: "+why);}
};
int main(){json::Array results;unsigned failures=0;auto run=[&](const char *name,bool expected,int strict,auto edit){Toy t;probe_identity::Identity identity(t.m,"view");edit(t);t.verify();bool accepted=false;std::string reason;try{if(strict==1)identity.checkLegacyExactSet(t.m,"view");else identity.check(t.m,"view",strict==0);accepted=true;}catch(const std::exception &e){reason=e.what();}bool pass=accepted==expected;if(!pass)++failures;results.push_back(json::Object{{"case",name},{"IR_valid",true},{"expected_accept",expected},{"actual_accept",accepted},{"control_detected",pass},{"reason",reason}});};
 try{
 run("canonical fshl import accepted",true,false,[](Toy&t){t.fshl32();});
 run("old strict exact-set rejects same positive",false,true,[](Toy&t){t.fshl32();});
 run("not successful closed changed rejects same import",false,2,[](Toy&t){t.fshl32();});
 run("ordinary external declaration refused",false,false,[](Toy&t){auto *f=Function::Create(t.view->getFunctionType(),GlobalValue::ExternalLinkage,"external",t.m);IRBuilder<> b(t.view->getEntryBlock().getTerminator());b.CreateCall(f,{t.view->getArg(0)});});
 run("noncanonical intrinsic attributes refused",false,false,[](Toy&t){auto *f=t.fshl32();f->removeFnAttr(Attribute::NoFree);});
 run("new definition refused",false,false,[](Toy&t){auto *f=Function::Create(t.view->getFunctionType(),GlobalValue::ExternalLinkage,"extra",t.m);auto *bb=BasicBlock::Create(t.c,"entry",f);IRBuilder<> b(bb);b.CreateRet(f->getArg(0));});
 run("unused fshl refused",false,false,[](Toy&t){t.fshl32(false);});
 run("nonview fshl use refused",false,false,[](Toy&t){t.fshl32(true,t.ordinary);});
 run("ordinary body mutation refused",false,false,[](Toy&t){IRBuilder<> b(t.ordinary->getEntryBlock().getTerminator());auto *a=b.CreateAdd(t.ordinary->getArg(0),b.getInt32(1));cast<ReturnInst>(t.ordinary->getEntryBlock().getTerminator())->setOperand(0,a);});
 run("global initializer mutation refused",false,false,[](Toy&t){t.global->setInitializer(ConstantInt::get(Type::getInt32Ty(t.c),10));});
 run("named metadata payload mutation refused",false,false,[](Toy&t){t.md->replaceOperandWith(0,ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(t.c),10)));});
 run("new named metadata refused",false,false,[](Toy&t){t.m.getOrInsertNamedMetadata("unexpected")->addOperand(t.md);});
 run("wrong-width canonical intrinsic refused",false,false,[](Toy&t){auto *f=Intrinsic::getDeclaration(&t.m,Intrinsic::fshl,{Type::getInt64Ty(t.c)});IRBuilder<> b(t.view->getEntryBlock().getTerminator());auto *a=b.CreateZExt(t.view->getArg(0),b.getInt64Ty());b.CreateCall(f,{a,a,b.getInt64(13)});});
 Toy t;probe_identity::Identity source(t.m,"");t.fshl32();t.verify();bool refused=false;try{source.check(t.m,"",true);}catch(...){refused=true;}if(!refused)++failures;results.push_back(json::Object{{"case","sourceIdentity forbids even canonical import"},{"IR_valid",true},{"control_detected",refused}});
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 2;}
 outs()<<formatv("{0:2}",json::Value(json::Object{{"schema","intrinsic-import-identity-controls-v1"},{"cases",std::move(results)},{"failures",failures},{"Image_API_calls",0},{"Guest_native_runs",0}}))<<"\n";return failures?1:0;
}
