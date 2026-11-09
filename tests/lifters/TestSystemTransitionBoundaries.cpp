#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/lifters/ignil/IGNILSimplify.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include "../../lib/LLVMImportMetadata.hpp"
#ifdef DRAGON_WITH_Z3
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#endif
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace dragon;
using namespace dragon::analysis;
using namespace dragon::llvmlifter;

namespace {
using O = ignil::Opcode;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t entry = 0x1000, cell = 0x4000;
// Appended ABI opcode. Use its numeric tail value so tests-only builds can
// compile against the old public header, which has no UNSUPPORTED enumerator.
constexpr O unsupported = static_cast<O>(72);
struct MachineCase { const char *name; Architecture arch; Bytes code; };

const std::vector<MachineCase> &boundaries() {
  static const std::vector<MachineCase> cases{
    {"x64 syscall",Architecture::X86_64,{0x0f,0x05}},
    {"x64 sysenter",Architecture::X86_64,{0x0f,0x34}},
    {"x64 sysretq",Architecture::X86_64,{0x48,0x0f,0x07}},
    {"x64 iretq",Architecture::X86_64,{0x48,0xcf}},
    {"x64 sysret32",Architecture::X86_64,{0x0f,0x07}},
    {"x64 sysexit32",Architecture::X86_64,{0x0f,0x35}},
    {"x64 sysexitq",Architecture::X86_64,{0x48,0x0f,0x35}},
    {"i386 int80",Architecture::X86_32,{0xcd,0x80}},
    {"i386 sysenter",Architecture::X86_32,{0x0f,0x34}},
    {"i386 iret",Architecture::X86_32,{0xcf}},
    {"i386 sysexit",Architecture::X86_32,{0x0f,0x35}},
    {"a64 svc",Architecture::ARM64,{0x01,0x00,0x00,0xd4}},
    {"a64 hvc",Architecture::ARM64,{0x02,0x00,0x00,0xd4}},
    {"a64 smc",Architecture::ARM64,{0x03,0x00,0x00,0xd4}},
    {"a64 eret",Architecture::ARM64,{0xe0,0x03,0x9f,0xd6}},
    {"a64 drps",Architecture::ARM64,{0xe0,0x03,0xbf,0xd6}},
    {"x64 ud2",Architecture::X86_64,{0x0f,0x0b}},
    {"i386 ud2",Architecture::X86_32,{0x0f,0x0b}},
    {"a64 brk",Architecture::ARM64,{0x00,0x00,0x20,0xd4}},
    {"x64 int3",Architecture::X86_64,{0xcc}},
    {"i386 int3",Architecture::X86_32,{0xcc}},
    {"i386 into",Architecture::X86_32,{0xce}},
    {"x64 hlt",Architecture::X86_64,{0xf4}},
    {"i386 hlt",Architecture::X86_32,{0xf4}},
  };
  // LLVM 21 does not decode INT1 (F1) in either x86 mode or INTO in x64.
  // They are not claimed as decoded coverage or silently counted as passes.
  return cases;
}

Bytes joined(Bytes a, const Bytes &b) { a.insert(a.end(),b.begin(),b.end());return a; }
Bytes ret(Architecture arch) { return arch == Architecture::ARM64 ? Bytes{0xc0,3,0x5f,0xd6} : Bytes{0xc3}; }
Bytes value(Architecture arch, bool suffix) {
  if (arch == Architecture::ARM64)
    return suffix ? Bytes{0x80,0xca,0x8e,0xd2} : Bytes{0x80,0x46,0x82,0xd2}; // X0=7654/1234
  return suffix ? Bytes{0xb8,0xe0,0xac,0x68,0x24} : Bytes{0xb8,0xdf,0x9b,0x57,0x13};
}
Graph decode(Architecture arch, const Bytes &bytes,
             DisassemblyStrategy strategy = DisassemblyStrategy::LINEAR_SWEEP) {
  Configuration config;config.disassemblyStrategy = strategy;
  Graph graph{arch,config};Disassembler dis{arch,config};dis.disassemble(bytes,entry,graph);return graph;
}
bool marked(const ignil::Block &block,std::uint64_t pc) {
  return std::find(block.unlifted().begin(),block.unlifted().end(),pc) != block.unlifted().end();
}
bool barrier(const ignil::Op &op) { return op.opcode == unsupported; }
void add(Graph &graph,ignil::Block block) {
  const auto address=block.startAddress();graph.addIGNILBlock(std::move(block));graph.addBlock(address);
}
void unknown(const FiniteValues &value) {
  CHECK(value.kind == ValueSetKind::Unknown);CHECK(value.values.empty());
}
void verify(LLVMLifter &lifter) { CHECK_FALSE(llvm::verifyModule(lifter.module())); }

std::map<std::size_t,std::uint64_t> exitConstants(const llvm::BasicBlock &block,
    const llvm::Function &function,const llvm::Module &module) {
  std::map<std::size_t,std::uint64_t> fields;
  for (const auto &instruction : block) {
    const auto *store=llvm::dyn_cast<llvm::StoreInst>(&instruction);
    if (!store) continue;
    const auto *constant=llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
    if (!constant || constant->getBitWidth()>64) continue;
    std::int64_t offset=0;
    const auto *base=llvm::GetPointerBaseWithConstantOffset(store->getPointerOperand(),offset,module.getDataLayout());
    if (base == function.getArg(3) && offset>=0) fields[static_cast<std::size_t>(offset)]=constant->getZExtValue();
  }
  return fields;
}
void preciseExit(LLVMLifter &lifter,Architecture arch,ExecutionMode mode,std::uint64_t pc) {
  const auto name=std::string(mode == ExecutionMode::StatefulV2 ? "dragon_v2_" : "dragon_static_entry_")+
      ArchitectureName(arch)+"_1000";
  const auto *function=lifter.module().getFunction(name);REQUIRE(function);REQUIRE(function->arg_size()==4);
  unsigned found=0;
  for (const auto &block : *function) {
    const auto fields=exitConstants(block,*function,lifter.module());
    if (!fields.contains(offsetof(ExecExitV2,reason)) || !fields.contains(offsetof(ExecExitV2,source_pc)) ||
        fields.at(offsetof(ExecExitV2,reason))!=DRAGON_EXIT_UNSUPPORTED ||
        fields.at(offsetof(ExecExitV2,source_pc))!=pc) continue;
    ++found;
    for (const auto offset : {offsetof(ExecExitV2,transfer_kind),offsetof(ExecExitV2,next_pc),
         offsetof(ExecExitV2,next_pc_valid),offsetof(ExecExitV2,return_pc),offsetof(ExecExitV2,detail)}) REQUIRE(fields.contains(offset));
    CHECK(fields.at(offsetof(ExecExitV2,transfer_kind))==DRAGON_TRANSFER_NONE);
    CHECK(fields.at(offsetof(ExecExitV2,next_pc))==pc);
    CHECK(fields.at(offsetof(ExecExitV2,next_pc_valid))==1);
    CHECK(fields.at(offsetof(ExecExitV2,return_pc))==0);
    CHECK(fields.at(offsetof(ExecExitV2,detail))==DRAGON_DETAIL_INSTRUCTION);
    CHECK(llvm::isa<llvm::ReturnInst>(block.getTerminator()));
  }
  CHECK(found==1);verify(lifter);
}
bool hasConstant(LLVMLifter &lifter,std::uint64_t wanted) {
  for (const auto &function : lifter.module()) for (const auto &block : function)
    for (const auto &instruction : block) for (const auto &operand : instruction.operands())
      if (const auto *constant=llvm::dyn_cast<llvm::ConstantInt>(operand.get()))
        if (constant->getBitWidth()<=64 && constant->getZExtValue()==wanted) return true;
  return false;
}
void configure(LLVMLifter &lifter,ExecutionMode mode) {
  REQUIRE(lifter.setExecutionMode(mode));
  if (mode==ExecutionMode::StaticAnalysis) REQUIRE(lifter.setStaticEntryPolicy(StaticEntryPolicy::FunctionStart));
}
} // namespace

