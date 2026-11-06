#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/FiniteValues.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#ifdef DRAGON_WITH_Z3
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#endif
#include <algorithm>
#include <map>
#include <set>
#include <type_traits>
#include <vector>

using namespace dragon;
namespace {
using O=ignil::Opcode;
constexpr std::uint64_t base=0x1000;
ignil::Block segmentLift(Architecture arch,const std::vector<std::uint8_t> &bytes) {
  Disassembler dis{arch,{}};const auto listing=dis.disassemble(bytes,base);
  REQUIRE(listing.size()==1);REQUIRE(listing[0].bytes()==bytes);
  return dis.liftTrace(listing);
}
std::set<std::uint32_t> undefined(const ignil::Block &block) {
  std::set<std::uint32_t> result;
  for(const auto &op:block.ops())if(op.opcode==O::UNDEF && op.dst)
    if(const auto *r=std::get_if<ignil::Reg>(&*op.dst))result.insert(r->offset);
  return result;
}
bool undefValue(const ignil::Block &block,const ignil::Value &value,unsigned depth=0) {
  if(depth>32)return false;
  const auto *temp=std::get_if<ignil::Temp>(&value);if(!temp)return false;
  for(const auto &op:block.ops())if(op.dst)if(const auto *dst=std::get_if<ignil::Temp>(&*op.dst))
    if(dst->id==temp->id) {
      if(op.opcode==O::UNDEF)return true;
      return op.opcode==O::COPY && op.srcs.size()==1 &&
          ignil::widthOf(op.srcs[0])==temp->width && undefValue(block,op.srcs[0],depth+1);
    }
  return false;
}
void honestUnsupported(const ignil::Block &block) {
  CHECK(std::find(block.unlifted().begin(),block.unlifted().end(),base)!=block.unlifted().end());
  // Ordinary unsupported memory operations must not retain a load/store at
  // the flat address. A fallback may omit the memory event or use fresh UNDEF.
  for(const auto &op:block.ops())if(op.opcode==O::LOAD || op.opcode==O::STORE) {
    REQUIRE_FALSE(op.srcs.empty());CHECK(undefValue(block,op.srcs[0]));
  }
}
void arithFlagsUnknown(const ignil::Block &block) {
  const auto regs=undefined(block);
  for(const auto &flag:{x86::CF,x86::PF,x86::AF,x86::ZF,x86::SF,x86::OF})CHECK(regs.contains(flag.offset));
  CHECK_FALSE(regs.contains(x86::DF.offset));
}
std::vector<std::uint8_t> withPrefix(std::uint8_t prefix,const std::vector<std::uint8_t> &body) {
  auto code=body;code.insert(code.begin(),prefix);return code;
}
bool constantTree(const slice::Expr &expression) {
  return expression.visit([](const auto &n)->bool {
    using T=std::decay_t<decltype(n)>;
    if constexpr(std::is_same_v<T,slice::Const>)return true;
    else if constexpr(std::is_same_v<T,slice::UnaryOp>)return n.operand && constantTree(*n.operand);
    else if constexpr(std::is_same_v<T,slice::BinOp>)return n.left && n.right && constantTree(*n.left) && constantTree(*n.right);
    else return false;
  });
}
}

