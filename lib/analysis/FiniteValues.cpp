#include <dragon/analysis/FiniteValues.hpp>
#include "LeafCallEffects.hpp"
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/disassembler/BasicBlock.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>
#include <dragon/lifters/ignil/arm/armIGNILRegs.hpp>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <bit>
#include <deque>
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
    unsigned end=reg.offset+bytes(reg.width);
    if(reg.width==32) {
      bool found=false;
      for(const auto &p:parents)
        if(p.offset<=reg.offset && end<=p.offset+p.byteSize) {end=p.offset+p.byteSize;found=true;break;}
      if(!found)end=reg.offset+8;
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
    auto it=callProofs.find(target->value);
    if(it==callProofs.end()) {
      auto proof=detail::proveLeafCallee(graph,target->value,std::min(limits.maxBlocks,32u),
          std::min<std::uint64_t>(4096,limits.maxOperations-result.operations));
      result.operations+=proof.operations;
      proofFacts+=1+proof.clobberedBytes.size();
      it=callProofs.emplace(target->value,std::move(proof)).first;
      if(proofFacts>limits.maxFacts) {failed=true;return reject();}
    }
    if(!it->second.proven)return reject();
    const auto sp=arch==Architecture::X86_64?x86::RSP:x86::ESP;
    // IGNIL abstracts away this write, but a proof used for machine control-flow
    // recovery must not preserve stale data in the near CALL's return slot.
    auto stack=readRegister(t.state,sp.offset,sp.width);
    auto slot=operation(O::SUB,stack,constant(width/8,width),width,false);
    store(t.state,slot,Value{width});
    for(auto byte:it->second.clobberedBytes)writeRegister(t.state,ignil::Reg{byte,8,{}},Value{8});
    provenSites.insert(op.address);result.provenCalls=provenSites.size();
    return true;
  }
  Transfer transfer(std::uint64_t block,const State &in) {
    Transfer t{in,{}};
    const auto *ib=graph.getIGNILBlock(block);
    if(!ib || !ib->unlifted().empty() ||
       std::ranges::any_of(ib->ops(),[](const auto &op){return ignil::isLocalControl(op.opcode);})) {
      t.state.regs.clear();t.state.memory.clear();return t;
    }
    std::map<std::uint32_t,const ignil::Op *> definitions;
    for(const auto &op:ib->ops()) {
      if(!spend())return t;
      if(op.opcode==O::CALL) {
        if(!call(t,op)) {t.state.regs.clear();t.state.memory.clear();}
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
      if(auto selected=select(op,t,definitions))v=std::move(*selected);
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
  State entry(std::uint64_t identity) {
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
  std::map<std::uint64_t,detail::LeafCallProof> callProofs;
  std::set<std::uint64_t> provenSites,rejectedSites;
  std::uint64_t proofFacts=0;
  bool failed=false;
};
} // namespace

FiniteValueResult finiteValues(const Graph &graph,const std::set<std::uint64_t> &given,
    std::span<const FiniteValueQuery> queries,const FiniteValueLimits &limits) {
  Engine e{graph,limits};
  std::map<std::uint64_t,State> states;
  std::map<std::uint64_t,std::vector<std::uint64_t>> successors;
  const auto predecessors=BackwardSlicer::predecessors(graph);
  for(const auto &[at,from]:predecessors)for(auto p:from)successors[p].push_back(at);
  auto entries=given;
  if(entries.empty())entries=graph.validFunctionAddresses();
  if(entries.empty())for(const auto &[at,from]:predecessors)if(from.empty())entries.insert(at);
  if(entries.empty())entries=graph.validBlockAddresses(); // Cyclic rootless IR: admit inputs everywhere.
  // Direct callees have independent input states; CALL itself stays a barrier.
  for(const auto &[at,insn]:graph.listing())for(auto callee:insn.callees())entries.insert(callee);
  std::deque<std::uint64_t> queue;std::set<std::uint64_t> queued;
  std::uint64_t identity=1;
  for(auto at:entries)if(graph.isValidBlock(at)) {
    auto in=e.entry(identity++);e.merge(states[at],in);queue.push_back(at);queued.insert(at);
  }
  if(!limits.maxValues || limits.maxValues>256 || !limits.maxBlocks ||
     !limits.maxOperations || !limits.maxFacts) e.failed=true;
  std::uint64_t facts=0;
  for(const auto &[at,state]:states)facts+=state.regs.size()+state.memory.size();
  if(states.size()>limits.maxBlocks || facts>limits.maxFacts)e.failed=true;
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
    e.result.values[i]={ValueSetKind::Unreachable,ignil::widthOf(queries[i].value),{}};
  }
  for(auto at:graph.validBlockAddresses()) {
    const auto *ib=graph.getIGNILBlock(at);
    const bool jump=ib && !ib->empty() && ib->ops().back().opcode==O::JMP && !ib->ops().back().srcs.empty();
    if(!jump && !requested.contains(at))continue;
    const auto it=states.find(at);const bool reached=it!=states.end() && it->second.reached;
    Transfer out=reached && !e.failed?e.transfer(at,it->second):Transfer{};
    if(jump) {
      const auto &value=ib->ops().back().srcs[0];
      e.result.jumpTargets.emplace(at,e.report(reached?e.operand(out,value):Value{ignil::widthOf(value)},reached));
    }
    for(auto i:requested[at])
      e.result.values[i]=e.report(reached?e.operand(out,queries[i].value):Value{ignil::widthOf(queries[i].value)},reached);
  }
  if(e.failed) {
    e.result.complete=false;e.result.message="finite-value analysis budget exhausted or invalid limits";
    for(auto &[at,value]:e.result.jumpTargets)value={ValueSetKind::Unknown,value.width,{}};
    for(std::size_t i=0;i<queries.size();++i)e.result.values[i]={ValueSetKind::Unknown,ignil::widthOf(queries[i].value),{}};
  }
  return std::move(e.result);
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
} // namespace dragon::analysis
