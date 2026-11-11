#include <dragon/lifters/llvm/GuestMemorySSA.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {
bool probe(std::uint64_t left, std::uint64_t right) {
  llvm::LLVMContext context;
  llvm::Module module("memory-distance-ubsan", context);
  module.setDataLayout("e-p:64:64");
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context),
                                      {ptr, ptr}, false);
  auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                        "distance_case", module);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", function));
  auto *data = b.CreateLoad(ptr, function->getArg(1));
  auto *a = b.CreateGEP(b.getInt8Ty(), data, b.getInt64(left));
  auto *z = b.CreateGEP(b.getInt8Ty(), data, b.getInt64(right));
  constexpr std::uint64_t first = UINT64_C(0x1122334455667788);
  constexpr std::uint64_t second = UINT64_C(0x8877665544332211);
  b.CreateStore(b.getInt64(first), a)->setAlignment(llvm::Align(1));
  b.CreateStore(b.getInt64(second), z)->setAlignment(llvm::Align(1));
  auto *load = b.CreateLoad(b.getInt64Ty(), a);
  load->setAlignment(llvm::Align(1));
  auto *ret = b.CreateRet(load);
  if (llvm::verifyModule(module, &llvm::errs())) return false;
  dragon::llvmlifter::MemoryForwardOptions options{65536, false};
  options.enableCrossBlock = false;
  options.enablePhi = false;
  const auto stats = dragon::llvmlifter::detail::runGuestMemorySSA(module, options);
  if (!stats.available || !stats.complete ||
      llvm::verifyModule(module, &llvm::errs())) return false;
  // A finite byte-address model supplies the expected modular overlap.
  std::uint64_t expected = 0;
  for (unsigned i = 0; i != 8; ++i) {
    auto byte = (first >> (8 * i)) & 255;
    for (unsigned j = 0; j != 8; ++j)
      if (left + i == right + j) byte = (second >> (8 * j)) & 255;
    expected |= byte << (8 * i);
  }
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue()))
    return constant->getZExtValue() == expected;
  // Wrapped byte reconstruction is permitted to remain conservative. It is
  // never counted as an executed semantic match or dereferenced as a host VA.
  return llvm::isa<llvm::LoadInst>(ret->getReturnValue());
}
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string mode(argv[1]);
  if (mode == "distance") {
    if (!probe(0, UINT64_C(1) << 63)) return 3;
    std::cout << "production GuestMemorySSA half-range analysis passed\n";
    return 0;
  }
  if (mode != "sweep") return 2;
  const std::array values{UINT64_C(0), UINT64_C(1), UINT64_C(7), UINT64_C(8),
      UINT64_C(9), (UINT64_C(1) << 63) - 1, UINT64_C(1) << 63,
      (UINT64_C(1) << 63) + 1, std::numeric_limits<std::uint64_t>::max() - 7,
      std::numeric_limits<std::uint64_t>::max()};
  unsigned count = 0;
  for (const auto left : values)
    for (const auto right : values) {
      if (!probe(left, right)) return 4;
      ++count;
    }
  std::cout << "production GuestMemorySSA modular range probes=" << count << '\n';
  return count == 100 ? 0 : 5;
}
