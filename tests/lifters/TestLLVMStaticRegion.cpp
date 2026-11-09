#include <catch2/catch_test_macros.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t rootPC = 0x1000, extraPC = 0x2000, vectorPC = 0x2100;
const std::array architectures{Architecture::X86_32, Architecture::X86_64, Architecture::ARM64};

Bytes ret(Architecture arch) {
  return arch == Architecture::ARM64 ? Bytes{0xc0,3,0x5f,0xd6} : Bytes{0xc3};
}
Bytes jumpRegister(Architecture arch) {
  return arch == Architecture::ARM64 ? Bytes{0,0,0x1f,0xd6} : Bytes{0xff,0xe0};
}
Bytes scalarBody(Architecture arch) {
  return arch == Architecture::ARM64 ? Bytes{0xe1,0,0x80,0xd2,0xc0,3,0x5f,0xd6}
                                    : Bytes{0xb9,7,0,0,0,0xc3};
}
Bytes vectorBody(Architecture arch) {
  return arch == Architecture::ARM64 ? Bytes{0x5f,0xe5,5,0x4f,0xc0,3,0x5f,0xd6}
                                    : Bytes{0x66,0x0f,0xef,0xc9,0xc3};
}
Bytes direct(Architecture arch, bool call) {
  if (arch == Architecture::ARM64)
    return call ? Bytes{0xfd,0x7b,0xbf,0xa9,0xff,3,0,0x94,0xfd,0x7b,0xc1,0xa8,0xc0,3,0x5f,0xd6}
                : Bytes{0,4,0,0x14};
  return call ? Bytes{0xe8,0xfb,0x0f,0,0,0xc3} : Bytes{0xe9,0xfb,0x0f,0,0};
}