TEST_CASE("System transitions and traps emit an explicit decoded boundary",
          "[system-transition][ignil]") {
  CHECK(ignil::opcodeName(unsupported)=="UNSUPPORTED");
  for (const auto &test : boundaries()) {
    CAPTURE(test.name,test.arch);
    Disassembler dis{test.arch,{}};
    const auto instructions=dis.disassemble(test.code,entry);
    REQUIRE(instructions.size()==1);CHECK(instructions.front().size()==test.code.size());
    const auto trace=dis.liftTrace(instructions);
    CHECK(marked(trace,entry));
    CHECK(std::count_if(trace.begin(),trace.end(),barrier)==1);
    for (const auto &op : trace) if (barrier(op)) {
      CHECK(op.address==entry);CHECK_FALSE(op.dst);CHECK(op.srcs.empty());
    }
    CHECK(std::none_of(trace.begin(),trace.end(),[](const auto &op) {
      return op.opcode==O::CALL || op.opcode==O::RET;
    }));
    for (const auto strategy : {DisassemblyStrategy::LINEAR_SWEEP,DisassemblyStrategy::RECURSIVE_TRAVERSAL}) {
      CAPTURE(strategy);
      auto graph=decode(test.arch,joined(test.code,ret(test.arch)),strategy);
      REQUIRE(graph.get(entry));REQUIRE(graph.getIGNILBlock(entry));
      const auto &block=*graph.getIGNILBlock(entry);
      CHECK(marked(block,entry));CHECK(std::any_of(block.begin(),block.end(),barrier));
    }
  }
}

