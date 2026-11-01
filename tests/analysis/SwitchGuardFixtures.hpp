#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace dragon::test::guard {
constexpr std::uint64_t base = 0x1000, table = 0x1300;
constexpr std::uint64_t fallback = 0x1200, extra = 0x1180;
struct Fixture {
  std::vector<std::uint8_t> bytes;
  std::uint64_t dispatch;
};
inline void put(std::vector<std::uint8_t> &bytes, std::size_t off,
                std::uint64_t value, unsigned size) {
  for (unsigned i = 0; i < size; ++i)
    bytes[off + i] = value >> (i * 8);
}
inline Fixture finish(std::vector<std::uint8_t> prefix, std::uint8_t branch,
                      bool byteIndex = false, bool signExtend = false) {
  const auto dispatch = base + prefix.size() + 6;
  prefix.insert(prefix.end(), {0x0f, branch, 0, 0, 0, 0});
  put(prefix, prefix.size() - 4, fallback - dispatch, 4);
  if (byteIndex) {
    if (signExtend)
      prefix.insert(prefix.end(), {0x48, 0x0f, 0xbe, 0xc7, 0xff, 0x24, 0xc5});
    else
      prefix.insert(prefix.end(), {0x40, 0x0f, 0xb6, 0xc7, 0xff, 0x24, 0xc5});
  } else
    prefix.insert(prefix.end(), {0x89, 0xff, 0xff, 0x24, 0xfd});
  const auto off = prefix.size();
  prefix.resize(off + 4);
  put(prefix, off, table, 4);
  std::vector<std::uint8_t> bytes(table - base + 256 * 8, 0xcc);
  std::copy(prefix.begin(), prefix.end(), bytes.begin());
  for (auto address: std::array<std::uint64_t, 5>{0x1100, 0x1110, 0x1120, extra, fallback}) {
    const auto at = address - base;
    bytes[at] = 0xb8; // return the case address in EAX for the Unicorn oracle
    put(bytes, at + 1, address, 4);
    bytes[at + 5] = 0xc3;
  }
  for (unsigned i = 0; i < 256; ++i)
    put(bytes, table - base + i * 8, i < 3 ? 0x1100 + i * 0x10 : extra, 8);
  return {std::move(bytes), dispatch};
}
inline Fixture narrow(bool signedGuard) {
  return finish({0x83, 0xff, 0x02}, signedGuard ? 0x8f : 0x87, true);
}
inline Fixture hole(std::uint32_t rare) {
  // (EDI <= 2) || (EDI == rare), materialised as booleans before one CJMP.
  std::vector<std::uint8_t> prefix = {
      0x83, 0xff, 0x02, 0x0f, 0x96, 0xc0, 0x81, 0xff, 0, 0, 0, 0,
      0x0f, 0x94, 0xc1, 0x08, 0xc8, 0x84, 0xc0};
  put(prefix, 8, rare, 4);
  return finish(std::move(prefix), 0x84);
}
inline Fixture masked() {
  return finish({0x83, 0xe7, 0x01, 0x83, 0xff, 0x07}, 0x87);
}
inline Fixture signExtended(bool signedGuard) {
  auto fixture = finish({0x40, 0x80, 0xff, 2}, signedGuard ? 0x8f : 0x87, true, true);
  put(fixture.bytes, table - base - 8, extra, 8); // entry -1 for the Unicorn check
  return fixture;
}
inline Fixture wrappedPrefix() {
  // EDI in [0,2] or [256,258]; both ranges become DIL in [0,2].
  std::vector<std::uint8_t> prefix = {
      0x83, 0xff, 2, 0x0f, 0x96, 0xc0,
      0x81, 0xff, 0, 1, 0, 0, 0x0f, 0x93, 0xc1,
      0x81, 0xff, 2, 1, 0, 0, 0x0f, 0x96, 0xc2,
      0x20, 0xd1, 0x08, 0xc8, 0x84, 0xc0};
  return finish(std::move(prefix), 0x84, true);
}
} // namespace dragon::test::guard
