#include <dragon/disassembler/Disassembler.hpp>

#include <stack>
#include <unordered_set>

#include "InstructionImpl.hpp"
#include "disassemblers/ARMTarget.hpp"
#include "disassemblers/LLVMTarget.hpp"
#include "disassemblers/RISCVTarget.hpp"
#include "disassemblers/X86Target.hpp"
#include "disassemblers/arm/armAnalysis.hpp"
#include "disassemblers/arm/armIGNILLifter.hpp"
#include "disassemblers/x86/X86IGNILLifter.hpp"
#include "disassemblers/x86/x86Analysis.hpp"
#include "utils/Logger.hpp"

struct dragon::Disassembler::Impl {
  LLVMTarget target;
  internal::Logger logger;
};

dragon::Disassembler::Disassembler(Architecture architecture,
                                   const Configuration &configuration) :
    architecture_(architecture), configuration_(configuration),
    impl_(std::make_unique<Impl>()) {
  impl_->logger = internal::Logger{configuration_.logging};

  const unsigned syntaxVariant =
          (configuration_.syntax == Syntax::INTEL) ? 1 : 0;

  const std::string &cpu = configuration_.cpu;
  const std::string &features = configuration_.features;

  switch (architecture) {
    case Architecture::X86_32:
      impl_->target = internal::initX86(false, syntaxVariant, cpu, features);
      break;
    case Architecture::X86_64:
      impl_->target = internal::initX86(true, syntaxVariant, cpu, features);
      break;
    case Architecture::ARM32:
      impl_->target = internal::initARM(false, cpu, features);
      break;
    case Architecture::ARM64:
      impl_->target = internal::initARM(true, cpu, features);
      break;
    case Architecture::RISCV32:
      impl_->target = internal::initRISCV(false, cpu, features);
      break;
    case Architecture::RISCV64:
      impl_->target = internal::initRISCV(true, cpu, features);
      break;
    default:
      break;
  }

  impl_->logger.log("Disassembler: arch={} syntax={} strategy={}",
                    ArchitectureName(architecture_),
                    configuration_.syntax == Syntax::INTEL ? "intel" : "att",
                    configuration_.disassemblyStrategy ==
                                    DisassemblyStrategy::LINEAR_SWEEP
                            ? "linear_sweep"
                            : "recursive_traversal");
  if (!isValid())
    impl_->logger.log("Disassembler: target initialization failed for {}",
                      ArchitectureName(architecture_));
}

dragon::Disassembler::~Disassembler() = default;
dragon::Disassembler::Disassembler(Disassembler &&) noexcept = default;

bool dragon::Disassembler::isValid() const noexcept {
  return impl_ && impl_->target.disassembler != nullptr;
}

std::vector<dragon::Instruction>
dragon::Disassembler::disassemble(std::span<const std::uint8_t> bytes,
                                  std::uint64_t baseAddress) const {
  if (!isValid()) {
    impl_->logger.log("disassemble: called on invalid disassembler");
    return {};
  }
  // disassemble is just a wrapper over different disassembly algorithms
  const std::uint64_t root = baseAddress;
  return disassemble(bytes, baseAddress, std::span<const std::uint64_t>{&root, 1});
}

std::vector<dragon::Instruction>
dragon::Disassembler::disassemble(std::span<const std::uint8_t> bytes,
                                  std::uint64_t baseAddress,
                                  std::span<const std::uint64_t> roots) const {
  if (!isValid()) {
    impl_->logger.log("disassemble: called on invalid disassembler");
    return {};
  }

  switch (configuration_.disassemblyStrategy) {
    case DisassemblyStrategy::LINEAR_SWEEP:
      return linearSweep(bytes, baseAddress);
    case DisassemblyStrategy::RECURSIVE_TRAVERSAL:
      return recursiveTraversal(bytes, baseAddress, roots);
  }
  return {};
}

void dragon::Disassembler::disassemble(std::span<const std::uint8_t> bytes,
                                       std::uint64_t baseAddress,
                                       Graph &graph) const {
  disassemble(bytes, baseAddress, graph, AnalysisHints{});
}

