#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <algorithm>
#include <format>
#include <limits>
#include <unordered_map>

namespace dragon::llvmlifter {
namespace {
constexpr const char *ManifestName = "dragon.guarded-entry";
struct RegisterRead { llvm::LoadInst *load; std::uint64_t offset, bytes; };
std::string hexBytes(llvm::ArrayRef<std::uint8_t> bytes) {
  static constexpr char digits[]="0123456789abcdef";
  std::string result;result.reserve(bytes.size()*2);
  for(auto byte:bytes){result+=digits[byte>>4];result+=digits[byte&15];}return result;
}
std::optional<std::uint64_t> byteOffset(llvm::Value *pointer,llvm::Value *base) {
  if(pointer==base)return 0;
  auto *gep=llvm::dyn_cast<llvm::GetElementPtrInst>(pointer);
  if(!gep || gep->getPointerOperand()!=base || !gep->getSourceElementType()->isIntegerTy(8) ||
     gep->getNumIndices()!=1)return std::nullopt;
  auto *offset=llvm::dyn_cast<llvm::ConstantInt>(gep->idx_begin()->get());
  if(!offset || offset->getValue().getActiveBits()>64)return std::nullopt;
  return offset->getZExtValue();
}
unsigned chunkSize(std::uint64_t available) {
  return available>=8?8:available>=4?4:available>=2?2:1;
}
}

std::string LLVMLifter::guardedEntryManifest() const {
  const auto *metadata=module_->getNamedMetadata(ManifestName);
  if(!metadata || metadata->getNumOperands()!=1)return "{}";
  const auto *node=metadata->getOperand(0);
  if(node->getNumOperands()!=1)return "{}";
  const auto *value=llvm::dyn_cast<llvm::MDString>(node->getOperand(0));
  return value?value->getString().str():"{}";
}

std::unique_ptr<LLVMLifter> LLVMLifter::createGuardedEntryView(
    const GuardedEntryContext &request,std::string &error) const {
  error.clear();
  auto fail=[&](std::string message)->std::unique_ptr<LLVMLifter>{error=std::move(message);return nullptr;};
  if(mode_!=ExecutionMode::StaticAnalysis || staticEntryPolicy_!=StaticEntryPolicy::FunctionStart)
    return fail("guarded entry requires StaticAnalysis and FunctionStart");
  if(module_->getNamedMetadata(ManifestName))return fail("nested guarded views are not supported");
  if(request.contextId.empty() || request.contextId.size()>64 ||
     !std::all_of(request.contextId.begin(),request.contextId.end(),[](unsigned char c){
       return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';}))
    return fail("contextId must contain 1 to 64 ASCII letters digits underscore dot or hyphen");
  if(request.provenance.empty() || request.provenance.size()>4096 ||
     request.provenance.find('\0')!=std::string::npos || !llvm::json::isUTF8(request.provenance))
    return fail("provenance must be nonempty valid UTF-8 without NUL and at most 4096 bytes");
  if(request.registers.empty()&&request.memory.empty())return fail("entry facts are empty");
  if(request.registers.size()>256 || request.memory.size()>256-request.registers.size())
    return fail("entry facts exceed 256 ranges");
  const std::uint64_t registerExtent=arch_==Architecture::ARM64?788:
      (arch_==Architecture::X86_32||arch_==Architecture::X86_64)?488:0;
  if(!registerExtent)return fail("unsupported guarded-entry architecture");
  auto registers=request.registers,memory=request.memory;
  std::uint64_t minimumRegisters=0,minimumMemory=0;
  std::size_t totalBytes=0;
  auto ranges=[&](std::vector<GuardedEntryBytes> &rows,std::uint64_t limit,std::uint64_t &minimum){
    std::sort(rows.begin(),rows.end(),[](const auto &a,const auto &b){return a.offset<b.offset;});
    std::uint64_t previousEnd=0;bool first=true;
    for(const auto &row:rows){
      if(row.bytes.empty() || row.bytes.size()>4096-totalBytes || row.offset>limit ||
         row.bytes.size()>limit-row.offset || (!first&&row.offset<previousEnd))return false;
      totalBytes+=row.bytes.size();previousEnd=row.offset+row.bytes.size();first=false;
    }
    minimum=previousEnd;return true;
  };
  if(!ranges(registers,registerExtent,minimumRegisters) ||
     !ranges(memory,std::numeric_limits<std::uint64_t>::max(),minimumMemory))
    return fail("empty overlapping overflowing or oversized entry byte range");
  const auto &layout=module_->getDataLayout();
  if(!layout.isLittleEndian() || layout.getPointerSizeInBits(0)!=64 || layout.getIndexSizeInBits(0)!=64)
    return fail("guarded entry requires a little-endian 64-bit pointer/index layout");
  if(module_->size()>4096 || module_->global_size()>4096)
    return fail("source module exceeds guarded-view object limits");
  std::size_t instructionCount=0;
  for(const auto &function:*module_)for(const auto &block:function){
    if(block.size()>1000000-instructionCount)return fail("source module exceeds 1000000 instructions");
    instructionCount+=block.size();
  }
  std::string verification;llvm::raw_string_ostream verifyOutput(verification);
  if(llvm::verifyModule(*module_,&verifyOutput))return fail("invalid source module: "+verification);
  auto *source=module_->getFunction(staticFunctionName(request.entry));
  if(!source || source->isDeclaration() || source->arg_size()!=4 || source->isVarArg() ||
     !source->getReturnType()->isVoidTy() ||
     !source->hasFnAttribute("dragon.static-entry-policy") || !source->hasFnAttribute("dragon.entrypoints") ||
     source->getFnAttribute("dragon.static-entry-policy").getValueAsString()!="function-start" ||
     source->getFnAttribute("dragon.entrypoints").getValueAsString()!=std::format("{:x}",request.entry))
    return fail("requested root is not a canonical FunctionStart definition");
  for(auto &argument:source->args())if(!argument.getType()->isPointerTy() || argument.getType()->getPointerAddressSpace()!=0)
    return fail("root arguments do not use the execution descriptor ABI");
  // LLVM's convergence and noduplicate contracts constrain cloning/control
  // dependence. Do not erase those restrictions to make a source admissible.
  auto restricted=[](const llvm::Function &function){
    return function.hasFnAttribute(llvm::Attribute::Convergent) ||
           function.hasFnAttribute(llvm::Attribute::NoDuplicate);
  };
  if(restricted(*source))return fail("source function restricts duplication or convergence");
  for(auto &block:*source)for(auto &instruction:block)
    if(auto *call=llvm::dyn_cast<llvm::CallBase>(&instruction))
      if(call->hasFnAttr(llvm::Attribute::Convergent) || call->hasFnAttr(llvm::Attribute::NoDuplicate) ||
         (call->getCalledFunction()&&restricted(*call->getCalledFunction())))
        return fail("source call restricts duplication or convergence");
  const auto name=source->getName().str()+".guarded."+request.contextId;
  if(module_->getNamedValue(name))return fail("guarded entry symbol already exists");
  auto &entry=source->getEntryBlock();auto *entrySwitch=llvm::dyn_cast<llvm::SwitchInst>(entry.getTerminator());
  if(!entrySwitch || !entrySwitch->getCondition()->getType()->isIntegerTy(64) || entrySwitch->getNumCases()!=1 ||
     entrySwitch->case_begin()->getCaseValue()->getValue()!=llvm::APInt(64,request.entry))
    return fail("root requires its unchanged single-case external-entry switch");
  // Recognize the raw producer's descriptor loads and register initialization.
  // No names or caller-supplied metadata certify an arbitrary insertion point.
  llvm::LoadInst *regsBase=nullptr,*guestBase=nullptr;
  for(auto &instruction:entry){
    if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&instruction)){
      auto *slot=llvm::dyn_cast<llvm::AllocaInst>(store->getPointerOperand());
      if(!store->isSimple() || !slot || slot->getParent()!=&entry)
        return fail("source entry has noncanonical side effects");
    }else if(instruction.mayHaveSideEffects() && !instruction.isTerminator())
      return fail("source entry has noncanonical side effects");
    auto *load=llvm::dyn_cast<llvm::LoadInst>(&instruction);
    if(!load || !load->isSimple() || !load->getType()->isPointerTy())continue;
    if(auto offset=byteOffset(load->getPointerOperand(),source->getArg(0));offset&&*offset==offsetof(GuestStateV2,regs_bytes)){
      if(regsBase)return fail("ambiguous register descriptor load");regsBase=load;
    }
    if(load->getPointerOperand()==source->getArg(1)){
      if(guestBase)return fail("ambiguous Guest descriptor load");guestBase=load;
    }
  }
  if(!regsBase || !guestBase)return fail("missing canonical descriptor base loads");
  // Fallback reexecutes the register prologue in a fresh host frame. Reject
  // any source that could observe or retain an original host alloca address.
  for(auto &instruction:entry)if(auto *slot=llvm::dyn_cast<llvm::AllocaInst>(&instruction)){
    if(!slot->getAllocatedType()->isIntegerTy() || slot->isArrayAllocation())
      return fail("entry local is not a scalar register snapshot");
    for(auto *user:slot->users()){
      if(auto *load=llvm::dyn_cast<llvm::LoadInst>(user)){
        if(load->isSimple() && load->getPointerOperand()==slot && load->getFunction()==source)continue;
      }else if(auto *store=llvm::dyn_cast<llvm::StoreInst>(user)){
        if(store->isSimple() && store->getPointerOperand()==slot && store->getValueOperand()!=slot && store->getFunction()==source)continue;
      }
      return fail("entry register snapshot address escapes or has unsupported uses");
    }
  }
  std::vector<RegisterRead> reads;
  for(auto &instruction:entry)if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&instruction)){
    if(!load->isSimple() || !load->getType()->isIntegerTy())continue;
    const auto width=load->getType()->getIntegerBitWidth();
    auto offset=byteOffset(load->getPointerOperand(),regsBase);
    if(!offset || !width || width%8 || width>128 || *offset>registerExtent || width/8>registerExtent-*offset)continue;
    reads.push_back({load,*offset,width/8});
  }
  for(auto &instruction:entry)if(auto *load=llvm::dyn_cast<llvm::LoadInst>(&instruction)){
    if(load==regsBase || load==guestBase)continue;
    if(load->isSimple()){
      if(auto *slot=llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand());slot&&slot->getParent()==&entry)continue;
      if(std::any_of(reads.begin(),reads.end(),[&](const auto &read){return read.load==load;}))continue;
    }
    return fail("entry contains an extra noncanonical memory read");
  }
  const std::uint64_t pcOffset=arch_==Architecture::ARM64?264:128;
  auto *pc=llvm::dyn_cast<llvm::LoadInst>(entrySwitch->getCondition());
  auto *pcSlot=pc?llvm::dyn_cast<llvm::AllocaInst>(pc->getPointerOperand()):nullptr;
  if(!pc || !pc->isSimple() || !pc->getType()->isIntegerTy(64) || pc->getParent()!=&entry || !pcSlot || pcSlot->getParent()!=&entry)
    return fail("source entry PC no longer has the raw register snapshot provenance");
  llvm::StoreInst *pcInit=nullptr;
  for(auto &instruction:entry)if(auto *store=llvm::dyn_cast<llvm::StoreInst>(&instruction))
    if(store->getPointerOperand()==pcSlot){if(pcInit)return fail("ambiguous entry PC initialization");pcInit=store;}
  if(!pcInit || !pcInit->comesBefore(pc) ||
     std::none_of(reads.begin(),reads.end(),[&](const auto &read){return read.offset==pcOffset&&read.bytes==8&&read.load==pcInit->getValueOperand();}))
    return fail("entry switch is not tied to the initial architectural PC");
  for(const auto &row:registers)for(std::size_t byte=0;byte<row.bytes.size();++byte)
    if(std::none_of(reads.begin(),reads.end(),[&](const auto &read){const auto at=row.offset+byte;return at>=read.offset&&at-read.offset<read.bytes;}))
      return fail("register fact is not covered by the source entry's existing reads");

  llvm::SmallVector<char,0> bytes;llvm::raw_svector_ostream bitcode(bytes);
  llvm::WriteBitcodeToFile(*module_,bitcode,true);
  if(bytes.size()>64u*1024u*1024u)return fail("source bitcode exceeds 64 MiB clone limit");
  const auto sourceHash=llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
  auto view=std::make_unique<LLVMLifter>(arch_,module_->getName().str());
  auto parsed=llvm::parseBitcodeFile(llvm::MemoryBufferRef(llvm::StringRef(bytes.data(),bytes.size()),"guarded-source"),*view->ctx_);
  if(!parsed)return fail("cannot clone source bitcode: "+llvm::toString(parsed.takeError()));
  view->module_=std::move(*parsed);view->mode_=mode_;view->staticEntryPolicy_=staticEntryPolicy_;
  auto *ordinary=view->module_->getFunction(source->getName());
  if(!ordinary || ordinary->isDeclaration())return fail("source root missing after bitcode roundtrip");
  // Bitcode preserves instruction order. Validate every needed load's shape
  // and descriptor/byte offset in the new context before using that identity.
  std::unordered_map<const llvm::Instruction*,llvm::LoadInst*> parsedLoads;
  auto parsedLoad=[&](const llvm::Instruction *value)->llvm::LoadInst*{
    auto sourceIt=entry.begin();auto parsedIt=ordinary->getEntryBlock().begin();
    for(;sourceIt!=entry.end()&&parsedIt!=ordinary->getEntryBlock().end();++sourceIt,++parsedIt)
      if(&*sourceIt==value){auto *load=llvm::dyn_cast<llvm::LoadInst>(&*parsedIt);
        if(load&&load->isSimple()){parsedLoads[value]=load;return load;}return nullptr;}
    return nullptr;
  };
  auto *parsedRegs=parsedLoad(regsBase),*parsedGuest=parsedLoad(guestBase);
  if(!parsedRegs || !parsedGuest || !parsedRegs->getType()->isPointerTy() || !parsedGuest->getType()->isPointerTy() ||
     byteOffset(parsedRegs->getPointerOperand(),ordinary->getArg(0))!=offsetof(GuestStateV2,regs_bytes) ||
     parsedGuest->getPointerOperand()!=ordinary->getArg(1))
    return fail("descriptor provenance changed during bitcode roundtrip");
  for(const auto &read:reads){auto *load=parsedLoad(read.load);
    if(!load || !load->getType()->isIntegerTy(read.bytes*8) ||
       byteOffset(load->getPointerOperand(),parsedRegs)!=read.offset)
      return fail("register-load provenance changed during bitcode roundtrip");
  }
  auto *guarded=llvm::Function::Create(ordinary->getFunctionType(),llvm::GlobalValue::ExternalLinkage,name,*view->module_);
  guarded->copyAttributesFrom(ordinary);guarded->setCallingConv(ordinary->getCallingConv());
  llvm::ValueToValueMapTy map;
  for(unsigned n=0;n<4;++n){map[ordinary->getArg(n)]=guarded->getArg(n);guarded->getArg(n)->setName(ordinary->getArg(n)->getName());}
  llvm::SmallVector<llvm::ReturnInst*,8> returns;
  // Intentionally do not map ordinary itself: recursion/internal calls retain
  // the unrestricted symbol and never repeat this external-entry context.
  llvm::CloneFunctionInto(guarded,ordinary,map,llvm::CloneFunctionChangeType::LocalChangesOnly,returns);
  // Added memory reads and a fallback call are not covered by inherited
  // optimization promises. Preserve ABI/parameter/target attributes, but let
  // subsequent analyses rederive these function-level facts for the view.
  for(auto attribute:{llvm::Attribute::Memory,llvm::Attribute::Speculatable,
      llvm::Attribute::NoRecurse,llvm::Attribute::NoSync,llvm::Attribute::WillReturn,
      llvm::Attribute::NoFree,llvm::Attribute::NoCallback,llvm::Attribute::NoUnwind,
      llvm::Attribute::MustProgress})guarded->removeFnAttr(attribute);
  auto &copyEntry=guarded->getEntryBlock();auto *copySwitch=llvm::cast<llvm::SwitchInst>(copyEntry.getTerminator());
  auto *body=copySwitch->case_begin()->getCaseSuccessor();
  auto *bounds=llvm::BasicBlock::Create(*view->ctx_,"guarded.bounds",guarded);
  auto *fallback=llvm::BasicBlock::Create(*view->ctx_,"guarded.fallback",guarded);
  auto *accepted=llvm::BasicBlock::Create(*view->ctx_,"guarded.accept",guarded);
  copySwitch->setSuccessor(1,bounds);
  for(auto &phi:body->phis()){const auto index=phi.getBasicBlockIndex(&copyEntry);if(index>=0)phi.setIncomingBlock(index,accepted);}
  llvm::IRBuilder<> out(fallback);
  auto *fallbackCall=out.CreateCall(ordinary,{guarded->getArg(0),guarded->getArg(1),guarded->getArg(2),guarded->getArg(3)});
  fallbackCall->setCallingConv(ordinary->getCallingConv());out.CreateRetVoid();
  llvm::IRBuilder<>(accepted).CreateBr(body);
  // Cross-context SSA identities are recovered by ordinal, then cloned through
  // the explicit map. No source-context Value pointer survives in the view.
  auto mappedEntryValue=[&](const llvm::Instruction *value)->llvm::Value*{
    const auto found=parsedLoads.find(value);
    return found==parsedLoads.end()?nullptr:map.lookup(found->second);
  };
  auto *copyRegs=mappedEntryValue(regsBase),*copyGuest=mappedEntryValue(guestBase);
  llvm::IRBuilder<> builder(bounds);
  auto *regsSize=builder.CreateAlignedLoad(builder.getInt64Ty(),builder.CreateConstGEP1_64(builder.getInt8Ty(),guarded->getArg(0),offsetof(GuestStateV2,regs_size)),llvm::Align(1));
  auto *guestSize=builder.CreateAlignedLoad(builder.getInt64Ty(),builder.CreateConstGEP1_64(builder.getInt8Ty(),guarded->getArg(1),offsetof(GuestMemoryV2,size)),llvm::Align(1));
  llvm::Value *valid=builder.getTrue();
  if(!registers.empty())valid=builder.CreateAnd(builder.CreateICmpUGE(regsSize,builder.getInt64(minimumRegisters)),builder.CreateIsNotNull(copyRegs));
  if(!memory.empty())valid=builder.CreateAnd(valid,builder.CreateAnd(builder.CreateICmpUGE(guestSize,builder.getInt64(minimumMemory)),builder.CreateIsNotNull(copyGuest)));
  auto *checks=llvm::BasicBlock::Create(*view->ctx_,"guarded.facts",guarded);
  builder.CreateCondBr(valid,checks,fallback);builder.SetInsertPoint(checks);
  auto equalGuard=[&](llvm::Value *actual,llvm::ConstantInt *expected){
    auto *next=llvm::BasicBlock::Create(*view->ctx_,"guarded.next",guarded);
    builder.CreateCondBr(builder.CreateICmpEQ(actual,expected),next,fallback);builder.SetInsertPoint(next);
  };
  for(const auto &row:registers)for(std::size_t position=0;position<row.bytes.size();){
    const auto at=row.offset+position;
    const auto read=std::find_if(reads.begin(),reads.end(),[&](const auto &r){return at>=r.offset&&at-r.offset<r.bytes;});
    const auto count=chunkSize(std::min<std::uint64_t>(row.bytes.size()-position,read->bytes-(at-read->offset)));
    auto *value=mappedEntryValue(read->load);const auto width=count*8;
    if(at!=read->offset)value=builder.CreateLShr(value,llvm::ConstantInt::get(value->getType(),(at-read->offset)*8));
    if(value->getType()->getIntegerBitWidth()!=width)value=builder.CreateTrunc(value,builder.getIntNTy(width));
    std::uint64_t expected=0;for(unsigned n=0;n<count;++n)expected|=std::uint64_t(row.bytes[position+n])<<(n*8);
    equalGuard(value,llvm::ConstantInt::get(builder.getIntNTy(width),expected));position+=count;
  }
  for(const auto &row:memory)for(std::size_t position=0;position<row.bytes.size();){
    const auto count=chunkSize(row.bytes.size()-position);const auto width=count*8;
    auto *pointer=builder.CreateGEP(builder.getInt8Ty(),copyGuest,builder.getInt64(row.offset+position));
    auto *value=builder.CreateAlignedLoad(builder.getIntNTy(width),pointer,llvm::Align(1));
    std::uint64_t expected=0;for(unsigned n=0;n<count;++n)expected|=std::uint64_t(row.bytes[position+n])<<(n*8);
    equalGuard(value,llvm::ConstantInt::get(builder.getIntNTy(width),expected));position+=count;
  }
  builder.CreateBr(accepted);
  // A view prioritizes its selected external entry under existing shared pass
  // budgets. All ordinary functions retain their relative order and semantics.
  view->module_->getFunctionList().splice(view->module_->begin(),view->module_->getFunctionList(),guarded->getIterator());
  auto rowsJSON=[](const std::vector<GuardedEntryBytes> &rows){llvm::json::Array result;for(const auto &row:rows)result.push_back(llvm::json::Object{{"offset",std::format("0x{:x}",row.offset)},{"bytes",hexBytes(row.bytes)}});return result;};
  llvm::json::Object manifest{{"guarded",true},{"check_kind","runtime-byte-equality"},{"entry",std::format("0x{:x}",request.entry)},
      {"root_symbol",ordinary->getName().str()},{"view_symbol",name},{"source_sha256",hexBytes(sourceHash)},
      {"context_id",request.contextId},{"provenance",request.provenance},{"minimum_register_bytes",std::to_string(minimumRegisters)},
      {"minimum_guest_bytes",std::to_string(minimumMemory)},{"registers",rowsJSON(registers)},{"memory",rowsJSON(memory)}};
  std::string json;llvm::raw_string_ostream jsonStream(json);jsonStream<<llvm::json::Value(std::move(manifest));jsonStream.flush();
  view->module_->getOrInsertNamedMetadata(ManifestName)->addOperand(llvm::MDNode::get(*view->ctx_,llvm::MDString::get(*view->ctx_,json)));
  std::string finalError;llvm::raw_string_ostream finalOutput(finalError);
  if(llvm::verifyModule(*view->module_,&finalOutput))return fail("invalid guarded view: "+finalError);
  return view;
}
}
