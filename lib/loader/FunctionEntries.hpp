#pragma once
#include <cstdint>
#include <vector>

namespace llvm::object { class ObjectFile; }
namespace dragon::loader {
class LoadedImage;
namespace detail {
std::vector<std::uint64_t> collectFunctionEntries(
    const llvm::object::ObjectFile &object, const LoadedImage &image);
}
}