TEST_CASE("Ordinary calls returns nops and fences are not system boundaries",
          "[system-transition][controls]") {
  for (auto arch : {Architecture::X86_32,Architecture::X86_64,Architecture::ARM64}) {
    const std::vector<Bytes> cases=arch==Architecture::ARM64
        ? std::vector<Bytes>{{0xc0,3,0x3f,0xd6},{0xc0,3,0x5f,0xd6},{0x1f,0x20,3,0xd5}}
        : std::vector<Bytes>{{0xe8,0,0,0,0},{0xff,0xd0},{0xc3},{0x90},{0xf3,0x90},
                            {0x0f,0xae,0xf0},{0x0f,0xae,0xe8},{0x0f,0xae,0xf8}};
    for (std::size_t index=0;index<cases.size();++index) {
      CAPTURE(arch,index);Disassembler dis{arch,{}};
      const auto instructions=dis.disassemble(cases[index],entry);REQUIRE(instructions.size()==1);
      const auto block=dis.liftTrace(instructions);CHECK(block.unlifted().empty());
      CHECK(std::none_of(block.begin(),block.end(),barrier));
      if (index==0) CHECK(std::any_of(block.begin(),block.end(),[](const auto &op){return op.opcode==O::CALL;}));
      if ((arch==Architecture::ARM64 && index==1) || (arch!=Architecture::ARM64 && index==2))
        CHECK(std::any_of(block.begin(),block.end(),[](const auto &op){return op.opcode==O::RET;}));
    }
  }
}

TEST_CASE("LLVM modes stop at the exact decoded system instruction",
          "[system-transition][llvm]") {
  for (const auto &test : boundaries()) {
    CAPTURE(test.name,test.arch);
    const auto prefix=value(test.arch,false),suffix=value(test.arch,true);
    auto graph=decode(test.arch,joined(joined(joined(prefix,test.code),suffix),ret(test.arch)));
    const Function function{&graph,entry};REQUIRE(function.isValid());
    const auto pc=entry+prefix.size();REQUIRE(graph.get(pc));
    LLVMLifter legacy{test.arch};const auto before=legacy.ir();
    CHECK_FALSE(legacy.lift(function,graph));CHECK(legacy.ir()==before);CHECK(legacy.module().empty());
    for (auto mode : {ExecutionMode::StatefulV2,ExecutionMode::StaticAnalysis}) {
      CAPTURE(mode);LLVMLifter lifter{test.arch};configure(lifter,mode);
      REQUIRE(lifter.lift(function,graph));preciseExit(lifter,test.arch,mode,pc);
      CHECK(hasConstant(lifter,test.arch==Architecture::ARM64 ? 0x1234 : 0x13579bdf));
      CHECK_FALSE(hasConstant(lifter,test.arch==Architecture::ARM64 ? 0x7654 : 0x2468ace0));
    }
  }
}