void dragon::Disassembler::disassemble(std::span<const std::uint8_t> bytes,
                                       std::uint64_t baseAddress, Graph &graph,
                                       const AnalysisHints &hints) const {
  if (!isValid()) {
    impl_->logger.log("disassemble: called on invalid disassembler");
    return;
  }

  // A new analysis invocation may have different entries or memory contracts.
  // New cross-region paths can invalidate an old region's memory proof too.
  // Withdraw analysis-derived edges globally; explicit caller and decoder
  // edges are separate evidence and remain intact. Regions outside this
  // invocation remain unresolved until they are analysed again.
  for (const auto &[address, instruction] : graph.listing())
    graph.get(address)->clearInferredBranches();

  switch (architecture_) {
    case Architecture::X86_32:
    case Architecture::X86_64: {
      x86::X86Analysis x86Analysis{architecture_, *this, configuration_,
                                   *impl_->target.instrInfo,
                                   *impl_->target.regInfo};
      x86Analysis.analyze(bytes, baseAddress, graph, hints);
      break;
    }
    case Architecture::ARM32:
    case Architecture::ARM64: {
      arm::ARMAnalysis armAnalysis{architecture_, *this, configuration_,
                                   *impl_->target.instrInfo,
                                   *impl_->target.regInfo};
      armAnalysis.analyze(bytes, baseAddress, graph, hints);
      break;
    }
    case Architecture::RISCV32:
    case Architecture::RISCV64:
      impl_->logger.log("disassemble: RISC-V analysis not implemented yet");
      break;
    case Architecture::Unknown:
      break;
  }
}

dragon::ignil::Block
dragon::Disassembler::liftTrace(std::span<const Instruction> trace) const {
  if (!isValid() || trace.empty())
    return ignil::Block{0};

  switch (architecture_) {
    case Architecture::X86_32:
    case Architecture::X86_64: {
      x86::X86IGNILLifter lifter{impl_->logger, architecture_,
                                 *impl_->target.instrInfo,
                                 *impl_->target.regInfo};
      return lifter.liftTrace(trace);
    }
    case Architecture::ARM32:
    case Architecture::ARM64: {
      arm::ARMIGNILLifter lifter{impl_->logger, architecture_,
                                 *impl_->target.instrInfo,
                                 *impl_->target.regInfo};
      return lifter.liftTrace(trace);
    }
    case Architecture::RISCV32:
    case Architecture::RISCV64:
    case Architecture::Unknown:
      break;
  }
  return ignil::Block{0};
}