TEST_CASE("x64 FS and GS scalar memory effects use honest unsupported fallbacks","[x86-segments][ignil]") {
  struct Case {std::vector<std::uint8_t> bytes;std::vector<std::uint32_t> defs;bool flags;};
  const std::vector<Case> cases{
    {{0x48,0x8b,3},{x86::RAX.offset},false},       // mov rax,[rbx]
    {{0x48,0x89,3},{},false},                    // mov [rbx],rax
    {{0x48,0x01,3},{},true},                     // add [rbx],rax
    {{0x48,0x0f,0xc1,3},{x86::RAX.offset},true}, // xadd [rbx],rax
    {{0x48,0xf7,0x23},{x86::RAX.offset,x86::RDX.offset},true}, // mul [rbx]
    {{0x48,0x0f,0x44,3},{x86::RAX.offset},false}, // cmovz rax,[rbx]
    {{0x48,0xa1,0,0x30,0,0,0,0,0,0},{x86::RAX.offset},false}, // moffs load
    {{0x48,0xa3,0,0x30,0,0,0,0,0,0},{},false}, // moffs store
    {{0xff,0x33},{x86::RSP.offset},false},       // push [rbx], also changes SP
    {{0x8f,3},{x86::RSP.offset},false}           // pop [rbx], also changes SP
  };
  for(auto segment:{0x64,0x65})for(const auto &test:cases) {
    CAPTURE(segment,test.bytes);auto block=segmentLift(Architecture::X86_64,withPrefix(segment,test.bytes));
    honestUnsupported(block);const auto regs=undefined(block);
    for(auto r:test.defs)CHECK(regs.contains(r));
    if(test.flags)arithFlagsUnknown(block);
  }
}

TEST_CASE("Segmented SSE fallbacks clobber both modeled XMM halves","[x86-segments][ignil]") {
  const std::vector<std::vector<std::uint8_t>> loads{
    {0x0f,0x10,3},       // movups xmm0,[rbx]
    {0xf2,0x0f,0x10,3},  // movsd xmm0,[rbx], whose usual high-zero must not survive as a stale value
    {0xf3,0x0f,0x7e,3},  // movq xmm0,[rbx]
    {0x0f,0x58,3}        // addps xmm0,[rbx]
  };
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(auto segment:{0x64,0x65}) {
    for(const auto &bytes:loads) {
      CAPTURE(arch,segment,bytes);auto block=segmentLift(arch,withPrefix(segment,bytes));honestUnsupported(block);
      const auto regs=undefined(block);CHECK(regs.contains(x86::kOffXMM0));CHECK(regs.contains(x86::kOffXMM0H));
    }
    auto store=segmentLift(arch,withPrefix(segment,{0x0f,0x11,3}));honestUnsupported(store);
    CHECK_FALSE(undefined(store).contains(x86::kOffXMM0));CHECK_FALSE(undefined(store).contains(x86::kOffXMM0H));
  }
}

TEST_CASE("MASKMOVDQU implicit memory fallback is always recorded as unlifted","[x86-segments][ignil]") {
  // LLVM MASKMOVDQU/MASKMOVDQU64 has only the two XMM operands. The memory
  // destination through (E)DI is implicit, so a five-operand memory detector
  // cannot establish that this unsupported instruction has no memory effect.
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(auto segment:{0,0x64,0x65}) {
    CAPTURE(arch,segment);std::vector<std::uint8_t> code{0x66,0x0f,0xf7,0xc1};
    if(segment)code.insert(code.begin(),segment);
    auto block=segmentLift(arch,code);honestUnsupported(block);
    const auto regs=undefined(block);
    // The value and mask are read-only; no register result can substitute for
    // the required marker on this unsupported memory-writing instruction.
    for(auto offset:{x86::kOffXMM0,x86::kOffXMM0H,x86::kOffXMM0+8,x86::kOffXMM0H+8,x86::RDI.offset})
      CHECK_FALSE(regs.contains(offset));
  }
}

TEST_CASE("32-bit explicit segment memory operations are not silently flattened","[x86-segments][ignil]") {
  for(auto segment:{0x26,0x2e,0x36,0x3e,0x64,0x65}) {
    CAPTURE(segment);
    auto load=segmentLift(Architecture::X86_32,withPrefix(segment,{0x8b,3}));honestUnsupported(load);
    CHECK(undefined(load).contains(x86::EAX.offset));
    auto store=segmentLift(Architecture::X86_32,withPrefix(segment,{0x89,3}));honestUnsupported(store);
    auto add=segmentLift(Architecture::X86_32,withPrefix(segment,{0x01,3}));honestUnsupported(add);arithFlagsUnknown(add);
    auto moffs=segmentLift(Architecture::X86_32,withPrefix(segment,{0xa1,0,0x30,0,0}));honestUnsupported(moffs);
    CHECK(undefined(moffs).contains(x86::EAX.offset));
  }
}