void add(Graph &graph, const Bytes &bytes, std::uint64_t pc) {
  Disassembler dis{graph.architecture(), graph.configuration()};
  dis.disassemble(bytes, pc, graph);
}
Graph sample(Architecture arch, DisassemblyStrategy strategy) {
  Configuration config;config.disassemblyStrategy = strategy;
  Graph graph{arch, config};
  add(graph, jumpRegister(arch), rootPC);
  add(graph, scalarBody(arch), extraPC);
  add(graph, vectorBody(arch), vectorPC);
  return graph;
}
void configure(LLVMLifter &lifter) {
  REQUIRE(lifter.setExecutionMode(ExecutionMode::StaticAnalysis));
  REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
}
std::string symbol(Architecture arch, std::string_view address = "1000") {
  return "dragon_static_entry_" + ArchitectureName(arch) + "_" + std::string(address);
}
llvm::Function &function(LLVMLifter &lifter, Architecture arch, std::string_view address = "1000") {
  auto *f = lifter.module().getFunction(symbol(arch,address));
  REQUIRE(f);REQUIRE_FALSE(f->isDeclaration());
  return *f;
}
void verify(LLVMLifter &lifter) {
  std::string error;llvm::raw_string_ostream out{error};
  const bool bad = llvm::verifyModule(lifter.module(), &out);
  INFO(error);CHECK_FALSE(bad);
}
std::set<std::uint64_t> cases(const llvm::SwitchInst &sw) {
  std::set<std::uint64_t> result;
  for (const auto &c : sw.cases()) result.insert(c.getCaseValue()->getZExtValue());
  return result;
}
llvm::SwitchInst *dispatch(llvm::Function &f, const std::set<std::uint64_t> &targets) {
  llvm::SwitchInst *found = nullptr;
  for (auto &bb : f) if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(bb.getTerminator()))
    if (cases(*sw) == targets) { CHECK(found == nullptr);found = sw; }
  return found;
}
std::map<std::size_t,const llvm::Value *> exitValues(const llvm::BasicBlock &bb) {
  std::map<std::size_t,const llvm::Value *> values;
  const auto &f = *bb.getParent();
  for (const auto &i : bb) if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&i)) {
    std::int64_t offset = 0;
    const auto *base = llvm::GetPointerBaseWithConstantOffset(
        store->getPointerOperand(), offset, f.getParent()->getDataLayout());
    if (base == f.getArg(3) && offset >= 0) values[static_cast<std::size_t>(offset)] = store->getValueOperand();
  }
  return values;
}
void constantExit(const std::map<std::size_t,const llvm::Value *> &values,
                  std::size_t offset, std::uint64_t expected) {
  REQUIRE(values.contains(offset));
  const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(values.at(offset));
  REQUIRE(constant);CHECK(constant->getZExtValue() == expected);
}
bool writesRegister(const llvm::Function &f, std::uint64_t wanted) {
  for (const auto &bb : f) for (const auto &i : bb)
    if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&i)) {
      std::int64_t offset = 0;
      const auto *base = llvm::GetPointerBaseWithConstantOffset(
          store->getPointerOperand(), offset, f.getParent()->getDataLayout());
      const auto *load = llvm::dyn_cast<llvm::LoadInst>(base);
      if (!load || offset != static_cast<std::int64_t>(wanted)) continue;
      std::int64_t stateOffset = 0;
      const auto *state = llvm::GetPointerBaseWithConstantOffset(
          load->getPointerOperand(), stateOffset, f.getParent()->getDataLayout());
      if (state == f.getArg(0) && stateOffset == offsetof(GuestStateV2,regs_bytes)) return true;
    }
  return false;
}
std::string snapshot(const Graph &graph) {
  std::ostringstream out;
  for (auto a : graph.validBlockAddresses()) out << "b" << a << ';';
  for (auto a : graph.validFunctionAddresses()) out << "f" << a << ';';
  for (const auto &[a,i] : graph.listing()) {
    out << a << ':' << i.isBlockStart() << i.isFunctionStart() << ':';
    for (auto byte : i.bytes()) out << unsigned(byte) << ',';
    for (auto t : i.branches()) out << "j" << t << ';';
    for (auto t : i.callees()) out << "c" << t << ';';
  }
  for (auto a : graph.validBlockAddresses()) if (const auto *block = graph.getIGNILBlock(a)) {
    out << ignil::toString(*block);
    for (auto pc : block->unlifted()) out << "u" << pc << ';';
    for (const auto &op : block->ops()) out << op.address << ':' << op.aux << ';';
  }
  return out.str();
}
} // namespace

