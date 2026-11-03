#include "LLVMImportMetadata.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <optional>
#include <algorithm>

namespace dragon::detail {
namespace {
struct Value {
  std::uint64_t number;
  bool imported=false; // number is an import slot, never the loaded Guest target
};
using Known=std::optional<Value>;
using Registers=std::map<std::pair<unsigned,unsigned>,Value>;
class Provenance {
 public:
  explicit Provenance(const Graph& graph): graph_(graph),slots_(GraphLLVMMetadata::imports(graph)),
      pointerWidth_(graph.architecture()==Architecture::X86_32 ? 32 : 64) {}
  Known get(const ignil::Value& value) const {
    if (const auto *c=std::get_if<ignil::Const>(&value)) return Value{c->value};
    if (const auto *r=std::get_if<ignil::Reg>(&value)) {
      auto it=registers_.find({r->offset,r->width});return it==registers_.end() ? Known{} : it->second;
    }
    const auto &t=std::get<ignil::Temp>(value);auto it=temps_.find(t.id);
    return it==temps_.end() ? Known{} : it->second;
  }
  void clear() {registers_.clear();temps_.clear();}
  void apply(const ignil::Op& op) {
    using O=ignil::Opcode;
    if (op.opcode==O::CALL || op.opcode==O::OPAQUE || op.opcode==O::LABEL) {clear();return;}
    if (!op.dst) return;
    const auto width=ignil::widthOf(*op.dst);
    Known value;
    const auto a=op.srcs.empty() ? Known{} : get(op.srcs[0]);
    const auto b=op.srcs.size()<2 ? Known{} : get(op.srcs[1]);
    if (width<=64) {
      if (op.opcode==O::COPY && a && (!a->imported || width==pointerWidth_)) value=a;
      else if (op.opcode==O::LOAD && a && !a->imported && width==pointerWidth_ && slots_.contains(a->number)) value=Value{a->number,true};
      else if (a && b && !a->imported && !b->imported) {
        switch(op.opcode) {
          case O::ADD:value=Value{a->number+b->number};break;
          case O::SUB:value=Value{a->number-b->number};break;
          case O::AND:value=Value{a->number&b->number};break;
          case O::OR:value=Value{a->number|b->number};break;
          case O::SHL:if(b->number<width)value=Value{a->number<<b->number};break;
          default:break;
        }
      } else if (a && !a->imported && (op.opcode==O::ZEXT || op.opcode==O::TRUNC)) value=a;
      if (value && !value->imported && width<64) value->number&=(1ull<<width)-1;
    }
    if (const auto *r=std::get_if<ignil::Reg>(&*op.dst)) {
      const auto bytes=(r->width+7)/8;
      const auto killedBytes=r->width==32 && (graph_.architecture()==Architecture::X86_64 || graph_.architecture()==Architecture::ARM64) ? 8 : bytes;
      for(auto it=registers_.begin();it!=registers_.end();) {
        const auto [off,w]=it->first;
        if(off<r->offset+killedBytes && r->offset<off+(w+7)/8) it=registers_.erase(it);else ++it;
      }
      if (value) {
        registers_[{r->offset,r->width}]=*value;
        if (killedBytes==8 && r->width==32 && !value->imported) registers_[{r->offset,64}]=*value;
      }
    } else if (const auto *t=std::get_if<ignil::Temp>(&*op.dst)) {
      temps_.erase(t->id);if(value)temps_[t->id]=*value;
    }
  }
 private:
  const Graph& graph_;
  const std::map<std::uint64_t,std::string>& slots_;
  unsigned pointerWidth_;
  Registers registers_;
  std::map<unsigned,Value> temps_;
};
Known thunk(const Graph& graph,std::uint64_t target,std::set<std::uint64_t>& seen) {
  if (seen.size()>=8 || !seen.insert(target).second) return {};
  const auto *block=graph.getIGNILBlock(target);
  if(!block || !block->unlifted().empty())return {};
  Provenance values(graph);
  for (const auto &op:block->ops()) {
    using O=ignil::Opcode;
    if (op.opcode==O::CALL || op.opcode==O::STORE || op.opcode==O::OPAQUE || op.opcode==O::CJMP || op.opcode==O::RET || op.opcode==O::LOCALJMP || op.opcode==O::LOCALCJMP) return {};
    if(op.opcode==O::JMP) {
      if(op.srcs.size()!=1)return {};
      const auto destination=values.get(op.srcs[0]);
      if(!destination)return {};
      return destination->imported ? destination : thunk(graph,destination->number,seen);
    }
    values.apply(op);
  }
  return {};
}
}

void recordLLVMImports(llvm::Module& module,llvm::Function& implementation,const Function& function,const Graph& graph) {
  const auto &slots=GraphLLVMMetadata::imports(graph);if(slots.empty())return;
  auto &context=module.getContext();
  for (const auto &[address,block]:function.blocks()) {
    const auto *ir=graph.getIGNILBlock(address);if(!ir)continue;
    Provenance values(graph);
    const auto firstMissing=ir->unlifted().empty() ? UINT64_MAX : *std::min_element(ir->unlifted().begin(),ir->unlifted().end());
    for (const auto &op:ir->ops()) {
      if(op.address>=firstMissing || op.opcode==ignil::Opcode::OPAQUE)break;
      if((op.opcode==ignil::Opcode::CALL || op.opcode==ignil::Opcode::JMP) && op.srcs.size()==1) {
        auto target=values.get(op.srcs[0]);
        if(target && !target->imported) {std::set<std::uint64_t> seen;target=thunk(graph,target->number,seen);}
        if(target && target->imported && slots.contains(target->number)) {
          auto integer=[&](std::uint64_t n){return llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),n));};
          module.getOrInsertNamedMetadata("dragon.imports")->addOperand(llvm::MDNode::get(context,{
              llvm::MDString::get(context,implementation.getName()),integer(op.address),integer(target->number),llvm::MDString::get(context,slots.at(target->number))}));
        }
      }
      values.apply(op);
    }
  }
}
}
