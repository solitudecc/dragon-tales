#include <dragon/lifters/llvm/LLVMLifter.hpp>

#include <dragon/analysis/ArgumentRecovery.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>
#include "utils/FloatOps.hpp"
#include "LLVMImportMetadata.hpp"
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include "LLVMPredicateFolding.hpp"
#include "LLVMMBASimplification.hpp"
#include "LLVMPreparedRegion.hpp"
#include "LLVMLiftProvenance.hpp"
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/DCE.h>
#include <llvm/Transforms/Scalar/DFAJumpThreading.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

namespace {
std::uint32_t a64Word(const dragon::Instruction *instruction) {
  if (!instruction || instruction->bytes().size()!=4) return 0;
  const auto &b=instruction->bytes();
  return std::uint32_t(b[0]) | (std::uint32_t(b[1])<<8) | (std::uint32_t(b[2])<<16) | (std::uint32_t(b[3])<<24);
}
std::optional<unsigned> x86ControlWord(const dragon::Instruction *instruction,
                                     dragon::Architecture arch, bool call) {
  if (!instruction) return std::nullopt;
  const auto &b=instruction->bytes();std::size_t p=0;bool word16=false;
  while (p<b.size()) {
    const auto byte=b[p];
    if (byte==0x66) word16=true;
    else if (byte==0xf0 || byte==0xf2 || (byte==0xf3 && call)) return std::nullopt;
    else if (byte!=0x67 && byte!=0x26 && byte!=0x2e && byte!=0x36 && byte!=0x3e &&
             byte!=0x64 && byte!=0x65 && byte!=0xf3 &&
             !(arch==dragon::Architecture::X86_64 && byte>=0x40 && byte<=0x4f)) break;
    ++p;
  }
  if (p>=b.size()) return std::nullopt;
  // Long-mode operand-size-prefixed control transfers remain unsupported.
  if (word16 && arch==dragon::Architecture::X86_64) return std::nullopt;
  const unsigned word=arch==dragon::Architecture::X86_64 ? 8 : word16 ? 2 : 4;
  if (call) {
    if (b[p]==0xe8 && b.size()==p+1+(word==2 ? 2 : 4)) return word;
    if (b[p]==0xff && p+1<b.size() && ((b[p+1]>>3)&7)==2) return word;
  } else if ((b[p]==0xc3 && b.size()==p+1) || (b[p]==0xc2 && b.size()==p+3)) return word;
  return std::nullopt; // far/interrupt/authenticated effects are not near CALL/RET
}
std::optional<std::size_t> x86BranchOpcode(const dragon::Instruction *instruction,dragon::Architecture arch) {
  const auto &b=instruction->bytes();std::size_t p=0;
  while(p<b.size()) {
    const auto byte=b[p];
    if(byte==0x66 || byte==0xf0 || byte==0xf2 || byte==0xf3)return std::nullopt;
    if(byte==0x67 || byte==0x26 || byte==0x2e || byte==0x36 || byte==0x3e || byte==0x64 || byte==0x65 ||
       (arch==dragon::Architecture::X86_64 && byte>=0x40 && byte<=0x4f))++p;
    else return p;
  }
  return std::nullopt;
}
}

bool dragon::llvmlifter::LLVMLifter::lift(const Function &fn,
                                          const Graph &graph) {
  return liftFunction(fn,graph,fn.blocks(),false);
}

bool dragon::llvmlifter::LLVMLifter::liftStaticRegion(const Function &fn,
    const Graph &graph,std::span<const std::uint64_t> additionalBlockStarts,
    const StaticRegionOptions &options) {
  if(mode_!=ExecutionMode::StaticAnalysis || staticEntryPolicy_!=StaticEntryPolicy::FunctionStart ||
     !fn.isValid() || graph.architecture()!=arch_ ||
     !options.maxBlocks || options.maxBlocks>4096 ||
     !options.maxDispatchCases || options.maxDispatchCases>65536 ||
     additionalBlockStarts.size()>options.maxBlocks || fn.blocks().size()>options.maxBlocks)
    return false;
  // Preparation owns the exact immutable view used by lowering. Future
  // analyses must consume this view, never pre-rebuild indices in the source.
  auto region=detail::PreparedRegion::prepare(fn,graph,additionalBlockStarts,options);
  return region && detail::RegionLowering::run(*this,*region);
}

