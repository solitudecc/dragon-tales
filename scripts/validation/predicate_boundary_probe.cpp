// A producer for independent native predicate-boundary validation. This calls
// the public production pass; the Python oracle does not use this IR builder.
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>
#include <string>
using namespace dragon;
using namespace dragon::llvmlifter;
static bool save(const std::string &path, const std::string &value) {
  std::error_code error;
  llvm::raw_fd_ostream out(path, error);
  if (error) return false;
  out << value; out.close(); return !out.has_error();
}
int main(int argc, char **argv) {
  if (argc != 7) return 2;
  const unsigned width = std::stoul(argv[1]);
  const std::string direction = argv[2], mode = argv[3];
  if ((width != 32 && width != 64 && width != 128) ||
      (direction != "left" && direction != "right") ||
      (mode != "positive" && mode != "varying" && mode != "distinct")) return 2;
  for (unsigned i = 4; i != 7; ++i)
    if (llvm::sys::fs::exists(argv[i])) return 2;
  LLVMLifter lifter{Architecture::ARM64, "predicate-boundary"};
  auto &context = lifter.module().getContext();
  auto *word = llvm::IntegerType::get(context, width);
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "probe", lifter.module());
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *head = llvm::BasicBlock::Create(context, "history", function);
  auto *done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> b{entry}; auto *frame = function->getArg(0);
  auto address = [&](unsigned offset) {
    return b.CreateGEP(b.getInt8Ty(), frame, b.getInt64(offset));
  };
  auto load = [&](unsigned offset) {
    auto *value = b.CreateLoad(word, address(offset)); value->setAlignment(llvm::Align(1));
    return value;
  };
  auto store = [&](llvm::Value *value, unsigned offset) {
    auto *result = b.CreateStore(value, address(offset)); result->setAlignment(llvm::Align(1));
  };
  auto observer = lifter.module().getOrInsertFunction("observer",
      llvm::FunctionType::get(b.getVoidTy(), {ptr, b.getInt64Ty()}, false));
  auto *x = load(0), *count = load(16), *other = load(32);
  b.CreateCall(observer, {frame, b.CreateZExtOrTrunc(x, b.getInt64Ty())}); b.CreateBr(head);
  b.SetInsertPoint(head);
  auto *history = b.CreatePHI(word, 2, "carried_input"); history->addIncoming(x, entry);
  auto *iteration = b.CreatePHI(b.getInt32Ty(), 2, "iteration"); iteration->addIncoming(b.getInt32(0), entry);
  auto *accumulator = b.CreatePHI(word, 2, "accumulator");
  accumulator->addIncoming(llvm::ConstantInt::get(word, 0), entry);
  auto *intrinsic = llvm::Intrinsic::getOrInsertDeclaration(&lifter.module(),
      direction == "left" ? llvm::Intrinsic::fshl : llvm::Intrinsic::fshr, {word});
  auto *observed = b.CreateCall(intrinsic, {history, history, count}, "rotation");
  auto *mixed = b.CreateXor(observed, history);
  llvm::Value *left = mode == "varying" ? mixed
      : b.CreateOr(mixed, llvm::ConstantInt::get(word, 1));
  llvm::Value *right = left;
  if (mode == "distinct") {
    auto *second = b.CreateCall(intrinsic, {other, other, count}, "second_rotation");
    right = b.CreateOr(b.CreateXor(second, other), llvm::ConstantInt::get(word, 1));
  }
  auto *low = b.CreateAnd(b.CreateMul(left, right), llvm::ConstantInt::get(word, 7));
  auto *minusOne = b.CreateNSWAdd(low, llvm::ConstantInt::getAllOnesValue(word));
  auto *condition = b.CreateICmpEQ(minusOne, llvm::ConstantInt::get(word, 0), "predicate");
  auto *selected = b.CreateSelect(condition, llvm::ConstantInt::get(word, 7),
                                  llvm::ConstantInt::get(word, 98));
  auto *nextAccumulator = b.CreateAdd(accumulator, selected);
  auto *next = b.CreateAdd(history, llvm::ConstantInt::get(word, 1));
  auto *nextIteration = b.CreateAdd(iteration, b.getInt32(1));
  history->addIncoming(next, head); iteration->addIncoming(nextIteration, head);
  accumulator->addIncoming(nextAccumulator, head);
  b.CreateCondBr(b.CreateICmpULT(nextIteration, b.getInt32(3)), head, done);
  b.SetInsertPoint(done);
  store(nextAccumulator, 64); store(next, 96); store(mixed, 112);
  store(b.CreateZExt(condition, b.getInt8Ty()), 192); store(nextIteration, 200);
  b.CreateCall(observer, {frame, b.CreateZExtOrTrunc(next, b.getInt64Ty())});
  store(b.getInt64(0xdecafbad12345678ULL), 66048); b.CreateRetVoid();
  if (llvm::verifyModule(lifter.module(), &llvm::errs())) return 3;
  const auto before = lifter.ir(); const auto stats = lifter.runPredicateFolding();
  if (llvm::verifyModule(lifter.module(), &llvm::errs())) return 3;
  llvm::json::Object report{{"width", width}, {"direction", direction}, {"mode", mode},
    {"available", stats.available}, {"visited", stats.conditionsVisited},
    {"folded", stats.conditionsFolded}, {"queries", stats.solverQueries},
    {"unknown", stats.unknownQueries}, {"unsupported", stats.unsupportedConditions},
    {"budget_skipped", stats.budgetSkipped}};
  std::string text; llvm::raw_string_ostream stream(text);
  stream << llvm::formatv("{0:2}", llvm::json::Value(std::move(report))) << '\n';
  return save(argv[4], before) && save(argv[5], lifter.ir()) && save(argv[6], text) ? 0 : 4;
}
