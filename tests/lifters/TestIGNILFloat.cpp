#include <catch2/catch_test_macros.hpp>
#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/analysis/SliceExprZ3.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <vector>

TEST_CASE("IGNIL binary32 operations use their own precision in all backends", "[float32][ignil]") {
  using namespace dragon;
  using O = ignil::Opcode;
  struct Case { O op; std::vector<ignil::Value> inputs; ignil::Width width; uint64_t expected; };
  const Case cases[] = {
    {O::FADD32,{ignil::Const{0x3f800000,32},ignil::Const{0x33800000,32}},32,0x3f800000},
    {O::FADD32,{ignil::Const{0x3f800001,32},ignil::Const{0x33800000,32}},32,0x3f800002},
    {O::FSUB32,{ignil::Const{0x3f800000,32},ignil::Const{0x3f800000,32}},32,0},
    {O::FMUL32,{ignil::Const{1,32},ignil::Const{0x3f000000,32}},32,0},
    {O::FDIV32,{ignil::Const{0x3f800000,32},ignil::Const{0x40400000,32}},32,0x3eaaaaab},
    {O::SITOFP32,{ignil::Const{0x80000000,32}},32,0xcf000000},
    {O::UITOFP32,{ignil::Const{0xffffffff,32}},32,0x4f800000},
    {O::FPTOSI32,{ignil::Const{0xbfcccccd,32}},32,0xffffffff},
    {O::FPTOUI32,{ignil::Const{0x40b00000,32}},32,5},
    {O::FPEXT,{ignil::Const{0x3f000001,32}},64,0x3fe0000020000000ull},
    {O::FPTRUNC,{ignil::Const{0x3ff0000010000000ull,64}},32,0x3f800000},
    {O::FPTRUNC,{ignil::Const{0x3ff0000010000001ull,64}},32,0x3f800001},
    {O::FLT32,{ignil::Const{0xbf800000,32},ignil::Const{0,32}},1,1},
    {O::FEQ32,{ignil::Const{0,32},ignil::Const{0x80000000,32}},1,1},
    {O::FUNO32,{ignil::Const{0x7fc00001,32},ignil::Const{0,32}},1,1},
    {O::FEQ32,{ignil::Const{0x7fc00001,32},ignil::Const{0x7fc00001,32}},1,0},
    {O::FSQRT32,{ignil::Const{0x40000000,32}},32,0x3fb504f3},
    {O::FFLOOR32,{ignil::Const{0xbe800000,32}},32,0xbf800000},
    {O::FCEIL32,{ignil::Const{0xbe800000,32}},32,0x80000000},
    {O::FTRUNC32,{ignil::Const{0xbe800000,32}},32,0x80000000},
  };
  for (const auto &test : cases) {
    INFO(ignil::opcodeName(test.op));
    const ignil::Reg result{0,test.width,"result"};
    ignil::Block block{0x1000};
    block.append({test.op,result,test.inputs,0x1000});
    ignil::SymbolicExecutor executor;
    REQUIRE(executor.run(block).ok());
    CHECK(executor.evaluate(executor.readRegister(result)) == test.expected);
    Graph graph{Architecture::X86_64,Configuration{}};
    graph.addBlock(0x1000);
    graph.addIGNILBlock(block);
    auto expression = BackwardSlicer{}.slice(result,0x1000,graph);
    z3::context context;
    slice::SliceExprZ3 lowerer{context};
    auto lowered = lowerer.lower(expression).simplify();
    REQUIRE(lowered.is_numeral());
    CHECK(lowered.get_numeral_uint64() == test.expected);
    llvmlifter::LLVMLifter lifter{Architecture::X86_64};
    REQUIRE(lifter.liftBlock(block));
    CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
    lifter.runMem2Reg();
    lifter.runInstCombine();
    unsigned stores = 0;
    for (const auto &function : lifter.module())
      for (const auto &bb : function)
        for (const auto &instruction : bb)
          if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            const auto *bits = llvm::dyn_cast<llvm::ConstantInt>(store->getValueOperand());
            REQUIRE(bits != nullptr);
            CHECK(bits->getZExtValue() == test.expected);
            ++stores;
          }
    CHECK(stores == 1);
  }
}

TEST_CASE("LLVM floating results can feed integer widening of raw bits", "[float32][llvm]") {
  using namespace dragon;
  ignil::Block block{0x1000};
  const ignil::Temp result{0,32};
  block.append({ignil::Opcode::FADD32,result,{ignil::Const{0x3f800000,32},ignil::Const{0x3f800000,32}},0x1000});
  block.append({ignil::Opcode::ZEXT,ignil::Reg{272,128,"V0"},{result},0x1000});
  llvmlifter::LLVMLifter lifter{Architecture::ARM64};
  REQUIRE(lifter.liftBlock(block));
  CHECK_FALSE(llvm::verifyModule(lifter.module(),&llvm::errs()));
}