TEST_CASE("Explicit unsupported without a marker remains an LLVM boundary",
          "[system-transition][llvm][synthetic]") {
  auto graph=decode(Architecture::X86_64,{0xb8,7,0,0,0,0x90,0xb8,9,0,0,0,0xc3});
  REQUIRE(graph.getIGNILBlock(entry));
  ignil::Block block{entry};
  block.append({O::COPY,x86::RAX,{ignil::Const{7,64}},entry});
  block.append({unsupported,std::nullopt,{},entry+5});
  block.append({O::COPY,x86::RAX,{ignil::Const{9,64}},entry+6});
  block.append({O::RET,std::nullopt,{},entry+11});
  *graph.getIGNILBlock(entry)=block;REQUIRE(graph.getIGNILBlock(entry)->unlifted().empty());
  LLVMLifter legacy{Architecture::X86_64};const auto before=legacy.ir();
  CHECK_FALSE(legacy.lift(Function{&graph,entry},graph));CHECK(legacy.ir()==before);
  for (auto mode : {ExecutionMode::StatefulV2,ExecutionMode::StaticAnalysis}) {
    LLVMLifter lifter{Architecture::X86_64};configure(lifter,mode);
    REQUIRE(lifter.lift(Function{&graph,entry},graph));preciseExit(lifter,Architecture::X86_64,mode,entry+5);
    CHECK(hasConstant(lifter,7));CHECK_FALSE(hasConstant(lifter,9));
  }
}

TEST_CASE("IGNIL optimizers preserve the complete unsupported prefix",
          "[system-transition][optimizer][synthetic]") {
  ignil::Block block{entry};
  block.append({O::COPY,ignil::Temp{0,64},{ignil::Const{7,64}},entry});
  block.append({O::ADD,x86::RAX,{ignil::Temp{0,64},ignil::Const{0,64}},entry});
  block.append({O::STORE,std::nullopt,{ignil::Const{cell,64},x86::RAX},entry});
  block.append({unsupported,std::nullopt,{},entry+1});
  block.append({O::COPY,x86::RAX,{ignil::Const{9,64}},entry+2});
  block.append({O::RET,std::nullopt,{},entry+3});
  REQUIRE(block.unlifted().empty());
  const auto text=ignil::toString(block);
  const std::array outputs{ignil::simplifyBlock(block),ignil::deadStoreEliminate(block),
      ignil::deadStoreEliminate(block,std::vector<ignil::Reg>{}),
      ignil::deadStoreEliminate(ignil::simplifyBlock(block))};
  for (const auto &output : outputs) {
    CHECK(ignil::toString(output)==text);CHECK(output.unlifted().empty());
    REQUIRE(std::count_if(output.begin(),output.end(),barrier)==1);
    const auto it=std::find_if(output.begin(),output.end(),barrier);
    CHECK(it->address==entry+1);CHECK_FALSE(it->dst);CHECK(it->srcs.empty());
  }
}

TEST_CASE("Import provenance stops at an explicit boundary without a marker",
          "[system-transition][llvm][imports][synthetic]") {
  for (bool boundary : {false,true}) {
    CAPTURE(boundary);
    // NOP; CALL RAX; RET supplies a real transfer PC. Only the IGNIL is
    // replaced; import naming must not walk the stopped suffix.
    auto graph=decode(Architecture::X86_64,{0x90,0xff,0xd0,0xc3});
    dragon::detail::GraphLLVMMetadata::setImports(graph,{{cell,"test_import"}});
    ignil::Block block{entry};
    if (boundary) block.append({unsupported,std::nullopt,{},entry});
    block.append({O::LOAD,x86::RAX,{ignil::Const{cell,64}},entry});
    block.append({O::CALL,std::nullopt,{x86::RAX},entry+1});
    *graph.getIGNILBlock(entry)=std::move(block);
    REQUIRE(graph.getIGNILBlock(entry)->unlifted().empty());
    for (auto mode : {ExecutionMode::StatefulV2,ExecutionMode::StaticAnalysis}) {
      LLVMLifter lifter{Architecture::X86_64};configure(lifter,mode);
      REQUIRE(lifter.lift(Function{&graph,entry},graph));verify(lifter);
      const auto *imports=lifter.module().getNamedMetadata("dragon.imports");
      if (boundary) CHECK(imports==nullptr);
      else { REQUIRE(imports);CHECK(imports->getNumOperands()==1); }
    }
  }
}

