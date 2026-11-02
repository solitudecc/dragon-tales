#include <dragon/loader/ImageDisassembler.hpp>

#include <dragon/disassembler/Disassembler.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

namespace dragon::loader {

Graph disassembleImage(const LoadedImage &image, const Configuration &config) {
  // Windows x64 code passes arguments in RCX, RDX, R8, R9, not System V's
  // RDI, RSI, ... - without this, argument recovery read PE functions with
  // the wrong registers.
  Configuration cfg = config;
  if (image.format() == Format::PE &&
      image.architecture() == Architecture::X86_64 &&
      cfg.callingConvention == CallingConvention::Default)
    cfg.callingConvention = CallingConvention::Win64;
  Graph graph(image.architecture(), cfg);
  if (image.architecture() == Architecture::Unknown)
    return graph;

  // Known code addresses: the entry point and every function symbol.
  std::vector<std::uint64_t> seeds;
  seeds.insert(seeds.end(), image.functionEntries().begin(), image.functionEntries().end());
  if (auto entry = image.entryPoint())
    seeds.push_back(*entry);
  for (const Symbol *sym : image.definedSymbols())
    if (sym->kind == SymbolKind::Function)
      seeds.push_back(sym->address);

  // With no executable section, executableSections() hands back segments. A
  // segment can map the ELF and program headers in front of the code, and a
  // linear sweep through them falls out of step before reaching it, so start
  // each such sweep at the first known code address it contains.
  const bool fromSegments =
      std::none_of(image.sections().begin(), image.sections().end(),
                   [](const Section &s) {
                     return s.executable && !s.bytes.empty();
                   });

  Disassembler dis(image.architecture(), cfg);
  auto readMemory = [&image](std::uint64_t addr, std::span<std::uint8_t> out) {
    auto data = image.readVirtual(addr, out.size());
    if (!data)
      return false;
    std::copy(data->begin(), data->end(), out.begin());
    return true;
  };

  // Analyse [start, end of s) with `entries` as its entry points.
  auto analyse = [&](const Section &s, std::uint64_t start,
                     std::vector<std::uint64_t> entries) {
    Disassembler::AnalysisHints hints;
    hints.entryPoints = std::move(entries);
    hints.readMemory = readMemory;
    const std::span<const std::uint8_t> bytes(s.bytes);
    dis.disassemble(bytes.subspan(start - s.address), start, graph, hints);
  };

  const auto sections = image.executableSections();
  for (const Section *s : sections) {
    std::uint64_t start = s->address;
    if (fromSegments) {
      std::uint64_t first = UINT64_MAX;
      for (std::uint64_t a : seeds)
        if (a >= s->address && a < s->address + s->bytes.size())
          first = std::min(first, a);
      if (first != UINT64_MAX)
        start = first;
    }
    // The known code addresses go into the analysis itself: as function and
    // block starts, and as the roots of a recursive traversal. They used to
    // be added with Graph::addFunction() afterwards, which made no block (so
    // functions reached only through pointers had none) and gave a
    // recursive traversal nothing but the section start to begin from.
    // readMemory lets jump-table resolution read a table in another section.
    std::vector<std::uint64_t> entries;
    for (std::uint64_t a : seeds)
      if (a >= start && a < s->address + s->bytes.size())
        entries.push_back(a);
    if (entries.empty())
      entries.push_back(start);
    analyse(*s, start, std::move(entries));
  }

  // Each section is analysed on its own, so a call or a jump into another
  // section (`bl` from .text to a .plt stub, or the tail call `b stub`)
  // registers a function or block start there that nothing decoded - under
  // recursive traversal, the stub stays empty. Feed such addresses back as
  // entry points of their section (a cross-section jump is a tail call)
  // until none is left.
  for (int round = 0; round < 8; ++round) {
    bool any = false;
    for (const Section *s : sections) {
      std::vector<std::uint64_t> missing;
      auto collect = [&](const std::set<std::uint64_t> &addrs) {
        for (std::uint64_t a : addrs)
          if (s->contains(a) && a - s->address < s->bytes.size() &&
              !graph.get(a))
            missing.push_back(a);
      };
      collect(graph.validFunctionAddresses());
      collect(graph.validBlockAddresses());
      std::sort(missing.begin(), missing.end());
      missing.erase(std::unique(missing.begin(), missing.end()),
                    missing.end());
      if (missing.empty())
        continue;
      any = true;
      analyse(*s, s->address, std::move(missing));
    }
    if (!any)
      break;
  }

  return graph;
}

} // namespace dragon::loader
