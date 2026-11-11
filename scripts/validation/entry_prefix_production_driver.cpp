// Production API oracle adapter. Link only the frozen libdragon backend;
// do not add LLVM shared libraries (duplicate LLVM option registration).
// Usage: driver input.ll output.ll selected-function [max-blocks max-instructions]
// Input text is parsed into the LLVMLifter's existing Module/LLVMContext.
#include <dragon/lifters/llvm/LLVMLifter.hpp>
#include <llvm/ADT/SmallVector.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace dragon;
using namespace dragon::llvmlifter;

namespace {
std::string jsonString(const std::string &value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') { result += '\\'; result += char(c); }
    else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
    else result += char(c);
  }
  return result + '"';
}
struct Report {
  EntryPrefixStats stats;
  bool moduleIdentity = false, globalIdentity = false, argumentIdentity = false;
  bool ordinaryUnchanged = false, metadataUnchanged = false, verified = false;
  bool rejectedUnchanged = false;
  int finish(int code, const std::string &stage, const std::string &error = {}) {
    auto &out = llvm::errs();
    out << "{\"driver_exit\":" << code << ",\"stage\":" << jsonString(stage)
        << ",\"error\":" << jsonString(error) << ",\"available\":" << (stats.available ? "true" : "false")
        << ",\"complete\":" << (stats.complete ? "true" : "false")
        << ",\"changed\":" << (stats.changed ? "true" : "false")
        << ",\"message\":" << jsonString(stats.message);
#define COUNTER(field) out << ",\"" #field "\":" << stats.field
    COUNTER(candidateEdges); COUNTER(peeledEdges); COUNTER(clonedBlocks);
    COUNTER(clonedInstructions); COUNTER(originalExits); COUNTER(rejected);
    COUNTER(budgetExhausted); COUNTER(skippedSingleEntry); COUNTER(internalEdges);
    COUNTER(backedges); COUNTER(terminalEdges);
#undef COUNTER
#define CHECKED(field) out << ",\"" #field "\":" << (field ? "true" : "false")
    CHECKED(moduleIdentity); CHECKED(globalIdentity); CHECKED(argumentIdentity);
    CHECKED(ordinaryUnchanged); CHECKED(metadataUnchanged); CHECKED(verified);
    CHECKED(rejectedUnchanged);
#undef CHECKED
    out << "}\n";
    return code;
  }
};
std::string printed(const llvm::Value &value) {
  std::string text; llvm::raw_string_ostream out(text); value.print(out); return text;
}
std::string metadata(const llvm::Module &module) {
  std::string text; llvm::raw_string_ostream out(text);
  for (const auto &node : module.named_metadata()) node.print(out);
  return text;
}
std::string functionMetadata(const llvm::Function &function) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 8> nodes;
  function.getAllMetadata(nodes);
  std::string text; llvm::raw_string_ostream out(text);
  for (const auto &[kind, node] : nodes) {
    out << kind << ':';
    node->print(out); // No Module slot renumbering: bind the actual metadata graph.
    out << '\n';
  }
  return text;
}
bool number(const char *text, std::uint32_t &result) {
  if (!text || !*text) return false;
  std::uint64_t value = 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + (*p - '0');
    if (value > std::numeric_limits<std::uint32_t>::max()) return false;
  }
  result = static_cast<std::uint32_t>(value); return true;
}
struct Identity {
  llvm::GlobalValue *global;
  std::string name, text;
  std::vector<llvm::Argument *> arguments;
};
}