TEST_CASE("32-bit explicit source string segments are honestly unknown","[x86-segments][ignil]") {
  for(auto segment:{0x26,0x2e,0x36,0x3e,0x64,0x65})for(auto opcode:{0xa4,0xac,0xa6}) {
    CAPTURE(segment,opcode);auto block=segmentLift(Architecture::X86_32,
        {std::uint8_t(segment),std::uint8_t(opcode)});
    CHECK(std::find(block.unlifted().begin(),block.unlifted().end(),base)!=block.unlifted().end());
    // The string expert path may keep precise SI/DI updates and the fixed-ES
    // destination. If it emits its source LOAD, that first LOAD needs an
    // unknown address. Whole-instruction unsupported fallback is also valid.
    auto first=std::find_if(block.ops().begin(),block.ops().end(),[](const auto &op){return op.opcode==O::LOAD;});
    if(first!=block.ops().end()) {
      REQUIRE(first->srcs.size()==1);CHECK(undefValue(block,first->srcs[0]));
    }
  }
}

TEST_CASE("Segmented memory calls and jumps keep unknown control-flow skeletons","[x86-segments][ignil]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(auto segment:{0x64,0x65})
    for(bool call:{false,true}) {
      CAPTURE(arch,segment,call);auto block=segmentLift(arch,withPrefix(segment,{0xff,std::uint8_t(call?0x13:0x23)}));
      honestUnsupported(block);
      auto control=std::find_if(block.ops().begin(),block.ops().end(),[&](const auto &op){return op.opcode==(call?O::CALL:O::JMP);});
      REQUIRE(control!=block.ops().end());REQUIRE(control->srcs.size()==1);
      CHECK(undefValue(block,control->srcs[0]));
      CHECK(ignil::widthOf(control->srcs[0])>=(arch==Architecture::X86_64?64:32));
    }
}

TEST_CASE("Long-mode CS DS ES SS overrides retain flat memory semantics","[x86-segments][ignil]") {
  const std::vector<std::vector<std::uint8_t>> cases{
    {0x48,0x8b,3},{0x48,0x89,3},{0x48,0x01,3},{0x0f,0x10,3},{0x0f,0x11,3}
  };
  for(auto segment:{0x26,0x2e,0x36,0x3e})for(const auto &body:cases) {
    CAPTURE(segment,body);auto flat=segmentLift(Architecture::X86_64,body);
    auto prefixed=segmentLift(Architecture::X86_64,withPrefix(segment,body));
    REQUIRE(flat.unlifted().empty());CHECK(prefixed.unlifted().empty());
    CHECK(ignil::toString(prefixed)==ignil::toString(flat));
  }
}

TEST_CASE("LEA ignores segment bases while retaining address-size semantics","[x86-segments][ignil]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(auto segment:{0x26,0x2e,0x36,0x3e,0x64,0x65}) {
    const auto body=arch==Architecture::X86_64?std::vector<std::uint8_t>{0x48,0x8d,0x43,0x65}:
      std::vector<std::uint8_t>{0x8d,0x43,0x65};
    CAPTURE(arch,segment);auto flat=segmentLift(arch,body);auto prefixed=segmentLift(arch,withPrefix(segment,body));
    REQUIRE(flat.unlifted().empty());CHECK(prefixed.unlifted().empty());CHECK(ignil::toString(prefixed)==ignil::toString(flat));
#ifdef DRAGON_WITH_Z3
    ignil::SymbolicExecutor executor;executor.setRegister(arch==Architecture::X86_64?x86::RBX:x86::EBX,0x80000000);
    REQUIRE(executor.run(prefixed).ok());
    CHECK(executor.evaluate(executor.readRegister(arch==Architecture::X86_64?x86::RAX:x86::EAX))==0x80000065);
#endif
  }
  for(auto segment:{0x64,0x65}) {
    const std::vector<std::uint8_t> body{0x67,0x48,0x8d,0x43,0x65};
    auto flat=segmentLift(Architecture::X86_64,body);auto prefixed=segmentLift(Architecture::X86_64,withPrefix(segment,body));
    REQUIRE(flat.unlifted().empty());CHECK(prefixed.unlifted().empty());CHECK(ignil::toString(prefixed)==ignil::toString(flat));
#ifdef DRAGON_WITH_Z3
    ignil::SymbolicExecutor executor;executor.setRegister(x86::RBX,0x12345678fffffff0ull);
    REQUIRE(executor.run(prefixed).ok());CHECK(executor.evaluate(executor.readRegister(x86::RAX))==0x55);
#endif
  }
}

