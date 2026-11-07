#pragma once

#include <dragon/Configuration.hpp>
#include <dragon/disassembler/Graph.hpp>
#include <dragon/disassembler/Disassembler.hpp>
#include <dragon/loader/Loader.hpp>

/// The payoff of the loader: go straight from a file on disk to a populated
/// Graph, with no manual architecture or base-address bookkeeping.
namespace dragon::loader {

/// Disassemble every executable section of `image` (its executable segments
/// when the section headers are gone) into a fresh Graph built for
/// the architecture the image declares. Each section is disassembled at its own
/// virtual address, so cross-section calls and jumps resolve. The entry point
/// and every function-typed symbol are registered as function starts.
///
/// Returns an empty Graph (architecture Unknown) if the image's architecture is
/// one dragon does not model - callers should check image.architecture() first
/// when that matters.
[[nodiscard]] Graph disassembleImage(const LoadedImage &image,
                                     const Configuration &config = {});

/// Additional entries and explicit current-content assertions. Metadata seeds
/// remain independent entries. Hints are synchronous and never retained by the
/// returned Graph; section R/W/X flags do not establish immutable contents.
[[nodiscard]] Graph disassembleImage(const LoadedImage &image,
                                     const Configuration &config,
                                     const Disassembler::AnalysisHints &hints);

} // namespace dragon::loader
