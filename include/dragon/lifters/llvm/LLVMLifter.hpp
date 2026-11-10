#pragma once

#include <dragon/Architecture.hpp>
#include <dragon/lifters/llvm/Execution.hpp>
#include <dragon/disassembler/Function.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <dragon/lifters/llvm/LLVMDispatchSpecialization.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string_view>

namespace dragon::llvmlifter {

namespace detail {
class LiftCapture;
struct RegionLowering;
struct MemoryCheck;
struct ExitValues;
}

enum class ExecutionMode { Legacy = 0, StatefulV2 = 1, StaticAnalysis = 2 };

/// External entry contract for static functions. Internal branches, loops and
/// call continuations are unaffected. Neither policy assumes input memory.
enum class StaticEntryPolicy { Resumable = 0, FunctionStart = 1 };

struct PredicateFoldOptions {
  std::uint32_t timeoutMs = 25;          ///< Per solver query; zero disables queries.
  std::uint32_t maxQueries = 64;         ///< Shared by the whole module.
  std::uint32_t maxExpressionNodes = 256; ///< Per condition, also bounds total work.
  std::uint32_t resourceLimit = 200000; ///< Z3 resource units per candidate solver.
};

struct PredicateFoldStats {
  bool available = false; ///< False in a build without Z3; IR is then unchanged.
  std::size_t conditionsVisited = 0;
  std::size_t conditionsFolded = 0;
  std::size_t solverQueries = 0;
  std::size_t unknownQueries = 0;
  std::size_t unsupportedConditions = 0;
  std::size_t budgetSkipped = 0;
};

struct MBASimplifyOptions {
  std::uint32_t timeoutMs = 100;
  std::uint32_t maxQueries = 16; ///< Shared by the module; zero disables rewriting.
  std::uint32_t maxExpressionNodes = 128; ///< Clamped to 1024; depth is at most 64.
  std::uint32_t maxVariables = 3; ///< Clamped to 3 independent SSA inputs.
  std::uint32_t resourceLimit = 1000000;
};

struct MBASimplifyStats {
  bool available = false;
  std::size_t expressionsVisited = 0;
  std::size_t candidates = 0;
  std::size_t expressionsSimplified = 0;
  std::size_t solverQueries = 0;
  std::size_t unknownQueries = 0;
  std::size_t rejectedCandidates = 0;
  std::size_t budgetSkipped = 0;
};

struct StaticRegionOptions {
  std::uint32_t maxBlocks = 4096; ///< May only lower the hard 4096-block cap.
  std::uint32_t maxDispatchCases = 65536; ///< Total indirect dispatch cases; hard cap.
  /// Opt in to rebuilding machine IGNIL in a private Graph copy. Additional
  /// addresses may then be decoded instruction starts without existing blocks.
  /// Existing custom IGNIL rewrites are not the source of this machine view.
  bool rebuildDecodedBlocks = false;
};

class LLVMLifter {
  public:
  explicit LLVMLifter(Architecture arch,
                      std::string_view moduleName = "lifted");
  ~LLVMLifter();

  LLVMLifter(const LLVMLifter &) = delete;
  LLVMLifter &operator=(const LLVMLifter &) = delete;
  LLVMLifter(LLVMLifter &&) noexcept;
  LLVMLifter &operator=(LLVMLifter &&) noexcept = default;

  /// Select before lifting; a module cannot mix execution protocols.
  bool setExecutionMode(ExecutionMode mode);

  /// Opt in before lifting in StaticAnalysis mode. FunctionStart emits separate
  /// dragon_static_entry_* symbols accepting only fn.startAddress(); other PCs
  /// return Unsupported/Entry with registers and memory preserved. The default
  /// Resumable contract and StatefulV2 remain unchanged.
  bool setStaticEntryPolicy(StaticEntryPolicy policy);

  /// Lift one function (all its IGNIL blocks must be in the graph). Check the
  /// return value before treating the function as lifted. Legacy returns false
  /// if any function-member block has an unlifted marker, including a marker
  /// whose instruction emits no IGNIL ops; this rejection leaves the module
  /// unchanged. Legacy also rejects transfers it cannot report. StatefulV2 and
  /// StaticAnalysis retain explicit Unsupported exits at unlifted instructions.
  /// V2 target implementations require valid caller-owned Execution.h objects.
  /// StaticAnalysis uses the same state layout and opaque external declarations;
  /// it requires no runtime or external bodies for IR analysis/optimization.
  bool lift(const Function &fn, const Graph &graph);

