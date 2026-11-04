#include <catch2/catch_test_macros.hpp>
#include <dragon/loader/ImageDisassembler.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include "../loader/fixtures/fixtures.hpp"
#include <cstring>
#include <set>

namespace {
std::uint64_t read(const std::vector<std::uint8_t>& b,std::size_t p,unsigned n) {
  std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(b.at(p+i))<<(8*i);return v;
}
void write(std::vector<std::uint8_t>& b,std::size_t p,std::uint64_t v,unsigned n) {
  for(unsigned i=0;i<n;++i)b.at(p+i)=v>>(8*i);
}
auto image(bool pe,const std::vector<std::uint8_t>& code,bool a64=false,bool conflictingName=false) {
  using namespace dragon::loader;
  auto b=pe ? std::vector<std::uint8_t>(std::begin(kImpExpPeX64),std::end(kImpExpPeX64))
            : std::vector<std::uint8_t>(std::begin(kSlotsElfX64),std::end(kSlotsElfX64));
  const auto offset=b.size();b.insert(b.end(),code.begin(),code.end());
  if (conflictingName) {
    const std::string name="GetTickCount";auto found=std::search(b.begin(),b.end(),name.begin(),name.end());
    REQUIRE(found!=b.end());std::copy_n("Bad",3,found);
  }
  if(pe) {
    const auto nt=read(b,0x3c,4),opt=nt+24,section=opt+read(b,nt+20,2);
    write(b,section+8,code.size(),4);write(b,section+16,code.size(),4);write(b,section+20,offset,4);
    write(b,opt+112,0,8); // omit unrelated export entry hints inside the synthetic code
  } else {
    const auto sh=read(b,0x28,8)+64;
    write(b,0x18,0x400100,8);write(b,sh+16,0x400100,8);write(b,sh+24,offset,8);write(b,sh+32,code.size(),8);
    if(a64) {
      write(b,18,183,2);const auto rela=read(b,read(b,0x28,8)+4*64+24,8);
      write(b,rela+8,(1ull<<32)|1026,8);write(b,rela+32,(2ull<<32)|1025,8);write(b,rela+56,1027,8);
    }
  }
  return LoadedImage::loadBytes(std::move(b));
}
auto elf32(const std::vector<std::uint8_t>& code) {
  std::vector<std::uint8_t> b(1024);
  const std::string strings("\0puts\0__gmon_start__\0",21),names("\0.text\0.dynsym\0.dynstr\0.rel.plt\0.shstrtab\0",43);
  b[0]=0x7f;b[1]='E';b[2]='L';b[3]='F';b[4]=1;b[5]=1;b[6]=1;
  write(b,16,3,2);write(b,18,3,2);write(b,20,1,4);write(b,24,0x400100,4);
  write(b,28,52,4);write(b,32,0x300,4);write(b,40,52,2);write(b,42,32,2);
  write(b,44,1,2);write(b,46,40,2);write(b,48,6,2);write(b,50,5,2);
  for (const auto &[off,v]:std::vector<std::pair<unsigned,unsigned>>{{52,1},{56,0},{60,0x400000},{64,0x400000},{68,1024},{72,4096},{76,7},{80,4096}})write(b,off,v,4);
  std::copy(code.begin(),code.end(),b.begin()+0x100);
  std::copy(strings.begin(),strings.end(),b.begin()+0x230);std::copy(names.begin(),names.end(),b.begin()+0x280);
  write(b,0x210,1,4);b[0x21c]=0x12;write(b,0x220,6,4);b[0x22c]=0x12;
  write(b,0x260,0x400818,4);write(b,0x264,(1<<8)|7,4);write(b,0x268,0x400820,4);write(b,0x26c,(2<<8)|6,4);
  auto section=[&](unsigned index,unsigned name,unsigned type,unsigned flags,unsigned off,unsigned size,unsigned link,unsigned info,unsigned alignment,unsigned entrySize) {
    const auto p=0x300+index*40;
    const unsigned values[]={name,type,flags,0x400000+off,off,size,link,info,alignment,entrySize};
    for(unsigned i=0;i<10;++i)write(b,p+i*4,values[i],4);
  };
  section(1,1,1,6,0x100,code.size(),0,0,16,0);section(2,7,11,2,0x200,48,3,1,4,16);
  section(3,15,3,2,0x230,strings.size(),0,0,1,0);section(4,23,9,2,0x260,16,2,1,4,8);
  section(5,32,3,0,0x280,names.size(),0,0,1,0);
  return dragon::loader::LoadedImage::loadBytes(std::move(b));
}
void check(bool pe,const std::vector<std::uint8_t>& code,bool named=true,bool a64=false) {
  auto loaded=image(pe,code,a64);REQUIRE(loaded);REQUIRE(loaded->importSlots().size()==2);
  auto graph=dragon::loader::disassembleImage(*loaded,dragon::Configuration{});
  dragon::Function root{&graph,*loaded->entryPoint()};REQUIRE(root.isValid());
  dragon::llvmlifter::LLVMLifter lifter{loaded->architecture()};
  REQUIRE(lifter.setExecutionMode(dragon::llvmlifter::ExecutionMode::StatefulV2));REQUIRE(lifter.lift(root,graph));
  CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  const auto *metadata=lifter.module().getNamedMetadata("dragon.imports");
  if(named) {
    CHECK(metadata);CHECK(lifter.ir().find(pe ? "GetTickCount" : "puts")!=std::string::npos);
    if(metadata)for(const auto *node:metadata->operands()) {
      REQUIRE(node->getNumOperands()==4);
      auto *slot=llvm::mdconst::dyn_extract<llvm::ConstantInt>(node->getOperand(2));REQUIRE(slot);
      auto *name=llvm::dyn_cast<llvm::MDString>(node->getOperand(3));REQUIRE(name);
      CHECK(loaded->importSlots().at(slot->getZExtValue())==name->getString());
    }
  }
  else CHECK_FALSE(metadata);
  CHECK(lifter.ir().find("declare void @puts")==std::string::npos);
  CHECK(lifter.ir().find("declare void @GetTickCount")==std::string::npos);
}
}
TEST_CASE("V2 identifies PE IAT and ELF GOT calls without invoking host symbols", "[llvm][imports]") {
  check(true,{0xff,0x15,0x5a,0x10,0,0,0xc3});
  check(false,{0xff,0x15,0x12,0x07,0,0,0xc3});
}
TEST_CASE("V2 follows import thunks and register-loaded slots", "[llvm][imports]") {
  check(true,{0x48,0x8b,0x05,0x59,0x10,0,0,0xff,0xd0,0xc3});
  for(bool pe:{false,true}) {
    std::vector<std::uint8_t> b{0xe8,0x0b,0,0,0,0xc3};b.resize(16,0xcc);
    const std::vector<std::uint8_t> thunk=pe ? std::vector<std::uint8_t>{0xff,0x25,0x4a,0x10,0,0}
                                               : std::vector<std::uint8_t>{0xff,0x25,0x02,0x07,0,0};
    b.insert(b.end(),thunk.begin(),thunk.end());check(pe,b);
  }
}
TEST_CASE("V2 import provenance stops at unknown inputs and overwritten registers", "[llvm][imports]") {
  check(true,{0xff,0x10,0xc3},false);
  check(true,{0x48,0x8b,0x05,0x59,0x10,0,0,0x31,0xc0,0xff,0xd0,0xc3},false);
  std::vector<std::uint8_t> cycle{0xe8,0x0b,0,0,0,0xc3};cycle.resize(16,0xcc);
  const std::vector<std::uint8_t> jump{0xe9,0xfb,0xff,0xff,0xff};cycle.insert(cycle.end(),jump.begin(),jump.end());
  check(false,cycle,false);
}

