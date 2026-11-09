#include "LLVMLiftProvenance.hpp"
#include <algorithm>

namespace dragon::llvmlifter::detail {

struct CaptureState {
  bool active = false, sealed = false;
  std::shared_ptr<const RegionInstance> instance;
  const llvm::Function *function = nullptr;
  std::map<std::uint64_t,const llvm::BasicBlock *> blocks;
  std::vector<OperationSite> operations;
  std::vector<OperandSite> operands;
  std::vector<GuestAccessSite> accesses;
  std::vector<EdgeSite> edges;
  std::vector<ExitSite> exits;
  std::vector<CallSite> calls;
};

std::shared_ptr<CaptureState> LiftView::checked() const {
  auto state = state_.lock();
  if (!state || !state->active) throw std::logic_error("expired lowering provenance");
  return state;
}
bool LiftView::active() const { auto state = state_.lock();return state && state->active; }
bool LiftView::matches(const PreparedRegion &region) const { return checked()->instance == region.instance(); }
const llvm::Function &LiftView::function() const { return *checked()->function; }
const std::map<std::uint64_t,const llvm::BasicBlock *> &LiftView::blocks() const { return checked()->blocks; }
std::span<const OperationSite> LiftView::operations() const { return checked()->operations; }
std::span<const OperandSite> LiftView::operands() const { return checked()->operands; }
std::span<const GuestAccessSite> LiftView::accesses() const { return checked()->accesses; }
std::span<const EdgeSite> LiftView::edges() const { return checked()->edges; }
std::span<const ExitSite> LiftView::exits() const { return checked()->exits; }
std::span<const CallSite> LiftView::calls() const { return checked()->calls; }

LiftCapture::LiftCapture(const PreparedRegion &region, const CaptureLimits &limits)
    : region_(region), state_(std::make_shared<CaptureState>()),
      current_{region.instance(),SourceKind::Entry,{},{},{}}, remaining_(limits.maxRecords) {
  state_->instance = region.instance();
  failed_ = !remaining_ || remaining_ > 1048576;
}
LiftCapture::~LiftCapture() { state_->active = false; }
bool LiftCapture::ok() const { return !failed_; }
bool LiftCapture::spend() {
  if (failed_) return false;
  if (!remaining_) { failed_ = true;return false; }
  --remaining_;return true;
}
void LiftCapture::boundary(SourceKind kind, std::optional<std::uint64_t> block,
                           std::optional<std::uint64_t> pc) {
  current_ = {region_.instance(),kind,block,pc,{}};
  operationIndex_.reset();
}
const OpOrigin &LiftCapture::operation() const {
  if (!current_.operation) throw std::logic_error("missing operation provenance");
  return *current_.operation;
}
void LiftCapture::begin(std::uint64_t block, std::size_t index, const llvm::BasicBlock *at) {
  const auto origin = region_.origin(block,index);
  if (!origin) { failed_ = true;return; }
  current_ = {region_.instance(),SourceKind::Operation,block,origin->machinePC,origin};
  accessOrdinal_ = 0;operationIndex_.reset();
  if (spend()) {
    operationIndex_ = state_->operations.size();
    state_->operations.push_back({*origin,at});
  }
}
void LiftCapture::finish(const llvm::BasicBlock *at) {
  if (ok() && operationIndex_) state_->operations[*operationIndex_].end = at;
}
void LiftCapture::operand(unsigned index, const llvm::Value *value) {
  if (spend()) state_->operands.push_back({operation(),index,value});
}
void LiftCapture::result(const llvm::Value *value) {
  if (spend() && operationIndex_) state_->operations[*operationIndex_].result = value;
}
void LiftCapture::access(AccessRole role, const MemoryCheck &check, const llvm::Instruction *access) {
  if (!spend()) return;
  const llvm::Value *value = llvm::isa<llvm::LoadInst>(access) ? access
      : llvm::cast<llvm::StoreInst>(access)->getValueOperand();
  state_->accesses.push_back({operation(),role,accessOrdinal_++,check,access,value});
}
void LiftCapture::edge(EdgeRole role, const llvm::Instruction *terminator) {
  if (spend()) state_->edges.push_back({current_,role,terminator});
}
void LiftCapture::exit(const llvm::ReturnInst *ret, std::optional<ExitValues> fields) {
  if (spend()) state_->exits.push_back({current_,ret,fields});
}
void LiftCapture::call(const llvm::CallInst *call, const llvm::Value *target, std::uint64_t next,
    const ExitValues &request, const llvm::BranchInst *validateExit, const llvm::BranchInst *validateReturn,
    const llvm::BasicBlock *invalid, const llvm::BasicBlock *normal, const llvm::BasicBlock *propagate) {
  if (spend()) state_->calls.push_back({operation(),call,target,next,request,
                                      validateExit,validateReturn,invalid,normal,propagate});
}

bool LiftCapture::seal(const llvm::Function *function,
                      const std::map<std::uint64_t,llvm::BasicBlock *> &blocks) {
  if (!ok()) return false;
  auto inFunction = [&](const llvm::BasicBlock *bb) { return bb && bb->getParent() == function; };
  for (const auto &site : state_->operations)
    if (!region_.owns(site.origin) || !inFunction(site.begin) || !inFunction(site.end)) return false;
  for (const auto &site : state_->accesses) {
    const auto &check = site.check;
    if (!region_.owns(site.origin) || !check.bytes || !check.address || !check.index || !check.memoryBase ||
        !check.branch || !check.branch->isConditional() || check.branch->getSuccessor(0) != check.success ||
        check.branch->getSuccessor(1) != check.failure || !inFunction(check.success) || !inFunction(check.failure) ||
        site.instruction->getParent() != check.success || !site.after()) return false;
  }
  std::set<const llvm::Instruction *> captured;
  for (const auto &edge : state_->edges) {
    if (!edge.terminator || !inFunction(edge.terminator->getParent()) ||
        edge.terminator != edge.terminator->getParent()->getTerminator() ||
        !captured.insert(edge.terminator).second) return false;
  }
  for (const auto &exit : state_->exits) {
    if (!exit.instruction || !inFunction(exit.instruction->getParent()) ||
        !captured.insert(exit.instruction).second) return false;
  }
  // Every emitted branch/default/return must be accounted for, including
  // memory failure, entry refusal, and direct propagation of a callee Exit.
  for (const auto &bb : *function)
    if (!bb.getTerminator() || !captured.contains(bb.getTerminator())) return false;
  state_->function = function;
  state_->blocks.insert(blocks.begin(),blocks.end());
  state_->sealed = true;
  return true;
}
void LiftCapture::consume(const std::function<void(const LiftView &)> &consumer) {
  if (!state_->sealed || state_->active) throw std::logic_error("invalid lowering transaction");
  state_->active = true;
  struct Expire { CaptureState &state;~Expire() { state.active = false;state.sealed = false; } } expire{*state_};
  if (consumer) consumer(LiftView{state_});
}

bool RegionLowering::run(LLVMLifter &lifter, const PreparedRegion &region,
    const std::function<void(const LiftView &)> &consumer, const CaptureLimits &limits) {
  if (lifter.mode_ != ExecutionMode::StaticAnalysis ||
      lifter.staticEntryPolicy_ != StaticEntryPolicy::FunctionStart ||
      lifter.arch_ != region.graph().architecture()) return false;
  LiftCapture capture{region,limits};
  if (!capture.ok() || !lifter.liftFunction(region.root(),region.graph(),region.blocks(),true,&capture)) return false;
  if (region.rebuilt())
    lifter.module_->getFunction(lifter.staticFunctionName(region.root().startAddress()))
        ->addFnAttr("dragon.coverage-source","decoded-machine");
  capture.consume(consumer);
  return true;
}

} // namespace dragon::llvmlifter::detail