bool dragon::llvmlifter::LLVMLifter::liftFunction(const Function &fn,
    const Graph &graph,const std::map<std::uint64_t,BasicBlock> &blocks,bool staticRegion,
    detail::LiftCapture *capture) {
  // Refuse an invalid Function view rather than crash on it. Function's
  // constructor already defines what that means - not a known valid
  // function, no blocks, or an invalid block inside it - lift() simply
  // never checked. (Checking blocks().empty() instead would miss the
  // bad-block case, where blocks_ is partially filled but valid_ is false.)
  //
  // A blockless function is not exotic: discoverFunctionStarts() trusts
  // every call target unconditionally, so a call to an address outside the
  // disassembled range - an ordinary PLT stub reached from a raw .text
  // blob - becomes a function start with nothing under it. Graph::functions()
  // filters those out via isValid(), but a Function can also be built
  // directly from an address, which is exactly what the C shim's
  // dragon_graph_get_function() does - so every C and Python caller can
  // reach one.
  //
  // buildBlockMap() below cannot catch this: it only reports a missing
  // IGNIL block from inside its per-block loop, so with zero blocks it
  // returns an empty but perfectly valid map and that guard passes
  // vacuously - after which buildRegFile() -> allocateRegFile() reads
  // llfn->getEntryBlock() on a function with no basic blocks, and
  // segfaults. Checked before the llvm::Function is created, so a rejected
  // lift leaves nothing behind in the module.
  if (!fn.isValid())
    return false;

  // The Legacy void(mem, regs) ABI has no control-transfer result. Reject a
  // function it cannot represent before creating any LLVM objects. In
  // particular, an enumerated indirect-jump table does not prove that its
  // default edge is impossible. Unknown/out-of-region targets are not UB.
  if (mode_ != ExecutionMode::Legacy &&
      arch_ != Architecture::X86_32 && arch_ != Architecture::X86_64 && arch_ != Architecture::ARM64) return false;
  // Check this operand-less boundary before collecting registers or creating
  // LLVM objects. Stateful block lowering cuts off the op itself, so a later
  // liftOps check alone would never validate hand-built boundary shapes.
  for (const auto &[address, block] : blocks)
    if (const auto *ir = graph.getIGNILBlock(address))
      for (const auto &op : ir->ops())
        if (op.opcode == ignil::Opcode::UNSUPPORTED &&
            (op.dst || !op.srcs.empty() || op.aux != 0)) return false;
  auto localTarget = [&](const ignil::Value &value) {
    const auto *constant = std::get_if<ignil::Const>(&value);
    return constant && blocks.contains(constant->value);
  };
  if (mode_ == ExecutionMode::Legacy) for (const auto &[address, block] : blocks) {
    const auto *ir = graph.getIGNILBlock(address);
    // The Legacy ABI cannot report an unsupported-instruction boundary.
    // Check the marker even when the instruction emitted no IGNIL ops (for
    // example an unsupported memory store), before mutating the module.
    // UNDEF alone is still valid for architecturally undefined results.
    if (!ir || !ir->unlifted().empty() ||
        std::any_of(ir->begin(),ir->end(),[](const auto &op) {
          return op.opcode == ignil::Opcode::UNSUPPORTED;
        })) return false;
    // The decoder may recognize control flow with no supported IGNIL form.
    // Check machine calls too, so a dropped far/unknown call cannot bypass
    // the Legacy refusal merely by having no CALL op in the block.
    for (const auto *instruction : block.instructions()) {
      if (instruction->isCall()) return false;
      if (instruction->isReturn() && std::none_of(ir->ops().begin(),ir->ops().end(),[&](const auto& op) {
            return op.address==instruction->address() && op.opcode==ignil::Opcode::RET;
          })) return false;
    }
    for (const auto &op : ir->ops()) {
      if (op.opcode == ignil::Opcode::CALL) return false;
      if (op.opcode == ignil::Opcode::JMP &&
          (op.srcs.size() != 1 || !localTarget(op.srcs[0]))) return false;
      if (op.opcode == ignil::Opcode::CJMP &&
          (op.srcs.size() != 3 || !localTarget(op.srcs[1]) ||
           !localTarget(op.srcs[2]))) return false;
    }
  }

  // Generate a function without return value, and without parameters
  // we have not extracted information about that yet...
  // The name is the address of the function.
  std::string name = mode_ != ExecutionMode::Legacy
      ? (mode_ == ExecutionMode::StaticAnalysis ? staticFunctionName(fn.startAddress())
         : std::format("dragon_v2_{}_{:x}", ArchitectureName(arch_), fn.startAddress()))
      : std::format("sub_{:x}", fn.startAddress());
  auto *ptrTy = llvm::PointerType::getUnqual(*ctx_);
  auto *voidTy = llvm::Type::getVoidTy(*ctx_);
  auto *FnType = llvm::FunctionType::get(voidTy,
      mode_ != ExecutionMode::Legacy ? std::vector<llvm::Type*>{ptrTy,ptrTy,ptrTy,ptrTy}
                                        : std::vector<llvm::Type*>{ptrTy,ptrTy}, false);
  auto *llfn=module_->getFunction(name);
  const bool hadDeclaration=llfn!=nullptr;
  if (llfn && (mode_!=ExecutionMode::StaticAnalysis || !llfn->isDeclaration() ||
               llfn->getFunctionType()!=FnType)) return false;
  if (!llfn) llfn=llvm::Function::Create(FnType,llvm::GlobalValue::ExternalLinkage,name,*module_);
  const auto originalAttributes=llfn->getAttributes();
  std::vector<std::string> originalArgumentNames;
  if(hadDeclaration)for(const auto &argument:llfn->args())originalArgumentNames.push_back(argument.getName().str());
  auto failedLift=[&] {
    // A caller may already reference a forward declaration. Preserve that
    // symbol if the body cannot be lifted, rather than leaving dangling uses.
    llfn->deleteBody();llfn->removeFnAttr("dragon.entrypoints");
    llfn->removeFnAttr("dragon.static-entry-policy");
    llfn->removeFnAttr("dragon.signature");
    llfn->removeFnAttr("dragon.static-coverage");
    llfn->removeFnAttr("dragon.coverage-blocks");
    llfn->removeFnAttr("dragon.coverage-source");
    if(hadDeclaration) {
      llfn->setAttributes(originalAttributes);
      for(std::size_t i=0;i<originalArgumentNames.size();++i)llfn->getArg(i)->setName(originalArgumentNames[i]);
    }
    if (!hadDeclaration && llfn->use_empty()) llfn->eraseFromParent();
    return false;
  };
  if (mode_ != ExecutionMode::Legacy) {
    llfn->getArg(0)->setName("state"); llfn->getArg(1)->setName("memory");
    llfn->getArg(2)->setName("runtime"); llfn->getArg(3)->setName("exit");
  } else {
    llfn->getArg(0)->setName("mem"); llfn->getArg(1)->setName("regs");
  }

  // Record the recovered prototype as a function attribute: non-destructive,
  // survives the optimisation passes, and is greppable in the printed IR
  // (`"dragon.signature"="i32 sub_1000(i32 RDI, ...)"`). The body still uses
  // the (mem, regs) convention; turning this signature into a real typed
  // FunctionType - with the arguments seeded from LLVM parameters and a
  // modelled stack replacing the mem pointer - is the next step (see
  // ToDo/Roadmap.md).
  llfn->addFnAttr(
          "dragon.signature",
          analysis::toString(analysis::recoverSignature(fn, graph, arch_),
                             fn.startAddress()));
  if(staticRegion) {
    std::string addresses;
    for(const auto &[address,block]:blocks) {
      if(!addresses.empty())addresses+=',';
      addresses+=std::format("{:x}",address);
    }
    llfn->addFnAttr("dragon.static-coverage","decoded-region");
    llfn->addFnAttr("dragon.coverage-blocks",addresses);
    llfn->addFnAttr("dragon.coverage-source","graph-ignil");
  }

  llvm::Value *memPtr = llfn->getArg(0);
  llvm::Value *regsPtr = llfn->getArg(1);

  unsupportedAddresses_.clear();
  for (const auto &[address,block] : blocks) {
    const auto *ib=graph.getIGNILBlock(address);
    if (ib) unsupportedAddresses_.insert(ib->unlifted().begin(),ib->unlifted().end());
  }
  graph_ = &graph; // for resolved indirect-jump targets; see JMP
  struct ResetGraph {
    const Graph *&g;
    ~ResetGraph() { g = nullptr; }
  } resetGraph{graph_};

  // A dedicated entry block holds the allocas and the register loads and
  // branches to the function's first block. Using that block itself as the
  // LLVM entry broke two ways: a loop back to the function start made the
  // entry block a branch target ("Entry block to function must not have
  // predecessors!"), and with code below the entry address (gcc's cold
  // parts) the lowest-addressed block, not the entry, came first.
  auto *entryBB = llvm::BasicBlock::Create(*ctx_, "entry", llfn);
  if (stateful()) {
    llvm::IRBuilder<> prologue(entryBB);
    regsPtr = prologue.CreateAlignedLoad(ptrTy, prologue.CreateConstGEP1_64(prologue.getInt8Ty(), llfn->getArg(0), offsetof(GuestStateV2,regs_bytes)), llvm::Align(1));
    memPtr = prologue.CreateAlignedLoad(ptrTy, llfn->getArg(1), llvm::Align(1));
  }
  auto bbMap = buildBlockMap(llfn, blocks, graph);

  if (!bbMap || !bbMap->contains(fn.startAddress())) {
    return failedLift();
  }
  llvm::IRBuilder<>(entryBB).CreateBr(bbMap->at(fn.startAddress()));
  resumeEntries_=*bbMap;

  auto regFile = buildRegFile(llfn, blocks, graph);

  initializeRegistersFromParam(llfn, regFile, regsPtr);

  for (const auto &[addr, block]: blocks) {
    const auto *ib = graph.getIGNILBlock(addr);
    if (!ib)
      continue;
    TempMap temps{}; // fresh per block - Temp ids don't cross block boundaries
    if (!liftBlock(ib, llfn, &regFile, &temps, memPtr, regsPtr, bbMap.value(),capture) ||
        (capture && !capture->ok())) {
      return failedLift();
    }
  }
  if (stateful()) {
    if(capture)capture->boundary(detail::SourceKind::Entry);
    entryBB->getTerminator()->eraseFromParent();
    llvm::IRBuilder<> builder(entryBB);
    auto *pc=builder.CreateLoad(builder.getInt64Ty(),regFile.at(arch_ == Architecture::ARM64 ? 264 : 128).at(64));
    auto *unknown=llvm::BasicBlock::Create(*ctx_,"entry.unsupported",llfn);
    auto *dispatch=builder.CreateSwitch(pc,unknown,resumeEntries_.size());
    if(capture)capture->edge(detail::EdgeRole::Entry,dispatch);
    std::string entries;
    for (const auto &[address,block] : resumeEntries_) {
      if (mode_ == ExecutionMode::StaticAnalysis &&
          staticEntryPolicy_ == StaticEntryPolicy::FunctionStart &&
          address != fn.startAddress()) continue;
      dispatch->addCase(builder.getInt64(address),block);
      if (!entries.empty()) entries+=',';
      entries+=std::format("{:x}",address);
    }
    llfn->addFnAttr("dragon.entrypoints",entries);
    if (mode_ == ExecutionMode::StaticAnalysis &&
        staticEntryPolicy_ == StaticEntryPolicy::FunctionStart)
      llfn->addFnAttr("dragon.static-entry-policy","function-start");
    llvm::IRBuilder<> out(unknown);
    emitExitAtValue(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_NONE,pc,pc,regFile,regsPtr,out,DRAGON_DETAIL_ENTRY,0,capture);
  }
  if(capture && !capture->seal(llfn,*bbMap))return failedLift();
  if (stateful()) ::dragon::detail::recordLLVMImports(*module_,*llfn,fn,graph);
  return true;
}
void dragon::llvmlifter::LLVMLifter::collectRegsFromOps(
        const std::vector<ignil::Op> &ops, RegFile &regFile,
        std::map<std::pair<std::uint32_t, ignil::Width>, std::string_view>
                &regNames) const {
  // Lambda used to check if a value contains a register.
  // in that case, we will keep it for the alloca instruction
  auto collect = [&](const ignil::Value &v) {
    if (std::holds_alternative<ignil::Reg>(v)) {
      const auto &reg = std::get<ignil::Reg>(v);
      regFile[reg.offset].try_emplace(reg.width, nullptr);
      if (!reg.name.empty())
        regNames[{reg.offset, reg.width}] = reg.name;
    }
  };

  // x86-64 / AArch64: a write to a 32-bit general-purpose register (EAX, W0)
  // clears the upper half of its 64-bit parent. mergeIntoParent() does that - but only into a parent
  // slot that exists, and slots are made for the widths the code mentions. A
  // function that only ever says `eax` would have no RAX slot, so nothing
  // would be zero-extended and only 32 bits would be written back on return:
  // the caller's upper half of RAX would survive `xor eax, eax`. Give every
  // such destination its parent.
  auto collectParent = [&](const ignil::Value &v) {
    const auto *reg = std::get_if<ignil::Reg>(&v);
    if (reg && reg->width == 32 && zeroExtendsToGpr(reg->offset))
      regFile[reg->offset].try_emplace(64, nullptr);
  };

  for (const auto &op: ops) {
    if (op.dst.has_value()) {
      collect(op.dst.value());
      collectParent(op.dst.value());
    }
    for (const auto &src: op.srcs)
      collect(src);
  }
}

void dragon::llvmlifter::LLVMLifter::allocateRegFile(
        llvm::Function *llfn, RegFile &regFile,
        const std::map<std::pair<std::uint32_t, ignil::Width>,
                       std::string_view> &regNames) const {
  llvm::IRBuilder<> builder(&llfn->getEntryBlock(),
                            llfn->getEntryBlock().begin());
  for (auto &[offset, widthMap]: regFile) {
    for (auto &[width, slot]: widthMap) {
      // If we can provide a better naming...s
      auto it = regNames.find({offset, width});
      std::string name = (it != regNames.end())
                                 ? std::string(it->second)
                                 : "reg_" + std::to_string(offset) + "_" +
                                           std::to_string(width) + "b";
      slot = builder.CreateAlloca(llvm::IntegerType::get(*ctx_, width), nullptr,
                                  name);
    }
  }
}

dragon::llvmlifter::LLVMLifter::RegFile
dragon::llvmlifter::LLVMLifter::buildRegFile(llvm::Function *llfn,
                                             const std::map<std::uint64_t,BasicBlock> &blocks,
                                             const Graph &graph) const {
  RegFile regFile{};
  std::map<std::pair<uint32_t, ignil::Width>, std::string_view> regNames;

  // go through every block of the function
  for (const auto &[addr, block]: blocks) {
    // retrieve the lifted version
    const auto *ignilBlock = graph.getIGNILBlock(addr);
    if (!ignilBlock)
      continue;
    collectRegsFromOps(ignilBlock->ops(), regFile, regNames);
  }

  if (stateful()) {
    regFile[arch_ == Architecture::ARM64 ? 264 : 128].try_emplace(64,nullptr);
    if (arch_ != Architecture::ARM64) regFile[32].try_emplace(arch_ == Architecture::X86_64 ? 64 : 32,nullptr);
    else {
      regFile[240].try_emplace(64,nullptr);
      for (const auto &[address,block] : blocks)
        for (const auto &op : graph.getIGNILBlock(address)->ops())
          if (op.opcode == ignil::Opcode::RET && op.aux<=32) {
            const auto n=op.aux ? op.aux-1 : 30;
            if (n!=31) regFile[n*8].try_emplace(64,nullptr);
          }
    }
    if (arch_ != Architecture::ARM64 && (regFile.contains(480) || regFile.contains(486))) {
      regFile[480].try_emplace(8,nullptr);regFile[486].try_emplace(16,nullptr);
    }
  }
  allocateRegFile(llfn, regFile, regNames);
  return regFile;
}

