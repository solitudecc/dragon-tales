#pragma once
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/TrackingMDRef.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>
namespace probe_identity {
inline void ensure(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
inline std::string render(const llvm::Value &v){std::string s;llvm::raw_string_ostream o(s);v.print(o);return s;}
struct FunctionState {llvm::WeakVH value;std::vector<llvm::WeakVH> args;std::string body;};
struct GlobalState {llvm::WeakVH value;std::string body;};
struct NamedState {llvm::NamedMDNode *node;std::vector<llvm::Metadata *> operands;};
struct MetadataState {llvm::TrackingMDRef value;std::vector<llvm::Metadata *> operands;bool node=false,distinct=false;std::string text;llvm::WeakVH referencedValue;llvm::Value *originalValue=nullptr;};
struct Identity {
 llvm::Module *module;
 std::map<std::string,FunctionState> functions;
 std::map<std::string,GlobalState> globals;
 std::map<std::string,NamedState> named;
 std::map<llvm::Metadata *,MetadataState> metadata;
 std::string identifier,sourceFile,triple,layout,inlineAssembly;
 void rememberMetadata(llvm::Metadata *md){
  if(!md||metadata.contains(md))return;
  MetadataState state;state.value.reset(md);
  if(auto *n=llvm::dyn_cast<llvm::MDNode>(md)){state.node=true;state.distinct=n->isDistinct();for(auto &x:n->operands())state.operands.push_back(x.get());}
  else if(auto *s=llvm::dyn_cast<llvm::MDString>(md))state.text=s->getString().str();
  else if(auto *v=llvm::dyn_cast<llvm::ValueAsMetadata>(md)){state.referencedValue=v->getValue();state.originalValue=v->getValue();}
  auto children=state.operands;metadata.emplace(md,std::move(state));for(auto *child:children)rememberMetadata(child);
 }
 Identity(llvm::Module &m,llvm::StringRef except):module(&m),identifier(m.getModuleIdentifier()),sourceFile(m.getSourceFileName()),triple(m.getTargetTriple().str()),layout(m.getDataLayoutStr()),inlineAssembly(m.getModuleInlineAsm()){
  for(auto &f:m){FunctionState x;x.value=&f;for(auto &a:f.args())x.args.emplace_back(&a);if(f.getName()!=except)x.body=render(f);functions.emplace(f.getName().str(),std::move(x));}
  for(auto &v:m.global_values())if(!llvm::isa<llvm::Function>(v))globals.emplace(v.getName().str(),GlobalState{llvm::WeakVH(&v),render(v)});
  for(auto &n:m.named_metadata()){NamedState x{&n,{}};for(auto *md:n.operands()){x.operands.push_back(md);rememberMetadata(md);}named.emplace(n.getName().str(),std::move(x));}
 }
 void checkLegacyExactSet(llvm::Module &m,llvm::StringRef except){
  ensure(&m==module&&m.size()==functions.size(),"module/Function set changed");
  for(auto &[name,old]:functions){auto *f=m.getFunction(name);ensure(f&&static_cast<llvm::Value *>(old.value)==f,"Function identity changed");ensure(f->arg_size()==old.args.size(),"argument count changed");for(unsigned j=0;j<f->arg_size();++j)ensure(f->getArg(j)==static_cast<llvm::Value *>(old.args[j]),"Argument identity changed");if(f->getName()!=except)ensure(render(*f)==old.body,"ordinary root/caller/body changed");}
 }
 llvm::json::Object check(llvm::Module &m,llvm::StringRef except,bool successfulClosedChange=false){
  ensure(&m==module,"Module identity changed");
  ensure(m.getModuleIdentifier()==identifier&&m.getSourceFileName()==sourceFile&&m.getTargetTriple().str()==triple&&m.getDataLayoutStr()==layout&&m.getModuleInlineAsm()==inlineAssembly,"Module header changed");
  std::vector<llvm::Function *> added;for(auto &f:m)if(!functions.contains(f.getName().str()))added.push_back(&f);
  const bool permit=successfulClosedChange&&!except.empty();
  ensure(added.empty()||permit,"module/Function set changed");
  ensure(m.size()==functions.size()+added.size(),"original Function removed");
  llvm::json::Array imports;
  for(auto *f:added){
   ensure(f->getName()=="llvm.fshl.i32"&&f->getIntrinsicID()==llvm::Intrinsic::fshl&&f->isDeclaration(),"unexpected new Function or definition");
   auto *i32=llvm::Type::getInt32Ty(m.getContext());auto *type=llvm::FunctionType::get(i32,{i32,i32,i32},false);
   ensure(f->getFunctionType()==type&&f->getAddressSpace()==0,"new intrinsic FunctionType mismatch");
   ensure(f->getAttributes()==llvm::Intrinsic::getAttributes(m.getContext(),llvm::Intrinsic::fshl,type),"new intrinsic attributes not canonical");
   ensure(f->getCallingConv()==llvm::CallingConv::C&&f->getLinkage()==llvm::GlobalValue::ExternalLinkage&&f->getVisibility()==llvm::GlobalValue::DefaultVisibility&&f->getDLLStorageClass()==llvm::GlobalValue::DefaultStorageClass&&!f->hasComdat()&&!f->hasGC()&&!f->hasPersonalityFn()&&!f->hasPrefixData()&&!f->hasPrologueData()&&!f->hasSection()&&!f->isUsedByMetadata(),"new intrinsic declaration properties not canonical");
   ensure(!f->use_empty(),"new intrinsic is unused");
   llvm::json::Array uses;
   for(auto *user:f->users()){
    auto *call=llvm::dyn_cast<llvm::CallInst>(user);
    ensure(call&&call->getFunction()==m.getFunction(except)&&call->getCalledFunction()==f&&call->getCalledOperand()==f&&call->getFunctionType()==type,"new intrinsic user is not a direct CallInst in selected view");
    uses.push_back(llvm::json::Object{{"function",call->getFunction()->getName().str()},{"block",call->getParent()->getName().str()},{"call",render(*call)}});
   }
   std::string attrs;llvm::raw_string_ostream attrOut(attrs);f->getAttributes().print(attrOut);
   imports.push_back(llvm::json::Object{{"name",f->getName().str()},{"declaration",render(*f)},{"attributes",attrs},{"canonical_Intrinsic_getAttributes",true},{"users",std::move(uses)}});
  }
  for(auto &[name,old]:functions){auto *f=m.getFunction(name);ensure(f&&static_cast<llvm::Value *>(old.value)==f,"Function identity changed");ensure(f->arg_size()==old.args.size(),"Argument count changed");for(unsigned j=0;j<f->arg_size();++j)ensure(static_cast<llvm::Value *>(old.args[j])==f->getArg(j),"Argument identity changed");if(f->getName()!=except)ensure(render(*f)==old.body,"ordinary root/caller/body changed");}
  std::map<std::string,llvm::GlobalValue *> current;for(auto &v:m.global_values())if(!llvm::isa<llvm::Function>(v))current.emplace(v.getName().str(),&v);ensure(current.size()==globals.size(),"global set changed");
  for(auto &[name,old]:globals){auto it=current.find(name);ensure(it!=current.end()&&static_cast<llvm::Value *>(old.value)==it->second&&render(*it->second)==old.body,"global identity or bytes changed");}
  ensure(static_cast<std::size_t>(std::distance(m.named_metadata_begin(),m.named_metadata_end()))==named.size(),"named metadata set changed");
  for(auto &[name,old]:named){auto *n=m.getNamedMetadata(name);ensure(n==old.node&&n->getNumOperands()==old.operands.size(),"named metadata identity/arity changed");for(unsigned j=0;j<old.operands.size();++j)ensure(n->getOperand(j)==old.operands[j],"named metadata root changed");}
  for(auto &[original,old]:metadata){auto *md=old.value.get();ensure(md==original,"named metadata dependency identity changed");if(old.node){auto *n=llvm::cast<llvm::MDNode>(md);ensure(n->isDistinct()==old.distinct&&n->getNumOperands()==old.operands.size(),"named metadata node shape changed");for(unsigned j=0;j<old.operands.size();++j)ensure(n->getOperand(j).get()==old.operands[j],"named metadata edge/payload changed");}else if(auto *s=llvm::dyn_cast<llvm::MDString>(md))ensure(s->getString()==old.text,"named metadata string changed");else if(auto *v=llvm::dyn_cast<llvm::ValueAsMetadata>(md))ensure(v->getValue()==old.originalValue&&static_cast<llvm::Value *>(old.referencedValue)==old.originalValue,"named metadata Value changed");}
  return llvm::json::Object{{"all_original_Function_Argument_objects_preserved",true},{"all_nonview_Functions_exact_text",true},{"all_globals_identity_and_text_preserved",true},{"all_named_metadata_identity_graph_preserved",true},{"source_scope_forbids_additions",except.empty()},{"successful_closed_changed_required_for_import",true},{"imports",std::move(imports)}};
 }
};
}
