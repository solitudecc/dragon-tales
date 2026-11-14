// Standalone, pinned manual-O2 precision probe. No target execution, lifting,
// ELF-permission assumption, public API registration or expected-value rewrite.
#include "LLVMImageTableContext.hpp"
#include "LLVMGuestCellSSA.hpp"
#include "LLVMDispatchDecoderSSA.hpp"
#include "LLVMDispatchCorridor.hpp"
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/loader/Loader.hpp>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Transforms/Scalar/CorrelatedValuePropagation.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <sys/resource.h>
#include <tuple>
#include <vector>

using namespace llvm;
using namespace dragon::llvmlifter;
namespace {
constexpr const char *RawSHA = "0619b5e6d0d22b90a4ef4351929cfa9f1b61b3590c2bd81cfa32118dba7c2eed";
constexpr const char *ELFSHA = "f1065b49db0fcc984a56aece7258e7976b2b6257852f348fad191f1e8b8840fe";
constexpr const char *Root = "dragon_static_entry_x86_64_1010";
constexpr const char *Wrapper = "dragon_static_entry_x86_64_1000";
constexpr const char *View = "dragon_static_entry_x86_64_1010.guarded.image_table_manual_o2_v1";
constexpr std::uint64_t KernelSP = 0xc7f8;
constexpr std::uint64_t TableBase = 0x2000;
const std::vector<std::uint64_t> Targets{0x12ba, 0x137c, 0x12cf, 0x1338, 0x11c8, 0x1392};
void require(bool value, const Twine &message) {
  if (!value) throw std::runtime_error(message.str());
}
std::string sha(StringRef bytes) {
  const auto value = SHA256::hash(ArrayRef<std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (auto byte : value) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
std::string text(Module &module) {
  std::string result; raw_string_ostream stream(result); module.print(stream, nullptr); return result;
}
std::string text(Function &function) {
  std::string result; raw_string_ostream stream(result); function.print(stream); return result;
}
void save(const std::filesystem::path &path, StringRef bytes) {
  require(!std::filesystem::exists(path), "output already exists: " + path.string());
  std::error_code error; raw_fd_ostream stream(path.string(), error);
  require(!error, "cannot open output: " + path.string());
  stream << bytes; stream.close(); require(!stream.has_error(), "cannot write output: " + path.string());
}
void verify(Module &module) {
  std::string error; raw_string_ostream stream(error);
  require(!verifyModule(module, &stream), "invalid probe module: " + stream.str());
}
struct OrderedPrefixGate {
  std::filesystem::path directory, manifest;
  std::string manifestSHA;
  std::map<std::string, std::pair<std::string, std::string>> expected;
  std::set<std::pair<std::string, bool>> passed;
  json::Array records;
  static OrderedPrefixGate load() {
    auto environment = [](const char *name) {
      auto *value = std::getenv(name);
      require(value && *value, Twine("ordered prefix gate requires ") + name);
      return std::string(value);
    };
    OrderedPrefixGate gate;
    gate.directory = environment("DRAGON_ORDERED_BASELINE_DIR");
    gate.manifest = environment("DRAGON_ORDERED_BASELINE_MANIFEST");
    gate.manifestSHA = environment("DRAGON_ORDERED_BASELINE_MANIFEST_SHA256");
    require(gate.directory.is_absolute() && gate.manifest.is_absolute() &&
            gate.manifest.parent_path().lexically_normal() == gate.directory.lexically_normal() &&
            std::filesystem::is_directory(gate.directory), "ordered baseline paths must be explicit absolute siblings");
    auto bytes = MemoryBuffer::getFile(gate.manifest.string());
    require(bool(bytes) && sha((*bytes)->getBuffer()) == gate.manifestSHA,
            "ordered baseline manifest SHA mismatch");
    auto parsed = json::parse((*bytes)->getBuffer());
    require(bool(parsed), "cannot parse pinned ordered baseline manifest");
    auto *object = parsed->getAsObject();
    require(object && object->getString("raw_sha256").value_or("") == RawSHA &&
            object->getString("elf_sha256").value_or("") == ELFSHA &&
            object->getBoolean("standard_o2_recipe").value_or(false) &&
            !object->getBoolean("hard_structure_gate_passed").value_or(true),
            "ordered baseline is not the frozen failed O2 recipe");
    auto *stages = object->getArray("stages");
    require(stages && stages->size() == 17, "ordered baseline must retain all seventeen failed stages");
    const std::set<std::string> prefixes{"00-guarded-raw", "01-checked-context", "01b-cell-ssa",
      "01c-instcombine-gvn", "01a-predicates", "01d-decoder-ssa", "01e-decoder-instcombine-gvn",
      "01f-decoder-predicates", "01g-standard-o2"};
    for (auto &stage : *stages) {
      auto *entry = stage.getAsObject(); require(entry, "invalid baseline stage");
      auto name = entry->getString("stage").value_or("").str();
      if (!prefixes.contains(name)) continue;
      auto canonical = entry->getString("sha256").value_or("").str();
      auto bound = entry->getString("bound_sha256").value_or("").str();
      require(canonical.size() == 64 && bound.size() == 64 &&
              gate.expected.emplace(name, std::pair(canonical, bound)).second,
              "invalid or duplicate prefix hash record");
    }
    require(gate.expected.size() == 9, "ordered baseline prefix is incomplete");
    return gate;
  }
  void check(const std::filesystem::path &output, StringRef name,
             StringRef actual, bool bound) {
    auto expectedStage = expected.find(name.str());
    if (expectedStage == expected.end()) return;
    const auto suffix = bound ? ".bound-wrapper.ll" : ".ll";
    auto file = directory / (name.str() + suffix);
    auto baseline = MemoryBuffer::getFile(file.string());
    const auto hash = bound ? expectedStage->second.second : expectedStage->second.first;
    const bool pinned = bool(baseline) && sha((*baseline)->getBuffer()) == hash;
    const bool equal = pinned && actual == (*baseline)->getBuffer();
    json::Value record(json::Object{{"stage", name.str()}, {"bound_wrapper", bound},
      {"baseline_file", file.string()}, {"expected_sha256", hash}, {"actual_sha256", sha(actual)},
      {"baseline_hash_verified", pinned}, {"exact_bytes_equal", equal}});
    save(output / (name.str() + (bound ? ".bound-bytegate.json" : ".canonical-bytegate.json")),
         formatv("{0:2}\n", record).str());
    records.push_back(std::move(record));
    require(equal, "ordered prefix exact-byte gate failed before corridor core: " + file.string());
    require(passed.emplace(name.str(), bound).second, "duplicate ordered prefix comparison");
  }
  void finish(const std::filesystem::path &output) {
    require(passed.size() == 18, "ordered core cannot run before eighteen prefix byte comparisons");
    save(output / "ORDERED-PREFIX-GATE.json", formatv("{0:2}\n", json::Value(json::Object{
      {"baseline_directory", directory.string()}, {"manifest", manifest.string()},
      {"manifest_sha256", manifestSHA}, {"files_compared", passed.size()}, {"passed", true},
      {"normalization", false}, {"records", std::move(records)}})).str());
  }
};
using Snapshots = std::map<std::string, std::string>;
Snapshots ordinarySnapshots(Module &module) {
  Snapshots result;
  for (auto &function : module)
    if (!function.isDeclaration() && function.getName() != View)
      result.emplace(function.getName().str(), text(function));
  return result;
}
void unchangedOrdinary(Module &module, const Snapshots &before) {
  require(ordinarySnapshots(module) == before, "an ordinary function changed");
}

std::unique_ptr<Module> copyInContext(Module &module, LLVMContext &context) {
  SmallVector<char, 0> bytes; raw_svector_ostream stream(bytes);
  WriteBitcodeToFile(module, stream, true);
  auto result = parseBitcodeFile(MemoryBufferRef(StringRef(bytes.data(), bytes.size()),
                                                "image-table-scope"), context);
  if (!result) throw std::runtime_error("cannot copy scoped module: " + toString(result.takeError()));
  return std::move(*result);
}
void importModule(LLVMLifter &target, std::unique_ptr<Module> module) {
  require(module->global_empty() && module->alias_empty() && module->ifunc_empty(),
          "scoped frozen module unexpectedly contains global data");
  auto &destination = target.module();
  destination.setModuleIdentifier(module->getModuleIdentifier());
  destination.setSourceFileName(module->getSourceFileName());
  destination.setDataLayout(module->getDataLayout());
  destination.setTargetTriple(module->getTargetTriple());
  for (auto &metadata : module->named_metadata()) {
    auto *copy = destination.getOrInsertNamedMetadata(metadata.getName());
    for (unsigned n = 0; n != metadata.getNumOperands(); ++n) copy->addOperand(metadata.getOperand(n));
  }
  destination.getFunctionList().splice(destination.end(), module->getFunctionList());
}
void rebindArguments(Function &body, Function &from, Function &to) {
  for (unsigned n = 0; n != from.arg_size(); ++n) {
    auto *old = from.getArg(n);
    for (auto it = old->use_begin(); it != old->use_end();) {
      Use &use = *it++; auto *instruction = dyn_cast<Instruction>(use.getUser());
      if (instruction && instruction->getFunction() == &body) use.set(to.getArg(n));
    }
  }
}
void importViewBody(Module &original, Module &transferred) {
  auto *source = original.getFunction(View), *candidate = transferred.getFunction(View);
  require(source && candidate, "scoped view disappeared");
  ValueToValueMapTy map;
  std::vector<Function *> declarations;
  auto cleanDeclarations = [&]() { for (auto *f : declarations) if (f->use_empty()) f->eraseFromParent(); };
  for (auto &global : transferred.global_values()) {
    auto *binding = original.getNamedValue(global.getName());
    if (!binding) {
      auto *f = dyn_cast<Function>(&global);
      require(f && f->isDeclaration() && f->isIntrinsic(), "unexpected scoped global definition");
      auto *declaration = Function::Create(f->getFunctionType(), GlobalValue::ExternalLinkage,
                                            f->getName(), original);
      declaration->copyAttributesFrom(f); declarations.push_back(declaration); binding = declaration;
    }
    require(binding->getType() == global.getType(), "scoped global type changed");
    map[&global] = binding;
  }
  auto *copy = Function::Create(source->getFunctionType(), GlobalValue::ExternalLinkage,
                                "image.table.probe.commit", original);
  auto argument = copy->arg_begin();
  for (auto &a : candidate->args()) map[&a] = &*argument++;
  SmallVector<ReturnInst *, 8> returns;
  CloneFunctionInto(copy, candidate, map, CloneFunctionChangeType::DifferentModule, returns);
  copy->setLinkage(GlobalValue::ExternalLinkage); copy->setComdat(nullptr);
  try { verify(original); }
  catch (...) { copy->eraseFromParent(); cleanDeclarations(); throw; }
  auto *backup = Function::Create(source->getFunctionType(), GlobalValue::InternalLinkage,
                                  "image.table.probe.rollback", original);
  backup->splice(backup->end(), source); rebindArguments(*backup, *source, *backup);
  for (unsigned n = 0; n != copy->arg_size(); ++n) copy->getArg(n)->replaceAllUsesWith(source->getArg(n));
  source->splice(source->end(), copy);
  try { verify(original); }
  catch (...) {
    copy->splice(copy->end(), source); source->splice(source->end(), backup);
    rebindArguments(*source, *backup, *source);
    backup->eraseFromParent(); copy->eraseFromParent(); cleanDeclarations(); throw;
  }
  backup->eraseFromParent(); copy->eraseFromParent(); verify(original);
}
// Scope public module optimizers in an owned complete clone. Ordinary roots
// become declarations only inside that clone; all effect attributes remain.
// After verification, transfer only the optimized view body back. No original
// body is detached, deleted, or exposed to a module-wide optimizer.
template<class Callback> void optimizeViewOnly(LLVMLifter &view, Callback callback) {
  LLVMLifter scoped(dragon::Architecture::X86_64, "image-table-scoped-analysis");
  require(scoped.setExecutionMode(ExecutionMode::StaticAnalysis) &&
          scoped.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart), "cannot set scoped mode");
  importModule(scoped, copyInContext(view.module(), scoped.module().getContext()));
  for (auto &f : scoped.module()) if (f.getName() != View && !f.isDeclaration()) {
    f.deleteBody(); f.setLinkage(GlobalValue::ExternalLinkage); f.setComdat(nullptr);
  }
  verify(scoped.module()); callback(scoped); verify(scoped.module());
  auto transferred = copyInContext(scoped.module(), view.module().getContext());
  importViewBody(view.module(), *transferred);
}
// Only called inside optimizeViewOnly's owned complete clone. No annotations,
// assumptions, alias promises or target-specific threshold changes are added.
void standardPipeline(LLVMLifter &scoped, bool fullO2) {
  LoopAnalysisManager lam;
  FunctionAnalysisManager fam;
  CGSCCAnalysisManager cgam;
  ModuleAnalysisManager mam;
  PassBuilder pb;
  pb.registerModuleAnalyses(mam); pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam); pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  ModulePassManager mpm;
  if (fullO2) mpm = pb.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
  else {
    FunctionPassManager fpm;
    fpm.addPass(CorrelatedValuePropagationPass());
    fpm.addPass(InstCombinePass()); fpm.addPass(GVNPass()); fpm.addPass(SimplifyCFGPass());
    mpm.addPass(createModuleToFunctionPassAdaptor(std::move(fpm)));
  }
  mpm.run(scoped.module(), mam);
}

Value *guestBase(Function &function) {
  Value *result = nullptr;
  for (auto &i : function.getEntryBlock()) if (auto *load = dyn_cast<LoadInst>(&i))
    if (load->isSimple() && load->getType()->isPointerTy() &&
        load->getPointerOperand() == function.getArg(1)) {
      require(!result, "ambiguous analysis Guest base"); result = load;
    }
  require(result != nullptr, "missing analysis Guest base"); return result;
}
bool tableAddress(Value *value, unsigned depth = 0) {
  if (depth > 12) return false;
  if (auto *constant = dyn_cast<ConstantInt>(value))
    return constant->getBitWidth() <= 64 && constant->getZExtValue() >= TableBase &&
           constant->getZExtValue() < TableBase + Targets.size() * 8;
  auto *binary = dyn_cast<BinaryOperator>(value);
  if (!binary || (binary->getOpcode() != Instruction::Add && binary->getOpcode() != Instruction::Or))
    return false;
  for (unsigned n = 0; n != 2; ++n)
    if (auto *constant = dyn_cast<ConstantInt>(binary->getOperand(n));
        constant && constant->equalsInt(TableBase)) return true;
  return false;
}
bool targetSwitch(SwitchInst *sw, Value *guest) {
  auto *load = sw ? dyn_cast<LoadInst>(sw->getCondition()) : nullptr;
  auto *gep = load ? dyn_cast<GetElementPtrInst>(load->getPointerOperand()) : nullptr;
  return load && gep && gep->getPointerOperand() == guest && gep->getNumIndices() == 1 &&
      gep->getSourceElementType()->isIntegerTy(8) && tableAddress(gep->idx_begin()->get());
}
StringRef centerRole(SwitchInst *sw, Value *guest) {
  if (targetSwitch(sw, guest)) return "original-target";
  if (sw && targetSwitch(dyn_cast<SwitchInst>(sw->getDefaultDest()->getTerminator()), guest))
    return "table-index";
  return {};
}
std::set<std::string> findCenters(Function &function) {
  std::set<std::string> result;
  auto *guest = guestBase(function);
  for (auto &block : function) {
    auto *sw = dyn_cast<SwitchInst>(block.getTerminator());
    if (!centerRole(sw, guest).empty()) result.insert(block.getName().str());
  }
  return result;
}
SwitchInst *selectedStateSwitch(Function &function) {
  SwitchInst *selected = nullptr;
  for (auto &block : function) {
    auto *sw = dyn_cast<SwitchInst>(block.getTerminator());
    if (!sw || !sw->getCondition()->getType()->isIntegerTy(32) || sw->getNumCases() != Targets.size()) continue;
    bool matches = true; std::size_t n = 0;
    for (auto c : sw->cases()) matches &= c.getCaseValue()->equalsInt(n++);
    if (matches) { require(!selected, "ambiguous original state index"); selected = sw; }
  }
  require(selected, "original state index is missing"); return selected;
}
struct OrderedProvider {
  bool complete = true, budgetExhausted = false;
  bool selectedCyclicSwitch = false;
  std::vector<dragon::llvmlifter::detail::DispatchCorridorEdge> edges;
  json::Object report;
};
// Candidate enumeration only. Neither scalar leaves nor path descriptions
// certify a state, an alias fact, a guard outcome or an execution invariant.
// The private core rechecks every original edge and complete selected path.
OrderedProvider orderedCandidates(Function &function) {
  OrderedProvider result;
  constexpr std::size_t MaxBlocks = 4096, MaxPhiNodes = 256;
  constexpr std::size_t MaxCandidates = 512, MaxWork = 16384, MaxPath = 6;
  std::size_t work = 0, phiNodes = 0, excludedEntries = 0, pathsFound = 0;
  std::string reason;
  auto reject = [&](const char *message, bool budget = false) {
    result.complete = false; result.budgetExhausted |= budget;
    if (reason.empty()) reason = message;
  };
  auto charge = [&](std::size_t count = 1) {
    if (count > MaxWork - work) { reject("provider work cap", true); return false; }
    work += count; return true;
  };
  std::map<BasicBlock *, std::size_t> order;
  std::size_t originalIndex = 0;
  for (auto &block : function) order.emplace(&block, originalIndex++);
  if (order.size() > MaxBlocks) reject("provider function block cap", true);
  SwitchInst *selected = nullptr;
  std::set<BasicBlock *> component;
  std::size_t eligible = 0, cyclicSwitches = 0;
  if (result.complete) for (auto it = scc_begin(&function); !it.isAtEnd(); ++it) {
    const auto &members = *it;
    bool cyclic = members.size() > 1;
    if (!cyclic) for (auto *next : successors(members.front())) cyclic |= next == members.front();
    if (!cyclic) continue;
    std::set<BasicBlock *> group(members.begin(), members.end());
    std::vector<BasicBlock *> ordered(members.begin(), members.end());
    std::sort(ordered.begin(), ordered.end(), [&](auto *a, auto *b) { return order.at(a) < order.at(b); });
    for (auto *block : ordered) {
      if (!charge()) break;
      auto *sw = dyn_cast<SwitchInst>(block->getTerminator());
      if (!sw) continue;
      ++cyclicSwitches;
      if (!sw->getCondition()->getType()->isIntegerTy() ||
          sw->getCondition()->getType()->getIntegerBitWidth() > 128 ||
          !isa<PHINode>(sw->getCondition())) continue;
      ++eligible; selected = sw; component = group;
    }
  }
  if (eligible > 1) {
    reject("ambiguous cyclic scalar-PHI switches"); selected = nullptr; component.clear();
  }
  else if (!eligible && cyclicSwitches) reject("cyclic switch lacks a scalar PHI selector");
  result.selectedCyclicSwitch = eligible == 1 && selected;
  json::Array descriptions, skipped;
  if (result.complete && selected) {
    struct Leaf { BasicBlock *source; unsigned slot; BasicBlock *root; std::string value; };
    std::vector<Leaf> leaves;
    std::set<std::pair<BasicBlock *, unsigned>> leafKeys;
    std::set<PHINode *> gray, visited;
    std::function<void(PHINode *, unsigned)> visit = [&](PHINode *phi, unsigned depth) {
      if (!charge() || result.budgetExhausted) return;
      if (depth > 32) { reject("selector PHI depth cap", true); return; }
      if (gray.contains(phi)) { reject("selector PHI dependency cycle"); return; }
      if (!visited.insert(phi).second) return;
      if (++phiNodes > MaxPhiNodes) { reject("selector PHI node cap", true); return; }
      if (!component.contains(phi->getParent())) { reject("selector PHI outside business SCC"); return; }
      gray.insert(phi);
      for (unsigned n = 0; n != phi->getNumIncomingValues(); ++n) {
        if (!charge() || result.budgetExhausted) break;
        auto *source = phi->getIncomingBlock(n);
        if (!component.contains(source)) { ++excludedEntries; continue; }
        auto *value = phi->getIncomingValue(n);
        if (auto *next = dyn_cast<PHINode>(value); next && component.contains(next->getParent())) {
          visit(next, depth + 1); continue;
        }
        auto *term = source->getTerminator();
        if (!isa<BranchInst, SwitchInst>(term)) { reject("unsupported original leaf terminator"); continue; }
        std::string identity; raw_string_ostream stream(identity); value->printAsOperand(stream, false);
        for (unsigned slot = 0; slot != term->getNumSuccessors(); ++slot) {
          if (!charge()) break;
          if (term->getSuccessor(slot) != phi->getParent()) continue;
          if (!leafKeys.emplace(source, slot).second) continue;
          if (leaves.size() == MaxCandidates) { reject("original leaf candidate cap", true); break; }
          leaves.push_back({source, slot, phi->getParent(), identity});
        }
      }
      gray.erase(phi);
    };
    visit(cast<PHINode>(selected->getCondition()), 0);
    std::stable_sort(leaves.begin(), leaves.end(), [&](const auto &a, const auto &b) {
      return std::tuple(order.at(a.source), a.slot, order.at(a.root)) <
             std::tuple(order.at(b.source), b.slot, order.at(b.root));
    });
    leaves.erase(std::unique(leaves.begin(), leaves.end(), [](const auto &a, const auto &b) {
      return a.source == b.source && a.slot == b.slot && a.root == b.root;
    }), leaves.end());
    struct Paths { std::vector<std::vector<BasicBlock *>> found; bool nonlinear = false, limited = false; };
    std::map<BasicBlock *, Paths> cache;
    for (const auto &leaf : leaves) {
      if (result.budgetExhausted) break;
      if (!cache.contains(leaf.root)) {
        Paths paths; std::vector<BasicBlock *> path;
        std::function<void(BasicBlock *)> search = [&](BasicBlock *block) {
          if (!charge() || paths.found.size() > 1) return;
          if (std::find(path.begin(), path.end(), block) != path.end()) { paths.nonlinear = true; return; }
          if (path.size() == MaxPath) { paths.limited = true; return; }
          path.push_back(block);
          if (block == selected->getParent()) { paths.found.push_back(path); ++pathsFound; }
          else {
            std::vector<BasicBlock *> next;
            for (auto *target : successors(block)) {
              if (!charge()) break;
              if (component.contains(target)) next.push_back(target);
            }
            std::sort(next.begin(), next.end(), [&](auto *a, auto *b) { return order.at(a) < order.at(b); });
            next.erase(std::unique(next.begin(), next.end()), next.end());
            for (auto *target : next) search(target);
          }
          path.pop_back();
        };
        search(leaf.root); cache.emplace(leaf.root, std::move(paths));
      }
      const auto &paths = cache.at(leaf.root);
      if (paths.limited) reject("CFG path exceeds six-block provider cap", true);
      if (paths.nonlinear || paths.found.size() != 1 || paths.limited) {
        reject("ambiguous, cyclic or missing bounded CFG continuation");
        skipped.push_back(json::Object{{"source", leaf.source->getName().str()},
          {"successor_slot", leaf.slot}, {"root", leaf.root->getName().str()},
          {"paths", paths.found.size()}, {"cycle", paths.nonlinear}, {"limited", paths.limited}});
        continue;
      }
      const auto &path = paths.found.front();
      if (std::find(path.begin(), path.end(), leaf.source) != path.end()) {
        reject("original source occurs inside candidate path"); continue;
      }
      if (result.edges.size() == MaxCandidates) { reject("selected candidate cap", true); break; }
      result.edges.push_back({leaf.source, leaf.slot, path});
      json::Array blocks;
      for (auto *block : path) blocks.push_back(json::Object{{"ir_order", order.at(block)},
          {"block", block->getName().str()}, {"instructions", block->size()}});
      descriptions.push_back(json::Object{{"source", leaf.source->getName().str()},
        {"source_ir_order", order.at(leaf.source)}, {"successor_slot", leaf.slot},
        {"candidate_scalar_leaf", leaf.value}, {"candidate_path", std::move(blocks)},
        {"state_proof_claimed", false}});
    }
  }
  json::Array members;
  for (auto &block : function) if (component.contains(&block)) members.push_back(block.getName().str());
  std::string switchIdentity;
  if (selected) { raw_string_ostream stream(switchIdentity); selected->print(stream); }
  result.report = json::Object{{"schema", "dragon-ordered-corridor-provider-v1"},
    {"complete", result.complete}, {"budget_exhausted", result.budgetExhausted}, {"message", reason},
    {"cyclic_switches", cyclicSwitches}, {"eligible_cyclic_scalar_phi_switches", eligible},
    {"selected_original_cyclic_switch", result.selectedCyclicSwitch},
    {"selected_switch", switchIdentity}, {"business_scc", std::move(members)},
    {"phi_nodes", phiNodes}, {"excluded_non_scc_entry_edges", excludedEntries},
    {"paths_found", pathsFound}, {"work", work}, {"max_work", MaxWork},
    {"max_candidates", MaxCandidates}, {"max_path_blocks", MaxPath},
    {"candidates", std::move(descriptions)}, {"skipped", std::move(skipped)},
    {"candidate_only_not_proof", true}};
  return result;
}
LoadInst *selectedStateLoad(Function &function) {
  Value *index = selectedStateSwitch(function)->getCondition();
  std::set<Value *> seen; std::vector<std::pair<Value *, unsigned>> pending{{index, 0}};
  LoadInst *selected = nullptr; auto *guest = guestBase(function);
  for (std::size_t n = 0; n != pending.size(); ++n) {
    auto [value, depth] = pending[n];
    require(depth <= 32 && seen.size() < 128, "state LOAD snapshot expression cap exceeded");
    if (!seen.insert(value).second || isa<ConstantInt>(value)) continue;
    if (auto *load = dyn_cast<LoadInst>(value)) {
      auto *gep = dyn_cast<GetElementPtrInst>(load->getPointerOperand());
      auto *at = gep && gep->getNumIndices() == 1 ? dyn_cast<ConstantInt>(gep->idx_begin()->get()) : nullptr;
      require(load->isSimple() && load->getType()->isIntegerTy(32) && gep &&
              gep->getPointerOperand() == guest && gep->getSourceElementType()->isIntegerTy(8) &&
              at && at->equalsInt(51108), "state LOAD differs from pinned manual cell");
      require(!selected || selected == load, "more than one original state LOAD atom"); selected = load;
      continue;
    }
    auto *instruction = dyn_cast<Instruction>(value);
    require(instruction && (isa<BinaryOperator>(instruction) || isa<CastInst>(instruction) ||
                            isa<FreezeInst>(instruction)), "unsupported state LOAD snapshot shape");
    for (auto &operand : instruction->operands()) pending.push_back({operand.get(), depth + 1});
  }
  require(selected, "state LOAD atom is missing"); return selected;
}
json::Object structure(Function &function, const std::set<std::string> &centers) {
  auto *guest = guestBase(function);
  std::size_t cyclicSwitches = 0, cyclicTableReads = 0, cyclicCenterBranches = 0;
  std::size_t cyclicTargets = 0, cyclicIndices = 0;
  std::set<BasicBlock *> cyclicBlocks;
  json::Array components;
  for (auto it = scc_begin(&function); !it.isAtEnd(); ++it) {
    const auto &component = *it;
    bool cyclic = component.size() > 1;
    if (!cyclic) for (auto *target : successors(component.front())) cyclic |= target == component.front();
    if (!cyclic) continue;
    json::Array blocks;
    for (auto *block : component) {
      cyclicBlocks.insert(block);
      blocks.push_back(block->getName().str());
      if (isa<SwitchInst>(block->getTerminator())) ++cyclicSwitches;
      auto role = centerRole(dyn_cast<SwitchInst>(block->getTerminator()), guest);
      if (role.empty() && isa<SwitchInst>(block->getTerminator()))
        for (const auto &center : centers)
          if (block->getName() == center || block->getName().starts_with(center + ".")) {
            role = "table-index-by-original-center"; break;
          }
      cyclicTargets += role == "original-target";
      cyclicIndices += role == "table-index" || role == "table-index-by-original-center";
      for (auto &i : *block) if (auto *load = dyn_cast<LoadInst>(&i)) {
        auto *gep = dyn_cast<GetElementPtrInst>(load->getPointerOperand());
        if (gep && gep->getPointerOperand() == guest && gep->getNumIndices() == 1 &&
            gep->getSourceElementType()->isIntegerTy(8) &&
            tableAddress(gep->idx_begin()->get())) ++cyclicTableReads;
      }
      auto *branch = dyn_cast<BranchInst>(block->getTerminator());
      if (branch && branch->isConditional()) {
        for (const auto &center : centers)
          if (block->getName() == center || block->getName().starts_with(center + ".")) {
            ++cyclicCenterBranches; break;
          }
      }
    }
    components.push_back(std::move(blocks));
  }
  json::Array identified;
  std::set<BasicBlock *> reachable;
  std::vector<BasicBlock *> pending{&function.getEntryBlock()};
  for (std::size_t n = 0; n != pending.size(); ++n) {
    auto *block = pending[n]; if (!reachable.insert(block).second) continue;
    for (auto *next : successors(block)) pending.push_back(next);
    auto role = centerRole(dyn_cast<SwitchInst>(block->getTerminator()), guest);
    if (role.empty() && isa<SwitchInst>(block->getTerminator()))
      for (const auto &center : centers)
        if (block->getName() == center || block->getName().starts_with(center + ".")) {
          role = "table-index-by-original-center"; break;
        }
    if (!role.empty()) identified.push_back(json::Object{{"block", block->getName().str()},
      {"role", role.str()}, {"switch", true},
      {"in_business_reachable_cyclic_scc", cyclicBlocks.contains(block)}});
  }
  // This manual fixture has no original business switch. Rejecting every
  // cyclic switch is deliberately stricter than recognizing a renamed center.
  // A syntactic pass is only a precision result, never a semantic oracle.
  return json::Object{{"cyclic_switches", cyclicSwitches}, {"cyclic_table_reads", cyclicTableReads},
          {"identified_cyclic_original_target_switches", cyclicTargets},
          {"identified_cyclic_table_index_switches", cyclicIndices},
          {"cyclic_center_conditional_branches", cyclicCenterBranches},
          {"cyclic_components", std::move(components)},
          {"identified_centers", std::move(identified)}, {"center_identity_established", !centers.empty()},
          {"strict_probe_no_cyclic_switch", cyclicSwitches == 0},
          {"central_dispatch_exited_business_scc", !centers.empty() && cyclicTargets == 0 &&
                 cyclicIndices == 0 && cyclicTableReads == 0 && cyclicCenterBranches == 0}};
}
json::Object memoryStats(const MemoryForwardStats &s) {
  return json::Object{{"available", s.available}, {"complete", s.complete}, {"changed", s.changed},
          {"blocks_visited", s.blocksVisited}, {"guest_loads", s.guestLoads},
          {"guest_stores", s.guestStores}, {"loads_forwarded", s.loadsForwarded},
          {"loads_reused", s.loadsReused}, {"call_barriers", s.callBarriers},
          {"alias_barriers", s.aliasBarriers}, {"partial_overlaps", s.partialOverlaps},
          {"phis_inserted", s.phisInserted}, {"join_barriers", s.joinBarriers},
          {"loop_barriers", s.loopBarriers}, {"budget_exhausted", s.budgetExhausted},
          {"byte_rebuilds", s.byteRebuilds}, {"leaf_calls_preserved", s.leafCallsPreserved},
          {"summary_calls_preserved", s.summaryCallsPreserved},
          {"summary_calls_rejected", s.summaryCallsRejected}};
}
std::string boundWrapper(Module &module, json::Object &receipt) {
  auto bound = CloneModule(module);
  auto *wrapper = bound->getFunction(Wrapper);
  auto *root = bound->getFunction(Root);
  auto *view = bound->getFunction(View);
  require(wrapper && root && view, "wrapper binding symbols missing");
  CallBase *selected = nullptr;
  for (auto &block : *wrapper) for (auto &i : block) if (auto *call = dyn_cast<CallBase>(&i)) {
    if (call->getCalledFunction() != root) continue;
    require(!selected, "wrapper contains more than one kernel call");
    auto *tag = call->getMetadata("dragon.guest.call.target");
    auto *constant = tag && tag->getNumOperands() == 1 ?
        dyn_cast<ConstantAsMetadata>(tag->getOperand(0)) : nullptr;
    auto *pc = constant ? dyn_cast<ConstantInt>(constant->getValue()) : nullptr;
    require(pc && pc->equalsInt(0x1010), "wrapper target metadata mismatch");
    require(call->getFunctionType() == view->getFunctionType(), "wrapper view ABI mismatch");
    selected = call;
  }
  require(selected, "wrapper kernel call missing");
  const auto rootBefore = text(*root), viewBefore = text(*view), wrapperBefore = text(*wrapper);
  selected->setCalledFunction(view);
  verify(*bound);
  require(rootBefore == text(*root) && viewBefore == text(*view), "binding mutated root or view");
  receipt = json::Object{{"changed_calls", 1}, {"target_metadata", 0x1010},
             {"wrapper_before_sha256", sha(wrapperBefore)}, {"wrapper_after_sha256", sha(text(*wrapper))},
             {"ordinary_kernel_unchanged", true}, {"view_unchanged", true}};
  return text(*bound);
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc >= 4 && argc <= 12,
            "usage: image-table-real-probe RAW ELF NEW-OUT [--predicates] [--closed-cfg] [--cell-ssa] [--decoder-ssa] [--decoder-uses] [--dispatch-components] [--standard-cvp|--standard-o2] [--ordered-corridor]");
    bool explicitPredicates = false, consumeClosedCFG = false, cellSSA = false, decoderSSA = false;
    bool decoderUses = false, dispatchComponents = false;
    bool standardCVP = false, standardO2 = false;
    bool orderedCorridor = false;
    for (int n = 4; n != argc; ++n) {
      StringRef option(argv[n]);
      if (option == "--predicates") { require(!explicitPredicates, "duplicate predicates option"); explicitPredicates = true; }
      else if (option == "--closed-cfg") { require(!consumeClosedCFG, "duplicate closed-cfg option"); consumeClosedCFG = true; }
      else if (option == "--cell-ssa") { require(!cellSSA, "duplicate cell-ssa option"); cellSSA = true; }
      else if (option == "--decoder-ssa") { require(!decoderSSA, "duplicate decoder-ssa option"); decoderSSA = true; }
      else if (option == "--decoder-uses") { require(!decoderUses, "duplicate decoder-uses option"); decoderUses = true; }
      else if (option == "--dispatch-components") { require(!dispatchComponents, "duplicate dispatch-components option"); dispatchComponents = true; }
      else if (option == "--standard-cvp") { require(!standardCVP && !standardO2, "duplicate/conflicting standard recipe"); standardCVP = true; }
      else if (option == "--standard-o2") { require(!standardCVP && !standardO2, "duplicate/conflicting standard recipe"); standardO2 = true; }
      else if (option == "--ordered-corridor") { require(!orderedCorridor, "duplicate ordered-corridor option"); orderedCorridor = true; }
      else throw std::runtime_error("unknown probe option");
    }
    require(!cellSSA || consumeClosedCFG, "cell-ssa recipe requires the explicit closed-cfg consumer");
    require(!decoderSSA || cellSSA, "decoder-ssa recipe requires the explicit cell-ssa consumer");
    require(!decoderUses || decoderSSA, "decoder-uses recipe requires explicit decoder-ssa");
    require(!dispatchComponents || decoderUses, "dispatch-components recipe requires explicit decoder-uses");
    require(!(standardCVP || standardO2) || dispatchComponents, "standard recipe requires explicit dispatch-components");
    require(!orderedCorridor || standardO2, "ordered-corridor recipe requires explicit standard-o2");
    std::optional<OrderedPrefixGate> prefixGate;
    if (orderedCorridor) prefixGate.emplace(OrderedPrefixGate::load());
    auto raw = MemoryBuffer::getFile(argv[1]); auto elf = MemoryBuffer::getFile(argv[2]);
    require(bool(raw) && bool(elf), "cannot read pinned inputs");
    require(sha((*raw)->getBuffer()) == RawSHA && sha((*elf)->getBuffer()) == ELFSHA,
            "require exact frozen B manual-O2 canonical raw and ELF");
    const std::filesystem::path output(argv[3]);
    require(!std::filesystem::exists(output), "output namespace already exists");
    require(std::filesystem::create_directories(output), "cannot create output namespace");
    std::string loaderError;
    auto image = dragon::loader::LoadedImage::loadBytes(std::vector<std::uint8_t>(
        (*elf)->getBufferStart(), (*elf)->getBufferEnd()), &loaderError);
    require(bool(image), "ELF loading failed: " + loaderError);
    auto bytes = image->readVirtual(TableBase, Targets.size() * 8);
    require(bytes && bytes->size() == Targets.size() * 8, "ELF table bytes missing");
    for (std::size_t n = 0; n != Targets.size(); ++n) {
      std::uint64_t value = 0;
      for (unsigned b = 0; b != 8; ++b) value |= std::uint64_t((*bytes)[n * 8 + b]) << (8 * b);
      require(value == Targets[n], "pinned ELF table differs from expected targets");
    }
    LLVMLifter source(dragon::Architecture::X86_64, "image-table-frozen-raw");
    require(source.setExecutionMode(ExecutionMode::StaticAnalysis) &&
            source.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart), "cannot set source execution mode");
    SMDiagnostic error;
    auto parsed = parseAssembly(MemoryBufferRef((*raw)->getBuffer(), argv[1]), error,
                                source.module().getContext());
    if (!parsed) { error.print(argv[0], errs()); throw std::runtime_error("cannot parse canonical raw"); }
    require(parsed->global_empty() && parsed->alias_empty() && parsed->ifunc_empty() &&
            parsed->named_metadata_empty(), "frozen import expects function-only canonical module");
    source.module().setModuleIdentifier(parsed->getModuleIdentifier());
    source.module().setSourceFileName(parsed->getSourceFileName());
    source.module().setDataLayout(parsed->getDataLayout());
    source.module().setTargetTriple(parsed->getTargetTriple());
    source.module().getFunctionList().splice(source.module().end(), parsed->getFunctionList());
    verify(source.module());
    const auto sourceBefore = source.ir();
    save(output / "source-parsed.ll", sourceBefore);
    std::vector<std::uint8_t> spBytes(8);
    for (unsigned n = 0; n != 8; ++n) spBytes[n] = static_cast<std::uint8_t>(KernelSP >> (8 * n));
    GuardedEntryContext context{0x1010, "image_table_manual_o2_v1",
      std::string("Frozen B manual-O2 ELF ") + ELFSHA + "; runtime checked SP and complete table bytes",
      {{32, spBytes}}, {{TableBase, *bytes}}};
    std::string guardError;
    auto view = source.createGuardedEntryView(context, guardError);
    require(bool(view), "guarded view rejected: " + guardError);
    require(source.ir() == sourceBefore, "guard creation changed its ordinary source module");
    save(output / "guarded-entry-manifest.json", view->guardedEntryManifest());
    auto ordinary = ordinarySnapshots(view->module());
    std::set<std::string> centerNames;
    json::Array stages;
    bool finalStructure = false;
    bool finalStrict = false;
    bool orderedCoreGate = false, orderedProviderComplete = false;
    bool orderedChanged = false, orderedCoreComplete = false;
    bool orderedHadCyclicSwitch = false;
    auto checkpoint = [&](StringRef name, json::Object stats) {
      verify(view->module()); unchangedOrdinary(view->module(), ordinary);
      require(source.ir() == sourceBefore, "source module changed after view analysis");
      auto *function = view->module().getFunction(View); require(function, "view disappeared");
      auto observedCenters = findCenters(*function);
      centerNames.insert(observedCenters.begin(), observedCenters.end());
      auto shape = structure(*function, centerNames);
      if (orderedCorridor) {
        // The manual holdout has no original business switch. Names can be
        // changed by O2; this opt-in gate therefore uses actual reachable SCCs.
        shape["legacy_named_center_gate"] =
            shape.getBoolean("central_dispatch_exited_business_scc").value_or(false);
        shape["central_dispatch_exited_business_scc"] =
            shape.getBoolean("strict_probe_no_cyclic_switch").value_or(false) &&
            shape.getInteger("cyclic_table_reads").value_or(-1) == 0;
        shape["structure_gate_basis"] = "actual reachable SCCs; every cyclic switch is conservatively rejected";
      }
      finalStructure = shape.getBoolean("central_dispatch_exited_business_scc").value_or(false);
      finalStrict = shape.getBoolean("strict_probe_no_cyclic_switch").value_or(false);
      auto ir = view->ir(); save(output / (name.str() + ".ll"), ir);
      if (prefixGate) prefixGate->check(output, name, ir, false);
      json::Object binding;
      auto bound = boundWrapper(view->module(), binding);
      save(output / (name.str() + ".bound-wrapper.ll"), bound);
      if (prefixGate) prefixGate->check(output, name, bound, true);
      json::Value stage(json::Object{{"stage", name.str()}, {"sha256", sha(ir)},
        {"bound_sha256", sha(bound)}, {"verified", true}, {"ordinary_functions_unchanged", true},
        {"source_module_unchanged", true}, {"stats", std::move(stats)},
        {"structure", std::move(shape)}, {"binding", std::move(binding)}});
      save(output / (name.str() + ".json"), formatv("{0:2}\n", stage).str());
      stages.push_back(std::move(stage));
    };
    checkpoint("00-guarded-raw", {});
    const dragon::llvmlifter::detail::ImageTableContextRequest request{View, TableBase, Targets, 4096, 65536, 1000000};
    const auto proof = dragon::llvmlifter::detail::runImageTableCheckedContext(view->module(), request, consumeClosedCFG);
    checkpoint("01-checked-context", json::Object{{"closed", proof.closed}, {"changed", proof.changed},
      {"budget_exhausted", proof.budgetExhausted}, {"guarded_bytes", proof.guardedBytes},
      {"preserved_stores", proof.preservedStores}, {"constant_values", proof.constantValues},
      {"work", proof.work}, {"message", proof.message}});
    if (proof.closed && proof.changed) {
      if (cellSSA) {
        auto *function = view->module().getFunction(View); require(function, "view disappeared before cell snapshot");
        auto *selected = selectedStateLoad(*function);
        std::string identity; raw_string_ostream selectedText(identity); selected->print(selectedText);
        // Snapshot once after the proof consumer's body transaction, before
        // this cell pass's first mutation. Its resolver never retries clones.
        const auto cell = dragon::llvmlifter::detail::forwardGuestCellLoad(
            *function, *guestBase(*function), *selected);
        checkpoint("01b-cell-ssa", json::Object{{"changed", cell.changed}, {"refused", cell.refused},
          {"budget_exhausted", cell.budgetExhausted}, {"blocks", cell.blocks}, {"nodes", cell.nodes},
          {"phis", cell.phis}, {"work", cell.work}, {"message", cell.message},
          {"selected_load_before", std::string(identity)}, {"guest_byte_offset", 51108}, {"guest_bytes", 4}});
        optimizeViewOnly(*view, [](LLVMLifter &scoped) { scoped.runInstCombine(); scoped.runGVN(); });
        checkpoint("01c-instcombine-gvn", {});
      }
      if (explicitPredicates) {
        PredicateFoldStats predicates;
        optimizeViewOnly(*view, [&](LLVMLifter &scoped) { predicates = scoped.runPredicateFolding(); });
        checkpoint("01a-predicates", json::Object{{"available", predicates.available},
          {"conditions_visited", predicates.conditionsVisited}, {"conditions_folded", predicates.conditionsFolded},
          {"solver_queries", predicates.solverQueries}, {"unknown_queries", predicates.unknownQueries},
          {"unsupported_conditions", predicates.unsupportedConditions}, {"budget_skipped", predicates.budgetSkipped}});
      }
      if (decoderSSA) {
        auto *function = view->module().getFunction(View); require(function, "view disappeared before decoder snapshot");
        auto *selected = selectedStateSwitch(*function);
        auto *originalRoot = selected->getCondition();
        std::string rootIdentity; raw_string_ostream rootText(rootIdentity); originalRoot->print(rootText);
        json::Array originalConsumers;
        for (auto &use : originalRoot->uses()) {
          std::string identity; raw_string_ostream stream(identity); use.getUser()->print(stream);
          originalConsumers.push_back(json::Object{{"operand", use.getOperandNo()}, {"instruction", identity}});
        }
        const auto decoder = decoderUses
            ? dragon::llvmlifter::detail::normalizeDispatchDecoderUses(*function, *selected)
            : dragon::llvmlifter::detail::normalizeDispatchDecoder(*function, *selected);
        checkpoint("01d-decoder-ssa", json::Object{{"changed", decoder.changed}, {"refused", decoder.refused},
          {"budget_exhausted", decoder.budgetExhausted}, {"nodes", decoder.nodes}, {"phis", decoder.phis},
          {"instructions", decoder.instructions}, {"work", decoder.work}, {"message", decoder.message},
          {"consume_original_uses", decoderUses}, {"original_root", rootIdentity},
          {"original_consumers", std::move(originalConsumers)}});
        if (decoderUses) {
          optimizeViewOnly(*view, [](LLVMLifter &scoped) { scoped.runInstCombine(); scoped.runGVN(); });
          checkpoint("01e-decoder-instcombine-gvn", {});
          PredicateFoldStats predicates;
          optimizeViewOnly(*view, [&](LLVMLifter &scoped) { predicates = scoped.runPredicateFolding(); });
          checkpoint("01f-decoder-predicates", json::Object{{"available", predicates.available},
            {"conditions_visited", predicates.conditionsVisited}, {"conditions_folded", predicates.conditionsFolded},
            {"solver_queries", predicates.solverQueries}, {"unknown_queries", predicates.unknownQueries},
            {"unsupported_conditions", predicates.unsupportedConditions}, {"budget_skipped", predicates.budgetSkipped}});
        }
      }
      if (standardCVP || standardO2) {
        optimizeViewOnly(*view, [&](LLVMLifter &scoped) { standardPipeline(scoped, standardO2); });
        checkpoint(standardO2 ? "01g-standard-o2" : "01g-standard-cvp", json::Object{
          {"recipe", standardO2 ? "LLVM21 default<O2>" : "CVP,InstCombine,GVN,SimplifyCFG"},
          {"ordinary_definitions_optimized", false}, {"new_assumptions_or_alias_promises", false}});
      }
      if (orderedCorridor) {
        prefixGate->finish(output);
        auto *function = view->module().getFunction(View); require(function, "view missing before original-edge snapshot");
        auto provider = orderedCandidates(*function);
        orderedProviderComplete = provider.complete;
        orderedHadCyclicSwitch = provider.selectedCyclicSwitch;
        checkpoint("01h-ordered-candidates", std::move(provider.report));
        // The checkpoint observes without mutating this function. Freeze all
        // original candidates together; the core publishes its whole body once.
        auto *moduleIdentity = &view->module();
        std::map<std::string, Function *> functionIdentity;
        std::map<std::string, std::vector<Argument *>> argumentIdentity;
        for (auto &f : view->module()) {
          functionIdentity[f.getName().str()] = &f;
          for (auto &arg : f.args()) argumentIdentity[f.getName().str()].push_back(&arg);
        }
        const auto threaded = dragon::llvmlifter::detail::threadDispatchCorridor(*function, provider.edges);
        // Body publication invalidates all pre-pass block/instruction handles.
        // Drop them before any following stage and fetch the function again.
        provider.edges.clear();
        function = view->module().getFunction(View);
        require(function && &view->module() == moduleIdentity, "ordered pass replaced module/view identity");
        for (const auto &[name, original] : functionIdentity) {
          auto *current = view->module().getFunction(name);
          require(current == original, "ordered pass replaced an original Function object");
          const auto &arguments = argumentIdentity[name];
          require(current->arg_size() == arguments.size(), "ordered pass changed argument count");
          for (unsigned n = 0; n != arguments.size(); ++n)
            require(current->getArg(n) == arguments[n], "ordered pass replaced an Argument object");
        }
        orderedChanged = threaded.changed; orderedCoreComplete = threaded.complete;
        checkpoint("01i-ordered-corridor", json::Object{{"changed", threaded.changed},
          {"complete", threaded.complete}, {"budget_exhausted", threaded.budgetExhausted},
          {"candidates_visited", threaded.candidatesVisited}, {"rejected", threaded.rejected},
          {"rewritten_edges", threaded.rewrittenEdges}, {"cloned_blocks", threaded.clonedBlocks},
          {"cloned_instructions", threaded.clonedInstructions}, {"snapshots", threaded.snapshots},
          {"repair_phis", threaded.repairPhis}, {"repair_cells", threaded.repairCells},
          {"work", threaded.work}, {"message", threaded.message},
          {"provider_complete", orderedProviderComplete}, {"ordinary_function_argument_identity_preserved", true}});
        // Capture this before any further CFG cleanup can rename/lower a
        // remaining dispatcher. Later passes cannot wash away a failed gate.
        orderedCoreGate = orderedHadCyclicSwitch && orderedProviderComplete && threaded.complete &&
                          threaded.changed && finalStructure && finalStrict;
      }
      const char *controlNames[]{"02-control-flow", "04-control-flow"};
      const char *memoryNames[]{"03-guest-memory", "05-guest-memory"};
      for (unsigned round = 0; round != 2; ++round) {
        optimizeViewOnly(*view, [](LLVMLifter &scoped) { scoped.runControlFlowSimplification(); });
        checkpoint(controlNames[round], json::Object{{"round", round}});
        if (round == 0 && dispatchComponents) {
          DispatcherSpecializationStats specialized;
          optimizeViewOnly(*view, [&](LLVMLifter &scoped) { specialized = scoped.runDispatcherSpecialization(); });
          checkpoint("02a-dispatch-specialization", json::Object{{"available", specialized.available},
            {"complete", specialized.complete}, {"changed", specialized.changed},
            {"conditions_visited", specialized.conditionsVisited}, {"unique_conditions", specialized.uniqueConditions},
            {"switches_visited", specialized.switchesVisited}, {"branches_rewritten", specialized.branchesRewritten},
            {"switches_rewritten", specialized.switchesRewritten}, {"unresolved_switches", specialized.unresolvedSwitches},
            {"budget_exhausted", specialized.budgetExhausted}});
          DeflatteningStats deflattened;
          optimizeViewOnly(*view, [&](LLVMLifter &scoped) { deflattened = scoped.runProvableDeflattening(); });
          checkpoint("02b-dispatch-deflattening", json::Object{{"available", deflattened.available},
            {"complete", deflattened.complete}, {"changed", deflattened.changed},
            {"dispatchers_visited", deflattened.dispatchersVisited}, {"dispatchers_rewritten", deflattened.dispatchersRewritten},
            {"edges_rewritten", deflattened.edgesRewritten}, {"unresolved_dispatchers", deflattened.unresolvedDispatchers},
            {"budget_exhausted", deflattened.budgetExhausted}});
          optimizeViewOnly(*view, [](LLVMLifter &scoped) { scoped.runControlFlowSimplification(); });
          checkpoint("02c-dispatch-control-flow", {});
        }
        MemoryForwardOptions options;
        options.maxRewrites = 64; options.runMem2Reg = false;
        options.proveLeafCalls = false; options.consumeCallEffectSummaries = false;
        MemoryForwardStats memory;
        optimizeViewOnly(*view, [&](LLVMLifter &scoped) { memory = scoped.runGuestMemorySSA(options, false); });
        checkpoint(memoryNames[round], memoryStats(memory));
      }
      optimizeViewOnly(*view, [](LLVMLifter &scoped) { scoped.runControlFlowSimplification(); });
      checkpoint("06-control-flow", json::Object{{"round", 2}});
    }
    struct rusage usage{}; getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    const auto peakRSS = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    const auto peakRSS = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    const bool hardGate = proof.closed && finalStructure && finalStrict &&
                          (!orderedCorridor || orderedCoreGate);
    json::Object report{{"schema", "dragon-image-table-context-real-probe-v1"},
      {"llvm_version", LLVM_VERSION_STRING},
      {"raw_sha256", RawSHA}, {"elf_sha256", ELFSHA}, {"kernel_entry", 0x1010},
      {"wrapper_initial_sp", 0xc808}, {"kernel_sp", KernelSP}, {"table_base", TableBase},
      {"table_bytes_sha256", sha(StringRef(reinterpret_cast<const char *>(bytes->data()), bytes->size()))},
      {"guarded_out_pointer", false}, {"image_immutability_assumed", false},
      {"sp_provenance", json::Object{
        {"scope", "read-only archived input and driver plus original wrapper bytes; no target execution"},
        {"archive_sha256", "9c623d69bf90ded9c486a70861224c85966bb9698a97f3a9ba791e0cd3f77359"},
        {"raw_member", "practical-region-holdouts-20261113/normal-manual/manual/2026111307/plain/O2/product-O2/unoptimized.ll"},
        {"driver_sha256", "e0fb71e08b117ddb6c929dc60ff66af938073695ced0fe68b6f50aa9c69c75bb"},
        {"input0_sha256", "787ebc3dcebd09221b68c82815be58b97adc848d5dabb181272b2e764a2c7e9f"},
        {"all_16_archived_inputs_share_wrapper_sp", true},
        {"wrapper_subtracts", 8}, {"machine_call_pushes", 8}}},
      {"ordinary_functions_unchanged", true}, {"source_module_unchanged", true},
      {"proof_closed", proof.closed}, {"identified_center_gate_passed", finalStructure},
      {"explicit_default_predicates", explicitPredicates},
      {"consume_closed_cfg", consumeClosedCFG}, {"selected_cell_ssa", cellSSA},
      {"selected_decoder_ssa", decoderSSA},
      {"consume_original_decoder_uses", decoderUses}, {"default_dispatch_components", dispatchComponents},
      {"standard_cvp_recipe", standardCVP}, {"standard_o2_recipe", standardO2},
      {"ordered_corridor_recipe", orderedCorridor}, {"ordered_provider_complete", orderedProviderComplete},
      {"ordered_core_complete", orderedCoreComplete}, {"ordered_core_stage_gate_passed", orderedCoreGate},
      {"ordered_original_cyclic_switch_was_selected", orderedHadCyclicSwitch},
      {"ordered_prefix_files_compared", prefixGate ? prefixGate->passed.size() : 0},
      {"only_context_copies", orderedCorridor && orderedChanged && !orderedCoreGate},
      {"strict_probe_no_cyclic_switch_passed", finalStrict},
      {"hard_structure_gate_passed", hardGate},
      {"conditional_fast_projection_reported", false},
      {"semantic_validation_performed", false}, {"full_cff_completion_claimed", false},
      {"producer_runs", 0}, {"lift_runs", 0}, {"target_native_runs", 0},
      {"peak_rss_bytes", peakRSS}, {"stages", std::move(stages)}};
    save(output / "REPORT.json", formatv("{0:2}\n", json::Value(std::move(report))).str());
    outs() << "proof_closed=" << proof.closed << " structure_gate=" << finalStructure
           << " native_runs=0 peak_rss_bytes=" << peakRSS << '\n';
    return hardGate ? 0 : 4;
  } catch (const std::exception &error) {
    errs() << error.what() << '\n'; return 2;
  }
}
