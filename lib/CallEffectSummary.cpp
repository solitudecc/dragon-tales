#include <dragon/lifters/llvm/CallEffectSummary.hpp>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

#include <limits>

namespace dragon::llvmlifter {
namespace {

constexpr char MetadataName[] = "dragon.call.effect";

bool validRange(const CallMemoryRange &range) {
  return range.bytes != 0 && range.bytes <= 1u << 20;
}

bool validValue(const CallValueEffect &value) {
  return value.widthBits <= 128 && (!value.hasConstant || value.widthBits != 0);
}

bool validSummary(const CallEffectSummary &summary) {
  if (summary.version != 1 || !summary.proven)
    return false;
  if (summary.boundary == CallBoundaryKind::SystemCall &&
      !summary.hasSyscallNumber)
    return false;
  if (summary.memory == CallMemoryEffectKind::Unknown)
    return false;
  if ((summary.memory == CallMemoryEffectKind::NoMemory ||
       summary.memory == CallMemoryEffectKind::ReadOnly) &&
      !summary.writes.empty())
    return false;
  // KnownRanges is a complete write footprint, not a promise that an
  // unlisted write range is harmless. An empty footprint must use NoMemory or
  // ReadOnly explicitly; otherwise the consumer would silently under-invalidate.
  if (summary.memory == CallMemoryEffectKind::KnownRanges &&
      summary.writes.empty())
    return false;
  for (const auto &range : summary.reads)
    if (!validRange(range)) return false;
  for (const auto &range : summary.writes)
    if (!validRange(range)) return false;
  for (const auto &value : summary.values)
    if (!validValue(value)) return false;
  return summary.reads.size() <= 256 && summary.writes.size() <= 256 &&
         summary.values.size() <= 64;
}

llvm::ConstantAsMetadata *integerMetadata(llvm::LLVMContext &context,
                                          std::uint64_t value) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), value));
}

llvm::MDNode *rangeNode(llvm::LLVMContext &context,
                        const CallMemoryRange &range) {
  return llvm::MDNode::get(context,
                           {integerMetadata(context,
                                             static_cast<std::uint64_t>(range.base)),
                            integerMetadata(context, range.offset),
                            integerMetadata(context, range.bytes)});
}

llvm::MDNode *valueNode(llvm::LLVMContext &context,
                        const CallValueEffect &value) {
  return llvm::MDNode::get(
      context,
      {integerMetadata(context, static_cast<std::uint64_t>(value.location)),
       integerMetadata(context, value.index), integerMetadata(context, value.widthBits),
       integerMetadata(context, value.hasConstant ? 1 : 0),
       integerMetadata(context, value.constant)});
}

std::optional<std::uint64_t> integer(const llvm::MDOperand &operand) {
  const auto *constant = llvm::mdconst::dyn_extract<llvm::ConstantInt>(operand);
  if (!constant || constant->getBitWidth() > 64) return std::nullopt;
  return constant->getZExtValue();
}

std::optional<CallMemoryRange> parseRange(const llvm::MDOperand &operand) {
  const auto *node = llvm::dyn_cast<llvm::MDNode>(operand);
  if (!node || node->getNumOperands() != 3) return std::nullopt;
  const auto base = integer(node->getOperand(0));
  const auto offset = integer(node->getOperand(1));
  const auto bytes = integer(node->getOperand(2));
  if (!base || !offset || !bytes || *base > 2 || *bytes > UINT32_MAX)
    return std::nullopt;
  CallMemoryRange range{static_cast<CallMemoryRange::Base>(*base), *offset,
                        static_cast<std::uint32_t>(*bytes)};
  return validRange(range) ? std::optional<CallMemoryRange>(range) : std::nullopt;
}