bool dragon::llvmlifter::LLVMLifter::liftBlock(const ignil::Block &block,
                                               std::string_view name) {
  if (block.empty() || !block.unlifted().empty() || mode_ != ExecutionMode::Legacy)
    return false;

  if (std::any_of(block.begin(),block.end(),[](const auto &op) {
        return op.opcode == ignil::Opcode::UNSUPPORTED;
      })) return false;

  bool live=true;
  for (const auto &op : block.ops()) {
    if (op.opcode == ignil::Opcode::LABEL) live=true;
    if (!live) continue;
    if (op.opcode == ignil::Opcode::CALL) return false;
    if (op.opcode == ignil::Opcode::RET || op.opcode == ignil::Opcode::LOCALJMP) live=false;
  }
  std::string fnName = name.empty() ? "trace" : std::string(name);
  auto *ptrTy = llvm::PointerType::getUnqual(*ctx_);
  auto *voidTy = llvm::Type::getVoidTy(*ctx_);
  auto *FnType = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
  auto *llfn = llvm::Function::Create(
          FnType, llvm::GlobalValue::ExternalLinkage, fnName, *module_);
  llfn->getArg(0)->setName("mem");
  llfn->getArg(1)->setName("regs");
  auto *memPtr  = llfn->getArg(0);
  auto *regsPtr = llfn->getArg(1);

  auto *entry = llvm::BasicBlock::Create(*ctx_, "entry", llfn);

  RegFile regFile{};
  std::map<std::pair<std::uint32_t, ignil::Width>, std::string_view> regNames;
  collectRegsFromOps(block.ops(), regFile, regNames);
  allocateRegFile(llfn, regFile, regNames);

  initializeRegistersFromParam(llfn, regFile, regsPtr);

  TempMap temps{};
  llvm::IRBuilder<> builder(entry);
  const std::map<std::uint64_t, llvm::BasicBlock *> noTargets;
  if (!liftOps(block.ops(),regFile,temps,memPtr,regsPtr,builder,noTargets,true)) {
    llfn->eraseFromParent();
    return false;
  }
  if (!builder.GetInsertBlock()->getTerminator())
    emitReturn(regFile, regsPtr, builder);

  return true;
}
std::optional<std::map<std::uint64_t, llvm::BasicBlock *>>
dragon::llvmlifter::LLVMLifter::buildBlockMap(llvm::Function *llfn,
                                              const std::map<std::uint64_t,BasicBlock> &blocks,
                                              const Graph &graph) const {
  std::map<std::uint64_t, llvm::BasicBlock *> addrBlock;

  for (const auto &[addr, block]: blocks) {
    // If a block cannot be obtained from IGNIL, it doesn't have
    // any sense to lift the function to LLVM IR, since a block
    // would be lost.
    if (!graph.getIGNILBlock(addr))
      return std::nullopt;

    addrBlock[addr] =
            llvm::BasicBlock::Create(*ctx_, "bb_" + std::to_string(addr), llfn);
  }

  return addrBlock;
}
bool dragon::llvmlifter::LLVMLifter::liftBlock(
        const ignil::Block *ib, llvm::Function *llfn, RegFile *regs,
        TempMap *temps, llvm::Value *memPtr, llvm::Value *regsPtr,
        const std::map<std::uint64_t, llvm::BasicBlock *> &bbMap, detail::LiftCapture *capture) {
  llvm::IRBuilder<> builder(bbMap.at(ib->startAddress()));

  std::optional<std::uint64_t> unsupportedPC;
  if (stateful()) {
    emitBudgetCheck(ib->startAddress(),*regs,regsPtr,builder);
    for (const auto *instruction : BasicBlock(graph_,ib->startAddress()).instructions()) {
      bool unsupported=instruction->isTrap() || unsupportedAddresses_.contains(instruction->address());
      if (instruction->isCall() || instruction->isReturn()) {
        if (arch_ == Architecture::ARM64) {
          const auto word=a64Word(instruction);
          const bool normal=instruction->isCall() ? ((word&0xfc000000)==0x94000000 || (word&0xfffffc1f)==0xd63f0000)
                                                    : ((word&0xfffffc1f)==0xd65f0000);
          unsupported |= !normal;
        } else unsupported |= !x86ControlWord(instruction,arch_,instruction->isCall()).has_value();
      }
      if(instruction->isJump() && !instruction->isCall() && !instruction->isReturn()) {
        if(arch_==Architecture::ARM64 && !instruction->isConditional()) {
          const auto word=a64Word(instruction);
          unsupported |= (word&0xfc000000)!=0x14000000 && (word&0xfffffc1f)!=0xd61f0000;
        } else if(arch_!=Architecture::ARM64) {
          const auto p=x86BranchOpcode(instruction,arch_);
          if(!p)unsupported=true;
          else if(!instruction->isConditional()) {
            const auto &b=instruction->bytes();
            unsupported |= !((b[*p]==0xe9 && b.size()==*p+5) || (b[*p]==0xeb && b.size()==*p+2) ||
                (b[*p]==0xff && *p+1<b.size() && ((b[*p+1]>>3)&7)==4));
          }
        }
      }
      if (unsupported) {unsupportedPC=instruction->address();break;}
    }
    for (const auto &op : ib->ops())
      if (op.opcode == ignil::Opcode::OPAQUE || op.opcode == ignil::Opcode::UNSUPPORTED)
        unsupportedPC=unsupportedPC ? std::min(*unsupportedPC,op.address) : op.address;
  }
  auto ops=std::span<const ignil::Op>{ib->ops()};
  if (unsupportedPC) {
    const auto stop=std::find_if(ops.begin(),ops.end(),[&](const auto &op) {return op.address>=*unsupportedPC;});
    ops=ops.first(static_cast<std::size_t>(stop-ops.begin()));
  }
  if (!liftOps(ops,*regs,*temps,memPtr,regsPtr,builder,bbMap,false,capture,ib->startAddress()))
    return false;
  // A block that ends in a trap (ud2, hlt, brk) or an instruction without
  // semantics has no IGNIL terminator and no successor; LLVM still needs
  // one. Control does not continue past it.
  if (!builder.GetInsertBlock()->getTerminator()) {
    if (stateful()) {
      const auto *last=BasicBlock(graph_,ib->startAddress()).terminator();
      const auto stop=unsupportedPC.value_or(last ? last->address()+last->size() : ib->startAddress());
      if(capture)capture->boundary(unsupportedPC ? detail::SourceKind::UnsupportedInstruction : detail::SourceKind::StreamEnd,
                                  ib->startAddress(),stop);
      emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_NONE,stop,nullptr,*regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);
    } else builder.CreateUnreachable();
  }
  return true;
}
bool dragon::llvmlifter::LLVMLifter::liftOps(
        std::span<const ignil::Op> ops, RegFile &regs, TempMap &temps,
        llvm::Value *memPtr, llvm::Value *regsPtr, llvm::IRBuilder<> &builder,
        const std::map<std::uint64_t,llvm::BasicBlock *> &bbMap,bool trace,
        detail::LiftCapture *capture,std::uint64_t blockAddress) {
  using O=ignil::Opcode;
  std::map<std::uint32_t,llvm::BasicBlock *> labels;
  auto *function=builder.GetInsertBlock()->getParent();
  for (const auto &op : ops) if (op.opcode == O::LABEL) {
    if (op.dst || !op.srcs.empty() || labels.contains(op.aux)) return false;
    labels.emplace(op.aux,llvm::BasicBlock::Create(*ctx_,"local_"+std::to_string(op.aux),function));
  }
  for (const auto &op : ops)
    if ((op.opcode == O::LOCALJMP || op.opcode == O::LOCALCJMP) &&
        (op.dst || !labels.contains(op.aux) ||
         op.srcs.size() != (op.opcode == O::LOCALCJMP ? 1u : 0u) ||
         (op.opcode == O::LOCALCJMP && ignil::widthOf(op.srcs[0]) != 1))) return false;
  for (std::size_t opIndex=0;opIndex<ops.size();++opIndex) {
    const auto &op=ops[opIndex];
    if (op.opcode == O::UNSUPPORTED &&
        (!stateful() || op.dst || !op.srcs.empty() || op.aux != 0)) return false;
    if (op.opcode!=O::LABEL && builder.GetInsertBlock()->getTerminator()) continue;
    if(capture)capture->begin(blockAddress,opIndex,builder.GetInsertBlock());
    if (op.opcode == O::LABEL) {
      auto *target=labels.at(op.aux);
      if (!builder.GetInsertBlock()->getTerminator()) {
        auto *branch=builder.CreateBr(target);
        if(capture)capture->edge(detail::EdgeRole::LabelFallthrough,branch);
      }
      builder.SetInsertPoint(target);
      if (stateful()) emitBudgetCheck(op.address,regs,regsPtr,builder);
      if(capture)capture->finish(builder.GetInsertBlock());
      continue;
    }
    if (op.opcode == O::LOCALJMP) {
      auto *branch=builder.CreateBr(labels.at(op.aux));
      if(capture) {capture->edge(detail::EdgeRole::LocalJump,branch);capture->finish(builder.GetInsertBlock());}
      continue;
    }
    if (op.opcode == O::LOCALCJMP) {
      auto *fallthrough=llvm::BasicBlock::Create(*ctx_,"local_next",function);
      auto *condition=resolve(op.srcs[0],regs,temps,builder);
      if(capture)capture->operand(0,condition);
      auto *branch=builder.CreateCondBr(condition,labels.at(op.aux),fallthrough);
      if(capture)capture->edge(detail::EdgeRole::LocalConditional,branch);
      builder.SetInsertPoint(fallthrough);
      if(capture)capture->finish(builder.GetInsertBlock());
      continue;
    }
    if (stateful()) {
      const bool unsupported = op.opcode == O::OPAQUE || unsupportedAddresses_.contains(op.address);
      if (unsupported) {
        emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_NONE,op.address,nullptr,regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);
        if(capture)capture->finish(builder.GetInsertBlock());
        continue;
      }
    }
    if (stateful()) for (const auto &source : op.srcs)
      if (const auto *temporary=std::get_if<ignil::Temp>(&source);temporary && !temps.contains(temporary->id))
        return false; // a machine entry cannot depend on hidden transient SSA
    // A resolved machine trace drops its external branches, while preserving
    // the internal loop belonging to each individual machine instruction.
    if (trace && (op.opcode == O::JMP || op.opcode == O::CJMP)) continue;
    liftOp(op,regs,temps,memPtr,regsPtr,builder,bbMap,capture);
    if (stateful() && op.opcode == O::CALL) {
      // Every Guest call returns to the dispatcher. A later invocation enters
      // the exact Guest continuation with fresh register loads, never old SSA.
      if (mode_ == ExecutionMode::StatefulV2) if (const auto *instruction=graph_->get(op.address)) {
        const auto next=instruction->address()+instruction->size();
        if (!bbMap.contains(next)) {
          auto *continuation=llvm::BasicBlock::Create(*ctx_,"call.continue",function);
          resumeEntries_.emplace(next,continuation);
          builder.SetInsertPoint(continuation);
        }
      }
      temps.clear();
    }
    if(capture)capture->finish(builder.GetInsertBlock());
  }
  return true;
}