dragon::Instruction
dragon::Disassembler::decodeOne(std::span<const std::uint8_t> bytes,
                                std::uint64_t address,
                                std::size_t &offset) const {
  const auto &target = impl_->target;

  llvm::MCInst inst;
  std::uint64_t instSize{0};

  const auto status = target.disassembler->getInstruction(
          inst, instSize,
          llvm::ArrayRef(bytes.data() + offset, bytes.size() - offset),
          address + offset, llvm::nulls());

  if (status != llvm::MCDisassembler::Success || instSize == 0) {
    impl_->logger.log("decodeOne: failed at 0x{:x}, skipping invalid encoding",
                      address + offset);
    // A64 instructions are fixed 4-byte words. Byte-wise recovery can decode
    // a plausible instruction across an invalid word and the next real one,
    // losing function entries supplied by symbols/FDEs (libcrypto's BTI).
    offset += architecture_ == Architecture::ARM64
                  ? std::min<std::size_t>(4,bytes.size()-offset) : 1;
    return {address + offset,
            0,
            "",
            "",
            {},
            false,
            false,
            false,
            false,
            false,
            false};
  }

  std::string full;
  llvm::raw_string_ostream stream(full);
  target.printer->printInst(&inst, address + offset, "", *target.subtargetInfo,
                            stream);

  // printInst indents with a leading tab; the mnemonic is the first
  // whitespace-delimited token and everything after it is the operand list.
  //
  // Deliberately not using MCInstPrinter::getMnemonic() here: it returns
  // LLVM's raw/canonical opcode name (e.g. "movz", "subs", "b."), which on
  // AArch64 routinely differs from printInst's alias-resolved text (e.g.
  // "mov", "cmp", "b.le") - splitting `full` against a substring that may
  // not even appear in it (or may appear as an unrelated prefix) silently
  // produced empty or garbled operands.
  std::string mnemonic;
  std::string operands;
  if (const std::size_t start = full.find_first_not_of(" \t");
      start != std::string::npos) {
    mnemonic = full.substr(start);
    if (const std::size_t sep = mnemonic.find_first_of(" \t");
        sep != std::string::npos) {
      operands = mnemonic.substr(sep);
      mnemonic.resize(sep);
      operands.erase(0, operands.find_first_not_of(" \t"));
    }
  }

  std::vector rawBytes(bytes.begin() + offset,
                                     bytes.begin() + offset + instSize);

  const auto &desc = target.instrInfo->get(inst.getOpcode());
  bool isJump = desc.isBranch();
  bool isCall = desc.isCall();
  bool isReturn = desc.isReturn();
  bool isConditional = desc.isConditionalBranch();
  bool isTrap = desc.isTrap();
  bool hasIndirect = desc.isIndirectBranch();

  // A32 has one branch opcode, Bcc, for both `b` and `bCC`: an unconditional
  // `b` is just Bcc with the AL (14) predicate, and LLVM flags every Bcc as a
  // conditional branch. Read the predicate operand so `b` is not given a
  // fall-through edge it does not have.
  if (architecture_ == Architecture::ARM32 && isConditional) {
    constexpr std::int64_t kArmCondAL = 14;
    const int predIdx = desc.findFirstPredOperandIdx();
    if (predIdx >= 0 && inst.getOperand(predIdx).isImm() &&
        inst.getOperand(predIdx).getImm() == kArmCondAL)
      isConditional = false;
  }

  offset += instSize;

  Instruction insn(address + offset - instSize,
                   static_cast<std::uint8_t>(instSize), std::move(mnemonic),
                   std::move(operands), std::move(rawBytes), isReturn, isCall,
                   isJump, isConditional, isTrap, hasIndirect);
  // We keep the LLVM object just in case we are going to use it later.
  insn.impl_ = std::make_unique<Instruction::Impl>(Instruction::Impl{inst});

  // Resolve direct branch target from immediate operand.
  //
  // AArch64 encodes B/Bcc/CBZ/CBNZ/TBZ/TBNZ targets as a signed
  // *instruction-count* offset (not bytes) from the branch instruction's own
  // address (not address+size like x86's variable-length encoding), and the
  // target is always the last operand: B=[tgt], Bcc=[cond,tgt],
  // CBZ/CBNZ=[Rt,tgt], TBZ/TBNZ=[Rt,bit,tgt]. Verified against
  // `llvm-mc -disassemble -show-inst` on raw AArch64 encodings.
  //
  // A32 is different again: the immediate is a byte offset (already scaled),
  // always operand 0 of B/Bcc/BL/BLX, and relative to the value an A32
  // instruction reads from PC - its own address + 8, not address + size.
  // `ea000002` at 0 branches to 0x10 (llvm-objdump agrees).
  constexpr std::uint64_t kArmPcBias = 8;
  if (isJump && !hasIndirect && inst.getNumOperands() > 0) {
    if (architecture_ == Architecture::ARM64) {
      const auto &op = inst.getOperand(inst.getNumOperands() - 1);
      if (op.isImm())
        insn.addBranch(insn.address() +
                       static_cast<std::uint64_t>(op.getImm()) * 4);
    } else if (architecture_ == Architecture::ARM32) {
      const auto &op = inst.getOperand(0);
      if (op.isImm())
        insn.addBranch(insn.address() + kArmPcBias +
                       static_cast<std::uint64_t>(op.getImm()));
    } else {
      const auto &op = inst.getOperand(0);
      if (op.isImm()) {
        insn.addBranch(insn.address() + instSize +
                       static_cast<std::uint64_t>(op.getImm()));
      }
    }
  }

  // If it is a call, we need to calculate possible callees.
  // BL follows the same AArch64 instruction-count scaling as branches above.
  if (isCall && !hasIndirect && inst.getNumOperands() > 0) {
    if (architecture_ == Architecture::ARM64) {
      const auto &op = inst.getOperand(0);
      if (op.isImm())
        insn.addCallee(insn.address() +
                       static_cast<std::uint64_t>(op.getImm()) * 4);
    } else if (architecture_ == Architecture::ARM32) {
      const auto &op = inst.getOperand(0);
      if (op.isImm())
        insn.addCallee(insn.address() + kArmPcBias +
                       static_cast<std::uint64_t>(op.getImm()));
    } else {
      const auto &op = inst.getOperand(0);
      if (op.isImm()) {
        insn.addCallee(insn.address() + instSize +
                       static_cast<std::uint64_t>(op.getImm()));
      }
    }
  }

  // Edge count: known targets, or 1 for fall-through on non-terminator
  std::size_t edgeCount = insn.branches().size();
  if (edgeCount == 0 && !isReturn && !isTrap)
    edgeCount = 1;
  insn.setEdges(edgeCount);

  return insn;
}