std::optional<CallValueEffect> parseValue(const llvm::MDOperand &operand) {
  const auto *node = llvm::dyn_cast<llvm::MDNode>(operand);
  if (!node || node->getNumOperands() != 5) return std::nullopt;
  const auto location = integer(node->getOperand(0));
  const auto index = integer(node->getOperand(1));
  const auto width = integer(node->getOperand(2));
  const auto hasConstant = integer(node->getOperand(3));
  const auto constant = integer(node->getOperand(4));
  if (!location || !index || !width || !hasConstant || !constant || *location > 1 ||
      *width > 128 || *hasConstant > 1)
    return std::nullopt;
  CallValueEffect value{static_cast<CallValueEffect::Location>(*location),
                        static_cast<std::uint32_t>(*index),
                        static_cast<std::uint32_t>(*width), *hasConstant != 0,
                        *constant};
  return validValue(value) ? std::optional<CallValueEffect>(value) : std::nullopt;
}

} // namespace

bool attachCallEffectSummary(llvm::CallBase &call,
                             const CallEffectSummary &summary) {
  if (!validSummary(summary)) return false;
  auto &context = call.getContext();
  llvm::SmallVector<llvm::Metadata *, 7> reads;
  llvm::SmallVector<llvm::Metadata *, 7> writes;
  llvm::SmallVector<llvm::Metadata *, 7> values;
  for (const auto &range : summary.reads) reads.push_back(rangeNode(context, range));
  for (const auto &range : summary.writes) writes.push_back(rangeNode(context, range));
  for (const auto &value : summary.values) values.push_back(valueNode(context, value));
  auto *node = llvm::MDNode::get(
      context,
      {llvm::MDString::get(context, "v1"),
       integerMetadata(context, static_cast<std::uint64_t>(summary.boundary)),
       integerMetadata(context, summary.hasSyscallNumber ? 1 : 0),
       integerMetadata(context, summary.syscallNumber),
       integerMetadata(context, summary.proven ? 1 : 0),
       integerMetadata(context, static_cast<std::uint64_t>(summary.memory)),
       llvm::MDNode::get(context, reads), llvm::MDNode::get(context, writes),
       llvm::MDNode::get(context, values)});
  call.setMetadata(MetadataName, node);
  return true;
}

std::optional<CallEffectSummary>
getCallEffectSummary(const llvm::CallBase &call) {
  const auto *node = call.getMetadata(MetadataName);
  if (!node || node->getNumOperands() != 9) return std::nullopt;
  const auto *version = llvm::dyn_cast<llvm::MDString>(node->getOperand(0));
  if (!version || version->getString() != "v1") return std::nullopt;
  const auto boundary = integer(node->getOperand(1));
  const auto hasSyscall = integer(node->getOperand(2));
  const auto syscall = integer(node->getOperand(3));
  const auto proven = integer(node->getOperand(4));
  const auto memory = integer(node->getOperand(5));
  const auto *reads = llvm::dyn_cast<llvm::MDNode>(node->getOperand(6));
  const auto *writes = llvm::dyn_cast<llvm::MDNode>(node->getOperand(7));
  const auto *values = llvm::dyn_cast<llvm::MDNode>(node->getOperand(8));
  if (!boundary.has_value() || !hasSyscall.has_value() ||
      !proven.has_value() || !memory.has_value() || *boundary > 1 ||
      *hasSyscall > 1 || *proven > 1 || *memory > 3 || !reads || !writes ||
      !values)
    return std::nullopt;
  CallEffectSummary summary;
  summary.boundary = static_cast<CallBoundaryKind>(*boundary);
  summary.hasSyscallNumber = *hasSyscall != 0;
  summary.syscallNumber = *syscall;
  summary.proven = *proven != 0;
  summary.memory = static_cast<CallMemoryEffectKind>(*memory);
  if (reads->getNumOperands() > 256 || writes->getNumOperands() > 256 ||
      values->getNumOperands() > 64)
    return std::nullopt;
  for (const auto &operand : reads->operands()) {
    auto range = parseRange(operand);
    if (!range) return std::nullopt;
    summary.reads.push_back(*range);
  }
  for (const auto &operand : writes->operands()) {
    auto range = parseRange(operand);
    if (!range) return std::nullopt;
    summary.writes.push_back(*range);
  }
  for (const auto &operand : values->operands()) {
    auto value = parseValue(operand);
    if (!value) return std::nullopt;
    summary.values.push_back(*value);
  }
  return validSummary(summary) ? std::optional<CallEffectSummary>(summary)
                               : std::nullopt;
}

} // namespace dragon::llvmlifter