void dragon::llvmlifter::LLVMLifter::liftOp(
        const ignil::Op &op, RegFile &regs, TempMap &temps, llvm::Value *memPtr,
        llvm::Value *regsPtr, llvm::IRBuilder<> &builder,
        const std::map<std::uint64_t, llvm::BasicBlock *> &bbMap, detail::LiftCapture *capture) {

  // Store a result into dst: Temp → SSA table, Reg → alloca + sub-reg merge.
  auto emit = [&](llvm::Value *val) {
    if (!op.dst)
      return;
    // IGNIL temps and registers contain raw bits. Keeping float results as
    // integers also lets ordinary TRUNC/ZEXT/bitwise ops consume them safely;
    // LLVM folds redundant round-trip bitcasts in float-only chains.
    if (val->getType()->isFloatingPointTy())
      val = builder.CreateBitCast(val,llvm::IntegerType::get(*ctx_,
                           val->getType()->getPrimitiveSizeInBits().getFixedValue()));
    auto *resultType = llvm::IntegerType::get(*ctx_,ignil::widthOf(*op.dst));
    if (val->getType() != resultType) val = builder.CreateZExtOrTrunc(val,resultType);
    if(capture)capture->result(val);
    if (const auto *t = std::get_if<ignil::Temp>(&*op.dst)) {
      // If it is a temporal value, it will be used
      // for SSA
      temps[t->id] = val;
    } else if (const auto *r = std::get_if<ignil::Reg>(&*op.dst)) {
      builder.CreateStore(val, regs.at(r->offset).at(r->width));
      mergeIntoParent(r->offset, r->width, val, regs, builder);
      syncOverlapping(r->offset, r->width, val, regs, builder);
    }
  };

  // Lambda functions to quickly manage the common operands of the operations
  auto operand = [&](unsigned index) {
    auto *value=resolve(op.srcs[index],regs,temps,builder);
    if(capture)capture->operand(index,value);
    return value;
  };
  auto r0 = [&] { return operand(0); };
  auto r1 = [&] { return operand(1); };
  auto r2 = [&] { return operand(2); };
  // Lambda to quickly retrieve the integer type of the destination
  auto dstTy = [&] {
    return llvm::IntegerType::get(*ctx_, ignil::widthOf(*op.dst));
  };
  // SHL/SHR/SAR's shift-count operand (srcs[1]) is narrower than the
  // shifted value on real ISAs (x86 shift-by-immediate/CL is always an
  // 8-bit count regardless of operand width) - IGNIL preserves that
  // narrower width, but LLVM's shift instructions require both operands to
  // share exactly one type. Normalize with zext/trunc before building the
  // instruction, or LLVM happily builds a type-mismatched shl/lshr/ashr
  // that only fails much later, when something tries to parse the printed
  // IR back in (verifier catches it too, but nothing here calls verify()).
  auto shiftAmt = [&](llvm::Value *lhs) {
    auto *rhs = r1();
    if (rhs->getType() != lhs->getType())
      rhs = builder.CreateZExtOrTrunc(rhs, lhs->getType());
    return rhs;
  };
  // A 32-bit address is unsigned, but a GEP sign-extends its index.
  detail::MemoryCheck checked;
  auto memAddress = [&](llvm::Value *addr) {
    if (stateful()) {
      auto bytes = op.opcode == ignil::Opcode::STORE ? (ignil::widthOf(op.srcs[1])+7)/8 : (ignil::widthOf(*op.dst)+7)/8;
      return memoryAddress(addr,bytes,op.address,regs,regsPtr,memPtr,builder,&checked,capture);
    }
    auto *indexTy = module_->getDataLayout().getIndexType(memPtr->getType());
    return builder.CreateGEP(builder.getInt8Ty(), memPtr,
                             builder.CreateZExtOrBitCast(addr, indexTy));
  };

  if (ignil::detail::floatBinaryOp(op.opcode)) {
    auto *type = ignil::detail::floatBits(op.opcode) == 32 ? builder.getFloatTy() : builder.getDoubleTy();
    auto asFloat = [&](llvm::Value *value) { return builder.CreateBitCast(value,type); };
    auto *left = asFloat(r0()), *right = asFloat(r1());
    switch (op.opcode) {
      case ignil::Opcode::FADD: case ignil::Opcode::FADD32: emit(builder.CreateFAdd(left,right)); break;
      case ignil::Opcode::FSUB: case ignil::Opcode::FSUB32: emit(builder.CreateFSub(left,right)); break;
      case ignil::Opcode::FMUL: case ignil::Opcode::FMUL32: emit(builder.CreateFMul(left,right)); break;
      case ignil::Opcode::FDIV: case ignil::Opcode::FDIV32: emit(builder.CreateFDiv(left,right)); break;
      case ignil::Opcode::FLT: case ignil::Opcode::FLT32: emit(builder.CreateFCmpOLT(left,right)); break;
      case ignil::Opcode::FEQ: case ignil::Opcode::FEQ32: emit(builder.CreateFCmpOEQ(left,right)); break;
      default: emit(builder.CreateFCmpUNO(left,right)); break;
    }
    return;
  }
  if (ignil::detail::floatUnaryOp(op.opcode)) {
    auto *type = ignil::detail::floatBits(op.opcode) == 32 ? builder.getFloatTy() : builder.getDoubleTy();
    auto *source = r0();
    switch (op.opcode) {
      case ignil::Opcode::SITOFP: case ignil::Opcode::SITOFP32:
        emit(builder.CreateSIToFP(source,type)); return;
      case ignil::Opcode::UITOFP: case ignil::Opcode::UITOFP32:
        emit(builder.CreateUIToFP(source,type)); return;
      default: break;
    }
    auto *value = builder.CreateBitCast(source,type);
    switch (op.opcode) {
      case ignil::Opcode::FPTOSI: case ignil::Opcode::FPTOSI32:
        emit(builder.CreateFPToSI(value,dstTy())); break;
      case ignil::Opcode::FPTOUI: case ignil::Opcode::FPTOUI32:
        emit(builder.CreateFPToUI(value,dstTy())); break;
      case ignil::Opcode::FPEXT: emit(builder.CreateFPExt(value,builder.getDoubleTy())); break;
      case ignil::Opcode::FPTRUNC: emit(builder.CreateFPTrunc(value,builder.getFloatTy())); break;
      default: {
        const auto intrinsic = op.opcode == ignil::Opcode::FSQRT || op.opcode == ignil::Opcode::FSQRT32 ? llvm::Intrinsic::sqrt
                             : op.opcode == ignil::Opcode::FFLOOR || op.opcode == ignil::Opcode::FFLOOR32 ? llvm::Intrinsic::floor
                             : op.opcode == ignil::Opcode::FCEIL || op.opcode == ignil::Opcode::FCEIL32 ? llvm::Intrinsic::ceil
                             : llvm::Intrinsic::trunc;
        emit(builder.CreateUnaryIntrinsic(intrinsic,value));
        break;
      }
    }
    return;
  }

  switch (op.opcode) {
    case ignil::Opcode::UNSUPPORTED:
      emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_NONE,op.address,nullptr,
               regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);
      break;

    // --- Data movement ---
    case ignil::Opcode::COPY:
      emit(r0());
      break;

    case ignil::Opcode::LOAD: {
      auto *load=builder.CreateAlignedLoad(dstTy(), memAddress(r0()), llvm::Align(1));
      if(capture)capture->access(detail::AccessRole::Load,checked,load);
      emit(load);
      break;
    }
    case ignil::Opcode::STORE: {
      auto *store=builder.CreateAlignedStore(r1(), memAddress(r0()), llvm::Align(1));
      if(capture)capture->access(detail::AccessRole::Store,checked,store);
      break;
    }

    // --- Arithmetic ---
    case ignil::Opcode::ADD:
      emit(builder.CreateAdd(r0(), r1()));
      break;
    case ignil::Opcode::SUB:
      emit(builder.CreateSub(r0(), r1()));
      break;
    case ignil::Opcode::NEG:
      emit(builder.CreateNeg(r0()));
      break;
    case ignil::Opcode::MUL:
      emit(builder.CreateMul(r0(), r1()));
      break;
    case ignil::Opcode::SDIV:
      emit(builder.CreateSDiv(r0(), r1()));
      break;
    case ignil::Opcode::UDIV:
      emit(builder.CreateUDiv(r0(), r1()));
      break;
    case ignil::Opcode::SMOD:
      emit(builder.CreateSRem(r0(), r1()));
      break;
    case ignil::Opcode::UMOD:
      emit(builder.CreateURem(r0(), r1()));
      break;

    // --- Bitwise ---
    case ignil::Opcode::AND:
      emit(builder.CreateAnd(r0(), r1()));
      break;
    case ignil::Opcode::OR:
      emit(builder.CreateOr(r0(), r1()));
      break;
    case ignil::Opcode::XOR:
      emit(builder.CreateXor(r0(), r1()));
      break;
    case ignil::Opcode::NOT:
      emit(builder.CreateNot(r0()));
      break;
    case ignil::Opcode::SHL: {
      auto *lhs = r0();
      emit(builder.CreateShl(lhs, shiftAmt(lhs)));
      break;
    }
    case ignil::Opcode::SHR: {
      auto *lhs = r0();
      emit(builder.CreateLShr(lhs, shiftAmt(lhs)));
      break;
    }
    case ignil::Opcode::SAR: {
      auto *lhs = r0();
      emit(builder.CreateAShr(lhs, shiftAmt(lhs)));
      break;
    }

    // --- Width conversions ---
    case ignil::Opcode::ZEXT:
      emit(builder.CreateZExt(r0(), dstTy()));
      break;
    case ignil::Opcode::SEXT:
      emit(builder.CreateSExt(r0(), dstTy()));
      break;
    case ignil::Opcode::TRUNC:
      emit(builder.CreateTrunc(r0(), dstTy()));
      break;

    // --- Comparisons (always produce i1) ---
    case ignil::Opcode::EQ:
      emit(builder.CreateICmpEQ(r0(), r1()));
      break;
    case ignil::Opcode::NEQ:
      emit(builder.CreateICmpNE(r0(), r1()));
      break;
    case ignil::Opcode::ULT:
      emit(builder.CreateICmpULT(r0(), r1()));
      break;
    case ignil::Opcode::ULE:
      emit(builder.CreateICmpULE(r0(), r1()));
      break;
    case ignil::Opcode::UGT:
      emit(builder.CreateICmpUGT(r0(), r1()));
      break;
    case ignil::Opcode::UGE:
      emit(builder.CreateICmpUGE(r0(), r1()));
      break;
    case ignil::Opcode::SLT:
      emit(builder.CreateICmpSLT(r0(), r1()));
      break;
    case ignil::Opcode::SLE:
      emit(builder.CreateICmpSLE(r0(), r1()));
      break;
    case ignil::Opcode::SGT:
      emit(builder.CreateICmpSGT(r0(), r1()));
      break;
    case ignil::Opcode::SGE:
      emit(builder.CreateICmpSGE(r0(), r1()));
      break;

    // --- Control flow ---
    case ignil::Opcode::JMP: {
      auto *target = r0();
      if (stateful() && (llvm::isa<llvm::UndefValue>(target) || llvm::isa<llvm::PoisonValue>(target))) {
        emitExit(DRAGON_EXIT_UNKNOWN_TARGET,DRAGON_TRANSFER_BRANCH,op.address,target,regs,regsPtr,builder,0,0,capture);break;
      }
      if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(target)) {
        auto it = bbMap.find(ci->getZExtValue());
        if (it != bbMap.end()) {
          auto *branch=builder.CreateBr(it->second);
          if(capture)capture->edge(detail::EdgeRole::Jump,branch);
          break;
        }
      }
      // The default view dispatches only analysis-derived Graph targets. An
      // explicit static region can execute any of its decoded block starts,
      // but still checks the real target and retains the unknown-target exit.
      const Instruction *insn = graph_ ? graph_->get(op.address) : nullptr;
      auto *intTy = llvm::dyn_cast<llvm::IntegerType>(target->getType());
      const bool region=mode_==ExecutionMode::StaticAnalysis &&
          builder.GetInsertBlock()->getParent()->hasFnAttribute("dragon.static-coverage") &&
          builder.GetInsertBlock()->getParent()->getFnAttribute("dragon.static-coverage").getValueAsString()=="decoded-region";
      if (insn && intTy && insn->isJump() && (region || !insn->branches().empty())) {
        std::vector<std::pair<std::uint64_t, llvm::BasicBlock *>> cases;
        if(region) {
          for(const auto &[address,block]:bbMap)
            if(intTy->getBitWidth()>=64 || address<(std::uint64_t{1}<<intTy->getBitWidth()))
              cases.emplace_back(address,block);
        } else {
          for (std::uint64_t t: insn->branches())
            if (auto it = bbMap.find(t); it != bbMap.end())
              cases.emplace_back(t, it->second);
        }
        if (!cases.empty()) {
          auto *fn = builder.GetInsertBlock()->getParent();
          auto *other = llvm::BasicBlock::Create(*ctx_, "jmp.unresolved", fn);
          if (stateful()) {
            llvm::IRBuilder<> exitBuilder(other);
            emitExit(DRAGON_EXIT_TRANSFER,DRAGON_TRANSFER_BRANCH,op.address,target,regs,regsPtr,exitBuilder,0,0,capture);
          } else llvm::IRBuilder<>(other).CreateUnreachable();
          auto *sw = builder.CreateSwitch(target, other, cases.size());
          for (const auto &[t, bb]: cases)
            sw->addCase(llvm::ConstantInt::get(intTy, t), bb);
          if(capture)capture->edge(detail::EdgeRole::Jump,sw);
          break;
        }
      }
      if (stateful()) emitExit(DRAGON_EXIT_TRANSFER,DRAGON_TRANSFER_BRANCH,op.address,target,regs,regsPtr,builder,0,0,capture);
      else builder.CreateUnreachable(); // rejected by Legacy preflight
      break;
    }

    case ignil::Opcode::CJMP: {
      auto *cond = r0();
      auto *taken = r1();
      auto *fall = r2();
      const auto *takenCI = llvm::dyn_cast<llvm::ConstantInt>(taken);
      const auto *fallCI = llvm::dyn_cast<llvm::ConstantInt>(fall);
      auto destination = [&](llvm::Value *target, const llvm::ConstantInt *constant) {
        if (constant) if (auto it=bbMap.find(constant->getZExtValue());it!=bbMap.end()) return it->second;
        auto *other=llvm::BasicBlock::Create(*ctx_,"branch.exit",builder.GetInsertBlock()->getParent());
        llvm::IRBuilder<> out(other);
        if (stateful()) emitExit(DRAGON_EXIT_TRANSFER,DRAGON_TRANSFER_BRANCH,op.address,target,regs,regsPtr,out,0,0,capture);
        else out.CreateUnreachable();
        return other;
      };
      auto *takenBB=destination(taken,takenCI), *fallBB=destination(fall,fallCI);
      if (!cond->getType()->isIntegerTy(1)) cond=builder.CreateTrunc(cond,builder.getInt1Ty());
      auto *branch=builder.CreateCondBr(cond,takenBB,fallBB);
      if(capture)capture->edge(detail::EdgeRole::Conditional,branch);
      break;
    }

    case ignil::Opcode::CALL: {
      if (stateful()) {
        const auto *instruction=graph_->get(op.address);
        if (!instruction || op.srcs.size()!=1) {
          emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_CALL,op.address,nullptr,regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);break;
        }
        auto *target=r0(); // snapshot target before SP/LR changes (CALL SP / BLR X30)
        const auto next=instruction->address()+instruction->size();
        if (arch_ == Architecture::ARM64) {
          const auto word=a64Word(instruction);
          if ((word&0xfc000000)!=0x94000000 && (word&0xfffffc1f)!=0xd63f0000) {
            emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_CALL,op.address,nullptr,regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);break;
          }
          auto *link=builder.getInt64(next);builder.CreateStore(link,regs.at(240).at(64));
          mergeIntoParent(240,64,link,regs,builder);syncOverlapping(240,64,link,regs,builder);
        } else {
          const auto word=x86ControlWord(instruction,arch_,true);
          if (!word) {emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_CALL,op.address,nullptr,regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);break;}
          const auto width=arch_ == Architecture::X86_64 ? 64u : 32u;
          auto *type=builder.getIntNTy(width);
          target=builder.CreateZExtOrTrunc(target,builder.getIntNTy(*word*8));
          auto *sp=builder.CreateLoad(type,regs.at(32).at(width));
          auto *newSP=builder.CreateSub(sp,llvm::ConstantInt::get(type,*word));
          detail::MemoryCheck check;
          auto *slot=memoryAddress(newSP,*word,op.address,regs,regsPtr,memPtr,builder,&check,capture);
          auto *store=builder.CreateAlignedStore(llvm::ConstantInt::get(builder.getIntNTy(*word*8),next),slot,llvm::Align(1));
          if(capture)capture->access(detail::AccessRole::CallReturnSlot,check,store);
          builder.CreateStore(newSP,regs.at(32).at(width));
          mergeIntoParent(32,width,newSP,regs,builder);syncOverlapping(32,width,newSP,regs,builder);
        }
        if (mode_ == ExecutionMode::StaticAnalysis)
          emitStaticCall(op,target,next,regs,regsPtr,builder,bbMap,capture);
        else emitExit(DRAGON_EXIT_TRANSFER,DRAGON_TRANSFER_CALL,op.address,target,regs,regsPtr,builder,0,next);
        break;
      }
      auto *ptrTy = llvm::PointerType::getUnqual(*ctx_);
      auto *fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_),
                                           {ptrTy}, false);
      auto *target = r0();
      llvm::Value *callee;
      if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(target)) {
        callee =
                module_->getOrInsertFunction(
                               std::format("fn_{:x}", ci->getZExtValue()), fnTy)
                        .getCallee();
      } else {
        callee = builder.CreateIntToPtr(target, ptrTy); // indirect call
      }
      builder.CreateCall(fnTy, callee, {memPtr});
      break;
    }

    case ignil::Opcode::RET: {
      if (!stateful()) {emitReturn(regs,regsPtr,builder);break;}
      const auto *instruction=graph_->get(op.address);
      llvm::Value *target=nullptr;
      if (arch_ == Architecture::ARM64) {
        const auto word=a64Word(instruction);const auto n=op.aux ? op.aux-1 : 30;
        if ((word&0xfffffc1f)!=0xd65f0000 || op.aux>32 || ((word>>5)&31)!=n) {
          emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_RETURN,op.address,nullptr,regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);break;
        }
        target=n==31 ? static_cast<llvm::Value*>(builder.getInt64(0)) : builder.CreateLoad(builder.getInt64Ty(),regs.at(n*8).at(64));
      } else {
        const auto word=x86ControlWord(instruction,arch_,false);
        if (!word || op.aux>0xffff) {emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_RETURN,op.address,nullptr,regs,regsPtr,builder,DRAGON_DETAIL_INSTRUCTION,0,capture);break;}
        const auto width=arch_ == Architecture::X86_64 ? 64u : 32u;auto *type=builder.getIntNTy(width);
        auto *sp=builder.CreateLoad(type,regs.at(32).at(width));
        detail::MemoryCheck check;
        auto *slot=memoryAddress(sp,*word,op.address,regs,regsPtr,memPtr,builder,&check,capture);
        target=builder.CreateAlignedLoad(builder.getIntNTy(*word*8),slot,llvm::Align(1));
        if(capture)capture->access(detail::AccessRole::ReturnTarget,check,llvm::cast<llvm::Instruction>(target));
        auto *newSP=builder.CreateAdd(sp,llvm::ConstantInt::get(type,*word+op.aux));
        builder.CreateStore(newSP,regs.at(32).at(width));
        mergeIntoParent(32,width,newSP,regs,builder);syncOverlapping(32,width,newSP,regs,builder);
      }
      emitExit(DRAGON_EXIT_RETURN,DRAGON_TRANSFER_RETURN,op.address,target,regs,regsPtr,builder,0,0,capture);
      break;
    }

    case ignil::Opcode::UNDEF:
      if (op.dst) {
        llvm::Value *value=llvm::UndefValue::get(dstTy());
        if (stateful()) {
          value=builder.CreateFreeze(value);
          if (const auto *reg=std::get_if<ignil::Reg>(&*op.dst))
            if ((arch_ == Architecture::ARM64 && reg->offset>=784 && reg->offset<=787) ||
                (arch_ != Architecture::ARM64 && reg->offset>=136 && reg->offset<=142))
              value=builder.CreateAnd(value,llvm::ConstantInt::get(dstTy(),1));
        }
        emit(value);
      }
      break;

    // An instruction x86 defines but IGNIL cannot express, lowered the way
    // every lifter models an intrinsic: a call to a declared-but-never-defined
    // function. `readnone` lets LLVM CSE two calls that agree, which is
    // exactly right for CPUID and exactly wrong for RDTSC - so an impure kind
    // gets a name carrying its own address instead, making each site a
    // distinct function that nothing can fold together.
    case ignil::Opcode::OPAQUE: {
      if (!op.dst)
        break;
      const auto kind = ignil::opaqueKindOf(op.aux);
      std::string name = "dragon.opaque." +
                         std::string(ignil::opaqueKindName(kind)) + "." +
                         std::to_string(ignil::opaqueIndexOf(op.aux));
      if (!ignil::opaqueIsPure(kind))
        name += std::format(".at{:x}", op.address);

      std::vector<llvm::Value *> args;
      std::vector<llvm::Type *> argTys;
      args.reserve(op.srcs.size());
      argTys.reserve(op.srcs.size());
      for (const auto &src: op.srcs) {
        args.push_back(resolve(src, regs, temps, builder));
        argTys.push_back(args.back()->getType());
      }

      auto *fty = llvm::FunctionType::get(dstTy(), argTys, /*isVarArg=*/false);
      auto callee = module_->getOrInsertFunction(name, fty);
      if (auto *fn = llvm::dyn_cast<llvm::Function>(callee.getCallee())) {
        fn->setDoesNotAccessMemory();
        fn->setDoesNotThrow();
      }
      emit(builder.CreateCall(callee, args));
      break;
    }
  }
}