  /// Explicitly extend execution coverage with already decoded machine blocks.
  /// Requires StaticAnalysis + FunctionStart. Original function members remain
  /// included; additionalBlockStarts must be exact valid machine block starts
  /// with IGNIL (or opt in to rebuilding from decoded instructions). Runtime
  /// indirect JMP targets dispatch into this set; anything
  /// outside it retains the ordinary Transfer exit. No table bytes, Graph edges
  /// or function membership are assumed or modified. CALL and RET keep their
  /// existing protocols. This is extra coverage, not a completeness proof or
  /// de-flattening. Invalid selections/budgets fail before module mutation.
  bool liftStaticRegion(const Function &fn, const Graph &graph,
      std::span<const std::uint64_t> additionalBlockStarts,
      const StaticRegionOptions &options = {});

  /// Lifts a single, already-flattened IGNIL block directly to LLVM IR -
  /// no Graph/Function needed. This is the Graph-free counterpart to
  /// Disassembler::liftTrace(): a linear, already-resolved execution trace
  /// has no unresolved branch left to encode (the block's op order *is*
  /// the path that was actually taken), so JMP/CJMP ops - which liftTrace
  /// still emits per-instruction, same as normal lifting - are dropped
  /// rather than translated. Graph-based Legacy rejects transfers it cannot
  /// represent; V2/StaticAnalysis use explicit exits for unknown boundaries.
  /// Reachable CALL is rejected: the Legacy callee protocol has no register state. RET, if present,
  /// must be the block's true terminator - anything after it is ignored,
  /// matching how a real `ret` would end execution. If the block has no
  /// RET, a synthetic `ret void` (with the usual register write-back) is
  /// appended so the function is always well-formed.
  ///
  /// Intra-instruction LABEL/LOCAL* branches remain runtime control flow,
  /// including REP loops, even in an otherwise linear machine trace.
  /// Requires a complete block: unlifted instruction markers cause refusal.
  /// Returns false for an empty block or malformed local control flow.
  bool liftBlock(const ignil::Block &block, std::string_view name = "trace");

  // Run optimization passes in the recommended order.
  void runMem2Reg(); // promotes allocas → SSA, inserts PHIs
  void runInstCombine(); // folds sub-register mask/OR patterns
  void runGVN(); // eliminates redundant loads
  void runDCE(); // removes dead flag computations

  /// Simplify known-state dispatch and predecessor-dependent branches using
  /// LLVM SCCP, DFA jump threading, jump threading and CFG cleanup. Run after
  /// mem2reg and expression/predicate simplification. Unknown entries, memory
  /// and call effects keep their normal LLVM semantics; no image or runtime
  /// state assumptions are introduced. This optional step is not a guarantee
  /// of complete de-flattening and does not change the default sweep.
  void runControlFlowSimplification();

  /// Prove integer branch/select conditions constant for all input values,
  /// then simplify the CFG and dead computations. Run after mem2reg and the
  /// ordinary integer passes. Loads/PHIs are independent unknown SSA inputs;
  /// no memory contents, callee effects or predecessor guards are assumed.
  /// Unsupported expressions, solver unknowns and exhausted budgets stay intact.
  /// Scalar widths up to 128 bits and DAG depth up to 64 are supported.
  [[nodiscard]] PredicateFoldStats runPredicateFolding(
      const PredicateFoldOptions &options = {});

  /// Synthesize smaller linear MBA expressions over integer SSA values.
  /// Every rewrite requires an UNSAT word-level equivalence query. No path,
  /// memory-initialization or call-effect assumptions are made. Without Z3
  /// this is an unavailable no-op. Run after mem2reg/InstCombine/GVN.
  [[nodiscard]] MBASimplifyStats runMBASimplification(
      const MBASimplifyOptions &options = {});

  /// Promote register allocas and perform conservative Guest-memory forwarding.
  /// Acyclic CFG edges carry exact-width facts; joins retain common facts and
  /// create PHIs for differing values. Loop facts are carried only after an
  /// exact entry/backedge check; canonical two-block loops may create a
  /// loop-carried PHI, while calls, unknown aliases and partial overlaps remain
  /// barriers otherwise. Little-endian constant ranges may be rebuilt byte-wise.
  /// The default optimization pipeline is unchanged; callers opt into this stage.
  [[nodiscard]] MemoryForwardStats runGuestMemorySSA(
      const MemoryForwardOptions &options = {});

