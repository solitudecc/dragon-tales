#pragma once

#include "LLVMPreparedRegion.hpp"
#include <llvm/IR/Instructions.h>
#include <functional>
#include <span>
#include <stdexcept>

namespace dragon::llvmlifter::detail {

enum class SourceKind { Operation, UnsupportedInstruction, StreamEnd, Entry };
struct SourcePoint {
  std::shared_ptr<const RegionInstance> instance;
  SourceKind kind;
  std::optional<std::uint64_t> block, machinePC;
  std::optional<OpOrigin> operation;
};
struct OperationSite {
  OpOrigin origin;
  const llvm::BasicBlock *begin, *end = nullptr;
  const llvm::Value *result = nullptr;
};
struct OperandSite {
  OpOrigin origin;
  unsigned index;
  const llvm::Value *value;
};
enum class AccessRole { Load, Store, CallReturnSlot, ReturnTarget };
struct MemoryCheck {
  const llvm::Value *address = nullptr, *index = nullptr, *memoryBase = nullptr;
  unsigned addressBits = 0;
  std::uint64_t bytes = 0;
  const llvm::BranchInst *branch = nullptr;
  const llvm::BasicBlock *success = nullptr, *failure = nullptr;
};
struct GuestAccessSite {
  OpOrigin origin;
  AccessRole role;
  unsigned ordinal;
  MemoryCheck check;
  const llvm::Instruction *instruction;
  const llvm::Value *value;
  /// A point after the access, not merely the successful bounds-check edge.
  const llvm::Instruction *after() const { return instruction->getNextNode(); }
};
enum class EdgeRole {
  Jump, Conditional, LocalJump, LocalConditional, LabelFallthrough,
  MemoryBounds, Entry, CallExitValidation, CallReturnValidation, CallContinuation
};
struct EdgeSite {
  SourcePoint source;
  EdgeRole role;
  /// The actual terminator and its successor index identify each edge. Switch
  /// cases/default come from this instruction, never original Graph successors.
  const llvm::Instruction *terminator;
};
struct ExitValues {
  std::uint64_t reason, kind, detail, returnPC;
  const llvm::Value *sourcePC, *nextPC;
  bool nextPCValid;
};
struct ExitSite {
  SourcePoint source;
  const llvm::ReturnInst *instruction;
  /// Empty only when propagating the callee's existing state/Exit unchanged.
  std::optional<ExitValues> fields;
};
struct CallSite {
  OpOrigin origin;
  const llvm::CallInst *instruction;
  const llvm::Value *target;
  std::uint64_t returnPC;
  ExitValues request; ///< Call request writes are not a terminal exit or effect.
  const llvm::BranchInst *validateExit, *validateReturn;
  const llvm::BasicBlock *invalid, *normal, *propagate;
};
struct CaptureLimits {
  std::uint32_t maxRecords = 1048576; ///< May only lower this hard cap.
};

struct CaptureState;
/// A synchronous borrow. Copying the view does not extend the transaction;
/// queries after callback return/throw fail before touching any LLVM pointer.
/// References obtained inside the callback must not escape it. Consumers are
/// trusted read-only code; they must not optimize/mutate/close the target here.
class LiftView {
 public:
  bool active() const;
  bool matches(const PreparedRegion &) const;
  const llvm::Function &function() const;
  const std::map<std::uint64_t,const llvm::BasicBlock *> &blocks() const;
  std::span<const OperationSite> operations() const;
  std::span<const OperandSite> operands() const;
  std::span<const GuestAccessSite> accesses() const;
  std::span<const EdgeSite> edges() const;
  std::span<const ExitSite> exits() const;
  std::span<const CallSite> calls() const;
 private:
  friend class LiftCapture;
  explicit LiftView(const std::shared_ptr<CaptureState> &state) : state_(state) {}
  std::shared_ptr<CaptureState> checked() const;
  std::weak_ptr<CaptureState> state_;
};

class LiftCapture {
 public:
  explicit LiftCapture(const PreparedRegion &, const CaptureLimits &);
  ~LiftCapture();
  LiftCapture(const LiftCapture &) = delete;
  LiftCapture &operator=(const LiftCapture &) = delete;
  bool ok() const;
  void boundary(SourceKind, std::optional<std::uint64_t> block = {},
                std::optional<std::uint64_t> pc = {});
  void begin(std::uint64_t block, std::size_t index, const llvm::BasicBlock *);
  void finish(const llvm::BasicBlock *);
  void operand(unsigned, const llvm::Value *);
  void result(const llvm::Value *);
  void access(AccessRole, const MemoryCheck &, const llvm::Instruction *);
  void edge(EdgeRole, const llvm::Instruction *);
  void exit(const llvm::ReturnInst *, std::optional<ExitValues>);
  void call(const llvm::CallInst *, const llvm::Value *, std::uint64_t,
            const ExitValues &, const llvm::BranchInst *, const llvm::BranchInst *,
            const llvm::BasicBlock *, const llvm::BasicBlock *, const llvm::BasicBlock *);
  /// Structural checks on this trusted producer's output, not validation of
  /// arbitrary external certificates or a proof of memory/value equivalence.
  bool seal(const llvm::Function *, const std::map<std::uint64_t,llvm::BasicBlock *> &);
  void consume(const std::function<void(const LiftView &)> &);
 private:
  bool spend();
  const OpOrigin &operation() const;
  const PreparedRegion &region_;
  std::shared_ptr<CaptureState> state_;
  SourcePoint current_;
  std::optional<std::size_t> operationIndex_;
  std::uint32_t remaining_;
  unsigned accessOrdinal_ = 0;
  bool failed_ = false;
};

/// Private preparation/lowering/consumption transaction, before any LLVM pass.
/// Consumer exceptions expire the borrow and propagate. The completed module
/// is not rolled back; as with pipeline errors, discard that failed target.
struct RegionLowering {
  static bool run(LLVMLifter &, const PreparedRegion &,
                 const std::function<void(const LiftView &)> & = {},
                 const CaptureLimits & = {});
};

} // namespace dragon::llvmlifter::detail
