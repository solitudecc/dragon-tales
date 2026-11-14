// Opt-in real-input adapter: discover candidates; the public core proves them.
// No Guest execution, known-memory reader, noalias/assume, or source-state oracle.
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/lifters/llvm/ImageTableContext.hpp>
#include <dragon/loader/Loader.hpp>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

using namespace llvm;
using namespace dragon::llvmlifter;
namespace {
void require(bool v, const Twine &why) { if (!v) throw std::runtime_error(why.str()); }
std::string sha(StringRef bytes) {
  auto h = SHA256::hash(ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
  constexpr char hex[] = "0123456789abcdef"; std::string out;
  for (auto b : h) { out += hex[b >> 4]; out += hex[b & 15]; } return out;
}
std::string printed(const Value &v) { std::string s; raw_string_ostream out(s); v.print(out); return s; }
std::string moduleIR(Module &m) { std::string s; raw_string_ostream out(s); m.print(out, nullptr); return s; }
std::string stringField(const json::Object &o, StringRef name) {
  auto v = o.getString(name); require(v.has_value(), "missing string: " + name); return v->str();
}
std::uint64_t number(StringRef text) {
  std::uint64_t out = 0;
  require(!text.empty() && !text.starts_with("-") && !text.starts_with("+") &&
          !text.getAsInteger(0, out), "invalid unsigned input integer"); return out;
}
std::uint64_t numberField(const json::Object &o, StringRef name) { return number(stringField(o, name)); }
const json::Object &objectField(const json::Object &o, StringRef name) {
  auto *v = o.getObject(name); require(v, "missing object: " + name); return *v;
}
std::string readPinned(const json::Object &o) {
  auto path = stringField(o, "path"); require(std::filesystem::path(path).is_absolute(), "absolute pin required");
  auto bytes = MemoryBuffer::getFile(path); require(bool(bytes), "missing input: " + path);
  require(sha((*bytes)->getBuffer()) == stringField(o, "sha256"), "changed input: " + path);
  return (*bytes)->getBuffer().str();
}
void save(const std::filesystem::path &path, StringRef bytes) {
  require(!std::filesystem::exists(path), "refuse existing output: " + path.string());
  std::error_code error; raw_fd_ostream out(path.string(), error); require(!error, "cannot open output");
  out << bytes; out.close(); require(!out.has_error(), "cannot write output");
}
void saveJSON(const std::filesystem::path &p, json::Value v) { save(p, formatv("{0:2}\n", v).str()); }
void verify(Module &m) {
  std::string s; raw_string_ostream out(s); require(!verifyModule(m, &out), "invalid LLVM: " + out.str());
}
std::vector<std::uint8_t> little(std::uint64_t n) {
  std::vector<std::uint8_t> b(8); for (unsigned i = 0; i != 8; ++i) b[i] = n >> (8 * i); return b;
}
std::uint64_t read64(StringRef frame, std::size_t off) {
  require(off <= frame.size() && 8 <= frame.size() - off, "short initial frame");
  std::uint64_t n = 0; for (unsigned i = 0; i != 8; ++i) n |= std::uint64_t(std::uint8_t(frame[off+i])) << (8*i); return n;
}
std::string hexadecimal(std::uint64_t n) { return formatv("0x{0:x-}", n).str(); }
Function *entryFunction(Module &m, std::uint64_t entry) {
  Function *found = nullptr;
  for (auto &f : m) if (!f.isDeclaration() && f.hasFnAttribute("dragon.entrypoints") &&
      f.hasFnAttribute("dragon.static-entry-policy") &&
      f.getFnAttribute("dragon.static-entry-policy").getValueAsString() == "function-start") {
    auto text = f.getFnAttribute("dragon.entrypoints").getValueAsString();
    std::uint64_t value = 0;
    if (text.getAsInteger(16, value) || value != entry) continue;
    require(!found, "ambiguous FunctionStart root"); found = &f;
  }
  require(found, "FunctionStart root absent"); return found;
}
// Constant byte GEP provenance only; no alias or no-wrap assertion is added.
std::optional<std::uint64_t> offset(Value *p, Value *base, unsigned depth = 0) {
  if (p == base) return 0;
  auto *g = dyn_cast<GetElementPtrInst>(p);
  if (depth == 8 || !g || !g->getSourceElementType()->isIntegerTy(8) || g->getNumIndices() != 1) return {};
  auto *index = dyn_cast<ConstantInt>(g->idx_begin()->get());
  if (!index || index->getBitWidth() > 64) return {};
  auto parent = offset(g->getPointerOperand(), base, depth + 1);
  return parent ? std::optional(*parent + index->getValue().sextOrTrunc(64).getZExtValue()) : std::nullopt;
}
Value *registerBase(Function &f) {
  Value *base = nullptr;
  for (auto &i : f.getEntryBlock()) if (auto *load = dyn_cast<LoadInst>(&i)) {
    auto at = offset(load->getPointerOperand(), f.getArg(0));
    if (load->isSimple() && load->getType()->isPointerTy() && at && *at == offsetof(GuestStateV2, regs_bytes)) {
      require(!base, "ambiguous architectural register base"); base = load;
    }
  }
  require(base, "missing architectural register base"); return base;
}
CallBase *kernelCall(Function &caller, Function &root, std::uint64_t entry) {
  CallBase *found = nullptr;
  for (auto &b : caller) for (auto &i : b) if (auto *call = dyn_cast<CallBase>(&i)) {
    if (call->getCalledFunction() != &root) continue;
    auto *tag = call->getMetadata("dragon.guest.call.target");
    auto *md = tag && tag->getNumOperands() == 1 ? dyn_cast<ConstantAsMetadata>(tag->getOperand(0)) : nullptr;
    auto *pc = md ? dyn_cast<ConstantInt>(md->getValue()) : nullptr;
    require(pc && pc->getValue().getActiveBits() <= 64 && pc->getZExtValue() == entry,
            "actual caller target metadata differs");
    require(!found, "ambiguous actual kernel call"); found = call;
  }
  require(found, "actual kernel call missing"); return found;
}
// These observations only propose a guard value. They are never facts to the
// Image proof. Ambiguous control/history or an unevaluable scalar refuses.
struct CallerCandidate {
  Function &caller; Value *regs; StringRef frame;
  std::set<Value *> gray; std::size_t nodes = 0;
  StoreInst *previousStore(Instruction &point, const std::function<bool(StoreInst &)> &matches) {
    BasicBlock *b = point.getParent(); Instruction *end = &point; std::set<BasicBlock *> visited;
    for (unsigned depth = 0; depth != 32; ++depth) {
      require(visited.insert(b).second, "caller candidate crosses a CFG cycle");
      auto it = end->getIterator();
      while (it != b->begin()) {
        auto &i = *--it;
        if (auto *store = dyn_cast<StoreInst>(&i); store && matches(*store)) return store;
      }
      if (b == &caller.getEntryBlock()) return nullptr;
      b = b->getSinglePredecessor(); require(b, "caller candidate has ambiguous predecessor history");
      end = b->getTerminator();
    }
    throw std::runtime_error("caller candidate predecessor cap");
  }
  APInt evaluate(Value *v, unsigned depth = 0) {
    require(depth <= 32 && ++nodes <= 128 && !isa<UndefValue, PoisonValue>(v), "caller scalar candidate cap/undefined input");
    require(v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() <= 64, "unsupported caller candidate width");
    if (auto *c = dyn_cast<ConstantInt>(v)) return c->getValue();
    require(gray.insert(v).second, "caller scalar candidate history cycle");
    struct Pop { std::set<Value *> &gray; Value *v; ~Pop() { gray.erase(v); } } pop{gray,v};
    if (auto *load = dyn_cast<LoadInst>(v)) {
      require(load->isSimple(), "caller candidate read is volatile/atomic");
      if (auto at = offset(load->getPointerOperand(), regs))
        return APInt(load->getType()->getIntegerBitWidth(), read64(frame, *at));
      auto *slot = dyn_cast<AllocaInst>(load->getPointerOperand());
      require(slot && slot->getAllocatedType() == load->getType(), "caller candidate lacks a private scalar slot");
      auto *store = previousStore(*load, [&](StoreInst &s) { return s.getPointerOperand() == slot; });
      require(store && store->isSimple(), "caller candidate private slot has no exact reaching STORE");
      return evaluate(store->getValueOperand(), depth + 1);
    }
    auto *b = dyn_cast<BinaryOperator>(v);
    require(b && (b->getOpcode() == Instruction::Add || b->getOpcode() == Instruction::Sub),
            "caller candidate operation is unsupported");
    require(!b->hasNoUnsignedWrap() && !b->hasNoSignedWrap(),
            "caller candidate is not unflagged scalar add/sub");
    auto left = evaluate(b->getOperand(0), depth+1), right = evaluate(b->getOperand(1), depth+1);
    if (b->getOpcode() == Instruction::Add) return left + right;
    if (b->getOpcode() == Instruction::Sub) return left - right;
    throw std::runtime_error("caller candidate operation is unsupported");
  }
  std::pair<std::uint64_t, std::string> value(CallBase &call, std::uint64_t at, bool requiredStore) {
    nodes = 0; gray.clear();
    auto *store = previousStore(call, [&](StoreInst &s) { return offset(s.getPointerOperand(), regs) == at; });
    if (!store) {
      require(!requiredStore, "caller lacks actual SP flush before CALL");
      return {read64(frame, at), "input-register candidate only; no explicit caller flush; not a proof of unchanged memory"};
    }
    require(store->isSimple(), "caller register flush is volatile/atomic");
    return {evaluate(store->getValueOperand()).getZExtValue(), printed(*store)};
  }
};
void addressTerms(Value *v, std::uint64_t &constant, std::vector<Value *> &terms, unsigned depth=0) {
  require(depth <= 12 && terms.size() <= 4, "table candidate expression cap");
  if (auto *c = dyn_cast<ConstantInt>(v); c && c->getBitWidth() <= 64) { constant += c->getZExtValue(); return; }
  if (auto *b = dyn_cast<BinaryOperator>(v); b && b->getOpcode() == Instruction::Add) {
    addressTerms(b->getOperand(0),constant,terms,depth+1); addressTerms(b->getOperand(1),constant,terms,depth+1); return;
  }
  terms.push_back(v);
}
json::Object discoverTable(Function &f, std::uint64_t base, const std::vector<std::uint64_t> &targets) {
  Value *guest = nullptr;
  for (auto &i : f.getEntryBlock()) if (auto *l = dyn_cast<LoadInst>(&i))
    if (l->isSimple() && l->getType()->isPointerTy() && l->getPointerOperand() == f.getArg(1)) {
      require(!guest, "ambiguous Guest base"); guest = l;
    }
  require(guest, "Guest base missing"); json::Object found; unsigned count=0;
  for (auto &block : f) {
    auto *sw = dyn_cast<SwitchInst>(block.getTerminator());
    auto *load = sw ? dyn_cast<LoadInst>(sw->getCondition()) : nullptr;
    auto *gep = load ? dyn_cast<GetElementPtrInst>(load->getPointerOperand()) : nullptr;
    if (!load || !load->isSimple() || !load->getType()->isIntegerTy(64) || !gep ||
        gep->getPointerOperand() != guest || gep->getNumIndices()!=1 || !gep->getSourceElementType()->isIntegerTy(8)) continue;
    std::uint64_t constant=0; std::vector<Value *> terms; addressTerms(gep->idx_begin()->get(),constant,terms);
    if (constant != base || terms.size()!=1) continue;
    auto *scale=dyn_cast<BinaryOperator>(terms.front());
    auto *amount=scale && scale->getOpcode()==Instruction::Shl ? dyn_cast<ConstantInt>(scale->getOperand(1)) : nullptr;
    if (!amount || !amount->equalsInt(3)) continue;
    std::set<std::uint64_t> actual; for (auto c : sw->cases()) actual.insert(c.getCaseValue()->getZExtValue());
    for (auto target : targets) require(actual.contains(target), "candidate target missing from actual switch");
    ++count; found=json::Object{{"block",block.getName().str()},{"target_load",printed(*load)},
      {"target_switch",printed(*sw)},{"actual_index_ssa",printed(*scale->getOperand(0))},
      {"actual_index_width",scale->getOperand(0)->getType()->getIntegerBitWidth()},
      {"candidate_only_not_a_memory_fact",true}};
  }
  require(count==1,"missing/ambiguous structured target table LOAD"); return found;
}
struct Identity {
  Module *module; std::map<std::string,Function *> functions;
  std::map<std::string,std::vector<Argument *>> arguments;
  std::map<std::string,std::string> ordinary;
  Identity(Module &m, StringRef view):module(&m) {
    for (auto &f : m) { auto key=f.getName().str(); functions[key]=&f;
      for (auto &a:f.args()) arguments[key].push_back(&a);
      if (f.getName()!=view) ordinary[key]=printed(f);
    }
  }
  void check(Module &m, StringRef view) {
    require(&m==module && m.size()==functions.size(),"module/Function set changed");
    for (auto &[name,old]:functions) { auto *f=m.getFunction(name); require(f==old,"Function identity changed");
      require(f->arg_size()==arguments[name].size(),"argument count changed");
      for (unsigned i=0;i<f->arg_size();++i) require(f->getArg(i)==arguments[name][i],"Argument identity changed");
      if (f->getName()!=view) require(printed(*f)==ordinary[name],"ordinary root/caller/body changed");
    }
  }
};
json::Object structure(Function &f) {
  std::size_t switches=0, cyclic=0; json::Array groups;
  for(auto &b:f) switches+=isa<SwitchInst>(b.getTerminator());
  for(auto it=scc_begin(&f);!it.isAtEnd();++it) {
    auto &s=*it; bool loop=s.size()>1;
    if(!loop) for(auto *next:successors(s.front())) loop|=next==s.front();
    if(!loop)continue;json::Array names;
    for(auto *b:s){names.push_back(b->getName().str());cyclic+=isa<SwitchInst>(b->getTerminator());}
    groups.push_back(std::move(names));
  }
  return json::Object{{"blocks",f.size()},{"switches",switches},{"cyclic_switches",cyclic},{"SCCs",std::move(groups)},
    {"precision_complete",false},{"semantic_validation",false}};
}
unsigned ordinaryFallbacks(Function &view, Function *ordinary) {
  unsigned count = 0;
  for (auto &b : view) for (auto &i : b) if (auto *call = dyn_cast<CallBase>(&i))
    if (call->getCalledFunction() == ordinary) {
      ++count;
      require(b.size() == 2 && isa<ReturnInst>(b.getTerminator()), "noncanonical ordinary fallback");
    }
  return count;
}
}
int main(int argc,char **argv) {
  json::Array stages; json::Object report{{"schema","dragon-tigress-dispatch-context-probe-v1"},
    {"ok",false},{"image_calls",0},{"decoder_calls",0},{"corridor_calls",0},{"native_C_UC_runs",0},
    {"known_memory_assertions",0},{"budget_changed",false}};
  std::filesystem::path output;
  try {
    require(argc==3,"usage: probe PLAN.json NEW-OUT"); output=argv[2];
    require(output.is_absolute() && !std::filesystem::exists(output),"new absolute output required");
    std::filesystem::create_directories(output);
    auto planBytes=MemoryBuffer::getFile(argv[1]);require(bool(planBytes),"plan missing");
    auto parsed=json::parse((*planBytes)->getBuffer());require(bool(parsed),"invalid plan JSON");
    auto *plan=parsed->getAsObject();require(plan,"plan must be object");
    require(stringField(*plan,"schema")=="dragon-tigress-dispatch-context-plan-v1","wrong plan schema");
    const auto raw=readPinned(objectField(*plan,"raw")), elf=readPinned(objectField(*plan,"elf"));
    const auto frame=readPinned(objectField(*plan,"initial_frame")); require(frame.size()==512+65536,"initial frame ABI differs");
    const auto entry=numberField(*plan,"entry"), callerEntry=numberField(*plan,"caller_entry");
    const auto tableBase=numberField(*plan,"table_offset"), count=numberField(*plan,"table_slots");
    require(count && count<=512 && count*8<=UINT64_MAX-tableBase,"candidate table domain invalid");
    std::string loadError; auto image=dragon::loader::LoadedImage::loadBytes(std::vector<std::uint8_t>(elf.begin(),elf.end()),&loadError);
    require(bool(image),"cannot load ELF: "+loadError); auto table=image->readVirtual(tableBase,count*8);
    require(table && table->size()==count*8,"candidate table has no full ELF backing");
    std::vector<std::uint64_t> targets; for(std::size_t i=0;i<count;++i) targets.push_back(read64(StringRef(reinterpret_cast<const char *>(table->data()),table->size()),i*8));
    LLVMLifter source(dragon::Architecture::X86_64,"tigress-dispatch-context-source");
    require(source.setExecutionMode(ExecutionMode::StaticAnalysis) && source.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart),"mode rejected");
    SMDiagnostic error;
    if(parseAssemblyInto(MemoryBufferRef(raw,stringField(objectField(*plan,"raw"),"path")),&source.module(),nullptr,error)) {
      error.print(argv[0],errs());throw std::runtime_error("raw LLVM parse failed");
    }
    verify(source.module());auto sourceBefore=source.ir();save(output/"source-parsed.ll",sourceBefore);
    auto *root=entryFunction(source.module(),entry), *caller=entryFunction(source.module(),callerEntry);
    auto *call=kernelCall(*caller,*root,entry);CallerCandidate candidates{*caller,registerBase(*caller),frame};
    auto sp=candidates.value(*call,dragon::x86::kOffRSP,true);
    auto rsi=candidates.value(*call,dragon::x86::kOffRSI,false);
    report["raw_sha256"]=sha(raw);report["elf_sha256"]=sha(elf);report["initial_frame_sha256"]=sha(frame);
    report["source_ModuleID_literal"]=sourceBefore.substr(0,sourceBefore.find('\n'));
    report["SP_candidate"]=hexadecimal(sp.first);report["SP_candidate_origin"]=sp.second;
    report["RSI_candidate"]=hexadecimal(rsi.first);report["RSI_candidate_origin"]=rsi.second;
    report["candidates_are_not_certificates"]=true;report["raw_discovery"]=discoverTable(*root,tableBase,targets);
    Identity sourceIdentity(source.module(),""); unsigned calls=0;
    for(bool extraRSI:{false,true}) {
      const std::string label=extraRSI?"B-SP-table-RSI":"A-SP-table";
      GuardedEntryContext context;context.entry=entry;context.contextId=extraRSI?"tigress_sp_table_rsi_v1":"tigress_sp_table_v1";
      context.provenance="pinned raw/ELF/input candidates; runtime equality and joint proof required; no input-derived fact";
      context.registers.push_back({dragon::x86::kOffRSP,little(sp.first)});
      if(extraRSI)context.registers.push_back({dragon::x86::kOffRSI,little(rsi.first)});
      context.memory.push_back({tableBase,*table});std::string why;
      auto view=source.createGuardedEntryView(context,why);require(bool(view),"guarded view refused: "+why);
      auto manifest=json::parse(view->guardedEntryManifest());require(bool(manifest),"view manifest invalid");
      auto *object=manifest->getAsObject();require(object,"view manifest not object");auto name=stringField(*object,"view_symbol");
      auto *f=view->module().getFunction(name);require(f,"guarded view symbol absent");verify(view->module());
      auto *ordinary=view->module().getFunction(root->getName());
      require(ordinary && ordinaryFallbacks(*f,ordinary)==1,"missing/ambiguous ordinary guard-miss fallback");
      const auto before=view->ir();save(output/(label+".before.ll"),before);
      save(output/(label+".manifest.json"),view->guardedEntryManifest());Identity identity(view->module(),name);
      auto beforeShape=structure(*f);
      dragon::llvmlifter::detail::ImageTableContextRequest request{name,tableBase,targets};
      ++calls;report["image_calls"]=calls;
      const auto stats=dragon::llvmlifter::detail::runClosedImageTableContext(view->module(),request);
      const auto after=view->ir();save(output/(label+".after.ll"),after);
      verify(view->module());identity.check(view->module(),name);sourceIdentity.check(source.module(),"");
      require(source.ir()==sourceBefore,"source module changed");
      if(!stats.changed)require(after==before,"unchanged/refused Image mutated module");
      require(!stats.changed || stats.closed,"Image changed without full closure");
      f=view->module().getFunction(name);
      require(ordinaryFallbacks(*f,ordinary)==1,"Image changed ordinary guard-miss fallback");
      stages.push_back(json::Object{{"case",label},{"guarded_RSI",extraRSI},{"verified",true},
        {"ordinary_bodies_and_identity_unchanged",true},{"guard_miss_calls_ordinary_root",true},
        {"before_sha256",sha(before)},{"after_sha256",sha(after)},
        {"closed",stats.closed},{"changed",stats.changed},{"budget_exhausted",stats.budgetExhausted},
        {"guarded_bytes",stats.guardedBytes},{"preserved_stores",stats.preservedStores},
        {"constant_values",stats.constantValues},{"work",stats.work},{"message",stats.message},
        {"before_structure",std::move(beforeShape)},{"after_structure",structure(*f)},
        {"downstream_status","not invoked in before-only Image admission probe"}});
    }
    require(readPinned(objectField(*plan,"raw"))==raw && readPinned(objectField(*plan,"elf"))==elf &&
            readPinned(objectField(*plan,"initial_frame"))==frame,"input bytes changed");
    report["ok"]=true;report["source_ordinary_exact"]=true;
    report["precision_complete"]=false;report["semantic_validation"]=false;
    report["stages"]=std::move(stages);saveJSON(output/"REPORT.json",std::move(report));return 0;
  } catch(const std::exception &e) {
    report["failure"]=e.what();report["stages"]=std::move(stages);
    if(!output.empty() && std::filesystem::exists(output) && !std::filesystem::exists(output/"REPORT.json"))
      saveJSON(output/"REPORT.json",std::move(report));
    errs()<<e.what()<<"\n";return 1;
  }
}