TEST_CASE("Graph merge discards conflicting import names", "[llvm][imports]") {
  const std::vector<std::uint8_t> code{0xff,0x15,0x5a,0x10,0,0,0xc3};
  auto a=image(true,code),b=image(true,code,false,true);REQUIRE(a);REQUIRE(b);
  auto graph=dragon::loader::disassembleImage(*a,dragon::Configuration{});
  graph.merge(dragon::loader::disassembleImage(*b,dragon::Configuration{}));
  dragon::Function root{&graph,*a->entryPoint()};REQUIRE(root.isValid());
  dragon::llvmlifter::LLVMLifter lifter{a->architecture()};
  REQUIRE(lifter.setExecutionMode(dragon::llvmlifter::ExecutionMode::StatefulV2));REQUIRE(lifter.lift(root,graph));
  CHECK_FALSE(lifter.module().getNamedMetadata("dragon.imports"));
  // A later graph must not resurrect a slot already known to be ambiguous.
  graph.merge(dragon::loader::disassembleImage(*a,dragon::Configuration{}));
  dragon::llvmlifter::LLVMLifter second{a->architecture()};
  REQUIRE(second.setExecutionMode(dragon::llvmlifter::ExecutionMode::StatefulV2));REQUIRE(second.lift(root,graph));
  CHECK_FALSE(second.module().getNamedMetadata("dragon.imports"));
}
TEST_CASE("V2 identifies A64 GOT and PLT slot provenance", "[llvm][imports]") {
  check(false,{0x10,0,0,0x90,0x11,0x0e,0x44,0xf9,0x20,0x02,0x3f,0xd6,0xc0,0x03,0x5f,0xd6},true,true);
  std::vector<std::uint8_t> b{0x04,0,0,0x94,0xc0,0x03,0x5f,0xd6};b.resize(16,0);
  const std::vector<std::uint8_t> thunk{0x10,0,0,0x90,0x11,0x0e,0x44,0xf9,0x10,0x62,0x20,0x91,0x20,0x02,0x1f,0xd6};
  b.insert(b.end(),thunk.begin(),thunk.end());check(false,b,true,true);
}