TEST_CASE("LLVM refuses malformed unsupported boundaries before module mutation",
          "[system-transition][llvm][synthetic]") {
  for (unsigned shape=0;shape<3;++shape) {
    CAPTURE(shape);
    auto graph=decode(Architecture::X86_64,{0x90,0xc3});
    ignil::Block block{entry};
    ignil::Op op{unsupported,std::nullopt,{},entry};
    if (shape==0) op.dst=x86::RAX;
    if (shape==1) op.srcs={x86::RAX};
    if (shape==2) op.aux=1;
    block.append(op);block.append({O::RET,std::nullopt,{},entry+1});
    *graph.getIGNILBlock(entry)=std::move(block);
    for (auto mode : {ExecutionMode::Legacy,ExecutionMode::StatefulV2,ExecutionMode::StaticAnalysis}) {
      LLVMLifter lifter{Architecture::X86_64};configure(lifter,mode);
      const auto before=lifter.ir();
      CHECK_FALSE(lifter.lift(Function{&graph,entry},graph));
      CHECK(lifter.ir()==before);CHECK(lifter.module().empty());
    }
  }
}

TEST_CASE("Explicit unsupported suffixes cannot produce finite or constant facts",
          "[system-transition][finite-values][synthetic]") {
  for (bool literalTarget : {false,true}) for (bool proveCalls : {false,true}) {
    CAPTURE(literalTarget,proveCalls);Graph graph{Architecture::X86_64,{}};
    ignil::Block block{entry};block.append({O::COPY,x86::RAX,{ignil::Const{7,64}},entry});
    block.append({unsupported,std::nullopt,{},entry+1});
    block.append({O::COPY,x86::RAX,{ignil::Const{9,64}},entry+2});
    block.append({O::JMP,std::nullopt,{literalTarget ? ignil::Value{ignil::Const{0x2000,64}} : ignil::Value{x86::RAX}},entry+3});
    add(graph,std::move(block));REQUIRE(graph.getIGNILBlock(entry)->unlifted().empty());
    const std::array queries{FiniteValueQuery{entry,x86::RAX},FiniteValueQuery{entry,ignil::Const{9,64}}};
    FiniteValueLimits limits;limits.proveLeafCalls=proveCalls;
    const auto result=finiteValues(graph,{entry},queries,limits);REQUIRE(result.complete);
    REQUIRE(result.values.size()==2);for (const auto &v : result.values) unknown(v);
    REQUIRE(result.jumpTargets.contains(entry));unknown(result.jumpTargets.at(entry));
    CHECK_FALSE(std::holds_alternative<slice::Const>(BackwardSlicer{}.slice(x86::RAX,entry,graph).data));
  }
}

TEST_CASE("Initial memory does not cross an explicit unsupported operation",
          "[system-transition][finite-values][synthetic]") {
  Graph graph{Architecture::X86_64,{}};ignil::Block block{entry};
  block.append({unsupported,std::nullopt,{},entry});
  block.append({O::LOAD,ignil::Temp{0,64},{ignil::Const{cell,64}},entry+1});
  block.append({O::COPY,x86::RAX,{ignil::Temp{0,64}},entry+2});
  block.append({O::JMP,std::nullopt,{x86::RAX},entry+3});add(graph,std::move(block));
  const std::array state{FiniteEntryState{entry,{},{{cell,{7,0,0,0,0,0,0,0}}}}};
  const FiniteMemoryQuery point{entry,1,cell,64};
  const auto result=finiteMemoryValuesWithInitialState(graph,{entry},std::span{&point,1},state);
  REQUIRE(result.assumptionsValid);REQUIRE(result.analysis.complete);REQUIRE(result.analysis.values.size()==1);
  unknown(result.analysis.values[0]);unknown(result.analysis.jumpTargets.at(entry));
}