int main(int argc, char **argv) {
  Report report;
  if (argc != 4 && argc != 6)
    return report.finish(2, "arguments", "expected input output function [max-blocks max-instructions]");
  EntryPrefixOptions options;
  if (argc == 6 && (!number(argv[4], options.maxBlocks) || !number(argv[5], options.maxInstructions)))
    return report.finish(2, "arguments", "budgets must be decimal uint32 values");
  if (llvm::sys::fs::exists(argv[2]))
    return report.finish(2, "output", "refusing existing output path");
  auto input = llvm::MemoryBuffer::getFile(argv[1]);
  if (!input) return report.finish(2, "input", input.getError().message());
  LLVMLifter lifter(Architecture::X86_64, argv[1]);
  if (!lifter.setExecutionMode(ExecutionMode::StaticAnalysis))
    return report.finish(2, "mode", "failed to set StaticAnalysis before parsing");
  llvm::Module *originalModule = &lifter.module();
  llvm::SMDiagnostic diagnostic;
  if (llvm::parseAssemblyInto((*input)->getMemBufferRef(), originalModule, nullptr, diagnostic)) {
    std::string error; llvm::raw_string_ostream out(error); diagnostic.print(argv[0], out);
    return report.finish(2, "parse", error);
  }
  std::string error;
  { llvm::raw_string_ostream out(error);
    if (llvm::verifyModule(*originalModule, &out)) return report.finish(2, "input-verify", error); }
  auto *selected = originalModule->getFunction(argv[3]);
  if (!selected || selected->isDeclaration()) return report.finish(2, "selection", "missing defined function");
  std::size_t candidates = 0;
  for (auto &function : *originalModule)
    for (auto &block : function)
      if (block.getName() == "guarded.accept") {
        if (&function != selected) return report.finish(2, "selection", "another function has guarded.accept");
        ++candidates;
      }
  if (candidates != 1) return report.finish(2, "selection", "exactly one selected guarded.accept required");
  std::vector<Identity> identities;
  for (auto &global : originalModule->global_values()) {
    Identity item{&global, global.getName().str(), printed(global), {}};
    if (auto *function = llvm::dyn_cast<llvm::Function>(&global))
      for (auto &argument : function->args()) item.arguments.push_back(&argument);
    identities.push_back(std::move(item));
  }
  const auto namedMetadata = metadata(*originalModule);
  const auto selectedMetadata = functionMetadata(*selected);
  const auto sourceAttributes = selected->getAttributes();
  const auto sourceLinkage = selected->getLinkage();
  const auto sourceVisibility = selected->getVisibility();
  const auto sourceComdat = selected->getComdat();
  const auto sourceCallingConvention = selected->getCallingConv();
  const auto sourceSection = selected->getSection().str();
  const auto before = lifter.ir();
  report.stats = lifter.runEntryPrefix(options);
  report.moduleIdentity = &lifter.module() == originalModule;
  if (!report.moduleIdentity) return report.finish(4, "identity", "Module object replaced");
  std::vector<llvm::GlobalValue *> current;
  for (auto &global : originalModule->global_values()) current.push_back(&global);
  if (current.size() != identities.size()) return report.finish(4, "identity", "global/function count changed");
  for (std::size_t i = 0; i < identities.size(); ++i) {
    const auto &identity = identities[i];
    if (current[i] != identity.global || current[i]->getName() != identity.name)
      return report.finish(4, "identity", "global/function identity or order changed");
    if (auto *function = llvm::dyn_cast<llvm::Function>(current[i])) {
      if (function->arg_size() != identity.arguments.size())
        return report.finish(4, "identity", "argument count changed");
      for (unsigned j = 0; j < function->arg_size(); ++j)
        if (function->getArg(j) != identity.arguments[j])
          return report.finish(4, "identity", "Argument object replaced");
    }
    if (current[i] != selected && printed(*current[i]) != identity.text)
      return report.finish(4, "ordinary", "non-selected function/global changed: " + identity.name);
  }
  report.globalIdentity = report.argumentIdentity = report.ordinaryUnchanged = true;
  if (selected->getAttributes() != sourceAttributes || selected->getLinkage() != sourceLinkage ||
      selected->getVisibility() != sourceVisibility || selected->getComdat() != sourceComdat ||
      selected->getCallingConv() != sourceCallingConvention || selected->getSection() != sourceSection)
    return report.finish(4, "identity", "selected function attributes changed");
  if (metadata(*originalModule) != namedMetadata || functionMetadata(*selected) != selectedMetadata)
    return report.finish(4, "metadata", "named or selected function metadata changed");
  report.metadataUnchanged = true;
  error.clear();
  { llvm::raw_string_ostream out(error);
    if (llvm::verifyModule(*originalModule, &out)) return report.finish(4, "output-verify", error); }
  report.verified = true;
  if (report.stats.candidateEdges != 1)
    return report.finish(4, "stats", "production candidate count differs from preflight");
  if (!report.stats.changed) {
    report.rejectedUnchanged = lifter.ir() == before;
    if (!report.rejectedUnchanged) return report.finish(4, "rejection", "IR changed despite changed=false");
    return report.finish(3, "no-rewrite", "production API produced no prefix copy");
  }
  if (!report.stats.available || report.stats.peeledEdges != 1 || report.stats.rejected != 0)
    return report.finish(4, "stats", "unexpected partial/multiple candidate result");
  // A safe truncated copy may be valid but incomplete. Preserve complete=false
  // in JSON; never upgrade it to complete because native execution succeeds.
  std::error_code ec;
  llvm::raw_fd_ostream output(argv[2], ec, llvm::sys::fs::OF_Text);
  if (ec) return report.finish(2, "output", ec.message());
  originalModule->print(output, nullptr);
  output.close();
  if (output.has_error()) {
    llvm::sys::fs::remove(argv[2]);
    return report.finish(2, "output", "write failed; partial output removed");
  }
  return report.finish(0, "written");
}
