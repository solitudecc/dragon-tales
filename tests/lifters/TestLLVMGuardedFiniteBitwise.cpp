#include <catch2/catch_test_macros.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <dragon/passes/PassPipeline.hpp>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <cstdint>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <set>
#include <string>
#include <vector>
namespace {
struct BitwiseFixture {
 dragon::llvmlifter::LLVMLifter lifter{dragon::Architecture::X86_64};
 llvm::Function *function=nullptr;
 BitwiseFixture(const std::string &expression,const std::vector<std::uint64_t> &addresses) {
  auto &m=lifter.module();REQUIRE(lifter.setExecutionMode(dragon::llvmlifter::ExecutionMode::StaticAnalysis));
  std::string ir="target datalayout = \"e-p:64:64\"\ndefine i64 @bitwise_view(ptr %regs, ptr %memory, i64 %unknown, i1 %choice) {\nentry:\n %guest = load ptr, ptr %memory, align 1\n"+expression+"\n br label %dispatch\ndispatch:\n %tableptr = getelementptr i8, ptr %guest, i64 %index\n %actual = load i64, ptr %tableptr, align 1\n switch i64 %actual, label %miss [\n";
  for(unsigned n=0;n<8;++n)ir+=" i64 "+std::to_string(100+n)+", label %yes\n";
  ir+="]\nyes:\n ret i64 %actual\nmiss:\n ret i64 -1\n}\n";
  llvm::SMDiagnostic diagnostic;REQUIRE_FALSE(llvm::parseAssemblyInto(llvm::MemoryBufferRef(ir,"independent-finite-bitwise"),&m,nullptr,diagnostic));
  function=m.getFunction("bitwise_view");REQUIRE(function);
  std::string manifest="{\"view_symbol\":\"bitwise_view\",\"memory\":[";const char *hex="0123456789abcdef";
  for(std::size_t n=0;n<addresses.size();++n){if(n)manifest+=",";std::string address;llvm::raw_string_ostream stream(address);stream<<llvm::format_hex(addresses[n],0);std::string bytes;auto value=std::uint64_t(100+n);for(unsigned j=0;j<8;++j){unsigned b=(value>>(j*8))&255;bytes+=hex[b>>4];bytes+=hex[b&15];}manifest+="{\"offset\":\""+address+"\",\"bytes\":\""+bytes+"\"}";}
  manifest+="]}";m.getOrInsertNamedMetadata("dragon.guarded-entry")->addOperand(llvm::MDNode::get(m.getContext(),llvm::MDString::get(m.getContext(),manifest)));
  REQUIRE_FALSE(llvm::verifyModule(m,&llvm::errs()));
 }
 std::string text(){std::string s;llvm::raw_string_ostream out(s);lifter.module().print(out,nullptr);return s;}
 dragon::passes::Result run(){dragon::passes::PassPipeline p;REQUIRE(p.add("guarded-state-corridors"));auto report=p.run(lifter);REQUIRE(report.ok);REQUIRE(report.runs.size()==1);REQUIRE_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));return report.runs.front().result;}
 std::set<std::uint64_t> predicted(unsigned expectedGuards=1){std::set<std::uint64_t> result;unsigned guards=0;for(auto &b:*function){auto *s=llvm::dyn_cast<llvm::SwitchInst>(b.getTerminator());if(!s || !s->getMetadata("dragon.guarded-transitions.generated"))continue;auto *load=llvm::dyn_cast<llvm::LoadInst>(s->getCondition());if(!load || load->getParent()!=&b)continue;++guards;auto *fallback=llvm::dyn_cast<llvm::SwitchInst>(s->getDefaultDest()->getTerminator());REQUIRE(fallback);CHECK(fallback->getCondition()==s->getCondition());CHECK(fallback->getNumCases()==8);for(auto c:s->cases())result.insert(c.getCaseValue()->getZExtValue());}CHECK(guards==expectedGuards);return result;}
};
std::string indexed(const std::string &expression){return expression+"\n %scaled = shl i64 %state, 3\n %index = add i64 %scaled, 512\n";}
std::vector<std::uint64_t> addresses(std::initializer_list<std::uint64_t> states){std::vector<std::uint64_t> out;for(auto x:states)out.push_back(512+x*8);return out;}
std::set<std::uint64_t> values(unsigned count){std::set<std::uint64_t> out;for(unsigned n=0;n<count;++n)out.insert(100+n);return out;}
}
TEST_CASE("Guarded finite bitwise handles the actual masked i32 and disjoint prefix shapes","[llvm][guarded-finite-bitwise]") {
 BitwiseFixture masked(indexed(" %raw32 = trunc i64 %unknown to i32\n %masked = and i32 %raw32, 1\n %joined = or disjoint i32 %masked, 24\n %state = zext nneg i32 %joined to i64"),addresses({24,25}));
 auto *f=masked.function;auto *arg=f->getArg(2);auto *metadata=masked.lifter.module().getNamedMetadata("dragon.guarded-entry")->getOperand(0);
 REQUIRE(masked.run().changed);CHECK(masked.predicted()==values(2));CHECK(masked.function==f);CHECK(f->getArg(2)==arg);CHECK(masked.lifter.module().getNamedMetadata("dragon.guarded-entry")->getOperand(0)==metadata);CHECK(masked.text().find("or disjoint i32")!=std::string::npos);CHECK(masked.text().find("zext nneg i32")!=std::string::npos);
 for(auto base:{std::uint64_t(90120),std::uint64_t(73792)}) {
  BitwiseFixture finite(" %state = select i1 %choice, i64 0, i64 50\n %scaled = shl i64 %state, 3\n %index = or disjoint i64 %scaled, "+std::to_string(base),{base,base|400});
  REQUIRE(finite.run().changed);CHECK(finite.predicted()==values(2));CHECK(finite.text().find("or disjoint i64")!=std::string::npos);
  BitwiseFixture phi(" switch i64 %unknown, label %unknown_state [ i64 0, label %p0 i64 1, label %p50 ]\np0:\n br label %join\np50:\n br label %join\nunknown_state:\n br label %join\njoin:\n %state = phi i64 [ 0, %p0 ], [ 50, %p50 ], [ %unknown, %unknown_state ]\n %scaled = shl i64 %state, 3\n %index = or disjoint i64 %scaled, "+std::to_string(base),{base,base|400});
  REQUIRE(phi.run().changed);CHECK(phi.predicted(2)==values(2));

 }
}
TEST_CASE("Guarded finite bitwise combines known sets exactly before mask fallback","[llvm][guarded-finite-bitwise]") {
 for(const auto &op:{std::string("and"),std::string("or"),std::string("xor")}) {
  auto expected=op=="and"?addresses({0,2}):op=="or"?addresses({3,5,7}):addresses({1,3,5,7});
  BitwiseFixture f(indexed(" %a = select i1 %choice, i64 1, i64 3\n %b = select i1 %choice, i64 2, i64 4\n %state = "+op+" i64 %a, %b"),expected);
  REQUIRE(f.run().changed);CHECK(f.predicted()==values(expected.size()));
 }
 BitwiseFixture precise(indexed(" %known = select i1 %choice, i64 34, i64 34\n %state = and i64 %known, 1"),addresses({0}));REQUIRE(precise.run().changed);CHECK(precise.predicted()==values(1));
 BitwiseFixture combined(indexed(" %masked = and i64 %unknown, 3\n %flipped = xor i64 %masked, 1\n %state = or disjoint i64 %flipped, 24"),addresses({24,25,26,27}));REQUIRE(combined.run().changed);CHECK(combined.predicted()==values(4));
}
TEST_CASE("Guarded finite masks retain zero sparse high bits and the eight value limit","[llvm][guarded-finite-bitwise]") {
 BitwiseFixture zero(indexed(" %state = and i64 %unknown, 0"),addresses({0}));REQUIRE(zero.run().changed);CHECK(zero.predicted()==values(1));
 BitwiseFixture eight(indexed(" %state = and i64 %unknown, 7"),addresses({0,1,2,3,4,5,6,7}));REQUIRE(eight.run().changed);CHECK(eight.predicted()==values(8));
 BitwiseFixture high(indexed(" %state = and i64 %unknown, -9223372036854775807"),addresses({0,1}));REQUIRE(high.run().changed);CHECK(high.predicted()==values(2));
 BitwiseFixture wide(indexed(" %wide = zext i64 %unknown to i128\n %mask = and i128 %wide, 79228162514264337593543950337\n %state = trunc i128 %mask to i64"),addresses({0,1}));REQUIRE(wide.run().changed);CHECK(wide.predicted()==values(2));
}
TEST_CASE("Guarded finite masks refuse unknown dense masks and visible poison","[llvm][guarded-finite-bitwise]") {
 for(const auto &expression:{std::string(" %state = and i64 %unknown, 15"),std::string(" %state = and i64 %unknown, -1"),std::string(" %dynamic = zext i1 %choice to i64\n %state = and i64 %unknown, %dynamic"),std::string(" %state = and i64 poison, 0"),std::string(" %state = and i64 undef, 0")}) {
  BitwiseFixture f(indexed(expression),addresses({0,1,2,3,4,5,6,7}));auto before=f.text();CHECK_FALSE(f.run().changed);CHECK(f.text()==before);
 }
}