void dragon::llvmlifter::LLVMLifter::writeBackRegisters(RegFile &regs,
                                                llvm::Value *regsPtr,
                                                llvm::IRBuilder<> &builder) {
  // FPTOP is authoritative; every boundary exports a coherent architectural
  // SW rather than stale TOP bits cached before a callee or x87 push/pop.
  if (stateful() && arch_ != Architecture::ARM64 && regs.contains(480)) {
    auto *top=builder.CreateLoad(builder.getInt8Ty(),regs.at(480).at(8));
    auto *sw=builder.CreateLoad(builder.getInt16Ty(),regs.at(486).at(16));
    auto *bits=builder.CreateShl(builder.CreateAnd(builder.CreateZExt(top,builder.getInt16Ty()),builder.getInt16(7)),builder.getInt16(11));
    auto *updated=builder.CreateOr(builder.CreateAnd(sw,builder.getInt16(0xc7ff)),bits);
    builder.CreateStore(updated,regs.at(486).at(16));
    mergeIntoParent(486,16,updated,regs,builder);syncOverlapping(486,16,updated,regs,builder);
  }
  // Write back the widest register at each offset so callers can observe
  // the final register state even after aggressive optimisation passes.
  // A view inside a register already written back (AH within RAX) holds the
  // same bytes - syncOverlapping() keeps them equal - so it is skipped.
  std::uint32_t writtenUntil = 0;
  bool any = false;
  for (auto &[offset, widthMap]: regs) {
    auto &[width, slot] = *widthMap.rbegin(); // widest at this offset
    const std::uint32_t end = offset + (width + 7) / 8;
    if (any && end <= writtenUntil)
      continue;
    writtenUntil = std::max(writtenUntil, end);
    any = true;
    auto *ty  = llvm::IntegerType::get(*ctx_, width);
    auto *val = builder.CreateLoad(ty, slot);
    auto *ptr = builder.CreateGEP(builder.getInt8Ty(), regsPtr,
                                  builder.getInt32(offset));
    builder.CreateAlignedStore(val, ptr, llvm::Align(1));
  }
}