  /// Rewrite only uniquely constant branch/switch conditions in the current
  /// LLVM module. Finite but differing dispatcher targets remain explicit and
  /// are reported as unresolved; no entry or memory assumptions are added.
  [[nodiscard]] DispatcherSpecializationStats runDispatcherSpecialization(
      const DispatcherSpecializationOptions &options = {});

  [[nodiscard]] std::string ir() const; // dump textual LLVM IR
  [[nodiscard]] llvm::Module &module();

  /// Recover `fn`'s prototype (arguments + return) for this lifter's
  /// architecture and render it as a C-like string, e.g.
  /// `i32 sub_1000(i32 RDI, i32 RSI)`. This is the same recovery lift()
  /// records on each lifted function as a "dragon.signature" attribute;
  /// exposed here so a caller can get the prototype without parsing the IR.
  /// See dragon::analysis::recoverSignature for the method and its limits.
  [[nodiscard]] std::string recoveredSignatureString(const Function &fn,
                                                     const Graph &graph) const;

  private:
  friend struct detail::RegionLowering;
  // All allocas for a lifted function.
  // In this way we keep the registers that have been
  // used, and we only generate those that are used by
  // the function.
  // map<register offset, map<bit width, AllocaInst>>
  using RegFile =
          std::map<std::uint32_t, std::map<ignil::Width, llvm::AllocaInst *>>;

  // SSA values produced by Temp operands within the current block.
  using TempMap = std::map<std::uint32_t, llvm::Value *>;

  bool liftFunction(const Function &fn, const Graph &graph,
      const std::map<std::uint64_t, BasicBlock> &blocks, bool staticRegion,
      detail::LiftCapture *capture = nullptr);

  // First pass: scan every IGNIL op in every block of the function,
  // collect all (offset, width) pairs, emit one alloca per pair
  // in the entry block.
  RegFile buildRegFile(llvm::Function *llfn, const std::map<std::uint64_t, BasicBlock> &blocks,
                       const Graph &graph) const;

  // Shared by buildRegFile() and liftBlock(): scan `ops`, recording every
  // (offset, width) Reg pair referenced (as dst or src) into `regFile`
  // (value left null - allocateRegFile fills it in) and any name seen.
  // Not static: on x86-64 a 32-bit destination also needs its 64-bit parent
  // slot, which depends on the architecture being lifted.
  void collectRegsFromOps(
          const std::vector<ignil::Op> &ops, RegFile &regFile,
          std::map<std::pair<std::uint32_t, ignil::Width>, std::string_view>
                  &regNames) const;

  // Shared by buildRegFile() and liftBlock(): given a regFile whose
  // (offset, width) keys are already collected, create one alloca per pair
  // at the entry block's insertion point, named from `regNames` where
  // known.
  void allocateRegFile(
          llvm::Function *llfn, RegFile &regFile,
          const std::map<std::pair<std::uint32_t, ignil::Width>,
                         std::string_view> &regNames) const;

  // Shared by liftOp()'s RET case and liftBlock()'s synthetic-return
  // fallback: write the widest register at each offset back to `regsPtr`
  // and terminate the current block with `ret void`.
  void emitReturn(RegFile &regs, llvm::Value *regsPtr,
                  llvm::IRBuilder<> &builder);

  // Map each IGNIL block address to a llvm::BasicBlock.
  // Returns nullopt if any block in fn is missing an IGNIL translation,
  // since the function cannot be correctly lifted with a gap.
  std::optional<std::map<std::uint64_t, llvm::BasicBlock *>>
  buildBlockMap(llvm::Function *llfn, const std::map<std::uint64_t, BasicBlock> &blocks,
                const Graph &graph) const;

  // Translate one IGNIL block into its llvm::BasicBlock.
  bool liftBlock(const ignil::Block *ib, llvm::Function *llfn, RegFile *regs,
                 TempMap *temps, llvm::Value *memPtr, llvm::Value *regsPtr,
                 const std::map<std::uint64_t, llvm::BasicBlock *> &bbMap,
                 detail::LiftCapture *capture = nullptr);

  // Translate one IGNIL Op; records the result in temps if the op has a
  // Temp destination
  void liftOp(const ignil::Op &op, RegFile &regs, TempMap &temps,
              llvm::Value *memPtr, llvm::Value *regsPtr,
              llvm::IRBuilder<> &builder,
              const std::map<std::uint64_t, llvm::BasicBlock *> &bbMap,
              detail::LiftCapture *capture = nullptr);

