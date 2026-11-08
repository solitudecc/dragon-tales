#include <dragon/analysis/FiniteValues.hpp>
#include "LeafCallEffects.hpp"
#include "SnapshotIdentity.hpp"
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <bit>
#include <deque>
#include <iterator>
#include <optional>
#include <tuple>
#include <type_traits>

namespace dragon::analysis {
namespace {
using W = ignil::Width;
using O = ignil::Opcode;
std::uint64_t mask(W w) { return w >= 64 ? ~std::uint64_t{0} : w ? (std::uint64_t{1} << w) - 1 : 0; }
unsigned bytes(W w) { return (unsigned(w) + 7) / 8; }
struct Pointer {
  std::uint64_t base, offset;
  W width;
  auto operator<=>(const Pointer &) const = default;
};
struct Value {
  W width = 0;
  std::vector<std::uint64_t> finite; // Empty = unknown.
  std::vector<Pointer> pointers;
  bool operator==(const Value &) const = default;
};
struct Slot {
  Pointer address;
  W width;
  auto operator<=>(const Slot &) const = default;
};
using Register = std::pair<std::uint32_t,W>;
struct State {
  bool reached = false;
  std::map<Register,Value> regs;
  std::map<Slot,Value> memory;
  bool operator==(const State &) const = default;
};
struct Transfer {
  State state;
  std::map<std::uint32_t,Value> temps;
};
class Engine {
 public:
  Engine(const Graph &g, const FiniteValueLimits &l) : graph(g), limits(l) {
    switch (g.architecture()) {
      case Architecture::X86_32: case Architecture::X86_64: parents=x86::registerFile();break;
      case Architecture::ARM32: case Architecture::ARM64: parents=arm::registerFile();break;
      default:break;
    }
  }
  Value constant(std::uint64_t x,W w) const {
    return w && w<=64 ? Value{w,{x&mask(w)}, {}} : Value{w};
  }
  Value bounded(Value v) const {
    if (!v.width || v.width>64) return {v.width};
    for (auto &x:v.finite) x &= mask(v.width);
    std::sort(v.finite.begin(),v.finite.end());
    v.finite.erase(std::unique(v.finite.begin(),v.finite.end()),v.finite.end());
    std::sort(v.pointers.begin(),v.pointers.end());
    v.pointers.erase(std::unique(v.pointers.begin(),v.pointers.end()),v.pointers.end());
    if (v.finite.size()>limits.maxValues || v.pointers.size()>limits.maxValues) return {v.width};
    return v;
  }
  Value smallUnknown(W w) const {
    if (w && w<7 && (std::uint64_t{1}<<w)<=limits.maxValues) {
      Value v{w};for(std::uint64_t i=0;i<(std::uint64_t{1}<<w);++i)v.finite.push_back(i);
      return v;
    }
    return {w};
  }
  Value resize(Value v,W w) const {
    if(v.width==w)return v;
    if(!v.pointers.empty())return {w}; // No invented relation for truncated stack pointers.
    v.width=w;return bounded(std::move(v));
  }
  Value unite(Value a,const Value &b) const {
    if(a.width!=b.width)return {a.width};
    if(!a.pointers.empty() && !b.pointers.empty()) {
      a.pointers.insert(a.pointers.end(),b.pointers.begin(),b.pointers.end());
      return bounded(std::move(a));
    }
    if(a.finite.empty() || b.finite.empty())return {a.width};
    a.finite.insert(a.finite.end(),b.finite.begin(),b.finite.end());
    return bounded(std::move(a));
  }
  Value extract(const Value &v,unsigned first,W w) const {
    if(first==0 && v.width==w)return v;
    if(!v.pointers.empty() || first>=64)return {w};
    Value r{w};for(auto x:v.finite)r.finite.push_back((x>>first)&mask(w));
    return bounded(std::move(r));
  }
  Value append(const Value &low,const Value &high,W width,unsigned shift) {
    if(low.finite.empty() || high.finite.empty() || shift>=64)return {width};
    Value r{width};
    for(auto l:low.finite)for(auto h:high.finite) {
      if(!spend())return {width};
      r.finite.push_back(l | (h<<shift));
      if(r.finite.size()>limits.maxValues)r=bounded(std::move(r));
      if(r.finite.empty())return {width};
    }
    return bounded(std::move(r));
  }
  Value readRegister(const State &s,std::uint32_t offset,W width) {
    if(!width || width>64)return {width};
    if(offset>UINT32_MAX-bytes(width))return {width};
    for(const auto &[key,v]:s.regs)
      if(key.first<=offset && offset+bytes(width)<=key.first+bytes(key.second))
        return extract(v,(offset-key.first)*8,width);
    Value r=constant(0,width);
    unsigned done=0;
    while(done<bytes(width)) {
      const Value *source=nullptr;Register chosen{};
      for(const auto &[key,v]:s.regs)
        if(key.first<=offset+done && offset+done<key.first+bytes(key.second) &&
            (!source || key.second>chosen.second)) {source=&v;chosen=key;}
      if(!source)return smallUnknown(width);
      const unsigned n=std::min<unsigned>(bytes(width)-done,chosen.first+bytes(chosen.second)-offset-done);
      auto part=extract(*source,(offset+done-chosen.first)*8,W(n*8));
      r=append(r,part,width,done*8);
      if(r.finite.empty())return smallUnknown(width);
      done+=n;
    }
    return resize(std::move(r),width);
  }
  void writeRegister(State &s,const ignil::Reg &reg,Value value) {
    // Register space has no modular addressing. Reject an invalid actual
    // span before looking up a parent: a wrapped end can falsely fit a small
    // architectural parent and preserve facts that this write overlaps.
    if(!reg.width || reg.offset>UINT32_MAX-bytes(reg.width)) {s.regs.clear();return;}
    unsigned end=reg.offset+bytes(reg.width);
    if(reg.width==32) {
      bool found=false;
      for(const auto &p:parents)
        if(p.offset<=reg.offset && end<=p.offset+p.byteSize) {end=p.offset+p.byteSize;found=true;break;}
      if(!found) {
        // The existing generic 32-bit write model also clears four upper
        // bytes. It cannot be applied when that implicit extent overflows.
        if(reg.offset>UINT32_MAX-8) {s.regs.clear();return;}
        end=reg.offset+8;
      }
    }
    std::vector<std::pair<Register,Value>> preserved;
    for(auto it=s.regs.begin();it!=s.regs.end();) {
      const auto [start,w]=it->first;
      const auto stop=start+bytes(w);
      if(stop<=reg.offset || end<=start) {++it;continue;}
      if(start<reg.offset)preserved.push_back({{start,W((reg.offset-start)*8)},extract(it->second,0,W((reg.offset-start)*8))});
      if(end<stop)preserved.push_back({{end,W((stop-end)*8)},extract(it->second,(end-start)*8,W((stop-end)*8))});
      it=s.regs.erase(it);
    }
    for(auto &[key,v]:preserved)if(known(v))s.regs.insert_or_assign(key,std::move(v));
    // Register bytes are zero-filled above a non-byte-wide write.
    value=resize(std::move(value),W(bytes(reg.width)*8));
    if(known(value))s.regs.insert_or_assign(Register{reg.offset,W(bytes(reg.width)*8)},std::move(value));
    for(unsigned at=reg.offset+bytes(reg.width);at<end;) {
      const auto n=std::min<unsigned>(8,end-at);
      s.regs.insert_or_assign(Register{at,W(n*8)},constant(0,W(n*8)));at+=n;
    }
  }
  static bool known(const Value &v) {return !v.pointers.empty() || !v.finite.empty();}
  std::optional<Pointer> address(const Value &v) const {
    if(v.pointers.size()==1)return v.pointers.front();
    if(v.finite.size()==1)return Pointer{0,v.finite.front(),64};
    return {};
  }
  static Pointer advance(Pointer p,unsigned n) {p.offset=(p.offset+n)&mask(p.width);return p;}
  static bool sameSpace(const Pointer &a,const Pointer &b) {return a.base==b.base && a.width==b.width;}
  static bool overlap(const Slot &a,const Slot &b) {
    if(!sameSpace(a.address,b.address))return true; // Unconstrained bases may alias.
    for(unsigned i=0;i<bytes(a.width);++i)
      for(unsigned j=0;j<bytes(b.width);++j)
        if(advance(a.address,i).offset==advance(b.address,j).offset)return true;
    return false;
  }
  Value readMemory(const State &s,Pointer at,W width) {
    if(!width || width>64)return {width};
    for(const auto &[key,v]:s.memory)if(sameSpace(key.address,at)) {
      const auto delta=(at.offset-key.address.offset)&mask(at.width);
      if(delta<bytes(key.width) && delta+bytes(width)<=bytes(key.width))
        return extract(v,unsigned(delta)*8,width);
    }
    Value result=constant(0,width);
    for(unsigned i=0;i<bytes(width);++i) {
      Value part{8};const auto p=advance(at,i);
      for(const auto &[key,v]:s.memory)if(sameSpace(key.address,p)) {
        const auto delta=(p.offset-key.address.offset)&mask(p.width);
        if(delta<bytes(key.width)) {part=extract(v,unsigned(delta)*8,8);break;}
      }
      result=append(result,part,width,i*8);
      if(result.finite.empty())return {width};
    }
    return result;
  }
  Value load(const State &s,const Value &at,W width) {
    if(auto p=address(at))return readMemory(s,*p,width);
    std::optional<Value> result;
    for(auto p:at.pointers) {
      auto v=readMemory(s,p,width);
      result=result?unite(std::move(*result),v):std::move(v);
    }
    for(auto a:at.finite) {
      auto v=readMemory(s,Pointer{0,a,64},width);
      result=result?unite(std::move(*result),v):std::move(v);
    }
    return result?std::move(*result):Value{width};
  }
  void store(State &s,const Value &at,Value value) {
    auto p=address(at);
    if(!p || !value.width || value.width>64) {s.memory.clear();return;}
    value=resize(std::move(value),W(bytes(value.width)*8));
    const Slot written{*p,value.width};
    std::vector<std::pair<Slot,Value>> preserved;
    for(auto it=s.memory.begin();it!=s.memory.end();) {
      if(!overlap(written,it->first)) {++it;continue;}
      if(sameSpace(written.address,it->first.address)) {
        // Preserve disjoint bytes, including wrapped addresses.
        for(unsigned i=0;i<bytes(it->first.width);++i) {
          const Slot byte{advance(it->first.address,i),8};
          if(!overlap(written,byte))preserved.push_back({byte,extract(it->second,i*8,8)});
        }
      }
      it=s.memory.erase(it);
    }
    for(auto &[key,v]:preserved)if(known(v))s.memory.insert_or_assign(key,std::move(v));
    if(known(value))s.memory.insert_or_assign(written,std::move(value));
  }
  Value operand(const Transfer &t,const ignil::Value &v) {
    return std::visit([&](const auto &x)->Value {
      using T=std::decay_t<decltype(x)>;
      if constexpr(std::is_same_v<T,ignil::Const>)return constant(x.value,x.width);
      else if constexpr(std::is_same_v<T,ignil::Reg>)return readRegister(t.state,x.offset,x.width);
      else {auto it=t.temps.find(x.id);return it==t.temps.end()?smallUnknown(x.width):resize(it->second,x.width);}
    },v);
  }
  Value operation(O op,const Value &left,const Value &right,W width,bool unary) {
    if(!width || width>64)return {width};
    if(!unary && (op==O::ADD || op==O::SUB)) {
      if(!left.pointers.empty() && !right.finite.empty() && width==left.width) {
        Value result{width};
        for(auto p:left.pointers)for(auto offset:right.finite) {
          if(!spend())return {width};
          auto q=p;
          q.offset=(op==O::ADD?p.offset+offset:p.offset-offset)&mask(p.width);
          result.pointers.push_back(q);
        }
        return bounded(std::move(result));
      }
      if(op==O::ADD && !right.pointers.empty() && !left.finite.empty())return operation(op,right,left,width,false);
      if(op==O::SUB && !left.pointers.empty() && !right.pointers.empty() &&
         width<=left.width && width<=right.width) {
        Value result{width};
        for(auto a:left.pointers)for(auto b:right.pointers) {
          if(!spend() || !sameSpace(a,b))return {width};
          result.finite.push_back((a.offset-b.offset)&mask(width));
        }
        return bounded(std::move(result));
      }
    }
    auto a=left.finite.empty()?smallUnknown(left.width):left;
    auto b=right.finite.empty()?smallUnknown(right.width):right;
    if(a.finite.empty() || (!unary && b.finite.empty())) {
      if(!unary && (op==O::AND || op==O::OR) && left.width && right.width) {
        const auto &constants=a.finite.empty()?b:a;
        const W freeWidth=a.finite.empty()?left.width:right.width;
        if(!constants.finite.empty()) {
          Value result{width};
          for(auto c:constants.finite) {
            const auto freeBits=mask(std::min(width,freeWidth)) & (op==O::AND?c:~c);
            const auto count=std::popcount(freeBits);
            if(count>8 || (std::uint64_t{1}<<count)>limits.maxValues)return {width};
            auto subset=freeBits;
            for(;;) {
              if(!spend())return {width};
              result.finite.push_back(op==O::AND?subset:(c|subset));
              if(subset==0)break;
              subset=(subset-1)&freeBits;
            }
          }
          return bounded(std::move(result));
        }
      }
      if(!unary && ((op==O::AND || op==O::MUL) &&
         ((a.finite.size()==1 && a.finite[0]==0)||(b.finite.size()==1 && b.finite[0]==0))))return constant(0,width);
      if(op>=O::EQ && op<=O::SGE)return smallUnknown(1);
      return {width};
    }
    Value result{width};
    for(auto x:a.finite)for(auto y:unary?std::vector<std::uint64_t>{0}:b.finite) {
      if(!spend())return {width};
      auto c=calculate(op,x,left.width,y,right.width,width,unary);
      if(!c)return {width};
      result.finite.push_back(*c&mask(width));
      if(result.finite.size()>limits.maxValues)result=bounded(std::move(result));
      if(result.finite.empty())return {width};
    }
    return bounded(std::move(result));
  }
  static std::optional<std::uint64_t> calculate(O op,std::uint64_t x,W xw,
      std::uint64_t y,W yw,W dst,bool unary) {
    if(!xw || xw>64 || (!unary && (!yw || yw>64)))return {};
    llvm::APInt a(xw,x);
    if(unary) {
      switch(op) {
        case O::SEXT:return a.sextOrTrunc(dst).getZExtValue();
        case O::ZEXT:case O::TRUNC:case O::COPY:return a.zextOrTrunc(dst).getZExtValue();
        case O::NEG:a=a.zextOrTrunc(std::max(xw,dst));a=-a;break;
        case O::NOT:a=a.zextOrTrunc(std::max(xw,dst));a=~a;break;
        default:return {};
      }
      return a.zextOrTrunc(dst).getZExtValue();
    }
    llvm::APInt b(yw,y);
    if(op==O::SHL || op==O::SHR || op==O::SAR) {
      // IGNIL shifts use max(source width, count width), independently of
      // destination width. SAR sign-extends before shifting; SHL/SHR zero-extend.
      const W w=std::max(xw,yw);
      a=op==O::SAR?a.sextOrTrunc(w):a.zextOrTrunc(w);
      const auto shift=b.getLimitedValue(w);
      if(shift==w)a=op==O::SAR && a.isNegative()?llvm::APInt::getAllOnes(w):llvm::APInt(w,0);
      else if(op==O::SHL)a=a.shl(shift);
      else if(op==O::SHR)a=a.lshr(shift);
      else a=a.ashr(shift);
      return a.zextOrTrunc(dst).getZExtValue();
    }
    const bool compare=op>=O::EQ && op<=O::SGE;
    const W w=compare?std::max(xw,yw):std::max({xw,yw,dst});
    const bool sign=op==O::SDIV || op==O::SMOD ||
                    op==O::SLT || op==O::SLE || op==O::SGT || op==O::SGE;
    a=sign?a.sextOrTrunc(w):a.zextOrTrunc(w);
    b=sign?b.sextOrTrunc(w):b.zextOrTrunc(w);
    switch(op) {
      case O::ADD:a+=b;break;case O::SUB:a-=b;break;case O::MUL:a*=b;break;
      case O::AND:a&=b;break;case O::OR:a|=b;break;case O::XOR:a^=b;break;
      case O::UDIV:case O::UMOD:case O::SDIV:case O::SMOD:
        if(b.isZero() || (sign && a.isMinSignedValue() && b.isAllOnes()))return {};
        if(op==O::UDIV)a=a.udiv(b);else if(op==O::UMOD)a=a.urem(b);
        else if(op==O::SDIV)a=a.sdiv(b);else a=a.srem(b);
        break;
      case O::EQ:return a==b;case O::NEQ:return a!=b;
      case O::ULT:return a.ult(b);case O::ULE:return a.ule(b);
      case O::UGT:return a.ugt(b);case O::UGE:return a.uge(b);
      case O::SLT:return a.slt(b);case O::SLE:return a.sle(b);
      case O::SGT:return a.sgt(b);case O::SGE:return a.sge(b);
      default:return {};
    }
    return a.zextOrTrunc(dst).getZExtValue();
  }
  // Word masks implement selects in IGNIL. Keeping the shared mask identity
  // avoids inventing (a|b) and zero from independent Cartesian mask choices.
  std::optional<Value> select(const ignil::Op &op,const Transfer &t,
      const std::map<std::uint32_t,const ignil::Op *> &defs) {
    if(op.opcode!=O::OR || !op.dst || op.srcs.size()!=2)return {};
    const auto width=ignil::widthOf(*op.dst);
    auto definition=[&](const ignil::Value &v)->const ignil::Op * {
      auto *tmp=std::get_if<ignil::Temp>(&v);if(!tmp)return nullptr;
      auto it=defs.find(tmp->id);return it==defs.end()?nullptr:it->second;
    };
    for(unsigned side=0;side<2;++side) {
      auto *taken=definition(op.srcs[side]),*kept=definition(op.srcs[1-side]);
      if(!taken || !kept || taken->opcode!=O::AND || kept->opcode!=O::AND ||
         taken->srcs.size()!=2 || kept->srcs.size()!=2)continue;
      for(unsigned a=0;a<2;++a)for(unsigned b=0;b<2;++b) {
        const auto *negated=definition(kept->srcs[b]);
        const auto *maskValue=std::get_if<ignil::Temp>(&taken->srcs[a]);
        if(!negated || !maskValue || negated->opcode!=O::NOT || negated->srcs.size()!=1)continue;
        const auto *origin=std::get_if<ignil::Temp>(&negated->srcs[0]);
        if(!origin || origin->id!=maskValue->id)continue;
        bool widths=ignil::widthOf(op.srcs[0])==width && ignil::widthOf(op.srcs[1])==width;
        for(const auto &v:taken->srcs)widths &= ignil::widthOf(v)==width;
        for(const auto &v:kept->srcs)widths &= ignil::widthOf(v)==width;
        widths &= origin->width==width;
        if(!widths)continue;
        auto stable=[&](const ignil::Op *definition) {
          for(const auto &v:definition->srcs) {
            if(std::holds_alternative<ignil::Reg>(v))return false;
            if(const auto *tmp=std::get_if<ignil::Temp>(&v)) {
              auto it=defs.find(tmp->id);
              if(it==defs.end() || it->second>=definition)return false;
            }
          }
          return true;
        };
        if(!stable(taken) || !stable(kept) || !stable(negated))continue;
        const auto choices=operand(t,taken->srcs[a]);
        if(choices.finite.empty() || !std::ranges::all_of(choices.finite,
             [&](auto x){return x==0 || x==mask(width);}))continue;
        std::optional<Value> result;
        for(auto x:choices.finite) {
          auto v=operand(t,x?taken->srcs[1-a]:kept->srcs[1-b]);
          result=result?unite(std::move(*result),v):std::move(v);
        }
        return result;
      }
    }
    return {};
  }
  bool call(Transfer &t,const ignil::Op &op) {
    if(!limits.proveLeafCalls)return false;
    auto reject=[&] {rejectedSites.insert(op.address);result.rejectedCalls=rejectedSites.size();return false;};
    const auto arch=graph.architecture();
    if(arch!=Architecture::X86_32 && arch!=Architecture::X86_64)return reject();
    const auto *instruction=graph.get(op.address);
    const auto *target=op.srcs.size()==1?std::get_if<ignil::Const>(&op.srcs[0]):nullptr;
    if(!instruction || !instruction->isCall() || instruction->hasIndirectTarget() ||
       instruction->bytes().size()!=5 || instruction->bytes()[0]!=0xe8 || !target ||
       instruction->callees()!=std::set<std::uint64_t>{target->value})return reject();
    const auto width=W(arch==Architecture::X86_64?64:32);
    // The x86 lifter represents direct code addresses as 64-bit constants in
    // both modes; the near CALL's stack/address width still follows the mode.
    if(target->width!=64)return reject();
    const auto sp=arch==Architecture::X86_64?x86::RSP:x86::ESP;
    // IGNIL abstracts away this write, but a proof used for machine control-flow
    // recovery must not preserve stale data in the near CALL's return slot.
    auto stack=readRegister(t.state,sp.offset,sp.width);
    auto slot=operation(O::SUB,stack,constant(width/8,width),width,false);
    // The proof model executes fixed code. A known CALL push into decoded
    // instruction bytes cannot reuse even an otherwise universal callee proof.
    for(auto at:slot.finite)for(unsigned i=0;i<bytes(width);++i) {
      const auto byte=(at+i)&mask(width);
      auto instruction=graph.listing().upper_bound(byte);
      // Different x86 decode paths can overlap. A later short instruction
      // must not hide an earlier (at most 15-byte) instruction's suffix.
      while(instruction!=graph.listing().begin()) {
        --instruction;
        if(byte-instruction->first>=15)break;
        if(byte-instruction->first<instruction->second.bytes().size())return reject();
      }
    }
    auto lookup=[&](const detail::CallContext &context)->const detail::LeafCallProof * {
      const auto key=std::pair{target->value,context};
      auto it=callProofs.find(key);
      if(it!=callProofs.end())return &it->second;
      const auto contextCost=detail::contextFacts(context);
      if(failed || contextCost>8192 || proofFacts+contextCost+1>limits.maxFacts) {
        failed=true;return nullptr;
      }
      auto proof=detail::proveLeafCallee(graph,target->value,std::min(limits.maxBlocks,32u),
          std::min<std::uint64_t>(4096,limits.maxOperations-result.operations),limits.maxFacts,context);
      result.operations+=proof.operations;
      proofFacts+=contextCost+1+proof.clobberedBytes.size()+proof.stackWriteBytes.size()+
          proof.registerConstants.size()+proof.memoryConstants.size();
      it=callProofs.emplace(key,std::move(proof)).first;
      if(proofFacts>limits.maxFacts) {failed=true;return nullptr;}
      return &it->second;
    };
    // A universal proof can be sound but lose values read from caller memory.
    // Only such reads request a precision retry after a universal success.
    // A failed specialization must not discard a sound universal fallback.
    const auto *proof=lookup({});
    if(!proof)return reject();
    if(!proof->proven || proof->needsMemoryContext) {
      const auto *universal=proof;
      auto context=callerContext(t.state,sp,stack,slot);
      if(failed)return reject();
      if(context && (!context->bindings.empty() || !context->memory.empty() || context->entrySP) &&
         (!universal->proven || !context->memory.empty())) {
        const auto *specialized=lookup(*context);
        if(failed)return reject();
        if(specialized && specialized->proven)proof=specialized;
      }
      if(!proof->proven)return reject();
    }
    // Recheck the chosen effect at the actual call address. A failed numeric
    // specialization must not let a universal fallback write into stale code.
    for(auto base:slot.finite)for(auto offset:proof->stackWriteBytes) {
      if(!spend())return reject();
      const auto byte=(base+offset)&mask(width);
      auto instruction=graph.listing().upper_bound(byte);
      while(instruction!=graph.listing().begin()) {
        --instruction;
        if(byte-instruction->first>=15)break;
        if(byte-instruction->first<instruction->second.bytes().size())return reject();
      }
    }
    store(t.state,slot,Value{width});
    for(auto offset:proof->stackWriteBytes) {
      auto address=operation(O::ADD,slot,constant(offset,width),width,false);
      store(t.state,address,Value{8});
    }
    for(auto byte:proof->clobberedBytes)writeRegister(t.state,ignil::Reg{byte,8,{}},Value{8});
    for(auto [byte,value]:proof->registerConstants)
      writeRegister(t.state,ignil::Reg{byte,8,{}},constant(value,8));
    for(auto [offset,value]:proof->memoryConstants) {
      auto address=operation(O::ADD,slot,constant(offset,width),width,false);
      store(t.state,address,constant(value,8));
    }
    if(t.state.regs.size()+t.state.memory.size()+proofFacts>limits.maxFacts) {
      failed=true;return reject();
    }
    provenSites.insert(op.address);result.provenCalls=provenSites.size();
    return true;
  }
  std::optional<detail::CallContext> callerContext(const State &state,const ignil::Reg &sp,
      const Value &stack,const Value &slot) {
    detail::CallContext context;
    if(slot.finite.size()==1 && slot.pointers.empty())context.entrySP=slot.finite.front();
    const auto stackAddress=address(stack),slotAddress=address(slot);
    std::set<std::uint32_t> covered,domain;
    auto excluded=[&](std::uint32_t at) {
      return (sp.offset<=at && at<sp.offset+8) ||
          (x86::RIP.offset<=at && at<x86::RIP.offset+8);
    };
    for(const auto &parent:parents) {
      if(!spend())return {};
      for(unsigned i=0;i<parent.byteSize;++i)if(!excluded(parent.offset+i))domain.insert(parent.offset+i);
      if(parent.byteSize<bytes(sp.width) || excluded(parent.offset))continue;
      auto value=readRegister(state,parent.offset,sp.width);
      if(!value.finite.empty() || value.pointers.size()!=1 || !stackAddress || !slotAddress)continue;
      const auto &pointer=value.pointers.front();
      if(pointer.width!=sp.width || !sameSpace(pointer,*stackAddress) || !sameSpace(pointer,*slotAddress))continue;
      bool allowed=true;
      for(unsigned i=0;i<bytes(sp.width);++i)allowed &= !excluded(parent.offset+i) && !covered.contains(parent.offset+i);
      if(!allowed)continue;
      context.bindings.push_back({parent.offset,sp.width,detail::CallBinding::EntrySPRelative,
          (pointer.offset-slotAddress->offset)&mask(sp.width)});
      for(unsigned i=0;i<bytes(sp.width);++i)covered.insert(parent.offset+i);
    }
    for(auto at:domain) {
      if(!spend())return {};
      if(covered.contains(at))continue;
      const auto value=readRegister(state,at,8);
      if(value.pointers.empty() && value.finite.size()==1)
        context.bindings.push_back({at,8,detail::CallBinding::Constant,value.finite.front()});
    }
    // Transfer only facts proved in the same address space as this call's SP.
    // The new machine CALL overwrites its return slot before the callee runs.
    // Omitted/partly known bytes remain unknown, never zero-initialized.
    auto contextSize=detail::contextFacts(context);
    if(contextSize>8192)return {};
    if(slotAddress)for(const auto &[key,value]:state.memory) {
      if(!spend())return {};
      if(!sameSpace(key.address,*slotAddress))continue;
      auto relative=[&](unsigned index)->std::optional<std::uint64_t> {
        const auto at=advance(key.address,index);
        // Concrete slots in the engine use a 64-bit address space even in
        // i386. Do not wrap unrelated high-address facts into low memory.
        if(at.base==0 && at.offset>mask(sp.width))return {};
        const auto offset=(at.offset-slotAddress->offset)&mask(sp.width);
        if(offset<bytes(sp.width) ||
           (offset>1048576 && offset<mask(sp.width)-1048575))return {};
        return offset;
      };
      bool pointer=value.width==sp.width && value.finite.empty() && value.pointers.size()==1 &&
          value.pointers.front().width==sp.width && sameSpace(value.pointers.front(),*slotAddress);
      for(unsigned i=0;pointer && i<bytes(sp.width);++i) {
        if(!spend())return {};
        pointer=relative(i).has_value();
      }
      if(pointer) {
        context.memory.push_back({*relative(0),sp.width,detail::CallBinding::EntrySPRelative,
            (value.pointers.front().offset-slotAddress->offset)&mask(sp.width)});
        contextSize+=1+bytes(sp.width);
      } else for(unsigned i=0;i<bytes(key.width);++i) {
        if(!spend())return {};
        const auto at=relative(i);
        if(!at)continue;
        const auto part=extract(value,i*8,8);
        if(part.pointers.empty() && part.finite.size()==1) {
          context.memory.push_back({*at,8,detail::CallBinding::Constant,part.finite.front()});
          contextSize+=2;
        }
      }
      if(contextSize>8192)return {};
    }
    if(!detail::canonicalizeCallContext(context,graph.architecture()))return {};
    const auto cost=detail::contextFacts(context);
    if(state.regs.size()+state.memory.size()+proofFacts+cost>limits.maxFacts) {
      failed=true;return {};
    }
    if(cost>limits.maxOperations-result.operations) {failed=true;return {};}
    result.operations+=cost;
    return context;
  }
  Transfer transfer(std::uint64_t block,const State &in,
                    std::optional<std::size_t> beforeOp = std::nullopt) {
    Transfer t{in,{}};
    const auto *ib=graph.getIGNILBlock(block);
    if(!ib || !ib->unlifted().empty() ||
       std::ranges::any_of(ib->ops(),[](const auto &op){return ignil::isLocalControl(op.opcode);})) {
      t.state.regs.clear();t.state.memory.clear();return t;
    }
    std::map<std::uint32_t,const ignil::Op *> definitions;
    const auto count=std::min(beforeOp.value_or(ib->ops().size()),ib->ops().size());
    for(std::size_t index=0;index<count;++index) {
      const auto &op=ib->ops()[index];
      if(!spend())return t;
      if(op.opcode==O::CALL) {
        if(!call(t,op)) {t.state.regs.clear();t.state.memory.clear();}
        // CALL can grow the owned proof cache while block-local SSA snapshots
        // are still live. It bypasses the destination-write check below.
        if(t.temps.size()+t.state.regs.size()+t.state.memory.size()+proofFacts>limits.maxFacts) {
          failed=true;return t;
        }
        continue;
      }
      if(op.opcode==O::STORE) {
        if(op.srcs.size()==2)store(t.state,operand(t,op.srcs[0]),operand(t,op.srcs[1]));
        else t.state.memory.clear();
        continue;
      }
      if(!op.dst)continue;
      const W width=ignil::widthOf(*op.dst);
      Value v{width};
      if(limits.proveLeafCalls && op.opcode==O::XOR && op.srcs.size()==2 &&
          detail::sameSnapshot(op.srcs[0],op.srcs[1],[&](auto id,auto sourceWidth) {
            const auto it=t.temps.find(id);
            return it!=t.temps.end() && it->second.width==sourceWidth;
          }))v=constant(0,width);
      else if(auto selected=select(op,t,definitions))v=std::move(*selected);
      else if(op.opcode==O::COPY && op.srcs.size()==1)v=resize(operand(t,op.srcs[0]),width);
      else if(op.opcode==O::LOAD && op.srcs.size()==1)v=load(t.state,operand(t,op.srcs[0]),width);
      else if(op.srcs.size()==1 && (op.opcode==O::NEG || op.opcode==O::NOT ||
              op.opcode==O::ZEXT || op.opcode==O::SEXT || op.opcode==O::TRUNC))
        v=operation(op.opcode,operand(t,op.srcs[0]),{},width,true);
      else if(op.srcs.size()==2)v=operation(op.opcode,operand(t,op.srcs[0]),operand(t,op.srcs[1]),width,false);
      if(const auto *reg=std::get_if<ignil::Reg>(&*op.dst))writeRegister(t.state,*reg,std::move(v));
      else if(const auto *tmp=std::get_if<ignil::Temp>(&*op.dst)) {
        t.temps.insert_or_assign(tmp->id,std::move(v));definitions.insert_or_assign(tmp->id,&op);
      }
      if(t.temps.size()+t.state.regs.size()+t.state.memory.size()+proofFacts>limits.maxFacts) {
        failed=true;return t;
      }
    }
    return t;
  }
  bool merge(State &to,const State &from) {
    if(!from.reached)return false;
    if(!to.reached) {to=from;return true;}
    State merged;merged.reached=true;
    std::set<Register> registers;
    for(const auto &[k,v]:to.regs)registers.insert(k);
    for(const auto &[k,v]:from.regs)registers.insert(k);
    for(const auto &p:parents)if(p.byteSize<=8)registers.insert({p.offset,W(p.byteSize*8)});
    for(const auto &[at,w]:registers) {
      auto v=unite(readRegister(to,at,w),readRegister(from,at,w));
      if(known(v))merged.regs.emplace(Register{at,w},std::move(v));
    }
    std::set<Slot> slots;
    for(const auto &[k,v]:to.memory)slots.insert(k);
    for(const auto &[k,v]:from.memory)slots.insert(k);
    for(const auto &k:slots) {
      auto v=unite(readMemory(to,k.address,k.width),readMemory(from,k.address,k.width));
      if(known(v))merged.memory.emplace(k,std::move(v));
    }
    if(merged==to)return false;
    to=std::move(merged);return true;
  }
  State entry(std::uint64_t identity,const FiniteEntryState *initial=nullptr) {
    State s;s.reached=true;
    std::optional<ignil::Reg> sp;
    switch(graph.architecture()) {
      case Architecture::X86_32:sp=x86::ESP;break;
      case Architecture::X86_64:sp=x86::RSP;break;
      case Architecture::ARM32:break; // A32 has no modelled general lifter/register file.
      case Architecture::ARM64:sp=arm::SP;break;
      default:break;
    }
    if(sp)s.regs.emplace(Register{sp->offset,sp->width},Value{sp->width,{}, {Pointer{identity,0,sp->width}}});
    if(initial) {
      for(const auto &fact:initial->registers) {
        // Constraints are not architectural writes. In particular a 32-bit
        // constraint does not zero the parent's upper bytes. A partial SP
        // constraint also cannot retain the unconstrained whole-SP identity.
        const auto end=fact.offset+bytes(fact.width);
        std::vector<std::pair<Register,Value>> preserved;
        for(auto it=s.regs.begin();it!=s.regs.end();) {
          const auto [start,width]=it->first;
          const auto stop=start+bytes(width);
          if(stop<=fact.offset || end<=start) {++it;continue;}
          if(start<fact.offset)preserved.push_back({{start,W((fact.offset-start)*8)},extract(it->second,0,W((fact.offset-start)*8))});
          if(end<stop)preserved.push_back({{end,W((stop-end)*8)},extract(it->second,(end-start)*8,W((stop-end)*8))});
          it=s.regs.erase(it);
        }
        for(auto &[key,value]:preserved)if(known(value))s.regs.emplace(key,std::move(value));
        s.regs.emplace(Register{fact.offset,fact.width},constant(fact.value,fact.width));
      }
      for(const auto &fact:initial->memory) {
        for(std::size_t offset=0;offset<fact.bytes.size();) {
          const auto count=std::min<std::size_t>(8,fact.bytes.size()-offset);
          std::uint64_t value=0;
          for(std::size_t i=0;i<count;++i)value|=std::uint64_t(fact.bytes[offset+i])<<(8*i);
          const auto width=W(count*8);
          s.memory.emplace(Slot{Pointer{0,fact.address+offset,64},width},constant(value,width));
          offset+=count;
        }
      }
    }
    return s;
  }
  FiniteValues report(const Value &v,bool reached) const {
    if(!reached)return {ValueSetKind::Unreachable,v.width,{}};
    return {v.finite.empty()?ValueSetKind::Unknown:ValueSetKind::Finite,v.width,v.finite};
  }
  bool spend() {
    if(result.operations>=limits.maxOperations) {failed=true;return false;}
    ++result.operations;return true;
  }
  const Graph &graph;
  const FiniteValueLimits &limits;
  std::vector<ignil::RegExtent> parents;
  FiniteValueResult result;
  std::map<std::pair<std::uint64_t,detail::CallContext>,detail::LeafCallProof> callProofs;
  std::set<std::uint64_t> provenSites,rejectedSites;
  std::uint64_t proofFacts=0;
  bool failed=false;
};
std::set<std::uint64_t> admittedEntries(const Graph &graph,
    const std::set<std::uint64_t> &given,const BackwardSlicer::Predecessors &predecessors) {
  auto entries=given;
  if(entries.empty())entries=graph.validFunctionAddresses();
  if(entries.empty())for(const auto &[at,from]:predecessors)if(from.empty())entries.insert(at);
  if(entries.empty())entries=graph.validBlockAddresses(); // Rootless cyclic IR still admits inputs.
  for(const auto &[at,instruction]:graph.listing())for(auto callee:instruction.callees())entries.insert(callee);
  return entries;
}
template<typename Query>
FiniteValueResult runFinite(const Graph &graph,const std::set<std::uint64_t> &given,
    std::span<const Query> queries,const FiniteValueLimits &limits,
    std::span<const FiniteEntryState> initialStates={},std::uint64_t initialCost=0) {
  constexpr bool memoryQueries=std::is_same_v<Query,FiniteMemoryQuery>;
  auto queryWidth=[](const Query &query) {
    if constexpr(memoryQueries)return query.width;
    else return ignil::widthOf(query.value);
  };
  Engine e{graph,limits};
  e.result.operations=initialCost;
  // Bounded owned assumptions remain resident during the analysis, alongside
  // live abstract states and cached call proofs; charge all of them.
  e.proofFacts=initialCost;
  std::map<std::uint64_t,State> states;
  std::map<std::uint64_t,std::vector<std::uint64_t>> successors;
  const auto predecessors=BackwardSlicer::predecessors(graph);
  for(const auto &[at,from]:predecessors)for(auto p:from)successors[p].push_back(at);
  const auto entries=admittedEntries(graph,given,predecessors);
  // Direct callees have independent input states; CALL itself stays a barrier.
  std::map<std::uint64_t,const FiniteEntryState *> initial;
  for(const auto &state:initialStates)initial.emplace(state.entry,&state);
  std::deque<std::uint64_t> queue;std::set<std::uint64_t> queued;
  std::uint64_t identity=1;
  for(auto at:entries)if(graph.isValidBlock(at)) {
    const auto found=initial.find(at);
    auto in=e.entry(identity++,found==initial.end()?nullptr:found->second);
    e.merge(states[at],in);queue.push_back(at);queued.insert(at);
  }
  if(!limits.maxValues || limits.maxValues>256 || !limits.maxBlocks ||
     !limits.maxOperations || !limits.maxFacts) e.failed=true;
  std::uint64_t facts=0;
  for(const auto &[at,state]:states)facts+=state.regs.size()+state.memory.size();
  if(states.size()>limits.maxBlocks || facts+e.proofFacts>limits.maxFacts)e.failed=true;
  while(!queue.empty() && !e.failed) {
    auto at=queue.front();queue.pop_front();queued.erase(at);++e.result.blockVisits;
    auto out=e.transfer(at,states.at(at));
    for(auto next:successors[at]) {
      if(!states.contains(next) && states.size()>=limits.maxBlocks) {e.failed=true;break;}
      auto &destination=states[next];
      const auto before=destination.regs.size()+destination.memory.size();
      if(e.merge(destination,out.state) && queued.insert(next).second)queue.push_back(next);
      facts=facts-before+destination.regs.size()+destination.memory.size();
      if(facts+e.proofFacts>limits.maxFacts) {e.failed=true;break;}
    }
  }
  std::map<std::uint64_t,std::vector<std::size_t>> requested;
  e.result.values.resize(queries.size());
  for(std::size_t i=0;i<queries.size();++i) {
    requested[queries[i].block].push_back(i);
    e.result.values[i]={memoryQueries?ValueSetKind::Unknown:ValueSetKind::Unreachable,queryWidth(queries[i]),{}};
  }
  for(auto at:graph.validBlockAddresses()) {
    const auto *ib=graph.getIGNILBlock(at);
    const bool jump=ib && !ib->empty() && ib->ops().back().opcode==O::JMP && !ib->ops().back().srcs.empty();
    if(!jump && !requested.contains(at))continue;
    const auto it=states.find(at);const bool reached=it!=states.end() && it->second.reached;
    Transfer out=reached && !e.failed && (jump || !memoryQueries)?e.transfer(at,it->second):Transfer{};
    if(jump) {
      const auto &value=ib->ops().back().srcs[0];
      e.result.jumpTargets.emplace(at,e.report(reached?e.operand(out,value):Value{ignil::widthOf(value)},reached));
    }
    if constexpr(memoryQueries) {
      // All cells requested at the same LOAD share one prefix evaluation.
      // Do not retain a State per query: table queries may contain many cells.
      std::map<std::size_t,std::vector<std::size_t>> points;
      for(auto i:requested[at]) {
        const auto &query=queries[i];
        if(!ib || !query.width || query.width>64 || query.beforeOpIndex>=ib->ops().size())continue;
        const auto &op=ib->ops()[query.beforeOpIndex];
        if(op.opcode!=O::LOAD || !op.dst || op.srcs.size()!=1 || ignil::widthOf(*op.dst)!=query.width)continue;
        points[query.beforeOpIndex].push_back(i);
      }
      for(const auto &[before,indices]:points) {
        Transfer prefix=reached && !e.failed?e.transfer(at,it->second,before):Transfer{};
        for(auto i:indices) {
          const auto &query=queries[i];
          auto value=reached && !e.failed?e.readMemory(prefix.state,Pointer{0,query.address,64},query.width):Value{query.width};
          e.result.values[i]=e.report(value,reached);
        }
      }
    } else {
      for(auto i:requested[at])
        e.result.values[i]=e.report(reached?e.operand(out,queries[i].value):Value{queryWidth(queries[i])},reached);
    }
  }
  if(e.failed) {
    e.result.complete=false;e.result.message="finite-value analysis budget exhausted or invalid limits";
    for(auto &[at,value]:e.result.jumpTargets)value={ValueSetKind::Unknown,value.width,{}};
    for(std::size_t i=0;i<queries.size();++i)e.result.values[i]={ValueSetKind::Unknown,queryWidth(queries[i]),{}};
  }
  return std::move(e.result);
}
struct InitialValidation {
  std::uint64_t cost=0;
  std::string error;
};
InitialValidation validateInitial(const Graph &graph,const std::set<std::uint64_t> &given,
    std::span<const FiniteEntryState> states,const FiniteValueLimits &limits) {
  constexpr std::uint64_t hardCap=262144;
  InitialValidation result;
  auto charge=[&](std::uint64_t amount) {
    const auto cap=std::min<std::uint64_t>({hardCap,limits.maxFacts,limits.maxOperations});
    if(amount>cap-result.cost) {
      result.error="finite initial-state initialization budget exhausted";
      return false;
    }
    result.cost+=amount;
    return true;
  };
  auto invalid=[&](const char *why) {
    result.error=std::string("invalid finite initial-state assumptions: ")+why;
    return result;
  };
  // Check each collection's size before walking or copying caller data.
  if(!charge(states.size()))return result;
  const auto entries=admittedEntries(graph,given,BackwardSlicer::predecessors(graph));
  std::set<std::uint64_t> seen;
  auto insertRange=[](std::map<std::uint64_t,std::uint64_t> &ranges,
      std::uint64_t first,std::uint64_t last) {
    const auto next=ranges.lower_bound(first);
    if(next!=ranges.end() && next->first<=last)return false;
    if(next!=ranges.begin() && std::prev(next)->second>=first)return false;
    ranges.emplace_hint(next,first,last);
    return true;
  };
  for(const auto &state:states) {
    if(!seen.insert(state.entry).second)return invalid("duplicate entry");
    if(!entries.contains(state.entry) || !graph.isValidBlock(state.entry))
      return invalid("entry is not an admitted valid root");
    if(!charge(state.registers.size()) || !charge(state.memory.size()))return result;
    std::map<std::uint64_t,std::uint64_t> registers,memory;
    for(const auto &fact:state.registers) {
      if(fact.width<8 || fact.width>64 || fact.width%8)
        return invalid("register width must be whole bytes in 8..64");
      const auto count=bytes(fact.width);
      if(!charge(count))return result;
      if(fact.value>mask(fact.width))return invalid("register value does not fit its width");
      // The engine uses uint32 exclusive register ends; reject an end that
      // would wrap even when the fact's final byte itself fits uint32.
      if(fact.offset>UINT32_MAX-count)return invalid("register range overflows");
      if(!insertRange(registers,fact.offset,std::uint64_t(fact.offset)+count-1))
        return invalid("overlapping register facts");
    }
    for(const auto &fact:state.memory) {
      if(fact.bytes.empty())return invalid("empty memory range");
      if(!charge(fact.bytes.size()))return result;
      if(fact.bytes.size()-1>UINT64_MAX-fact.address)return invalid("memory range overflows");
      if(!insertRange(memory,fact.address,fact.address+fact.bytes.size()-1))
        return invalid("overlapping memory facts");
    }
  }
  return result;
}
template<typename Query>
FiniteInitialStateResult runInitial(const Graph &graph,const std::set<std::uint64_t> &given,
    std::span<const Query> queries,std::span<const FiniteEntryState> states,
    const FiniteValueLimits &limits) {
  auto width=[](const Query &query) {
    if constexpr(std::is_same_v<Query,FiniteMemoryQuery>)return query.width;
    else return ignil::widthOf(query.value);
  };
  FiniteInitialStateResult result;
  const auto validation=validateInitial(graph,given,states,limits);
  if(!validation.error.empty()) {
    result.analysis.complete=false;
    result.analysis.message=validation.error;
    result.analysis.operations=validation.cost;
    result.analysis.values.reserve(queries.size());
    for(const auto &query:queries)
      result.analysis.values.push_back({ValueSetKind::Unknown,width(query),{}});
    for(auto address:graph.validBlockAddresses()) {
      const auto *block=graph.getIGNILBlock(address);
      if(block && !block->empty() && block->ops().back().opcode==O::JMP && !block->ops().back().srcs.empty())
        result.analysis.jumpTargets.emplace(address,FiniteValues{ValueSetKind::Unknown,ignil::widthOf(block->ops().back().srcs[0]),{}});
    }
    return result;
  }
  result.assumptions.assign(states.begin(),states.end());
  result.assumptionsValid=true;
  result.analysis=runFinite(graph,given,queries,limits,result.assumptions,validation.cost);
  return result;
}
} // namespace

FiniteValueResult finiteValues(const Graph &graph,const std::set<std::uint64_t> &given,
    std::span<const FiniteValueQuery> queries,const FiniteValueLimits &limits) {
  return runFinite(graph,given,queries,limits);
}

FiniteValueResult finiteMemoryValues(const Graph &graph,const std::set<std::uint64_t> &given,
    std::span<const FiniteMemoryQuery> queries,const FiniteValueLimits &limits) {
  return runFinite(graph,given,queries,limits);
}

FiniteInitialStateResult finiteValuesWithInitialState(const Graph &graph,
    const std::set<std::uint64_t> &entries,std::span<const FiniteValueQuery> queries,
    std::span<const FiniteEntryState> states,const FiniteValueLimits &limits) {
  return runInitial(graph,entries,queries,states,limits);
}
FiniteInitialStateResult finiteMemoryValuesWithInitialState(const Graph &graph,
    const std::set<std::uint64_t> &entries,std::span<const FiniteMemoryQuery> queries,
    std::span<const FiniteEntryState> states,const FiniteValueLimits &limits) {
  return runInitial(graph,entries,queries,states,limits);
}

std::string FiniteValueResult::toJSON() const {
  auto encode=[](const FiniteValues &v) {
    llvm::json::Array values;
    for(auto x:v.values)values.push_back("0x"+llvm::utohexstr(x));
    return llvm::json::Object{{"kind",static_cast<int>(v.kind)},
      {"width",static_cast<unsigned>(v.width)},{"values",std::move(values)}};
  };
  llvm::json::Array targets,queried;
  for(const auto &[block,v]:jumpTargets) {
    auto object=encode(v);object["block"]="0x"+llvm::utohexstr(block);
    targets.push_back(std::move(object));
  }
  for(const auto &v:values)queried.push_back(encode(v));
  std::string text;llvm::raw_string_ostream out(text);
  out<<llvm::json::Value(llvm::json::Object{{"complete",complete},{"message",message},
    {"block_visits",blockVisits},{"operations",operations},
    {"proven_calls",provenCalls},{"rejected_calls",rejectedCalls},
    {"jump_targets",std::move(targets)},{"values",std::move(queried)}});
  return text;
}

std::string FiniteInitialStateResult::toJSON() const {
  auto json=llvm::cantFail(llvm::json::parse(analysis.toJSON()));
  auto &object=*json.getAsObject();
  object["conditional"]=true;
  object["assumptions_valid"]=assumptionsValid;
  llvm::json::Array entries;
  for(const auto &state:assumptions) {
    llvm::json::Array registers,memory;
    for(const auto &fact:state.registers)
      registers.push_back(llvm::json::Object{{"offset","0x"+llvm::utohexstr(fact.offset)},
          {"width",unsigned(fact.width)},{"value","0x"+llvm::utohexstr(fact.value)}});
    for(const auto &fact:state.memory) {
      constexpr char digits[]="0123456789abcdef";
      std::string bytes;bytes.reserve(fact.bytes.size()*2);
      for(auto byte:fact.bytes) {bytes.push_back(digits[byte>>4]);bytes.push_back(digits[byte&15]);}
      memory.push_back(llvm::json::Object{{"address","0x"+llvm::utohexstr(fact.address)},
          {"bytes",std::move(bytes)}});
    }
    entries.push_back(llvm::json::Object{{"entry","0x"+llvm::utohexstr(state.entry)},
        {"registers",std::move(registers)},{"memory",std::move(memory)}});
  }
  object["entry_states"]=std::move(entries);
  std::string text;llvm::raw_string_ostream out(text);out<<json;
  return text;
}
} // namespace dragon::analysis