void dragon::llvmlifter::LLVMLifter::emitReturn(RegFile &regs, llvm::Value *regsPtr, llvm::IRBuilder<> &builder) {
  writeBackRegisters(regs,regsPtr,builder); builder.CreateRetVoid();
}

llvm::Value *
dragon::llvmlifter::LLVMLifter::resolve(const ignil::Value &v, RegFile &regs,
                                        const TempMap &temps,
                                        llvm::IRBuilder<> &builder) const {

  // Check if it is a constant, in that case, generate
  // a constant llvm value width specific width and value.
  //
  // Defensively mask to `c->width` bits first: some X86IGNILLifter operand
  // decoding paths sign-extend an immediate to a full 64-bit pattern (e.g.
  // `cmp eax, 0xFFFFFFFF` produces Const{width=32,
  // value=0xFFFFFFFFFFFFFFFF}) without masking it back down to the
  // declared width. llvm::APInt's constructor asserts (in an
  // assertions-enabled build only - NDEBUG builds silently truncate) that
  // the value fits in `BitWidth` unsigned bits, so an unmasked over-wide
  // value here crashes any assertions-enabled build. Masking here is a
  // narrow, defensive fix at this one choke point (every ignil::Const
  // passes through here) - it does not fix the upstream sign-extension
  // bug itself, which is still real and worth root-causing (see
  // ToDo/Roadmap.md).
  if (const auto *c = std::get_if<ignil::Const>(&v)) {
    std::uint64_t masked = c->width >= 64
                                   ? c->value
                                   : c->value & ((std::uint64_t{1} << c->width) - 1);
    return llvm::ConstantInt::get(*ctx_, llvm::APInt(c->width, masked));
  }

  // Check if it is an existing register, in that case
  // we need to create a load
  if (const auto *r = std::get_if<ignil::Reg>(&v)) {
    auto *mem = regs.at(r->offset).at(r->width);
    return builder.CreateLoad(llvm::IntegerType::get(*ctx_, r->width), mem);
  }

  const auto &t = std::get<ignil::Temp>(v);
  return temps.at(t.id);
}
void dragon::llvmlifter::LLVMLifter::mergeIntoParent(
        std::uint32_t offset, ignil::Width width, llvm::Value *value,
        RegFile &regs, llvm::IRBuilder<> &builder) {
  auto it = regs.find(offset);
  if (it == regs.end())
    return;

  for (auto &[bigWidth, slot]: it->second) {
    if (bigWidth == width)
      continue;

    // A narrower slot is the low bits of the new value (`mov ecx, 0xffff`
    // then `inc cx` must read CX = 0xffff, not the stale CX slot).
    if (bigWidth < width) {
      builder.CreateStore(
              builder.CreateTrunc(value, llvm::IntegerType::get(*ctx_, bigWidth)),
              slot);
      continue;
    }

    auto *bigTy = llvm::IntegerType::get(*ctx_, bigWidth);

    // x86-64 / AArch64: writing a 32-bit register implicitly zero-extends
    // into the full 64-bit register (`mov ecx, 1` clears bits 32-63 of RCX,
    // `mov w1, #1` those of X1 - the IGNIL convention SymbolicExecutor and
    // dead-store elimination follow too) -
    // unlike 8/16-bit sub-register writes (AL, AX, ...), which leave the
    // rest of the register untouched. Every other width transition here
    // preserves the untouched high bits; this one specific transition must
    // not, or a register zeroed via its 32-bit name (a very common x86-64
    // idiom, e.g. `xor ecx, ecx`) would keep stale high bits when read back
    // through its 64-bit name.
    if (width == 32 && bigWidth == 64 && zeroExtendsToGpr(offset)) {
      builder.CreateStore(builder.CreateZExt(value, bigTy), slot);
      continue;
    }

    auto *mask = llvm::ConstantInt::get(
            bigTy, ~llvm::APInt::getLowBitsSet(bigWidth, width));
    auto *parent = builder.CreateLoad(bigTy, slot);
    auto *cleared = builder.CreateAnd(parent, mask);
    auto *ext = builder.CreateZExt(value, bigTy);
    builder.CreateStore(builder.CreateOr(cleared, ext), slot);
  }
}

bool dragon::llvmlifter::LLVMLifter::zeroExtendsToGpr(
        std::uint32_t offset) const {
  switch (arch_) {
    case Architecture::X86_64:
      return offset < x86::kOffRIP && offset % 8 == 0; // RAX..R15
    case Architecture::ARM64:
      return offset <= arm::kOffPC && offset % 8 == 0; // X0..X30, XZR, SP
    default:
      return false;
  }
}

void dragon::llvmlifter::LLVMLifter::syncOverlapping(
        std::uint32_t offset, ignil::Width width, llvm::Value *value,
        RegFile &regs, llvm::IRBuilder<> &builder) {
  // Slots are keyed by (offset, width), and mergeIntoParent() only reaches
  // the ones at the same offset. A view at another offset that shares bytes
  // with the write - AH (byte 1) against RAX/EAX/AX (byte 0) - must follow
  // it too, or `mov ah, 1; mov rax, 0` leaves a stale AH that emitReturn()
  // then writes over byte 1 of RAX, and `mov rax, x; mov bl, ah` reads the
  // old AH.
  const std::uint32_t ws = offset, we = offset + (width + 7) / 8;
  for (auto &[off, widthMap]: regs) {
    if (off == offset)
      continue;
    for (auto &[w, slot]: widthMap) {
      const std::uint32_t ss = off, se = off + (w + 7) / 8;
      if (se <= ws || we <= ss)
        continue;
      auto *slotTy = llvm::IntegerType::get(*ctx_, w);
      if (ws <= ss && se <= we) {
        // The slot lies inside the write (AH after a RAX/EAX/AX write).
        llvm::Value *v = value;
        if (ss > ws)
          v = builder.CreateLShr(v, (ss - ws) * 8);
        builder.CreateStore(builder.CreateTrunc(v, slotTy), slot);
      } else if (ss <= ws && we <= se) {
        // The write lies inside the slot (RAX/EAX/AX after an AH write).
        const unsigned shift = (ws - ss) * 8;
        auto *mask = llvm::ConstantInt::get(
                slotTy, ~llvm::APInt::getBitsSet(w, shift, shift + width));
        auto *old = builder.CreateLoad(slotTy, slot);
        auto *ins = builder.CreateShl(builder.CreateZExt(value, slotTy), shift);
        builder.CreateStore(builder.CreateOr(builder.CreateAnd(old, mask), ins),
                            slot);
      }
      // Straddling views do not occur in the x86/AArch64 register files.
    }
  }
}

