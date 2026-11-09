#pragma once
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace dragon::analysis::detail {
struct FiniteExpressionBound {
  ignil::Width width{};
  std::vector<std::uint64_t> values;
};
struct FiniteExpressionFacts {
  std::map<std::size_t,FiniteExpressionBound> byOp;
  std::uint64_t storageFacts{};
  [[nodiscard]] std::uint64_t facts() const noexcept {return storageFacts;}
};

namespace finite_expression_detail {
// Only the bounds survive construction. The bounded DAG describes actual
// operand snapshots, not equality of abstract Unknowns or of memory addresses.
template<class Spend> class Builder {
  using Width=ignil::Width;
  using Id=std::size_t;
  using Span=std::pair<std::uint32_t,Width>;
  enum class Kind {Leaf,Constant,Or};
  struct Node {Kind kind;Width width;std::uint64_t value;Id left{},right{};};
  struct Temp {Width width;Id id;};
  using Key=std::tuple<Kind,Width,std::uint64_t,Id,Id>;
  static constexpr std::uint64_t hardFacts=65536,hardNodes=8192;
  const ignil::Block &block_;
  std::span<const ignil::RegExtent> parents_;
  std::uint32_t maxValues_;
  std::uint64_t maxFacts_;
  Spend &spend_;
  bool stopped_=false;
  std::vector<Node> nodes_;
  std::map<Key,Id> intern_;
  std::map<Span,Id> regs_;
  std::map<std::uint32_t,Temp> temps_;
  FiniteExpressionFacts plan_;

  static bool valid(Width w) {return w && w<=64;}
  static unsigned bytes(Width w) {return (unsigned(w)+7)/8;}
  static std::uint64_t mask(Width w) {
    return w==64?~std::uint64_t{0}:(std::uint64_t{1}<<w)-1;
  }
  bool step() {
    if(stopped_ || !spend_()) {stopped_=true;return false;}
    return true;
  }
  bool room(std::uint64_t count) {
    const std::uint64_t used=nodes_.size()+intern_.size()+regs_.size()+temps_.size()+plan_.facts();
    if(used>maxFacts_ || count>maxFacts_-used) {stopped_=true;return false;}
    return true;
  }
  Id node(Kind kind,Width width,std::uint64_t value=0,Id left=0,Id right=0) {
    if(!valid(width) || !step())return 0;
    const Key key{kind,width,value,left,right};
    if(kind!=Kind::Leaf) {
      if(!step())return 0;
      const auto found=intern_.find(key);
      if(found!=intern_.end())return found->second;
    }
    if(nodes_.size()>=hardNodes || !room(kind==Kind::Leaf?1:2)) {stopped_=true;return 0;}
    nodes_.push_back({kind,width,value,left,right});
    const Id id=nodes_.size();
    if(kind!=Kind::Leaf)intern_.emplace(key,id);
    return id;
  }
  const Node &get(Id id) const {return nodes_[id-1];}
  Id operand(const ignil::Value &value) {
    if(!step())return 0;
    const auto width=ignil::widthOf(value);
    if(!valid(width))return 0;
    if(const auto *c=std::get_if<ignil::Const>(&value))
      return node(Kind::Constant,width,c->value&mask(width));
    if(const auto *t=std::get_if<ignil::Temp>(&value)) {
      const auto it=temps_.find(t->id);
      return it!=temps_.end() && it->second.width==width?it->second.id:0;
    }
    const auto &r=std::get<ignil::Reg>(value);
    if(r.offset>UINT32_MAX-bytes(width))return 0;
    const Span span{r.offset,width};const auto it=regs_.find(span);
    if(it!=regs_.end())return it->second;
    const Id id=node(Kind::Leaf,width);
    if(!id || !room(1))return 0;
    regs_.emplace(span,id);return id;
  }
  bool clearRegisters() {
    for(std::size_t i=0;i<regs_.size();++i)if(!step())return false;
    regs_.clear();return true;
  }
  bool write(const ignil::Value &destination,Id value) {
    if(!step())return false;
    const auto width=ignil::widthOf(destination);
    if(const auto *t=std::get_if<ignil::Temp>(&destination)) {
      if(!temps_.contains(t->id) && !room(1))return false;
      temps_.insert_or_assign(t->id,Temp{width,value});return true;
    }
    const auto *r=std::get_if<ignil::Reg>(&destination);
    if(!r || !width || r->offset>UINT32_MAX-bytes(width))return false;
    std::uint64_t end=std::uint64_t(r->offset)+bytes(width);
    if(width==32) {
      bool found=false;
      for(const auto &p:parents_) {
        if(!step())return false;
        if(p.offset<=r->offset && end<=std::uint64_t(p.offset)+p.byteSize) {
          end=std::uint64_t(p.offset)+p.byteSize;found=true;break;
        }
      }
      if(!found)end=std::uint64_t(r->offset)+8;
      if(end>UINT32_MAX)return false;
    }
    for(auto it=regs_.begin();it!=regs_.end();) {
      if(!step())return false;
      const auto begin=it->first.first;
      const auto stop=std::uint64_t(begin)+bytes(it->first.second);
      if(stop<=r->offset || end<=begin)++it;
      else it=regs_.erase(it);
    }
    // An unsupported/wide destination still invalidates every affected span.
    if(value && valid(width)) {
      if(!room(1))return false;
      regs_.emplace(Span{r->offset,width},value);
    }
    return true;
  }
  bool bound(std::size_t index,Width width,Id left,Id right) {
    if(!left || !right || !valid(width))return true;
    const auto &a=get(left);
    if(a.kind!=Kind::Or || a.width!=width || a.left!=right)return true;
    const auto &m=get(a.right);
    if(m.kind!=Kind::Constant || m.width!=width || get(right).width!=width)return true;
    // In arithmetic modulo 2^width, (x|m)-x == (~x)&m for every x.
    // Subsets are an upper bound even if x itself has a smaller value domain.
    const auto count=std::popcount(m.value);
    if(count>=64 || (std::uint64_t{1}<<count)>maxValues_)return true;
    const auto size=std::uint64_t{1}<<count;
    if(!room(1+size))return false;
    FiniteExpressionBound result{width,{}};result.values.reserve(size);
    for(std::uint64_t subset=0;;subset=(subset-m.value)&m.value) {
      if(!step())return false;
      result.values.push_back(subset);
      if(subset==m.value)break;
    }
    if(!step())return false;
    plan_.byOp.emplace(index,std::move(result));plan_.storageFacts+=1+size;
    return true;
  }
  Id definition(const ignil::Op &op,std::size_t index) {
    const auto width=ignil::widthOf(*op.dst);
    if(!valid(width))return 0;
    if(op.opcode==ignil::Opcode::COPY && op.srcs.size()==1 &&
       ignil::widthOf(op.srcs[0])==width)return operand(op.srcs[0]);
    if((op.opcode==ignil::Opcode::OR || op.opcode==ignil::Opcode::SUB) && op.srcs.size()==2 &&
       ignil::widthOf(op.srcs[0])==width && ignil::widthOf(op.srcs[1])==width) {
      Id left=operand(op.srcs[0]),right=operand(op.srcs[1]);
      if(op.opcode==ignil::Opcode::SUB) {
        if(!bound(index,width,left,right))return 0;
      } else if(left && right) {
        // Keep one canonical x|literal spelling, independent of operand order.
        if(get(left).kind==Kind::Constant && get(right).kind!=Kind::Constant)std::swap(left,right);
        if(get(right).kind==Kind::Constant)return node(Kind::Or,width,0,left,right);
      }
    }
    // LOAD and UNDEF always make a fresh leaf, even for equal addresses or
    // equal operation spellings. Unsupported operations are opaque as well.
    return node(Kind::Leaf,width);
  }
 public:
  Builder(const ignil::Block &block,std::span<const ignil::RegExtent> parents,
          std::uint32_t maxValues,std::uint64_t maxFacts,Spend &spend)
      :block_(block),parents_(parents),maxValues_(maxValues),
       maxFacts_(maxFacts<hardFacts?maxFacts:hardFacts),spend_(spend) {}
  std::optional<FiniteExpressionFacts> run() {
    if(!maxValues_ || maxValues_>256 || !maxFacts_ || !block_.unlifted().empty())return {};
    for(const auto &p:parents_) {
      if(!step() || !p.byteSize || p.offset>UINT32_MAX-p.byteSize)return {};
    }
    const auto &ops=block_.ops();
    for(std::size_t index=0;index<ops.size();++index) {
      if(!step())return {};
      const auto &op=ops[index];
      if(ignil::isLocalControl(op.opcode))return {};
      if((op.opcode==ignil::Opcode::JMP || op.opcode==ignil::Opcode::CJMP ||
          op.opcode==ignil::Opcode::RET) && index+1!=ops.size())return {};
      // The runtime CALL/STORE handlers ignore dst entirely. Do not assign
      // snapshot identities for malformed destinations they never execute.
      if(op.dst && (op.opcode==ignil::Opcode::CALL ||
                    op.opcode==ignil::Opcode::STORE))return {};
      if(op.opcode==ignil::Opcode::CALL && !clearRegisters())return {};
      if(!op.dst)continue;
      const auto value=definition(op,index);
      if(stopped_ || !write(*op.dst,value))return {};
    }
    if(stopped_)return {};
    return std::move(plan_);
  }
};
} // namespace finite_expression_detail

// Build once per immutable block/analysis. Nullopt means no usable plan,
// including a local cap or spend() failure; no partial bound list is returned.
// The caller owns the callback's global budget failure and must charge facts()
// while retaining this plan. Scratch DAG/bindings are capped at min(maxFacts,
// 65536), with at most 8192 nodes; every scan/node/output is spend()-charged.
// Register extents must be the same fixed target layout used by the caller's
// write model. With no matching extent, a 32-bit write invalidates eight bytes.
// Stored entries contain only integer tautologies: no runtime/entry/memory
// assumptions, no Graph mutation and no relation between separate LOAD events.
template<class Spend>
std::optional<FiniteExpressionFacts> buildFiniteExpressionFacts(
    const ignil::Block &block,std::span<const ignil::RegExtent> parents,
    std::uint32_t maxValues,std::uint64_t maxFacts,Spend &&spend) {
  finite_expression_detail::Builder<std::remove_reference_t<Spend>> builder(
      block,parents,maxValues,maxFacts,spend);
  return builder.run();
}
} // namespace dragon::analysis::detail
