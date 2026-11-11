// Validation-only observer. Link against LLVM, never libdragon. Do not optimize
// the emitted module: the only additions are independent host counter accesses.
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace {
struct Site {
  std::uint64_t id;llvm::BasicBlock *block;std::string kind,label;
  unsigned slot=0;bool member=false;std::vector<std::uint64_t> values;
};
int fail(llvm::StringRef message){llvm::errs()<<"observer: "<<message<<'\n';return 2;}
std::string sha(llvm::StringRef bytes) {
  auto digest=llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.data()),bytes.size()));
  return llvm::toHex(llvm::ArrayRef<std::uint8_t>(digest),true);
}
}
int main(int argc,char **argv) {
  if(argc<5 || argc>6)return fail("usage: fast_loop_instrument input.ll PLAN.json output.ll report.json [double-count|omit-central]");
  std::string fault=argc==6?argv[5]:"none";
  if(fault!="none" && fault!="double-count" && fault!="omit-central")return fail("unknown fault control");
  auto input=llvm::MemoryBuffer::getFile(argv[1]);if(!input)return fail("cannot read input IR");
  auto planBytes=llvm::MemoryBuffer::getFile(argv[2]);if(!planBytes)return fail("cannot read plan");
  if((*planBytes)->getBufferSize()>4*1024*1024)return fail("oversized plan");
  auto parsed=llvm::json::parse((*planBytes)->getBuffer());if(!parsed){llvm::consumeError(parsed.takeError());return fail("invalid plan JSON");}
  auto *plan=parsed->getAsObject();if(!plan)return fail("plan must be an object");
  auto schema=plan->getString("schema"),name=plan->getString("function"),expected=plan->getString("input_ir_sha256");
  auto *rows=plan->getArray("sites"),*centrals=plan->getArray("central_blocks");
  const auto inputSHA=sha((*input)->getBuffer());
  if(!schema || *schema!="final-ir-loop-observation-v1" || !name || !expected || *expected!=inputSHA || !rows || rows->empty() || rows->size()>8192 || !centrals)return fail("plan schema hash or site limits mismatch");
  llvm::LLVMContext context;llvm::SMDiagnostic diagnostic;
  auto module=llvm::parseIR((*input)->getMemBufferRef(),diagnostic,context);
  if(!module){diagnostic.print(argv[0],llvm::errs());return 2;}
  if(llvm::verifyModule(*module,&llvm::errs()))return fail("input IR does not verify");
  auto *function=module->getFunction(*name);if(!function || function->isDeclaration())return fail("planned function is missing");
  if(module->getNamedValue("dragon_observer_counts") || module->getNamedValue("dragon_observer_count"))return fail("observer global symbol collision");
  // These raw execution-ABI inputs do not make restrictive effect promises.
  // Reject rather than silently leave stale caller/callee effect attributes.
  for(auto &fn:*module)if(!fn.isDeclaration() && (fn.hasFnAttribute(llvm::Attribute::Memory) || fn.hasFnAttribute(llvm::Attribute::Speculatable)))return fail("defined function has unsupported memory/speculatable attributes");
  for(auto &fn:*module)for(auto &block:fn)for(auto &instruction:block)if(auto *call=llvm::dyn_cast<llvm::CallBase>(&instruction);call && (call->hasFnAttr(llvm::Attribute::Memory) || call->hasFnAttr(llvm::Attribute::Speculatable)))return fail("callsite has restrictive effect attributes");
  std::vector<Site> sites;std::set<std::string> centralNames,siteCentrals;
  for(const auto &value:*centrals){auto n=value.getAsString();if(!n || !centralNames.insert(n->str()).second)return fail("invalid duplicate central block");}
  for(const auto &value:*rows) {
    auto *row=value.getAsObject();if(!row)return fail("site must be an object");
    auto id=row->getInteger("id");auto blockName=row->getString("block"),kind=row->getString("kind"),label=row->getString("label");
    if(!id || *id<0 || static_cast<std::uint64_t>(*id)!=sites.size() || !blockName || !kind || !label)return fail("site identity mismatch");
    llvm::BasicBlock *block=nullptr;for(auto &b:*function)if(b.getName()==*blockName){block=&b;break;}
    if(!block || block->isEHPad())return fail("site block absent or unsupported EH");
    Site site{static_cast<std::uint64_t>(*id),block,kind->str(),label->str()};
    if(site.kind=="branch") {
      auto slot=row->getInteger("slot");auto *branch=llvm::dyn_cast<llvm::BranchInst>(block->getTerminator());
      if(!slot || *slot<0 || *slot>1 || !branch || !branch->isConditional())return fail("site branch slot mismatch");site.slot=static_cast<unsigned>(*slot);
    } else if(site.kind=="membership") {
      auto member=row->getBoolean("member");auto *values=row->getArray("values");auto *jump=llvm::dyn_cast<llvm::SwitchInst>(block->getTerminator());
      if(!member || !values || values->empty() || values->size()>16 || !jump || !jump->getCondition()->getType()->isIntegerTy(64))return fail("membership selector mismatch");
      auto *load=llvm::dyn_cast<llvm::LoadInst>(jump->getCondition());if(!load || !load->isSimple() || load->getParent()!=block)return fail("membership must reuse existing simple same-block LOAD");site.member=*member;
      std::set<std::uint64_t> supplied,actual;
      for(const auto &v:*values){auto integer=v.getAsInteger();if(!integer || *integer<0 || !supplied.insert(*integer).second)return fail("invalid membership candidate");site.values.push_back(static_cast<std::uint64_t>(*integer));}
      for(auto item:jump->cases())actual.insert(item.getCaseValue()->getZExtValue());
      if(supplied!=actual)return fail("membership values differ from actual small-switch cases");
    } else if(site.kind!="visit")return fail("unknown site kind");
    if(site.label=="central_dispatcher") {
      if(site.kind!="visit" || !llvm::isa<llvm::SwitchInst>(block->getTerminator()) || !siteCentrals.insert(blockName->str()).second)return fail("central observation shape mismatch");
    }
    sites.push_back(std::move(site));
  }
  if(centralNames!=siteCentrals)return fail("central plan coverage mismatch");
  if(fault=="omit-central" && siteCentrals.empty())return fail("omit-central control requires a central site");
  auto *i64=llvm::Type::getInt64Ty(context);auto *arrayType=llvm::ArrayType::get(i64,sites.size());
  auto *counts=new llvm::GlobalVariable(*module,arrayType,false,llvm::GlobalValue::ExternalLinkage,llvm::ConstantAggregateZero::get(arrayType),"dragon_observer_counts");counts->setAlignment(llvm::Align(8));
  auto *count=new llvm::GlobalVariable(*module,i64,true,llvm::GlobalValue::ExternalLinkage,llvm::ConstantInt::get(i64,sites.size()),"dragon_observer_count");count->setAlignment(llvm::Align(8));
  llvm::json::Array emitted,omitted;bool skipped=false;
  for(const auto &site:sites) {
    if(fault=="omit-central" && !skipped && site.label=="central_dispatcher"){skipped=true;omitted.push_back(site.id);continue;}
    llvm::IRBuilder<> ir(site.block->getTerminator());llvm::Value *increment=ir.getInt64(1);
    if(site.kind=="branch") {
      auto *branch=llvm::cast<llvm::BranchInst>(site.block->getTerminator());llvm::Value *condition=branch->getCondition();if(site.slot==1)condition=ir.CreateNot(condition,"observer.not");increment=ir.CreateZExt(condition,i64,"observer.edge");
    } else if(site.kind=="membership") {
      auto *selector=llvm::cast<llvm::SwitchInst>(site.block->getTerminator())->getCondition();llvm::Value *member=ir.getFalse();
      for(auto value:site.values)member=ir.CreateOr(member,ir.CreateICmpEQ(selector,ir.getInt64(value)),"observer.member");
      if(!site.member)member=ir.CreateNot(member);increment=ir.CreateZExt(member,i64,"observer.set");
    }
    if(fault=="double-count")increment=ir.CreateMul(increment,ir.getInt64(2));
    auto *address=ir.CreateGEP(arrayType,counts,{ir.getInt64(0),ir.getInt64(site.id)},"observer.address");
    auto *old=ir.CreateLoad(i64,address,"observer.old");old->setVolatile(true);old->setAlignment(llvm::Align(8));
    // No overflow/poison flags. Counts are modular u64, as declared to the
    // checker; the finite native runs remain far below this range.
    auto *next=ir.CreateAdd(old,increment,"observer.next");auto *store=ir.CreateStore(next,address);store->setVolatile(true);store->setAlignment(llvm::Align(8));
    emitted.push_back(llvm::json::Object{{"id",site.id},{"block",site.block->getName().str()},{"kind",site.kind},{"label",site.label}});
  }
  if(llvm::verifyModule(*module,&llvm::errs()))return fail("instrumented IR does not verify");
  std::string output;llvm::raw_string_ostream stream(output);module->print(stream,nullptr);stream.flush();
  std::error_code error;llvm::raw_fd_ostream destination(argv[3],error);if(error)return fail("cannot create output IR");destination<<output;destination.close();if(destination.has_error())return fail("cannot write output IR");
  llvm::json::Object report{{"schema","final-ir-loop-observer-emission-v1"},{"function",name->str()},{"input_ir_sha256",inputSHA},{"plan_sha256",sha((*planBytes)->getBuffer())},{"output_ir_sha256",sha(output)},{"fault",fault},{"declared_sites",sites.size()},{"emitted_sites",std::move(emitted)},{"omitted_sites",std::move(omitted)},{"counter_symbol","dragon_observer_counts"},{"count_symbol","dragon_observer_count"},{"counter_width",64},{"counter_arithmetic","unsigned modular"},{"guest_loads_added",0},{"cfg_changed",false},{"optimized_after_instrumentation",false}};
  llvm::raw_fd_ostream reportFile(argv[4],error);if(error)return fail("cannot create report");reportFile<<llvm::formatv("{0:2}",llvm::json::Value(std::move(report)))<<'\n';reportFile.close();if(reportFile.has_error())return fail("cannot write report");return 0;
}