  bool liftOps(std::span<const ignil::Op> ops, RegFile &regs, TempMap &temps,
               llvm::Value *memPtr, llvm::Value *regsPtr, llvm::IRBuilder<> &builder,
               const std::map<std::uint64_t,llvm::BasicBlock *> &bbMap, bool trace,
               detail::LiftCapture *capture = nullptr, std::uint64_t blockAddress = 0);

  // Resolve an ignil::Value to a llvm::Value (load from alloca or
  // look up the temp SSA value).
  llvm::Value *resolve(const ignil::Value &v, RegFile &regs,
                       const TempMap &temps, llvm::IRBuilder<> &builder) const;

  // After writing `value` (of `width` bits) to `offset`, propagate
  // into every other alloca that exists at the same offset: merged into
  // the wider ones, truncated into the narrower ones.
  void mergeIntoParent(std::uint32_t offset, ignil::Width width,
                       llvm::Value *value, RegFile &regs,
                       llvm::IRBuilder<> &builder);

  // After writing `value` to (offset, width), update every slot at a
  // *different* offset that shares bytes with it (AH vs RAX/EAX/AX).
  void syncOverlapping(std::uint32_t offset, ignil::Width width,
                       llvm::Value *value, RegFile &regs,
                       llvm::IRBuilder<> &builder);

  // True when a 32-bit write at `offset` zero-extends into a 64-bit parent:
  // the general-purpose registers of x86-64 (EAX -> RAX) and AArch64
  // (W0 -> X0).
  [[nodiscard]] bool zeroExtendsToGpr(std::uint32_t offset) const;

  // Prologue: load each register's initial value from %regs and store into
  // its alloca. Sub-register allocas are initialized by truncating the widest
  // value at the same offset.
  void initializeRegistersFromParam(llvm::Function *llfn, RegFile &regFile,
                                    llvm::Value *regsPtr);
  void reloadRegisters(RegFile &regs, llvm::Value *regsPtr, llvm::IRBuilder<> &builder);
  detail::ExitValues writeExitAtValue(std::uint64_t reason, std::uint64_t kind, llvm::Value *sourcePC,
                llvm::Value *target, RegFile &regs, llvm::IRBuilder<> &builder,
                std::uint64_t detail, std::uint64_t returnPC);
  void emitStaticCall(const ignil::Op &op, llvm::Value *target, std::uint64_t next,
                RegFile &regs, llvm::Value *regsPtr, llvm::IRBuilder<> &builder,
                const std::map<std::uint64_t,llvm::BasicBlock *> &bbMap,
                detail::LiftCapture *capture = nullptr);

  void emitExit(std::uint64_t reason, std::uint64_t kind, std::uint64_t sourcePC,
                llvm::Value *target, RegFile &regs, llvm::Value *regsPtr,
                llvm::IRBuilder<> &builder, std::uint64_t detail = 0, std::uint64_t returnPC = 0,
                detail::LiftCapture *capture = nullptr);
  void emitExitAtValue(std::uint64_t reason, std::uint64_t kind, llvm::Value *sourcePC,
                llvm::Value *target, RegFile &regs, llvm::Value *regsPtr,
                llvm::IRBuilder<> &builder, std::uint64_t detail, std::uint64_t returnPC,
                detail::LiftCapture *capture = nullptr);
  void writeBackRegisters(RegFile &regs, llvm::Value *regsPtr, llvm::IRBuilder<> &builder);
  void emitBudgetCheck(std::uint64_t pc, RegFile &regs, llvm::Value *regsPtr, llvm::IRBuilder<> &builder);
  llvm::Value *memoryAddress(llvm::Value *address, std::uint64_t bytes, std::uint64_t pc,
                             RegFile &regs, llvm::Value *regsPtr, llvm::Value *memPtr, llvm::IRBuilder<> &builder,
                             detail::MemoryCheck *check = nullptr, detail::LiftCapture *capture = nullptr);
  bool stateful() const { return mode_ != ExecutionMode::Legacy && graph_; }
  [[nodiscard]] std::string staticFunctionName(std::uint64_t address) const;
  ExecutionMode mode_ = ExecutionMode::Legacy;
  StaticEntryPolicy staticEntryPolicy_ = StaticEntryPolicy::Resumable;
  std::set<std::uint64_t> unsupportedAddresses_;
  std::map<std::uint64_t,llvm::BasicBlock *> resumeEntries_;
  Architecture arch_;
  // The graph being lifted by lift(), null otherwise (liftBlock).
  const Graph *graph_ = nullptr;
  std::unique_ptr<llvm::LLVMContext> ctx_;
  std::unique_ptr<llvm::Module> module_;
};

} // namespace dragon::llvmlifter