TEST_CASE("V2 identifies i386 absolute GOT calls and PLT thunks", "[llvm][imports]") {
  for (bool thunk:{false,true}) {
    std::vector<std::uint8_t> code=thunk ? std::vector<std::uint8_t>{0xe8,0x0b,0,0,0,0xc3}
                                             : std::vector<std::uint8_t>{0xff,0x15,0x18,0x08,0x40,0,0xc3};
    if(thunk) {code.resize(16,0xcc);const std::vector<std::uint8_t> stub{0xff,0x25,0x18,0x08,0x40,0};code.insert(code.end(),stub.begin(),stub.end());}
    auto loaded=elf32(code);REQUIRE(loaded);REQUIRE(loaded->importSlots().at(0x400818)=="puts");
    auto graph=dragon::loader::disassembleImage(*loaded,dragon::Configuration{});
    dragon::Function root{&graph,0x400100};REQUIRE(root.isValid());
    dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_32};
    REQUIRE(lifter.setExecutionMode(dragon::llvmlifter::ExecutionMode::StatefulV2));REQUIRE(lifter.lift(root,graph));
    CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
    CHECK(lifter.module().getNamedMetadata("dragon.imports"));CHECK(lifter.ir().find("puts")!=std::string::npos);
  }
}

namespace {
// (site, name) of every !dragon.imports entry for the image's entry function.
std::multiset<std::pair<std::uint64_t,std::string>> importSites(const std::vector<std::uint8_t>& code,bool a64) {
  auto loaded=image(false,code,a64);REQUIRE(loaded);
  auto graph=dragon::loader::disassembleImage(*loaded,dragon::Configuration{});
  dragon::Function root{&graph,*loaded->entryPoint()};REQUIRE(root.isValid());
  dragon::llvmlifter::LLVMLifter lifter{loaded->architecture()};
  REQUIRE(lifter.setExecutionMode(dragon::llvmlifter::ExecutionMode::StatefulV2));REQUIRE(lifter.lift(root,graph));
  CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
  std::multiset<std::pair<std::uint64_t,std::string>> out;
  if(const auto *metadata=lifter.module().getNamedMetadata("dragon.imports"))
    for(const auto *node:metadata->operands()) {
      auto *site=llvm::mdconst::dyn_extract<llvm::ConstantInt>(node->getOperand(1));REQUIRE(site);
      auto *name=llvm::dyn_cast<llvm::MDString>(node->getOperand(3));REQUIRE(name);
      out.emplace(site->getZExtValue(),name->getString().str());
    }
  return out;
}
}
// Found on real Lua/zlib/sqlite builds: a block's fall-through JMP (after
// `lea`/`nop`, or after `bl __stack_chk_fail`) and jumps into blocks that do
// work before a tail call were labelled as transfers to the import.
TEST_CASE("Import provenance names only machine transfers into pure thunks", "[llvm][imports]") {
  constexpr std::uint64_t base=0x400100;
  // je 4; inc edx; 4: jmp [rip+GOT puts]. The `inc` block falls into the
  // jump; only the machine jump at +4 transfers to puts.
  CHECK(importSites({0x74,0x02,0xff,0xc2,0xff,0x25,0x0e,0x07,0,0},false)==
        std::multiset<std::pair<std::uint64_t,std::string>>{{base+4,"puts"}});
  // jmp 0x10; 0x10: mov rcx, rbx; jmp 0x20; 0x20: jmp [rip+GOT puts].
  // The first jump reaches puts with RCX changed: not a thunk. The jump at
  // 0x20 is a member block of this function and itself a real transfer.
  std::vector<std::uint8_t> work{0xeb,0x0e};work.resize(0x10,0xcc);
  for(auto b:{0x48,0x89,0xd9,0xe9,0x08,0,0,0})work.push_back(b);
  work.resize(0x20,0xcc);
  for(auto b:{0xff,0x25,0xf2,0x06,0,0})work.push_back(b);
  CHECK(importSites(work,false)==std::multiset<std::pair<std::uint64_t,std::string>>{{base+0x13,"puts"},{base+0x20,"puts"}});
  // A64: cbz x0, 8; bl stub; 8: mov w4, #-1; b stub; stub: adrp/ldr/add/br.
  // The call's fall-through into `mov w4; b stub` is not a second transfer.
  const std::vector<std::uint8_t> a64{0x40,0,0,0xb4, 0x03,0,0,0x94, 0x04,0,0x80,0x12, 0x01,0,0,0x14,
      0x10,0,0,0x90,0x11,0x0e,0x44,0xf9,0x10,0x62,0x20,0x91,0x20,0x02,0x1f,0xd6};
  CHECK(importSites(a64,true)==std::multiset<std::pair<std::uint64_t,std::string>>{{base+4,"puts"},{base+0xc,"puts"}});
}
