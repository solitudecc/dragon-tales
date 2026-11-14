// Synthetic private-helper producer. Not a public builtin or machine lifter.
#include "../../lib/LLVMGuestCellSSA.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <string>

using namespace dragon::llvmlifter::detail;
static std::string text(llvm::Module &module) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  module.print(stream, nullptr);
  return result;
}
static bool save(const char *path, const std::string &value) {
  std::error_code error;
  llvm::raw_fd_ostream out(path, error);
  if (error) return false;
  out << value;
  out.close();
  return !out.has_error();
}
int main(int argc, char **argv) {
  if (argc != 7) return 2;
  const unsigned width = std::stoul(argv[1]);
  const std::string mode = argv[2];
  if ((width != 32 && width != 128) ||
      (mode != "closed" && mode != "call-before" && mode != "call-after"))
    return 2;
  for (unsigned i = 3; i < 7; ++i)
    if (llvm::sys::fs::exists(argv[i])) return 2;
  llvm::LLVMContext context;
  llvm::Module module("guest-cell-ssa-native", context);
  module.setDataLayout("e-p:64:64");
  auto *word = llvm::IntegerType::get(context, width);
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "probe", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *head = llvm::BasicBlock::Create(context, "head", function);
  auto *a = llvm::BasicBlock::Create(context, "body.a", function);
  auto *b = llvm::BasicBlock::Create(context, "body.b", function);
  auto *done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> builder(entry);
  auto *frame = function->getArg(0);
  auto address = [&](unsigned offset) {
    return builder.CreateGEP(builder.getInt8Ty(), frame, builder.getInt64(offset));
  };
  auto read = [&](llvm::Type *type, unsigned offset, const char *name = "") {
    auto *load = builder.CreateLoad(type, address(offset), name);
    load->setAlignment(llvm::Align(1));
    return load;
  };
  auto write = [&](llvm::Value *value, unsigned offset) {
    auto *store = builder.CreateStore(value, address(offset));
    store->setAlignment(llvm::Align(1));
  };
  auto bump = [&](unsigned offset) {
    write(builder.CreateAdd(read(builder.getInt64Ty(), offset), builder.getInt64(1)), offset);
  };
  auto opaque = module.getOrInsertFunction("opaque_cell",
      llvm::FunctionType::get(builder.getVoidTy(),
          {pointer, builder.getInt32Ty(), builder.getInt32Ty()}, false));
  auto *initial = read(word, 0, "initial");
  auto *routeSeed = read(builder.getInt32Ty(), 32);
  auto *trips = builder.CreateAdd(
      builder.CreateAnd(read(builder.getInt32Ty(), 40), builder.getInt32(3)),
      builder.getInt32(3));
  write(initial, 2048);
  builder.CreateBr(head);
  builder.SetInsertPoint(head);
  auto *historical = builder.CreatePHI(word, 3, "historical");
  auto *iteration = builder.CreatePHI(builder.getInt32Ty(), 3, "iteration");
  auto *checksum = builder.CreatePHI(word, 3, "checksum");
  historical->addIncoming(initial, entry);
  iteration->addIncoming(builder.getInt32(0), entry);
  checksum->addIncoming(llvm::ConstantInt::get(word, 0), entry);
  auto *selected = read(word, 2048, "selected.original.cell");
  auto *rotated = builder.CreateOr(
      builder.CreateShl(checksum, llvm::ConstantInt::get(word, 5)),
      builder.CreateLShr(checksum, llvm::ConstantInt::get(word, width - 5)));
  auto *nextChecksum = builder.CreateAdd(builder.CreateXor(rotated, selected), historical);
  auto *nextIteration = builder.CreateAdd(iteration, builder.getInt32(1));
  auto *route = builder.CreateICmpNE(builder.CreateAnd(
      builder.CreateAdd(routeSeed, iteration), builder.getInt32(1)), builder.getInt32(0));
  write(selected, 96);
  write(historical, 112);
  bump(256);
  builder.CreateCondBr(route, a, b);
  for (auto *body : {a, b}) {
    builder.SetInsertPoint(body);
    const bool first = body == a;
    // Two distinct actual SSA expressions; no arithmetic is done by the helper.
    auto *next = first ? builder.CreateAdd(selected, llvm::ConstantInt::get(word, 1), "next.a")
                       : builder.CreateSub(selected, llvm::ConstantInt::getAllOnesValue(word), "next.b");
    if (mode == "call-before")
      builder.CreateCall(opaque, {frame, builder.getInt32(width), builder.getInt32(first ? 1 : 2)});
    write(next, 2048);
    if (mode == "call-after")
      builder.CreateCall(opaque, {frame, builder.getInt32(width), builder.getInt32(first ? 1 : 2)});
    bump(first ? 272 : 280);
    historical->addIncoming(next, body);
    iteration->addIncoming(nextIteration, body);
    checksum->addIncoming(nextChecksum, body);
    builder.CreateCondBr(builder.CreateICmpULT(nextIteration, trips), head, done);
  }
  builder.SetInsertPoint(done);
  write(read(word, 2048), 64);
  write(nextChecksum, 80);
  write(nextIteration, 200);
  write(builder.getInt64(0xdecafbad12345678ULL), 66048);
  builder.CreateRetVoid();
  if (llvm::verifyModule(module, &llvm::errs())) return 3;
  const auto before = text(module);
  const auto stats = forwardGuestCellLoad(*function, *frame, *selected);
  if (llvm::verifyModule(module, &llvm::errs())) return 3;
  const auto after = text(module);
  const bool expectedChange = mode != "call-after";
  if (stats.changed != expectedChange || stats.refused == expectedChange ||
      stats.budgetExhausted || (!expectedChange && before != after)) return 4;
  if (!save(argv[3], before) || !save(argv[4], after)) return 5;
  bool mutant = false;
  if (mode == "closed") {
    llvm::PHINode *cellPhi = nullptr;
    for (auto &instruction : *head)
      if (auto *phi = llvm::dyn_cast<llvm::PHINode>(&instruction);
          phi && phi->getName().starts_with("guest.cell.ssa")) cellPhi = phi;
    if (!cellPhi || cellPhi->getBasicBlockIndex(a) < 0) return 6;
    cellPhi->setIncomingValue(cellPhi->getBasicBlockIndex(a), initial);
    if (llvm::verifyModule(module, &llvm::errs())) return 3;
    if (!save(argv[5], text(module))) return 5;
    mutant = true;
  }
  llvm::json::Object report{{"width", width}, {"mode", mode},
    {"changed", stats.changed}, {"refused", stats.refused},
    {"budget_exhausted", stats.budgetExhausted}, {"blocks", stats.blocks},
    {"nodes", stats.nodes}, {"phis", stats.phis}, {"work", stats.work},
    {"message", stats.message}, {"refusal_byte_exact", before == after},
    {"bad_phi_mutant_emitted", mutant}, {"loop_blocks", 3},
    {"scope", "Private selected-LOAD helper; no public builtin or machine lifter"}};
  llvm::json::Array maps;
  std::ifstream mapped("/proc/self/maps");
  for (std::string line; std::getline(mapped, line);)
    if (line.find("libdragon.so") != std::string::npos) maps.push_back(line);
  report["runtime_maps"] = std::move(maps);
  return save(argv[6], llvm::formatv("{0:2}\n", llvm::json::Value(std::move(report))).str()) ? 0 : 5;
}