std::vector<dragon::Instruction>
dragon::Disassembler::disassembleBlock(std::span<const std::uint8_t> bytes,
                                       std::uint64_t baseAddress,
                                       std::uint64_t startAddress) const {
  if (!isValid() || startAddress < baseAddress)
    return {};
  const std::size_t startOffset = startAddress - baseAddress;
  if (startOffset >= bytes.size())
    return {};

  std::vector<Instruction> result;
  std::size_t offset = startOffset;
  while (offset < bytes.size()) {
    auto instr = decodeOne(bytes, baseAddress, offset);
    if (instr.size() == 0)
      break;
    const bool isTerminator = instr.isReturn() || instr.isTrap() || instr.isJump();
    result.push_back(std::move(instr));
    if (isTerminator)
      break;
  }
  impl_->logger.log("disassembleBlock: {} instrs from 0x{:x}", result.size(),
                    startAddress);
  return result;
}

std::vector<dragon::Instruction>
dragon::Disassembler::linearSweep(std::span<const std::uint8_t> bytes,
                                  std::uint64_t baseAddress) const {
  impl_->logger.log("LinearSweep: {} bytes at 0x{:x}", bytes.size(),
                    baseAddress);
  std::vector<Instruction> instructions;
  std::size_t offset{0};
  while (offset < bytes.size()) {
    auto instr = decodeOne(bytes, baseAddress, offset);
    if (instr.size() == 0)
      continue;
    instructions.emplace_back(std::move(instr));
  }
  impl_->logger.log("LinearSweep: decoded {} instructions",
                    instructions.size());
  return instructions;
}

std::vector<dragon::Instruction>
dragon::Disassembler::recursiveTraversal(
        std::span<const std::uint8_t> bytes, std::uint64_t baseAddress,
        std::span<const std::uint64_t> roots) const {
  impl_->logger.log("RecursiveTraversal: {} bytes at 0x{:x}", bytes.size(),
                    baseAddress);
  std::vector<Instruction> instructions;
  std::unordered_set<std::uint64_t> visited;
  std::stack<std::uint64_t> workList;

  for (std::uint64_t r: roots)
    if (r >= baseAddress && r - baseAddress < bytes.size())
      workList.push(r);
  if (workList.empty())
    workList.push(baseAddress);

  while (!workList.empty()) {
    const std::uint64_t address = workList.top();
    workList.pop();

    if (visited.contains(address))
      continue;
    if (address < baseAddress)
      continue;

    const std::size_t offsetVal = address - baseAddress;
    if (offsetVal >= bytes.size())
      continue;

    std::size_t offset = offsetVal;
    while (offset < bytes.size()) {
      const std::uint64_t instrAddress = baseAddress + offset;
      if (visited.contains(instrAddress))
        break;

      auto instr = decodeOne(bytes, baseAddress, offset);
      if (instr.size() == 0)
        break;

      visited.insert(instrAddress);

      // Follow every known target: branches, and the callees of direct
      // calls - a called function is code too, and without this the
      // traversal never reached any of them, leaving every call target a
      // function start with no instructions.
      for (std::uint64_t target: instr.branches())
        workList.push(target);
      for (std::uint64_t callee: instr.callees())
        workList.push(callee);

      // Control does not fall through an unconditional jump (direct or
      // indirect), a return or a trap. Decoding on after a direct `jmp X`
      // used to read whatever follows it - padding, data, the next
      // function - as reachable code.
      const bool isTerminator =
              instr.isReturn() || instr.isTrap() ||
              (instr.isJump() && !instr.isConditional());

      instructions.push_back(std::move(instr));
      if (isTerminator)
        break;
    }
  }

  std::ranges::sort(instructions,
                    [](const Instruction &a, const Instruction &b) {
                      return a.address() < b.address();
                    });

  impl_->logger.log("RecursiveTraversal: decoded {} instructions",
                    instructions.size());
  return instructions;
}