void dragon::llvmlifter::LLVMLifter::initializeRegistersFromParam(
        llvm::Function *llfn, RegFile &regFile, llvm::Value *regsPtr) {
  auto &entryBB = llfn->getEntryBlock();
  auto insertPt = entryBB.getTerminator() ? entryBB.getTerminator()->getIterator() : entryBB.end();
  llvm::IRBuilder<> builder(&entryBB, insertPt);
  reloadRegisters(regFile,regsPtr,builder);
}

void dragon::llvmlifter::LLVMLifter::reloadRegisters(
        RegFile &regFile, llvm::Value *regsPtr, llvm::IRBuilder<> &builder) {
  for (auto &[offset, widthMap] : regFile) {
    auto &[bigWidth, bigSlot] = *widthMap.rbegin();
    auto *bigTy = llvm::IntegerType::get(*ctx_, bigWidth);
    auto *ptr   = builder.CreateGEP(builder.getInt8Ty(), regsPtr,
                                    builder.getInt32(offset));
    auto *val   = builder.CreateAlignedLoad(bigTy, ptr, llvm::Align(1));
    builder.CreateStore(val, bigSlot);
    for (auto &[width, slot] : widthMap) {
      if (width == bigWidth)
        continue;
      builder.CreateStore(
              builder.CreateTrunc(val, llvm::IntegerType::get(*ctx_, width)),
              slot);
    }
  }
}

// --- Constructor / destructor ---

dragon::llvmlifter::LLVMLifter::LLVMLifter(Architecture arch,
                                           std::string_view moduleName) :
    arch_(arch), ctx_(std::make_unique<llvm::LLVMContext>()),
    module_(std::make_unique<llvm::Module>(moduleName, *ctx_)) {}

dragon::llvmlifter::LLVMLifter::~LLVMLifter() = default;

dragon::llvmlifter::LLVMLifter::LLVMLifter(LLVMLifter &&) noexcept = default;

// --- Optimization passes ---

// Shared boilerplate: run a FunctionPassManager over every non-declaration
// function in the module.
static void runFPM(llvm::Module &mod, llvm::FunctionPassManager fpm) {
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::ModulePassManager mpm;
  mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
  mpm.run(mod, mam);
}

void dragon::llvmlifter::LLVMLifter::runMem2Reg() {
  llvm::FunctionPassManager fpm;
  fpm.addPass(llvm::PromotePass());
  runFPM(*module_, std::move(fpm));
}

void dragon::llvmlifter::LLVMLifter::runInstCombine() {
  llvm::FunctionPassManager fpm;
  fpm.addPass(llvm::InstCombinePass());
  runFPM(*module_, std::move(fpm));
}

void dragon::llvmlifter::LLVMLifter::runGVN() {
  llvm::FunctionPassManager fpm;
  fpm.addPass(llvm::GVNPass());
  runFPM(*module_, std::move(fpm));
}

void dragon::llvmlifter::LLVMLifter::runDCE() {
  llvm::FunctionPassManager fpm;
  fpm.addPass(llvm::DCEPass());
  runFPM(*module_, std::move(fpm));
}

void dragon::llvmlifter::LLVMLifter::runControlFlowSimplification() {
  llvm::FunctionPassManager fpm;
  fpm.addPass(llvm::SCCPPass());
  // Ordinary jump threading avoids some loop headers. DFA threading handles
  // the predictable PHI/select state machines found in flattened switches.
  fpm.addPass(llvm::DFAJumpThreadingPass());
  fpm.addPass(llvm::JumpThreadingPass());
  fpm.addPass(llvm::SimplifyCFGPass());
  fpm.addPass(llvm::InstCombinePass());
  fpm.addPass(llvm::DCEPass());
  runFPM(*module_, std::move(fpm));
}

dragon::llvmlifter::PredicateFoldStats
dragon::llvmlifter::LLVMLifter::runPredicateFolding(const PredicateFoldOptions &options) {
  auto stats = detail::foldPredicates(*module_, options);
  if (stats.conditionsFolded) {
    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::SimplifyCFGPass());
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::DCEPass());
    runFPM(*module_, std::move(fpm));
  }
  return stats;
}

// --- Accessors ---

dragon::llvmlifter::MBASimplifyStats
dragon::llvmlifter::LLVMLifter::runMBASimplification(const MBASimplifyOptions &options) {
  auto stats = detail::simplifyMBA(*module_, options);
  if (stats.expressionsSimplified) {
    llvm::FunctionPassManager fpm;
    fpm.addPass(llvm::InstCombinePass());
    fpm.addPass(llvm::DCEPass());
    runFPM(*module_, std::move(fpm));
  }
  return stats;
}

dragon::llvmlifter::MemoryForwardStats
dragon::llvmlifter::LLVMLifter::runGuestMemorySSA(const MemoryForwardOptions &options) {
  if (mode_ == ExecutionMode::Legacy || module_->empty())
    return {};
  if (options.runMem2Reg)
    runMem2Reg();
  return detail::runGuestMemorySSA(*module_, options);
}

dragon::llvmlifter::DispatcherSpecializationStats
dragon::llvmlifter::LLVMLifter::runDispatcherSpecialization(
    const DispatcherSpecializationOptions &options) {
  return runDispatcherSpecialization(options, false);
}

dragon::llvmlifter::DispatcherSpecializationStats
dragon::llvmlifter::LLVMLifter::runDispatcherSpecialization(
    const DispatcherSpecializationOptions &options, bool allowPlainStores) {
  if (module_->empty()) return {};
  return ::dragon::llvmlifter::runDispatcherSpecialization(*module_, options,
                                                         allowPlainStores);
}

dragon::llvmlifter::DeflatteningStats
dragon::llvmlifter::LLVMLifter::runProvableDeflattening(
    const DeflatteningOptions &options) {
  if (module_->empty()) return {};
  return ::dragon::llvmlifter::runProvableDeflattening(*module_, options);
}

dragon::llvmlifter::MemoryForwardStats
dragon::llvmlifter::LLVMLifter::runGuestMemorySSA(
    const Graph &graph, const MemoryForwardOptions &options) {
  if (mode_ == ExecutionMode::Legacy || module_->empty())
    return {};
  if (options.runMem2Reg)
    runMem2Reg();
  return detail::runGuestMemorySSA(*module_, options, &graph);
}

std::string dragon::llvmlifter::LLVMLifter::ir() const {
  std::string out;
  llvm::raw_string_ostream os(out);
  module_->print(os, nullptr);
  return out;
}

llvm::Module &dragon::llvmlifter::LLVMLifter::module() { return *module_; }

std::string dragon::llvmlifter::LLVMLifter::recoveredSignatureString(
        const Function &fn, const Graph &graph) const {
  return analysis::toString(analysis::recoverSignature(fn, graph, arch_),
                            fn.startAddress());
}

bool dragon::llvmlifter::LLVMLifter::setExecutionMode(ExecutionMode mode) {
  if (!module_->empty() || (mode != ExecutionMode::Legacy && mode != ExecutionMode::StatefulV2 && mode != ExecutionMode::StaticAnalysis)) return false;
  if (staticEntryPolicy_ != StaticEntryPolicy::Resumable && mode != ExecutionMode::StaticAnalysis) return false;
  mode_ = mode; return true;
}

bool dragon::llvmlifter::LLVMLifter::setStaticEntryPolicy(StaticEntryPolicy policy) {
  if (mode_ != ExecutionMode::StaticAnalysis || !module_->empty() ||
      (policy != StaticEntryPolicy::Resumable && policy != StaticEntryPolicy::FunctionStart)) return false;
  staticEntryPolicy_ = policy;
  return true;
}

std::string dragon::llvmlifter::LLVMLifter::staticFunctionName(std::uint64_t address) const {
  return std::format("dragon_static_{}{}_{:x}",
      staticEntryPolicy_ == StaticEntryPolicy::FunctionStart ? "entry_" : "",
      ArchitectureName(arch_), address);
}

void dragon::llvmlifter::LLVMLifter::emitExit(std::uint64_t reason, std::uint64_t kind,
    std::uint64_t sourcePC, llvm::Value *target, RegFile &regs, llvm::Value *regsPtr,
    llvm::IRBuilder<> &builder, std::uint64_t detail, std::uint64_t returnPC, detail::LiftCapture *capture) {
  emitExitAtValue(reason,kind,builder.getInt64(sourcePC),target,regs,regsPtr,builder,detail,returnPC,capture);
}
void dragon::llvmlifter::LLVMLifter::emitExitAtValue(std::uint64_t reason, std::uint64_t kind,
    llvm::Value *sourcePC, llvm::Value *target, RegFile &regs, llvm::Value *regsPtr,
    llvm::IRBuilder<> &builder, std::uint64_t detail, std::uint64_t returnPC, detail::LiftCapture *capture) {
  const auto values=writeExitAtValue(reason,kind,sourcePC,target,regs,builder,detail,returnPC);
  emitReturn(regs,regsPtr,builder);
  if(capture)capture->exit(llvm::cast<llvm::ReturnInst>(builder.GetInsertBlock()->getTerminator()),values);
}
dragon::llvmlifter::detail::ExitValues dragon::llvmlifter::LLVMLifter::writeExitAtValue(std::uint64_t reason, std::uint64_t kind,
    llvm::Value *sourcePC, llvm::Value *target, RegFile &regs,
    llvm::IRBuilder<> &builder, std::uint64_t detail, std::uint64_t returnPC) {
  const bool known = target && !llvm::isa<llvm::UndefValue>(target) && !llvm::isa<llvm::PoisonValue>(target);
  if (target && !known) reason=DRAGON_EXIT_UNKNOWN_TARGET;
  auto *pc = known ? builder.CreateZExtOrTrunc(target,builder.getInt64Ty()) : sourcePC;
  const auto offset = arch_ == Architecture::ARM64 ? 264u : 128u;
  builder.CreateStore(pc,regs.at(offset).at(64));
  mergeIntoParent(offset,64,pc,regs,builder);
  syncOverlapping(offset,64,pc,regs,builder);
  auto *exit = builder.GetInsertBlock()->getParent()->getArg(3);
  // With no target, Unsupported/Budget stop *before* the source instruction.
  const std::uint64_t valid = !target || known;
  const std::pair<std::size_t,llvm::Value *> fields[]{
    {offsetof(ExecExitV2,reason),builder.getInt64(reason)},
    {offsetof(ExecExitV2,transfer_kind),builder.getInt64(kind)},
    {offsetof(ExecExitV2,source_pc),sourcePC},
    {offsetof(ExecExitV2,next_pc),pc},
    {offsetof(ExecExitV2,next_pc_valid),builder.getInt64(valid)},
    {offsetof(ExecExitV2,return_pc),builder.getInt64(returnPC)},
    {offsetof(ExecExitV2,detail),builder.getInt64(detail)}
  };
  for (const auto &[offset,value] : fields)
    builder.CreateAlignedStore(value,builder.CreateConstGEP1_64(builder.getInt8Ty(),exit,offset),llvm::Align(1));
  return {reason,kind,detail,returnPC,sourcePC,pc,valid!=0};
}

