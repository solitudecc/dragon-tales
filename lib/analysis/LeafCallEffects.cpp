#include "LeafCallEffects.hpp"
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <algorithm>
#include <compare>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <type_traits>

namespace dragon::analysis::detail {
namespace {
using W=ignil::Width;
using O=ignil::Opcode;
unsigned bytes(W w) {return (unsigned(w)+7)/8;}
std::uint64_t mask(W w) {return w>=64?~std::uint64_t{0}:w?(std::uint64_t{1}<<w)-1:0;}
bool hasSegmentOverride(const Instruction &instruction,Architecture architecture) {
  // Generic x86 address lifting does not model segment bases. Only inspect
  // the prefix region: immediates, displacements and opcode bytes may equal
  // 0x64/0x65 without selecting FS/GS.
  for(auto byte:instruction.bytes()) {
    switch(byte) {
      case 0x26:case 0x2e:case 0x36:case 0x3e:case 0x64:case 0x65:return true;
      case 0x66:case 0x67:case 0xf0:case 0xf2:case 0xf3:continue;
      default:
        if(architecture==Architecture::X86_64 && byte>=0x40 && byte<=0x4f)continue;
        return false;
    }
  }
  return false;
}
// Identities represent original register bytes or a byte of entry-SP+offset.
// Unknown is never an identity, even when two unknown bytes compare equal.
struct Byte {
  enum Kind {Unknown,Constant,Input,Stack} kind=Unknown;
  std::uint64_t value=0;
  unsigned index=0;
  auto operator<=>(const Byte &) const = default;
};
struct Value {
  W width=0;
  std::vector<Byte> data;
};
struct State {
  bool reached=false;
  std::map<std::uint32_t,Byte> regs; // Missing = original entry byte.
  std::map<std::uint64_t,Byte> memory; // Missing = unknown entry memory.
  bool operator==(const State &) const = default;
};
LeafCallProof proveTree(const Graph &,std::uint64_t,std::uint32_t,std::uint32_t,
                       std::uint32_t,std::set<std::uint64_t>);
class Prover {
 public:
  Prover(const Graph &g,std::uint32_t blocks,std::uint32_t operations,std::uint32_t facts,
      std::set<std::uint64_t> path):
      graph(g),maxBlocks(blocks),maxOperations(operations),maxFacts(std::min(facts,8192u)),
      sp(g.architecture()==Architecture::X86_64?x86::RSP:x86::ESP),parents(x86::registerFile()),active(std::move(path)) {}
  Byte initial(std::uint32_t at) const {
    if(sp.offset<=at && at<sp.offset+bytes(sp.width))return {Byte::Stack,0,at-sp.offset};
    return {Byte::Input,at,0};
  }
  Byte regByte(const State &s,std::uint32_t at) const {
    auto it=s.regs.find(at);return it==s.regs.end()?initial(at):it->second;
  }
  static Byte memByte(const State &s,std::uint64_t at) {
    auto it=s.memory.find(at);return it==s.memory.end()?Byte{}:it->second;
  }
  static Value unknown(W w) {return {w,std::vector<Byte>(bytes(w))};}
  static Value constant(std::uint64_t value,W w) {
    auto v=unknown(w);
    for(unsigned n=0;n<v.data.size();++n)v.data[n]={Byte::Constant,n<8?(value>>(8*n))&255u:0,0};
    if(w%8 && !v.data.empty())v.data.back().value &= mask(W(w%8));
    return v;
  }
  Value stack(std::uint64_t offset) const {
    auto v=unknown(sp.width);
    for(unsigned n=0;n<v.data.size();++n)v.data[n]={Byte::Stack,offset&mask(sp.width),n};
    return v;
  }
  static Value resize(Value v,W w) {
    const auto old=v.width;
    v.width=w;v.data.resize(bytes(w),Byte{Byte::Constant,0,0});
    if(w%8 && !v.data.empty()) {
      auto &last=v.data.back();
      if(last.kind==Byte::Constant)last.value &= mask(W(w%8));else last={};
    }
    // Non-byte inputs do not carry identities for their unused high bits.
    if(old%8 && w>old && bytes(old) && bytes(old)<=v.data.size()) {
      auto &last=v.data[bytes(old)-1];if(last.kind!=Byte::Constant)last={};
    }
    return v;
  }
  static std::optional<std::uint64_t> number(const Value &v) {
    if(!v.width || v.width>64 || v.data.size()!=bytes(v.width))return {};
    std::uint64_t n=0;
    for(unsigned i=0;i<v.data.size();++i) {
      if(v.data[i].kind!=Byte::Constant)return {};
      n|=v.data[i].value<<(8*i);
    }
    return n&mask(v.width);
  }
  std::optional<std::uint64_t> address(const Value &v) const {
    if(v.width!=sp.width || v.data.size()!=bytes(sp.width))return {};
    const auto offset=v.data[0].value;
    for(unsigned n=0;n<v.data.size();++n)
      if(v.data[n]!=Byte{Byte::Stack,offset,n})return {};
    return offset;
  }
  Value readRegister(const State &s,const ignil::Reg &r) const {
    if(!r.width || r.width>128 || r.offset>UINT32_MAX-bytes(r.width))return unknown(r.width);
    auto v=unknown(r.width);
    for(unsigned n=0;n<v.data.size();++n)v.data[n]=regByte(s,r.offset+n);
    return resize(std::move(v),r.width);
  }
  bool writeRegister(State &s,const ignil::Reg &r,Value v) {
    if(!r.width || r.width>128 || r.offset>UINT32_MAX-bytes(r.width))return false;
    auto end=r.offset+bytes(r.width);
    if(r.width==32) {
      bool found=false;
      for(const auto &p:parents)if(p.offset<=r.offset && end<=p.offset+p.byteSize) {
        end=p.offset+p.byteSize;found=true;break;
      }
      if(!found) {if(r.offset>UINT32_MAX-8)return false;end=r.offset+8;}
    }
    v=resize(std::move(v),r.width);
    for(auto at=r.offset;at<end;++at) {
      auto byte=at<r.offset+v.data.size()?v.data[at-r.offset]:Byte{Byte::Constant,0,0};
      touched.insert(at);
      if(byte==initial(at))s.regs.erase(at);else s.regs.insert_or_assign(at,byte);
    }
    return true;
  }
  Value operand(const State &s,const std::map<std::uint32_t,Value> &temps,const ignil::Value &v) const {
    return std::visit([&](const auto &x)->Value {
      using T=std::decay_t<decltype(x)>;
      if constexpr(std::is_same_v<T,ignil::Const>)return constant(x.value,x.width);
      else if constexpr(std::is_same_v<T,ignil::Reg>)return readRegister(s,x);
      else {auto it=temps.find(x.id);return it==temps.end()?unknown(x.width):resize(it->second,x.width);}
    },v);
  }
  Value operation(O opcode,const Value &a,const Value &b,W width) const {
    auto x=number(a),y=number(b);
    if(width==sp.width && (opcode==O::ADD || opcode==O::SUB)) {
      if(auto p=address(a);p && y)return stack(opcode==O::ADD?*p+*y:*p-*y);
      if(opcode==O::ADD)if(auto p=address(b);p && x)return stack(*p+*x);
    }
    if(x && y && width && width<=64) {
      switch(opcode) {
        case O::ADD:return constant(*x+*y,width);
        case O::SUB:return constant(*x-*y,width);
        case O::AND:return constant(*x&*y,width);
        case O::OR:return constant(*x|*y,width);
        case O::XOR:return constant(*x^*y,width);
        default:break;
      }
    }
    return unknown(width);
  }
  bool spend() {if(proof.operations>=maxOperations)return false;++proof.operations;return true;}
  bool budget(const State &current,const std::map<std::uint32_t,Value> &temps) const {
    std::uint64_t facts=current.regs.size()+current.memory.size()+writes.size()+touched.size()+
        returnState.regs.size()+returnState.memory.size();
    for(const auto &[at,s]:states)facts+=s.regs.size()+s.memory.size()+1;
    for(const auto &[id,v]:temps)facts+=v.data.size()+1;
    for(const auto &[entry,p]:callees)facts+=1+p.clobberedBytes.size()+p.stackWriteBytes.size()+
        p.registerConstants.size()+p.memoryConstants.size();
    return facts<=maxFacts;
  }
  bool writeMemory(State &state,std::uint64_t at,const Value &value) {
    for(unsigned n=0;n<bytes(value.width);++n) {
      const auto offset=(at+n)&mask(sp.width);
      if(offset<bytes(sp.width) || (offset>1048576 && offset<mask(sp.width)-1048575))return false;
      writes.insert(offset);
      if(value.data[n].kind==Byte::Unknown)state.memory.erase(offset);
      else state.memory.insert_or_assign(offset,value.data[n]);
    }
    return true;
  }
  bool call(State &state,const ignil::Op &op) {
    const auto *instruction=graph.get(op.address);
    const auto *target=op.srcs.size()==1?std::get_if<ignil::Const>(&op.srcs[0]):nullptr;
    if(!instruction || !instruction->isCall() || instruction->hasIndirectTarget() ||
       instruction->bytes().size()!=5 || instruction->bytes()[0]!=0xe8 || !target || target->width!=64 ||
       instruction->callees()!=std::set<std::uint64_t>{target->value})return false;
    auto currentSP=address(readRegister(state,sp));if(!currentSP)return false;
    auto it=callees.find(target->value);
    if(it==callees.end()) {
      auto child=proveTree(graph,target->value,maxBlocks,maxOperations-proof.operations,maxFacts,active);
      proof.operations+=child.operations;
      it=callees.emplace(target->value,std::move(child)).first;
    }
    if(!it->second.proven)return false;
    const auto calleeSP=(*currentSP-bytes(sp.width))&mask(sp.width);
    // Include the nested CALL's own push. Child proofs protect their own
    // return slots; every translated write must also protect this outer one.
    if(!writeMemory(state,calleeSP,unknown(sp.width)))return false;
    for(auto offset:it->second.stackWriteBytes)
      if(!writeMemory(state,(calleeSP+offset)&mask(sp.width),unknown(8)))return false;
    for(auto byte:it->second.clobberedBytes) {
      touched.insert(byte);state.regs.insert_or_assign(byte,Byte{});
    }
    for(auto [byte,value]:it->second.registerConstants)
      state.regs.insert_or_assign(byte,Byte{Byte::Constant,value,0});
    for(auto [offset,value]:it->second.memoryConstants)
      if(!writeMemory(state,(calleeSP+offset)&mask(sp.width),constant(value,8)))return false;
    return true;
  }
  bool merge(State &to,const State &from) const {
    if(!to.reached) {to=from;return true;}
    State result;result.reached=true;
    std::set<std::uint32_t> regs;
    for(const auto &[at,b]:to.regs)regs.insert(at);
    for(const auto &[at,b]:from.regs)regs.insert(at);
    for(auto at:regs) {
      auto a=regByte(to,at),b=regByte(from,at);auto value=a==b?a:Byte{};
      if(value!=initial(at))result.regs.emplace(at,value);
    }
    for(const auto &[at,b]:to.memory)if(b==memByte(from,at))result.memory.emplace(at,b);
    if(to==result)return false;to=std::move(result);return true;
  }
  bool block(std::uint64_t at,State &state) {
    if(!spend() || !graph.isValidBlock(at))return false;
    BasicBlock block{&graph,at};const auto *ir=graph.getIGNILBlock(at);
    if(!block.isValid() || !ir || !ir->unlifted().empty() || ir->empty())return false;
    auto instructions=block.instructions();if(instructions.empty())return false;
    std::set<std::uint64_t> addresses;
    for(const auto *instruction:instructions) {
      if(!spend() || !instruction->mcInst() || instruction->isTrap() ||
         hasSegmentOverride(*instruction,graph.architecture()))return false;
      if(instruction->isCall() && (instruction->hasIndirectTarget() || instruction->bytes().size()!=5 ||
          instruction->bytes()[0]!=0xe8 || std::ranges::none_of(ir->ops(),[&](const auto &op) {
            return op.opcode==O::CALL && op.address==instruction->address();
          })))return false;
      if(instruction->isJump() && instruction->hasIndirectTarget())return false;
      if(instruction->isReturn() && instruction->bytes()!=std::vector<std::uint8_t>{0xc3})return false;
      addresses.insert(instruction->address());
    }
    const auto *last=instructions.back();const auto &end=ir->ops().back();
    if(last->isReturn()) {
      if(end.opcode!=O::RET || end.address!=last->address() || end.aux || !block.successors().empty())return false;
    } else {
      const auto &next=block.successors();
      if(next.empty() || next.size()>(last->isConditional()?2u:1u))return false;
      if(last->isJump()) {
        if(end.opcode!=(last->isConditional()?O::CJMP:O::JMP) || end.address!=last->address())return false;
        auto expected=last->branches();if(last->isConditional())expected.insert(last->address()+last->size());
        if(next!=expected)return false;
      } else {
        // Splitting at a loop header adds a canonical IGNIL fallthrough JMP
        // after the last ordinary machine instruction.
        if(next!=std::set<std::uint64_t>{last->address()+last->size()})return false;
      }
    }
    std::map<std::uint32_t,Value> temps;
    for(const auto &op:ir->ops()) {
      if(!spend() || !addresses.contains(op.address))return false;
      for(const auto &src:op.srcs)if(!ignil::widthOf(src) || ignil::widthOf(src)>128)return false;
      switch(op.opcode) {
        case O::COPY:case O::LOAD:case O::STORE:case O::CALL:case O::ADD:case O::SUB:case O::NEG:case O::MUL:
        case O::SDIV:case O::UDIV:case O::SMOD:case O::UMOD:
        case O::AND:case O::OR:case O::XOR:case O::NOT:case O::SHL:case O::SHR:case O::SAR:
        case O::ZEXT:case O::SEXT:case O::TRUNC:
        case O::EQ:case O::NEQ:case O::ULT:case O::ULE:case O::UGT:case O::UGE:
        case O::SLT:case O::SLE:case O::SGT:case O::SGE:case O::UNDEF:break;
        case O::JMP:case O::CJMP:
          if(op.address!=last->address())return false;
          if(!last->isJump()) {
            const auto *target=op.srcs.size()==1?std::get_if<ignil::Const>(&op.srcs[0]):nullptr;
            if(&op!=&end || op.opcode!=O::JMP || !target ||
               target->value!=last->address()+last->size() || last->isReturn())return false;
          }
          break;
        case O::RET:if(op.address!=last->address() || !last->isReturn())return false;break;
        default:return false;
      }
      auto source=[&](unsigned n) {return operand(state,temps,op.srcs[n]);};
      if(op.opcode==O::CALL) {
        if(!call(state,op))return false;
      } else if(op.opcode==O::STORE) {
        if(op.srcs.size()!=2)return false;
        auto p=address(source(0));const auto value=source(1);
        if(!p || !value.width || value.width>128)return false;
        // Bound the footprint around entry SP. No possible write may touch any
        // byte of the machine return address, including wrapped partial writes.
        if(!writeMemory(state,*p,value))return false;
      } else if(op.dst) {
        const auto width=ignil::widthOf(*op.dst);if(!width || width>128)return false;
        auto value=unknown(width);
        if((op.opcode==O::COPY || op.opcode==O::ZEXT || op.opcode==O::TRUNC) && op.srcs.size()==1)
          value=resize(source(0),width);
        else if(op.opcode==O::LOAD && op.srcs.size()==1) {
          if(auto p=address(source(0)))for(unsigned n=0;n<value.data.size();++n)
            value.data[n]=memByte(state,(*p+n)&mask(sp.width));
          value=resize(std::move(value),width);
        } else if(op.srcs.size()==2)value=operation(op.opcode,source(0),source(1),width);
        if(auto *reg=std::get_if<ignil::Reg>(&*op.dst)) {
          if(!writeRegister(state,*reg,std::move(value)))return false;
        } else if(auto *tmp=std::get_if<ignil::Temp>(&*op.dst))temps.insert_or_assign(tmp->id,std::move(value));
        else return false;
      }
      if(!budget(state,temps))return false;
    }
    if(last->isReturn()) {
      if(address(readRegister(state,sp))!=std::optional<std::uint64_t>{0})return false;
      returns=true;
      for(auto at:touched)if(regByte(state,at)!=initial(at))clobbers.insert(at);
      merge(returnState,state);
      if(!budget(state,temps))return false;
    }
    return true;
  }
  LeafCallProof run(std::uint64_t entry) {
    states[entry].reached=true;std::deque<std::uint64_t> queue{entry};std::set<std::uint64_t> queued{entry};
    while(!queue.empty()) {
      const auto at=queue.front();queue.pop_front();queued.erase(at);auto state=states.at(at);
      if(states.size()>maxBlocks || !block(at,state))return std::move(proof);
      const BasicBlock control{&graph,at};
      for(auto next:control.successors()) {
        if(!states.contains(next) && states.size()>=maxBlocks)return std::move(proof);
        if(merge(states[next],state) && queued.insert(next).second)queue.push_back(next);
      }
    }
    if(!returns)return std::move(proof);
    for(auto at=x86::RIP.offset;at<x86::RIP.offset+8;++at)clobbers.insert(at);
    proof.proven=true;proof.clobberedBytes.assign(clobbers.begin(),clobbers.end());
    proof.stackWriteBytes.assign(writes.begin(),writes.end());
    for(auto at:touched) {
      // RIP setup inside a callee is scratch state used for PC-relative
      // addressing, never the caller's architectural PC after CALL/RET.
      if(x86::RIP.offset<=at && at<x86::RIP.offset+8)continue;
      auto b=regByte(returnState,at);
      if(b.kind==Byte::Constant)proof.registerConstants.emplace_back(at,std::uint8_t(b.value));
    }
    for(const auto &[at,b]:returnState.memory)
      if(b.kind==Byte::Constant)proof.memoryConstants.emplace_back(at,std::uint8_t(b.value));
    return std::move(proof);
  }
  const Graph &graph;
  std::uint32_t maxBlocks,maxOperations,maxFacts;
  ignil::Reg sp;
  std::vector<ignil::RegExtent> parents;
  LeafCallProof proof;
  State returnState;
  std::map<std::uint64_t,State> states;
  std::set<std::uint32_t> touched,clobbers;
  std::set<std::uint64_t> writes;
  std::set<std::uint64_t> active;
  std::map<std::uint64_t,LeafCallProof> callees;
  bool returns=false;
};
LeafCallProof proveTree(const Graph &graph,std::uint64_t entry,std::uint32_t maxBlocks,
    std::uint32_t maxOperations,std::uint32_t maxFacts,std::set<std::uint64_t> active) {
  if(active.size()>=4 || !active.insert(entry).second)return {};
  return Prover{graph,maxBlocks,maxOperations,maxFacts,std::move(active)}.run(entry);
}
}
LeafCallProof proveLeafCallee(const Graph &graph,std::uint64_t entry,
    std::uint32_t maxBlocks,std::uint32_t maxOperations,std::uint32_t maxFacts) {
  if(graph.architecture()!=Architecture::X86_32 && graph.architecture()!=Architecture::X86_64)return {};
  return proveTree(graph,entry,maxBlocks,maxOperations,maxFacts,{});
}
}