TEST_CASE("Ignored segment bytes do not break implicit SS or fixed ES operations","[x86-segments][ignil]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(auto segment:{0x64,0x65}) {
    for(const auto &body:std::vector<std::vector<std::uint8_t>>{{0x50},{0x58},{0xc3},{0x0f,0x1f,0},{0xaa},{0xab},{0xae},{0xaf}}) {
      CAPTURE(arch,segment,body);auto flat=segmentLift(arch,body);auto prefixed=segmentLift(arch,withPrefix(segment,body));
      REQUIRE(flat.unlifted().empty());CHECK(prefixed.unlifted().empty());CHECK(ignil::toString(prefixed)==ignil::toString(flat));
    }
    // Prefix length contributes to next-PC even though it does not alter the implicit SS stack access.
    auto flat=segmentLift(arch,{0xe8,1,0,0,0});auto prefixed=segmentLift(arch,{std::uint8_t(segment),0xe8,0,0,0,0});
    REQUIRE(flat.unlifted().empty());CHECK(prefixed.unlifted().empty());CHECK(ignil::toString(prefixed)==ignil::toString(flat));
  }
}

TEST_CASE("Segment-valued immediate and displacement bytes remain ordinary operands","[x86-segments][ignil]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64}) {
    std::vector<std::vector<std::uint8_t>> cases{{0xb8,0x64,0x65,0,0},{0x8b,0x43,0x64},{0x8b,0x43,0x65}};
    if(arch==Architecture::X86_64) {cases.push_back({0x48,0x8b,0x43,0x64});cases.push_back({0x48,0x8b,0x43,0x65});}
    for(const auto &code:cases) {CAPTURE(arch,code);auto block=segmentLift(arch,code);CHECK(block.unlifted().empty());}
  }
}

TEST_CASE("Unsupported segmented stores stop known-memory forwarding","[x86-segments][ignil]") {
  for(auto arch:{Architecture::X86_32,Architecture::X86_64})for(auto segment:{0x64,0x65}) {
    const bool wide=arch==Architecture::X86_64;
    std::vector<std::uint8_t> code{0xb8,0x60,0x10,0,0,0xbb,0,0x30,0,0};
    if(wide)code.push_back(0x48);
    code.insert(code.end(),{0xc7,3,0x40,0x10,0,0});
    code.push_back(segment);if(wide)code.push_back(0x48);code.insert(code.end(),{0x89,3});
    if(wide)code.push_back(0x48);code.insert(code.end(),{0x8b,0x2b,0xc3});
    Disassembler dis{arch,{}};Graph graph{arch,{}};dis.disassemble(code,base,graph);
    const auto *block=graph.getIGNILBlock(base);REQUIRE(block);CHECK_FALSE(block->unlifted().empty());
    const auto reg=wide?x86::RBP:x86::EBP;const analysis::FiniteValueQuery query{base,reg};
    const auto values=analysis::finiteValues(graph,{base},std::span{&query,1});
    REQUIRE(values.complete);REQUIRE(values.values.size()==1);CHECK(values.values[0].kind==analysis::ValueSetKind::Unknown);
    CHECK_FALSE(constantTree(BackwardSlicer{16}.slice(reg,base,graph)));
  }
}
