#include <dragon/lifters/ignil/IGNIL.hpp>
#include <dragon/lifters/ignil/IGNILSimplify.hpp>
#include <array>
#include <iostream>
#include <string>

using namespace dragon::ignil;

int main(int argc,char **argv) {
  if (argc!=2) return 2;
  const std::string mode=argv[1];
  const Reg result{0,64,"result"};
  if (mode!="sweep") {
    Opcode opcode;
    std::vector<Value> sources;
    if (mode=="neg" || mode=="div" || mode=="mod") {
      opcode=mode=="neg" ? Opcode::NEG : mode=="div" ? Opcode::SDIV : Opcode::SMOD;
      sources.push_back(Const{std::uint64_t{1}<<63,64});
      if (mode!="neg") sources.push_back(Const{~std::uint64_t{0},64});
    } else if (mode=="zero") {
      opcode=Opcode::SEXT;sources.push_back(Const{0,0});
    } else return 2;
    Block block{0x1000};block.append({opcode,result,sources,0x1000});
    auto out=simplifyBlock(block);
    if (out.size()!=1) return 3;
    const auto &op=out.ops()[0];
    if (mode=="neg") {
      if (op.opcode!=Opcode::COPY || op.srcs.size()!=1) return 4;
      const auto *c=std::get_if<Const>(&op.srcs[0]);
      if (!c || c->width!=64 || c->value!=(std::uint64_t{1}<<63)) return 5;
    } else if (op.opcode!=opcode) return 6;
    std::cout<<toString(out);return 0;
  }

  // This target instruments the production simplifier translation unit.
  // Consume the folded results so arithmetic cannot disappear as dead work.
  const std::array binary{Opcode::ADD,Opcode::SUB,Opcode::MUL,Opcode::AND,Opcode::OR,Opcode::XOR,
      Opcode::SHL,Opcode::SHR,Opcode::SAR,Opcode::UDIV,Opcode::SDIV,Opcode::UMOD,Opcode::SMOD,
      Opcode::EQ,Opcode::NEQ,Opcode::ULT,Opcode::ULE,Opcode::UGT,Opcode::UGE,
      Opcode::SLT,Opcode::SLE,Opcode::SGT,Opcode::SGE};
  unsigned combinations=0;std::uint64_t checksum=0;
  auto consume=[&](const Block &block) {
    const auto out=simplifyBlock(block);
    if (out.size()!=1) return false;
    const auto &op=out.ops()[0];
    checksum+=static_cast<unsigned>(op.opcode);
    if (!op.srcs.empty())
      if (const auto *c=std::get_if<Const>(&op.srcs[0])) checksum^=c->value;
    ++combinations;return true;
  };
  for (unsigned bits=1;bits<=64;++bits) {
    const auto width=static_cast<Width>(bits);
    const auto mask=bits==64 ? ~std::uint64_t{0} : (std::uint64_t{1}<<bits)-1;
    const auto sign=std::uint64_t{1}<<(bits-1);
    const std::array values{std::uint64_t{0},std::uint64_t{1},mask,sign,sign-1,
        std::uint64_t(bits-1),std::uint64_t(bits),std::uint64_t(bits+1)};
    for (auto left:values) {
      for (auto right:values) for (auto opcode:binary) {
        const bool comparison=opcode==Opcode::EQ || opcode==Opcode::NEQ ||
            opcode==Opcode::ULT || opcode==Opcode::ULE || opcode==Opcode::UGT || opcode==Opcode::UGE ||
            opcode==Opcode::SLT || opcode==Opcode::SLE || opcode==Opcode::SGT || opcode==Opcode::SGE;
        Block block{0x1000};
        block.append({opcode,Reg{0,comparison ? Width{1} : width,"result"},
                      {Const{left,width},Const{right,width}},0x1000});
        if (!consume(block)) return 7;
      }
      for (Width target:{Width{1},Width{8},Width{16},Width{32},Width{64},Width{128}})
        for (auto opcode:{Opcode::NEG,Opcode::NOT,Opcode::ZEXT,Opcode::SEXT,Opcode::TRUNC}) {
          Block block{0x1000};
          block.append({opcode,Reg{0,target,"result"},{Const{left,width}},0x1000});
          if (!consume(block)) return 8;
        }
    }
  }
  std::cout<<"sanitized fold combinations="<<combinations<<" checksum="<<checksum<<'\n';
  return combinations==109568 ? 0 : 9;
}