TEST_CASE("Unknown system paths remain incoming paths at a join",
          "[system-transition][finite-values][slicer][synthetic]") {
  for (bool marker : {false,true}) for (bool viaMemory : {false,true}) {
    CAPTURE(marker,viaMemory);Graph graph{Architecture::X86_64,{}};ignil::Block root{entry};
    root.append({O::CJMP,std::nullopt,{x86::ZF,ignil::Const{0x1010,64},ignil::Const{0x1020,64}},entry});add(graph,std::move(root));
    ignil::Block a{0x1010};a.append({unsupported,std::nullopt,{},0x1010});
    a.append({O::COPY,x86::RAX,{ignil::Const{9,64}},0x1011});
    a.append({O::STORE,std::nullopt,{ignil::Const{cell,64},ignil::Const{9,64}},0x1012});
    a.append({O::JMP,std::nullopt,{ignil::Const{0x1030,64}},0x1013});
    if (marker) a.noteUnlifted(0x1010);add(graph,std::move(a));
    ignil::Block b{0x1020};b.append({O::COPY,x86::RAX,{ignil::Const{0,64}},0x1020});
    b.append({O::STORE,std::nullopt,{ignil::Const{cell,64},ignil::Const{0,64}},0x1021});
    b.append({O::JMP,std::nullopt,{ignil::Const{0x1030,64}},0x1022});add(graph,std::move(b));
    ignil::Block join{0x1030};
    if (viaMemory) {
      join.append({O::LOAD,ignil::Temp{0,64},{ignil::Const{cell,64}},0x1030});
      join.append({O::COPY,x86::RAX,{ignil::Temp{0,64}},0x1030});
    }
    join.append({O::JMP,std::nullopt,{x86::RAX},0x1030});add(graph,std::move(join));
    auto preds=BackwardSlicer::predecessors(graph);REQUIRE(preds.contains(0x1030));
    auto incoming=preds.at(0x1030);std::sort(incoming.begin(),incoming.end());
    CHECK((incoming==std::vector<std::uint64_t>{0x1010,0x1020}));
    const std::array queries{FiniteValueQuery{0x1010,x86::RAX},FiniteValueQuery{0x1030,x86::RAX}};
    const auto result=finiteValues(graph,{entry},queries);REQUIRE(result.complete);REQUIRE(result.values.size()==2);
    for (const auto &v : result.values) unknown(v);unknown(result.jumpTargets.at(0x1030));
    CHECK_FALSE(std::holds_alternative<slice::Const>(BackwardSlicer{}.slice(x86::RAX,0x1030,graph).data));
    if (viaMemory) {
      const FiniteMemoryQuery point{0x1030,0,cell,64};
      const auto memory=finiteMemoryValues(graph,{entry},std::span{&point,1});REQUIRE(memory.complete);
      REQUIRE(memory.values.size()==1);unknown(memory.values[0]);
    }
  }
}

#ifdef DRAGON_WITH_Z3
namespace {
std::string hexPC(std::uint64_t pc) { std::ostringstream out;out<<"0x"<<std::hex<<pc;return out.str(); }
void stopped(const ignil::RunResult &result,std::uint64_t pc) {
  CHECK_FALSE(result.ok());CHECK(result.status==ignil::ExecStatus::UnsupportedOp);
  CHECK(result.exit==ignil::ExitKind::Error);CHECK(result.successors.empty());
  CHECK(result.error.find(hexPC(pc))!=std::string::npos);
}
}