TEST_CASE("Static decoded regions dispatch actual targets without publishing Graph edges", "[llvm][static-region]") {
  for (auto arch : architectures)
    for (auto strategy : {DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(arch,strategy);
      auto graph = sample(arch,strategy);const Function root{&graph,rootPC};
      REQUIRE(root.isValid());REQUIRE(root.blocks().size() == 1);
      REQUIRE(graph.get(rootPC));REQUIRE(graph.get(rootPC)->branches().empty());
      const auto before = snapshot(graph);
      const std::array<std::uint64_t,2> additional{extraPC,vectorPC};
      LLVMLifter lifter{arch};configure(lifter);
      REQUIRE(lifter.liftStaticRegion(root,graph,additional));
      auto &f = function(lifter,arch);verify(lifter);
      CHECK(f.getFnAttribute("dragon.static-coverage").getValueAsString() == "decoded-region");
      CHECK(f.getFnAttribute("dragon.coverage-blocks").getValueAsString() == "1000,2000,2100");
      auto *sw = dispatch(f,{rootPC,extraPC,vectorPC});REQUIRE(sw);
      const auto values = exitValues(*sw->getDefaultDest());
      constantExit(values,offsetof(ExecExitV2,reason),DRAGON_EXIT_TRANSFER);
      constantExit(values,offsetof(ExecExitV2,transfer_kind),DRAGON_TRANSFER_BRANCH);
      constantExit(values,offsetof(ExecExitV2,source_pc),rootPC);
      constantExit(values,offsetof(ExecExitV2,next_pc_valid),1);
      constantExit(values,offsetof(ExecExitV2,return_pc),0);
      constantExit(values,offsetof(ExecExitV2,detail),0);
      REQUIRE(values.contains(offsetof(ExecExitV2,next_pc)));
      const auto *next = values.at(offsetof(ExecExitV2,next_pc));
      if (const auto *zext = llvm::dyn_cast<llvm::ZExtInst>(next)) next = zext->getOperand(0);
      CHECK(next == sw->getCondition());
      CHECK(writesRegister(f,8)); // ECX/X1 occurs only in the additional region
      CHECK(writesRegister(f,arch == Architecture::ARM64 ? 768 : 151));
      if (arch != Architecture::ARM64) CHECK(writesRegister(f,279)); // XMM1 high half
      CHECK(snapshot(graph) == before);
      CHECK(Function(&graph,rootPC).blocks().size() == 1);
      CHECK(graph.get(rootPC)->branches().empty());
      CHECK(lifter.ir().find("unreachable") == std::string::npos);
    }
}

TEST_CASE("Static decoded region coverage does not widen external entries or ordinary lift", "[llvm][static-region]") {
  for (auto arch : architectures) {
    auto graph = sample(arch,DisassemblyStrategy::LINEAR_SWEEP);
    const Function root{&graph,rootPC};const std::array<std::uint64_t,1> additional{extraPC};
    LLVMLifter ordinary{arch};configure(ordinary);REQUIRE(ordinary.lift(root,graph));
    CHECK(dispatch(function(ordinary,arch),{rootPC,extraPC}) == nullptr);
    LLVMLifter region{arch};configure(region);REQUIRE(region.liftStaticRegion(root,graph,additional));
    auto &f = function(region,arch);
    const auto *entry = llvm::dyn_cast<llvm::SwitchInst>(f.getEntryBlock().getTerminator());REQUIRE(entry);
    CHECK((cases(*entry) == std::set<std::uint64_t>{rootPC}));
    CHECK(f.getFnAttribute("dragon.entrypoints").getValueAsString() == "1000");
    CHECK(f.getFnAttribute("dragon.static-entry-policy").getValueAsString() == "function-start");
    const auto values = exitValues(*entry->getDefaultDest());
    constantExit(values,offsetof(ExecExitV2,reason),DRAGON_EXIT_UNSUPPORTED);
    constantExit(values,offsetof(ExecExitV2,detail),DRAGON_DETAIL_ENTRY);
    constantExit(values,offsetof(ExecExitV2,transfer_kind),DRAGON_TRANSFER_NONE);
    constantExit(values,offsetof(ExecExitV2,next_pc_valid),1);
    verify(region);
  }
}

TEST_CASE("Static decoded regions distinguish cross function jumps from calls", "[llvm][static-region]") {
  for (auto arch : architectures) for (bool call : {false,true}) {
    CAPTURE(arch,call);
    Graph graph{arch,{}};add(graph,direct(arch,call),rootPC);add(graph,scalarBody(arch),extraPC);
    const Function root{&graph,rootPC};REQUIRE(root.isValid());
    REQUIRE(graph.isValidFunction(extraPC));CHECK_FALSE(root.blocks().contains(extraPC));
    const auto before = snapshot(graph);
    const std::array<std::uint64_t,1> additional{extraPC};
    LLVMLifter lifter{arch};configure(lifter);REQUIRE(lifter.liftStaticRegion(root,graph,additional));
    auto &f = function(lifter,arch);unsigned calls = 0;
    for (const auto &bb : f) for (const auto &i : bb)
      if (const auto *c = llvm::dyn_cast<llvm::CallInst>(&i))
        if (c->getCalledFunction() && c->getCalledFunction()->getName() == symbol(arch,"2000")) ++calls;
    CHECK(calls == (call ? 1u : 0u));
    auto *callee = lifter.module().getFunction(symbol(arch,"2000"));
    if (call) { REQUIRE(callee);CHECK(callee->isDeclaration()); }
    else CHECK(callee == nullptr);
    CHECK(snapshot(graph) == before);verify(lifter);
  }
}

TEST_CASE("Static decoded regions reject invalid additional starts without module mutation", "[llvm][static-region]") {
  for (auto arch : architectures) for (unsigned invalid : {0u,1u,2u}) {
    CAPTURE(arch,invalid);
    Graph graph{arch,{}};add(graph,jumpRegister(arch),rootPC);
    std::uint64_t bad = extraPC;
    if (invalid == 0) { // a real decoded block with no IGNIL
      Disassembler dis{arch,{}};
      for (const auto &i : dis.disassemble(ret(arch),extraPC)) graph.addInstruction(i);
      graph.addBlock(extraPC);
      REQUIRE(BasicBlock(&graph,extraPC).isValid());REQUIRE_FALSE(graph.getIGNILBlock(extraPC));
    } else {
      if (invalid == 2) {add(graph,scalarBody(arch),extraPC);bad = extraPC + 1;}
      graph.addBlock(bad);ignil::Block fake{bad};
      fake.append({ignil::Opcode::RET,std::nullopt,{},bad});graph.addIGNILBlock(std::move(fake));
      REQUIRE(graph.isValidBlock(bad));REQUIRE_FALSE(graph.get(bad));
    }
    const Function root{&graph,rootPC};REQUIRE(root.isValid());
    const std::array<std::uint64_t,1> additional{bad};
    LLVMLifter lifter{arch};configure(lifter);const auto before = lifter.ir();
    CHECK_FALSE(lifter.liftStaticRegion(root,graph,additional));
    CHECK(lifter.ir() == before);CHECK(lifter.module().empty());
    REQUIRE(lifter.lift(root,graph));verify(lifter); // refusal leaves a usable lifter
  }
}

TEST_CASE("Static decoded region unlifted blocks retain exact Unsupported boundaries", "[llvm][static-region]") {
  for (auto arch : architectures) {
    Graph graph{arch,{}};add(graph,jumpRegister(arch),rootPC);
    if (arch == Architecture::ARM64) add(graph,Bytes{0x20,0x14,0xc0,0xda,0xc0,3,0x5f,0xd6},extraPC);
    else { // actual MOVBE store: zero IGNIL operations but an observable memory effect
      Disassembler dis{arch,{}};auto listing = dis.disassemble(Bytes{0x0f,0x38,0xf1,3},extraPC);
      REQUIRE(listing.size() == 1);
      graph.addInstruction(listing.front());graph.addBlock(extraPC);
      graph.addIGNILBlock(dis.liftTrace(listing));
      REQUIRE(graph.getIGNILBlock(extraPC)->ops().empty());
    }
    const auto *ir = graph.getIGNILBlock(extraPC);REQUIRE(ir);
    REQUIRE((ir->unlifted() == std::vector<std::uint64_t>{extraPC}));
    const std::array<std::uint64_t,1> additional{extraPC};
    LLVMLifter lifter{arch};configure(lifter);
    REQUIRE(lifter.liftStaticRegion(Function(&graph,rootPC),graph,additional));
    auto &f = function(lifter,arch);unsigned found = 0;
    for (const auto &bb : f) {
      const auto values = exitValues(bb);
      const auto reason = values.find(offsetof(ExecExitV2,reason));
      if (reason == values.end()) continue;
      const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(reason->second);
      if (!constant || constant->getZExtValue() != DRAGON_EXIT_UNSUPPORTED) continue;
      const auto detail = values.find(offsetof(ExecExitV2,detail));
      if (detail == values.end()) continue;
      const auto *kind = llvm::dyn_cast<llvm::ConstantInt>(detail->second);
      if (!kind || kind->getZExtValue() != DRAGON_DETAIL_INSTRUCTION) continue;
      ++found;
      constantExit(values,offsetof(ExecExitV2,source_pc),extraPC);
      constantExit(values,offsetof(ExecExitV2,next_pc),extraPC);
      constantExit(values,offsetof(ExecExitV2,next_pc_valid),1);
      constantExit(values,offsetof(ExecExitV2,return_pc),0);
    }
    CHECK(found == 1);verify(lifter);
  }
}

TEST_CASE("Static decoded region limits and execution modes fail closed", "[llvm][static-region]") {
  auto graph = sample(Architecture::X86_64,DisassemblyStrategy::LINEAR_SWEEP);
  const Function root{&graph,rootPC};const std::array<std::uint64_t,2> additional{extraPC,vectorPC};
  for (auto mode : {ExecutionMode::Legacy,ExecutionMode::StatefulV2,ExecutionMode::StaticAnalysis}) {
    LLVMLifter lifter{Architecture::X86_64};REQUIRE(lifter.setExecutionMode(mode));
    const auto before = lifter.ir();CHECK_FALSE(lifter.liftStaticRegion(root,graph,additional));
    CHECK(lifter.ir() == before);
  }
  for (auto limits : {StaticRegionOptions{0,65536}, StaticRegionOptions{4096,0},
                      StaticRegionOptions{4097,65536}, StaticRegionOptions{4096,65537},
                      StaticRegionOptions{2,65536}, StaticRegionOptions{4096,2}}) {
    LLVMLifter lifter{Architecture::X86_64};configure(lifter);const auto before = lifter.ir();
    CHECK_FALSE(lifter.liftStaticRegion(root,graph,additional,limits));CHECK(lifter.ir() == before);
  }
  LLVMLifter sufficient{Architecture::X86_64};configure(sufficient);
  REQUIRE(sufficient.liftStaticRegion(root,graph,additional,StaticRegionOptions{3,3}));
  verify(sufficient);
  auto differentGraph = graph.clone();
  LLVMLifter borrowed{Architecture::X86_64};configure(borrowed);
  const auto empty = borrowed.ir();
  CHECK_FALSE(borrowed.liftStaticRegion(root,differentGraph,additional));
  CHECK(borrowed.ir() == empty); // Function views must belong to the supplied Graph

  Graph twoJumps{Architecture::X86_64,{}};
  add(twoJumps,jumpRegister(Architecture::X86_64),rootPC);
  add(twoJumps,jumpRegister(Architecture::X86_64),extraPC);
  add(twoJumps,ret(Architecture::X86_64),vectorPC);
  const Function twoRoot{&twoJumps,rootPC};
  LLVMLifter counted{Architecture::X86_64};configure(counted);
  const auto before = counted.ir();
  CHECK_FALSE(counted.liftStaticRegion(twoRoot,twoJumps,additional,StaticRegionOptions{3,5}));
  CHECK(counted.ir() == before); // 2 dispatches x 3 blocks is a total of 6 cases
  REQUIRE(counted.liftStaticRegion(twoRoot,twoJumps,additional,StaticRegionOptions{3,6}));
  verify(counted);
}

TEST_CASE("Failed static region bodies preserve prior definitions and callee declarations", "[llvm][static-region]") {
  for (auto arch : architectures) {
    Graph graph{arch,{}};add(graph,direct(arch,true),rootPC);add(graph,ret(arch),extraPC);
    add(graph,scalarBody(arch),0x3000);
    ignil::Block invalid{0x3000};
    invalid.append({ignil::Opcode::COPY,ignil::Reg{8,32,"destination"},{ignil::Temp{999,32}},0x3000});
    auto *badIR = graph.getIGNILBlock(0x3000);REQUIRE(badIR);
    *badIR = std::move(invalid);
    REQUIRE(badIR->ops().size() == 1);REQUIRE(badIR->ops().front().srcs.size() == 1);
    const auto *missing = std::get_if<ignil::Temp>(&badIR->ops().front().srcs.front());
    REQUIRE(missing);REQUIRE(missing->id == 999);REQUIRE(missing->width == 32);
    LLVMLifter lifter{arch};configure(lifter);
    REQUIRE(lifter.lift(Function(&graph,rootPC),graph));
    auto *caller = lifter.module().getFunction(symbol(arch));REQUIRE(caller);REQUIRE_FALSE(caller->isDeclaration());
    auto *callee = lifter.module().getFunction(symbol(arch,"2000"));REQUIRE(callee);REQUIRE(callee->isDeclaration());
    const auto before = lifter.ir();const std::array<std::uint64_t,1> bad{0x3000};
    CHECK_FALSE(lifter.liftStaticRegion(Function(&graph,extraPC),graph,bad));
    CHECK(lifter.ir() == before);
    CHECK(lifter.module().getFunction(symbol(arch)) == caller);
    CHECK(lifter.module().getFunction(symbol(arch,"2000")) == callee);
    CHECK(callee->isDeclaration());CHECK_FALSE(caller->isDeclaration());
    CHECK_FALSE(lifter.liftStaticRegion(Function(&graph,rootPC),graph,bad));
    CHECK(lifter.ir() == before); // an existing same-address definition is never deleted
    REQUIRE(lifter.lift(Function(&graph,extraPC),graph));
    CHECK(lifter.module().getFunction(symbol(arch,"2000")) == callee);
    CHECK_FALSE(callee->isDeclaration());verify(lifter);
  }
}

TEST_CASE("Static regions optionally rebuild orphan decoded instructions on a private graph", "[llvm][static-region]") {
  for (auto arch : architectures) {
    CAPTURE(arch);
    Graph graph{arch,{}};add(graph,jumpRegister(arch),rootPC);
    Disassembler dis{arch,{}};
    const auto decoded = dis.disassemble(scalarBody(arch),extraPC);
    REQUIRE(decoded.size() == 2);
    for (const auto &instruction : decoded) graph.addInstruction(instruction);
    REQUIRE(graph.get(extraPC));REQUIRE_FALSE(graph.isValidBlock(extraPC));
    REQUIRE_FALSE(graph.getIGNILBlock(extraPC));
    const Function root{&graph,rootPC};REQUIRE(root.isValid());
    const auto before = snapshot(graph);
    const std::array<std::uint64_t,1> additional{extraPC};
    LLVMLifter strict{arch};configure(strict);const auto empty = strict.ir();
    CHECK_FALSE(strict.liftStaticRegion(root,graph,additional));CHECK(strict.ir() == empty);

    StaticRegionOptions options;options.rebuildDecodedBlocks = true;
    LLVMLifter rebuilt{arch};configure(rebuilt);
    REQUIRE(rebuilt.liftStaticRegion(root,graph,additional,options));
    auto &f = function(rebuilt,arch);REQUIRE(dispatch(f,{rootPC,extraPC}));
    CHECK(writesRegister(f,8));verify(rebuilt);
    CHECK(snapshot(graph) == before);
    CHECK_FALSE(graph.isValidBlock(extraPC));CHECK_FALSE(graph.getIGNILBlock(extraPC));
    CHECK(graph.get(rootPC)->branches().empty());

    for (auto invalid : {extraPC + 1, std::uint64_t{0x4000}}) {
      REQUIRE_FALSE(graph.get(invalid));
      LLVMLifter rejected{arch};configure(rejected);const auto old = rejected.ir();
      const std::array<std::uint64_t,1> bad{invalid};
      CHECK_FALSE(rejected.liftStaticRegion(root,graph,bad,options));
      CHECK(rejected.ir() == old);CHECK(snapshot(graph) == before);
    }
  }
}

TEST_CASE("Static region rebuilding splits decoded instruction boundaries without stale prefix IR", "[llvm][static-region]") {
  for (auto arch : architectures) {
    CAPTURE(arch);
    // The requested instruction is inside the original single block. Once
    // split, the prefix must fall through, rather than retain its old full
    // block IGNIL containing the suffix assignment and indirect JMP.
    const Bytes bytes = arch == Architecture::ARM64
        ? Bytes{0xe1,0,0x80,0xd2, 1,0,0x80,0xd2, 0,0,0x1f,0xd6}
        : Bytes{0xb9,7,0,0,0, 0x31,0xc9, 0xff,0xe0};
    const auto splitPC = rootPC + (arch == Architecture::ARM64 ? 4 : 5);
    Graph graph{arch,{}};add(graph,bytes,rootPC);
    const Function root{&graph,rootPC};REQUIRE(root.isValid());REQUIRE(root.blocks().size() == 1);
    REQUIRE(graph.get(splitPC));REQUIRE_FALSE(graph.isValidBlock(splitPC));
    REQUIRE_FALSE(graph.getIGNILBlock(splitPC));
    const auto before = snapshot(graph);
    StaticRegionOptions options;options.rebuildDecodedBlocks = true;
    const std::array<std::uint64_t,1> additional{splitPC};
    LLVMLifter lifter{arch};configure(lifter);
    REQUIRE(lifter.liftStaticRegion(root,graph,additional,options));
    auto &f = function(lifter,arch);
    llvm::BasicBlock *prefix = nullptr, *suffix = nullptr;
    for (auto &bb : f) {
      if (bb.getName() == "bb_" + std::to_string(rootPC)) prefix = &bb;
      if (bb.getName() == "bb_" + std::to_string(splitPC)) suffix = &bb;
    }
    REQUIRE(prefix);REQUIRE(suffix);
    const auto *fall = llvm::dyn_cast<llvm::BranchInst>(prefix->getTerminator());
    REQUIRE(fall);REQUIRE(fall->isUnconditional());CHECK(fall->getSuccessor(0) == suffix);
    REQUIRE(dispatch(f,{rootPC,splitPC}));
    verify(lifter);CHECK(snapshot(graph) == before);
    CHECK(Function(&graph,rootPC).blocks().size() == 1);
    CHECK_FALSE(graph.isValidBlock(splitPC));CHECK_FALSE(graph.getIGNILBlock(splitPC));
  }
}

TEST_CASE("Static region rebuilding rejects machine bytes without decoder semantics", "[llvm][static-region]") {
  for (auto arch : architectures) {
    CAPTURE(arch);
    Graph graph{arch,{}};add(graph,jumpRegister(arch),rootPC);
    const Bytes bytes = arch == Architecture::ARM64 ? Bytes{0x60,6,0,0xf9} : Bytes{0x89,0x43,8};
    graph.addInstruction(Instruction(extraPC,static_cast<std::uint8_t>(bytes.size()),
        "store","",bytes,false,false,false,false,false,false));
    REQUIRE(graph.get(extraPC));REQUIRE_FALSE(graph.get(extraPC)->mcInst());
    Disassembler dis{arch,{}};
    for (const auto &i : dis.disassemble(ret(arch),extraPC + bytes.size())) graph.addInstruction(i);
    const auto before = snapshot(graph);
    const std::array<std::uint64_t,1> additional{extraPC};
    StaticRegionOptions options;options.rebuildDecodedBlocks = true;
    LLVMLifter lifter{arch};configure(lifter);const auto empty = lifter.ir();
    CHECK_FALSE(lifter.liftStaticRegion(Function(&graph,rootPC),graph,additional,options));
    CHECK(lifter.ir() == empty);CHECK(lifter.module().empty());CHECK(snapshot(graph) == before);
    CHECK_FALSE(graph.isValidBlock(extraPC));CHECK_FALSE(graph.getIGNILBlock(extraPC));
  }
}