void dragon::llvmlifter::LLVMLifter::emitStaticCall(const ignil::Op &op,
    llvm::Value *target, std::uint64_t next, RegFile &regs, llvm::Value *regsPtr,
    llvm::IRBuilder<> &builder, const std::map<std::uint64_t,llvm::BasicBlock *> &bbMap, detail::LiftCapture *capture) {
  auto *fn=builder.GetInsertBlock()->getParent();
  const auto request=writeExitAtValue(DRAGON_EXIT_TRANSFER,DRAGON_TRANSFER_CALL,builder.getInt64(op.address),
                                     target,regs,builder,0,next);
  writeBackRegisters(regs,regsPtr,builder);
  auto *type=fn->getFunctionType();
  // This declaration makes no promises about effects or preserved registers.
  // Guest targets remain numeric state; there is no native inttoptr bridge.
  std::string calleeName="dragon_static_external";
  if (const auto *constant=llvm::dyn_cast<llvm::ConstantInt>(target)) {
    const auto address=constant->getZExtValue();
    const auto internal=staticFunctionName(address);
    if (module_->getFunction(internal) || Function(graph_,address).isValid()) calleeName=internal;
  }
  auto callee=module_->getOrInsertFunction(calleeName,type);
  auto *call=builder.CreateCall(callee,{fn->getArg(0),fn->getArg(1),fn->getArg(2),fn->getArg(3)});
  // The numeric Guest target is the sole contract used by the optional
  // LeafCallProof consumer.  External/unknown targets intentionally carry no
  // metadata and remain ordinary CALL barriers.
  if (calleeName != "dragon_static_external") {
    auto *targetMetadata = llvm::MDNode::get(
        *ctx_, llvm::ConstantAsMetadata::get(
                   llvm::ConstantInt::get(
                       builder.getInt64Ty(),
                       llvm::cast<llvm::ConstantInt>(target)->getZExtValue())));
    call->setMetadata("dragon.guest.call.target", targetMetadata);
  }
  reloadRegisters(regs,regsPtr,builder);
  const auto pcOffset=arch_==Architecture::ARM64 ? 264u : 128u;
  auto *pc=builder.CreateLoad(builder.getInt64Ty(),regs.at(pcOffset).at(64));
  auto loadExit=[&](std::size_t offset) {
    return builder.CreateAlignedLoad(builder.getInt64Ty(),builder.CreateConstGEP1_64(
        builder.getInt8Ty(),fn->getArg(3),offset),llvm::Align(1));
  };
  auto *reason=loadExit(offsetof(ExecExitV2,reason));
  auto *kind=loadExit(offsetof(ExecExitV2,transfer_kind));
  auto *valid=loadExit(offsetof(ExecExitV2,next_pc_valid));
  auto *mirror=loadExit(offsetof(ExecExitV2,next_pc));
  auto *isReturn=builder.CreateICmpEQ(reason,builder.getInt64(DRAGON_EXIT_RETURN));
  auto *requiresPC=builder.CreateOr(isReturn,builder.CreateOr(
      builder.CreateICmpEQ(reason,builder.getInt64(DRAGON_EXIT_TRANSFER)),
      builder.CreateICmpEQ(reason,builder.getInt64(DRAGON_EXIT_NEED_TARGET))));
  auto *hasPC=builder.CreateICmpEQ(valid,builder.getInt64(1));
  auto *invalid=builder.CreateOr(builder.CreateICmpULT(reason,builder.getInt64(1)),
                                builder.CreateICmpUGT(reason,builder.getInt64(DRAGON_EXIT_BUDGET)));
  invalid=builder.CreateOr(invalid,builder.CreateICmpUGT(kind,builder.getInt64(DRAGON_TRANSFER_RETURN)));
  invalid=builder.CreateOr(invalid,builder.CreateICmpUGT(valid,builder.getInt64(1)));
  invalid=builder.CreateOr(invalid,builder.CreateAnd(requiresPC,builder.CreateNot(hasPC)));
  invalid=builder.CreateOr(invalid,builder.CreateAnd(hasPC,builder.CreateICmpNE(mirror,pc)));
  invalid=builder.CreateOr(invalid,builder.CreateAnd(
      builder.CreateICmpEQ(reason,builder.getInt64(DRAGON_EXIT_UNKNOWN_TARGET)),hasPC));
  auto *check=llvm::BasicBlock::Create(*ctx_,"call.exit.valid",fn);
  auto *bad=llvm::BasicBlock::Create(*ctx_,"call.exit.invalid",fn);
  auto *validateExit=builder.CreateCondBr(invalid,bad,check);
  if(capture)capture->edge(detail::EdgeRole::CallExitValidation,validateExit);
  llvm::IRBuilder<> error(bad);
  emitExitAtValue(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_NONE,pc,pc,regs,regsPtr,error,DRAGON_DETAIL_INVALID_EXIT,0,capture);
  builder.SetInsertPoint(check);
  auto *normal=llvm::BasicBlock::Create(*ctx_,"call.continue",fn);
  auto *propagate=llvm::BasicBlock::Create(*ctx_,"call.propagate",fn);
  auto *returned=builder.CreateAnd(isReturn,builder.CreateICmpEQ(kind,builder.getInt64(DRAGON_TRANSFER_RETURN)));
  auto *validateReturn=builder.CreateCondBr(builder.CreateAnd(returned,builder.CreateICmpEQ(pc,builder.getInt64(next))),normal,propagate);
  if(capture)capture->edge(detail::EdgeRole::CallReturnValidation,validateReturn);
  auto *propagated=llvm::IRBuilder<>(propagate).CreateRetVoid(); // state/Exit already came from the callee
  if(capture) {
    capture->exit(propagated,std::nullopt);
    capture->call(call,target,next,request,validateExit,validateReturn,bad,normal,propagate);
  }
  builder.SetInsertPoint(normal);
  if (auto it=bbMap.find(next);it!=bbMap.end()) {
    auto *branch=builder.CreateBr(it->second);
    if(capture)capture->edge(detail::EdgeRole::CallContinuation,branch);
  }
  else resumeEntries_.emplace(next,normal);
}

void dragon::llvmlifter::LLVMLifter::emitBudgetCheck(std::uint64_t pc,
    RegFile &regs, llvm::Value *regsPtr, llvm::IRBuilder<> &builder) {
  if (mode_ == ExecutionMode::StaticAnalysis) return;
  auto *fn=builder.GetInsertBlock()->getParent();
  auto *budgetPtr=builder.CreateConstGEP1_64(builder.getInt8Ty(),fn->getArg(2),offsetof(ExecContextV2,budget));
  auto *budget=builder.CreateAlignedLoad(builder.getInt64Ty(),budgetPtr,llvm::Align(1));
  auto *work=llvm::BasicBlock::Create(*ctx_,"budget.continue",fn);
  auto *stop=llvm::BasicBlock::Create(*ctx_,"budget.stop",fn);
  builder.CreateCondBr(builder.CreateICmpNE(budget,builder.getInt64(0)),work,stop);
  llvm::IRBuilder<> out(stop);
  emitExit(DRAGON_EXIT_BUDGET,DRAGON_TRANSFER_NONE,pc,nullptr,regs,regsPtr,out);
  builder.SetInsertPoint(work);
  builder.CreateAlignedStore(builder.CreateSub(budget,builder.getInt64(1)),budgetPtr,llvm::Align(1));
}

llvm::Value *dragon::llvmlifter::LLVMLifter::memoryAddress(llvm::Value *address,
    std::uint64_t bytes, std::uint64_t pc, RegFile &regs, llvm::Value *regsPtr,
    llvm::Value *memPtr, llvm::IRBuilder<> &builder, detail::MemoryCheck *check, detail::LiftCapture *capture) {
  auto *fn=builder.GetInsertBlock()->getParent();
  auto *index=builder.CreateZExtOrTrunc(address,builder.getInt64Ty());
  auto *sizePtr=builder.CreateConstGEP1_64(builder.getInt8Ty(),fn->getArg(1),offsetof(GuestMemoryV2,size));
  auto *size=builder.CreateAlignedLoad(builder.getInt64Ty(),sizePtr,llvm::Align(1));
  auto *enough=builder.CreateICmpUGE(size,builder.getInt64(bytes));
  auto *inside=builder.CreateICmpULE(index,builder.CreateSub(size,builder.getInt64(bytes)));
  auto *work=llvm::BasicBlock::Create(*ctx_,"memory.valid",fn);
  auto *stop=llvm::BasicBlock::Create(*ctx_,"memory.invalid",fn);
  auto *branch=builder.CreateCondBr(builder.CreateAnd(enough,inside),work,stop);
  if(check)*check={address,index,memPtr,address->getType()->getIntegerBitWidth(),bytes,branch,work,stop};
  if(capture)capture->edge(detail::EdgeRole::MemoryBounds,branch);
  llvm::IRBuilder<> out(stop);
  emitExit(DRAGON_EXIT_UNSUPPORTED,DRAGON_TRANSFER_NONE,pc,nullptr,regs,regsPtr,out,DRAGON_DETAIL_MEMORY,0,capture);
  builder.SetInsertPoint(work);
  return builder.CreateGEP(builder.getInt8Ty(),memPtr,index);
}