TEST_CASE("Symbolic block ops and trace runners stop at real system instructions",
          "[system-transition][symbolic]") {
  for (const auto &test : boundaries()) for (unsigned runner=0;runner<3;++runner) {
    CAPTURE(test.name,test.arch,runner);
    const bool a64=test.arch==Architecture::ARM64,x32=test.arch==Architecture::X86_32;
    const auto prefix=a64 ? Bytes{0xe0,0,0x80,0xd2,0x60,2,0,0xf9} :
        x32 ? Bytes{0xb8,7,0,0,0,0x89,3} : Bytes{0xb8,7,0,0,0,0x48,0x89,3};
    const auto suffix=a64 ? Bytes{0x20,1,0x80,0xd2,0x60,2,0,0xf9} :
        x32 ? Bytes{0xb8,9,0,0,0,0x89,3} : Bytes{0xb8,9,0,0,0,0x48,0x89,3};
    const auto code=joined(joined(joined(prefix,test.code),suffix),ret(test.arch));
    const auto pc=entry+prefix.size();Disassembler dis{test.arch,{}};
    const auto instructions=dis.disassemble(code,entry);
    REQUIRE(std::any_of(instructions.begin(),instructions.end(),[&](const auto &i){return i.address()==pc;}));
    const auto trace=dis.liftTrace(instructions);auto graph=decode(test.arch,code);REQUIRE(graph.getIGNILBlock(entry));
    ignil::SymbolicOptions options;options.stopAtTerminator=runner!=2;
    ignil::SymbolicExecutor executor{options};
    const ignil::Reg output=a64 ? arm::X0 : x32 ? x86::EAX : x86::RAX;
    executor.setRegister(a64 ? arm::X19 : x32 ? x86::EBX : x86::RBX,cell);
    executor.setRegister(output,0x55);executor.mapMemory(cell,Bytes(8,0xa5));
    const auto result=runner==0 ? executor.run(*graph.getIGNILBlock(entry)) :
                      runner==1 ? executor.run(trace.ops()) : executor.run(trace);
    stopped(result,pc);
    CHECK(executor.evaluate(executor.readRegister(output))==7);
    CHECK(executor.evaluate(executor.readMemory(cell,x32 ? 32 : 64))==7);
  }
}

TEST_CASE("Synthetic unsupported cannot execute a suffix even in trace mode",
          "[system-transition][symbolic][synthetic]") {
  for (bool stopAtTerminator : {false,true}) for (bool vectorAPI : {false,true}) {
    ignil::Block block{entry};block.append({O::COPY,x86::RAX,{ignil::Const{7,64}},entry});
    block.append({O::STORE,std::nullopt,{ignil::Const{cell,64},ignil::Const{7,64}},entry});
    block.append({unsupported,std::nullopt,{},entry+1});
    block.append({O::COPY,x86::RAX,{ignil::Const{9,64}},entry+2});
    block.append({O::STORE,std::nullopt,{ignil::Const{cell,64},ignil::Const{9,64}},entry+2});
    block.append({O::RET,std::nullopt,{},entry+3});REQUIRE(block.unlifted().empty());
    ignil::SymbolicOptions options;options.stopAtTerminator=stopAtTerminator;
    ignil::SymbolicExecutor executor{options};executor.mapMemory(cell,Bytes(8,0xa5));
    const auto result=vectorAPI ? executor.run(block.ops()) : executor.run(block);
    stopped(result,entry+1);CHECK(result.stats.opsExecuted==3);
    CHECK(executor.evaluate(executor.readRegister(x86::RAX))==7);
    CHECK(executor.evaluate(executor.readMemory(cell,64))==7);
  }
}

TEST_CASE("Symbolic single step rejects explicit unsupported without state changes",
          "[system-transition][symbolic][synthetic]") {
  ignil::SymbolicExecutor executor;
  executor.setRegister(x86::RAX,7);
  executor.mapMemory(cell,Bytes{7,0,0,0,0,0,0,0});
  ignil::RunResult result;
  const ignil::Op op{unsupported,std::nullopt,{},entry+1};
  // step() reports status directly; run() supplies Error/PC diagnostics.
  CHECK(executor.step(op,result)==ignil::ExecStatus::UnsupportedOp);
  CHECK(result.successors.empty());
  CHECK(executor.evaluate(executor.readRegister(x86::RAX))==7);
  CHECK(executor.evaluate(executor.readMemory(cell,64))==7);
}
#endif
