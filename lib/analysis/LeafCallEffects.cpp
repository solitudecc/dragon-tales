#include "LeafCallEffects.hpp"
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <algorithm>
#include <set>

namespace dragon::analysis::detail {
LeafCallProof proveLeafCallee(const Graph &graph,std::uint64_t entry,
    std::uint32_t maxBlocks,std::uint32_t maxOperations) {
  using O=ignil::Opcode;
  LeafCallProof proof;
  if(graph.architecture()!=Architecture::X86_32 && graph.architecture()!=Architecture::X86_64)return proof;
  const auto sp=graph.architecture()==Architecture::X86_64?x86::RSP:x86::ESP;
  auto spend=[&] {if(proof.operations>=maxOperations)return false;++proof.operations;return true;};
  const auto parents=x86::registerFile();
  std::set<std::uint64_t> pending{entry},visited;
  std::set<std::uint32_t> clobbers;
  bool returns=false;
  while(!pending.empty()) {
    const auto at=*pending.begin();pending.erase(pending.begin());
    if(!visited.insert(at).second)continue;
    if(visited.size()>maxBlocks || !spend() || !graph.isValidBlock(at))return proof;
    BasicBlock block{&graph,at};const auto *ir=graph.getIGNILBlock(at);
    if(!block.isValid() || !ir || !ir->unlifted().empty() || ir->empty())return proof;
    const auto instructions=block.instructions();
    if(instructions.empty())return proof;
    std::set<std::uint64_t> addresses;
    for(const auto *instruction:instructions) {
      if(!spend() || !instruction->mcInst() || instruction->isCall() || instruction->isTrap())return proof;
      if(instruction->isJump() && instruction->hasIndirectTarget())return proof;
      if(instruction->isReturn() && instruction->bytes()!=std::vector<std::uint8_t>{0xc3})return proof;
      addresses.insert(instruction->address());
    }
    const auto *last=instructions.back();
    if(last->isReturn()) {
      const auto &ret=ir->ops().back();
      if(ret.opcode!=O::RET || ret.address!=last->address() || ret.aux || !block.successors().empty())return proof;
      returns=true;
    } else {
      const auto &next=block.successors();
      if(next.empty() || next.size()>(last->isConditional()?2u:1u))return proof;
      const auto &end=ir->ops().back();
      if(last->isJump()) {
        if(end.opcode!=(last->isConditional()?O::CJMP:O::JMP) || end.address!=last->address())return proof;
        // All outgoing edges must come from direct machine targets/fallthrough.
        std::set<std::uint64_t> expected=last->branches();
        if(last->isConditional())expected.insert(last->address()+last->size());
        if(next!=expected)return proof;
      }
      pending.insert(next.begin(),next.end());
    }
    for(const auto &op:ir->ops()) {
      if(!spend() || !addresses.contains(op.address))return proof;
      // Only this explicit, side-effect-free subset is admitted. In particular,
      // STORE, CALL, local loops and opaque/system effects never qualify.
      switch(op.opcode) {
        case O::COPY:case O::LOAD:case O::ADD:case O::SUB:case O::NEG:case O::MUL:
        case O::SDIV:case O::UDIV:case O::SMOD:case O::UMOD:
        case O::AND:case O::OR:case O::XOR:case O::NOT:case O::SHL:case O::SHR:case O::SAR:
        case O::ZEXT:case O::SEXT:case O::TRUNC:
        case O::EQ:case O::NEQ:case O::ULT:case O::ULE:case O::UGT:case O::UGE:
        case O::SLT:case O::SLE:case O::SGT:case O::SGE:case O::UNDEF:break;
        case O::JMP:case O::CJMP:
          if(op.address!=last->address() || !last->isJump())return proof;
          break;
        case O::RET:
          if(op.address!=last->address() || !last->isReturn())return proof;
          break;
        default:return proof;
      }
      if(!op.dst)continue;
      const auto *reg=std::get_if<ignil::Reg>(&*op.dst);if(!reg)continue;
      if(!reg->width || reg->width>128)return proof;
      std::uint32_t end=reg->offset+(reg->width+7)/8;
      if(end<reg->offset)return proof;
      // Use the same implicit 32-bit parent clearing as finite-value transfer.
      if(reg->width==32) {
        bool found=false;
        for(const auto &parent:parents)
          if(parent.offset<=reg->offset && end<=parent.offset+parent.byteSize) {
            end=parent.offset+parent.byteSize;found=true;break;
          }
        if(!found)end=reg->offset+8;
      }
      if(end<reg->offset)return proof;
      if(reg->offset<sp.offset+sp.width/8 && sp.offset<end)return proof;
      for(auto byte=reg->offset;byte<end;++byte)clobbers.insert(byte);
    }
  }
  if(!returns)return proof;
  // CALL/RET changes the instruction pointer even in a body with no RIP reads.
  for(auto byte=x86::RIP.offset;byte<x86::RIP.offset+8;++byte)clobbers.insert(byte);
  proof.proven=true;
  proof.clobberedBytes.assign(clobbers.begin(),clobbers.end());
  return proof;
}
}
