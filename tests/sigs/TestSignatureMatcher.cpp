#include <catch2/catch_test_macros.hpp>

#include <dragon/sigs/PatternParser.hpp>
#include <dragon/sigs/SignatureDB.hpp>
#include <dragon/sigs/SignatureMatcher.hpp>

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dragon::sigs;

TEST_CASE("SignatureMatcher - computeCRC16 of empty data is 0",
         "[sigs][signature-matcher]") {
  // FLAIR's crc16 returns 0 for zero length (crc_length 00 lines).
  REQUIRE(SignatureMatcher::computeCRC16({}) == 0);
}

// -----------------------------------------------------------------------
// External reference. Regression: computeCRC16 was CRC-16/ARC (poly 0xA001,
// no complement, no byte swap), which never equals the CRC field of a real
// .pat line, so no signature with crcLength > 0 could match. Every other test
// here derives its CRC from computeCRC16 itself and could not notice.
//
// Vectors: three AArch64 functions (gcc -O1) and the .pat lines rizin 0.9.1
// generated for them with `rz-sign -o out.pat binary`. rizin/radare2 carry
// FLAIR's own flair/crc16.cpp and load IDA .sig files, so these CRCs are
// FLAIR's.
// -----------------------------------------------------------------------

namespace {
std::vector<std::uint8_t> fromHex(std::string_view hex) {
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<std::uint8_t>(
            std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  return out;
}

struct RizinVector {
  const char *name;
  const char *bytes;   // the whole function
  const char *patLine; // rz-sign's line for it
  std::uint16_t crc;   // CRC field of that line
};

const RizinVector kRizinVectors[] = {
        {"mix",
         "a201805224378f52e4c6b372037c03536014004a237c041b6000004a2100000b"
         "4204007141ffff540000014ac0035fd6",
         "A201805224378F52E4C6B372037C03536014004A237C041B6000004A2100000B"
         " 10 3B36 0030 :0000 mix",
         0x3B36},
        {"sum",
         "3f0000716d010054e20300aa04c8218b00008052414440b82304010b6108814b"
         "0000010b5f0004eb61ffff54c0035fd600008052feffff17",
         "3F0000716D....54E20300AA04C8218B00008052414440B82304010B6108814B"
         " 18 84E3 0038 :0000 sum",
         0x84E3},
        {"fold",
         "008440caa19991d2a1aabdf2e1fad5f221eafff2007c019b008440ca618a9dd2"
         "a150a3f2c13fd7f2c199f8f2007c019b008440cac0035fd6",
         "008440CAA19991D2A1AABDF2E1FAD5F221EAFFF2007C019B008440CA618A9DD2"
         " 18 FD16 0038 :0000 fold",
         0xFD16},
};
} // namespace

TEST_CASE("SignatureMatcher - computeCRC16 equals FLAIR's (rizin reference)",
         "[sigs][signature-matcher]") {
  for (const auto &v: kRizinVectors) {
    const auto code = fromHex(v.bytes);
    const auto parsed = PatternParser::parseLine(v.patLine);
    REQUIRE(parsed.has_value());
    REQUIRE(code.size() >= 32u + parsed->crcLength);
    INFO(v.name);
    CHECK(SignatureMatcher::computeCRC16(
                  std::span(code).subspan(32, parsed->crcLength)) == v.crc);
  }
}

TEST_CASE("SignatureMatcher - rizin-generated .pat lines match their functions",
         "[sigs][signature-matcher][integration]") {
  SignatureDB db;
  for (const auto &v: kRizinVectors) {
    auto parsed = PatternParser::parseLine(v.patLine);
    REQUIRE(parsed.has_value());
    db.addSignatures({std::move(*parsed)});
  }
  std::uint64_t address = 0x400000;
  for (const auto &v: kRizinVectors) {
    const auto code = fromHex(v.bytes);
    auto match = SignatureMatcher::matchAt(db, code, address);
    INFO(v.name);
    REQUIRE(match.has_value());
    CHECK(match->name == v.name);
    CHECK(match->address == address);
    address += 0x100;
  }
}

TEST_CASE("SignatureMatcher - computeCRC16 is deterministic and "
         "input-sensitive",
         "[sigs][signature-matcher]") {
  const std::vector<std::uint8_t> a = {0x01, 0x02, 0x03, 0x04};
  const std::vector<std::uint8_t> b = {0x01, 0x02, 0x03, 0x05};
  REQUIRE(SignatureMatcher::computeCRC16(a) == SignatureMatcher::computeCRC16(a));
  REQUIRE(SignatureMatcher::computeCRC16(a) != SignatureMatcher::computeCRC16(b));
}

static Signature makeSignature(std::vector<PatternByte> leading,
                               std::uint8_t crcLen, std::uint16_t crc,
                               std::uint32_t totalLen,
                               std::vector<FunctionRef> names) {
  Signature sig;
  sig.leadingBytes = std::move(leading);
  sig.crcLength = crcLen;
  sig.crc16 = crc;
  sig.totalLength = totalLen;
  sig.publicNames = std::move(names);
  return sig;
}

TEST_CASE("SignatureMatcher - matchAt succeeds when leading bytes and CRC "
         "both check out",
         "[sigs][signature-matcher]") {
  // Candidate bytes: 55 8b ec [10 20 30 40] c3  (8 bytes total)
  // Leading pattern: 55 8b ec (fixed, no wildcard)
  // CRC region: the 4 bytes right after (10 20 30 40)
  const std::vector<std::uint8_t> candidate = {0x55, 0x8b, 0xec, 0x10,
                                               0x20, 0x30, 0x40, 0xc3};
  const auto crc =
          SignatureMatcher::computeCRC16(std::span(candidate).subspan(3, 4));

  SignatureDB db;
  db.addSignatures({makeSignature(
          {{0x55, false}, {0x8b, false}, {0xec, false}}, 4, crc, 8,
          {{0, "func"}})});

  auto match = SignatureMatcher::matchAt(db, candidate, 0x4000);
  REQUIRE(match.has_value());
  REQUIRE(match->address == 0x4000);
  REQUIRE(match->name == "func");
  REQUIRE(match->length == 8);
}

TEST_CASE("SignatureMatcher - wildcard bytes match anything",
         "[sigs][signature-matcher]") {
  const std::vector<std::uint8_t> candidateA = {0x55, 0x01, 0xec};
  const std::vector<std::uint8_t> candidateB = {0x55, 0xff, 0xec};

  SignatureDB db;
  db.addSignatures({makeSignature(
          {{0x55, false}, {0, true}, {0xec, false}}, 0, 0, 0, {{0, "f"}})});

  REQUIRE(SignatureMatcher::matchAt(db, candidateA, 0x1000).has_value());
  REQUIRE(SignatureMatcher::matchAt(db, candidateB, 0x1000).has_value());
}

TEST_CASE("SignatureMatcher - a CRC mismatch fails the match",
         "[sigs][signature-matcher]") {
  const std::vector<std::uint8_t> candidate = {0x55, 0x8b, 0xec, 0x10, 0x20};

  SignatureDB db;
  db.addSignatures({makeSignature({{0x55, false}, {0x8b, false}, {0xec, false}},
                                  2, 0xDEAD /* deliberately wrong */, 5,
                                  {{0, "func"}})});

  REQUIRE_FALSE(SignatureMatcher::matchAt(db, candidate, 0x1000).has_value());
}

TEST_CASE("SignatureMatcher - totalLength gates insufficient candidate bytes",
         "[sigs][signature-matcher]") {
  const std::vector<std::uint8_t> shortCandidate = {0x55, 0x8b};

  SignatureDB db;
  db.addSignatures(
          {makeSignature({{0x55, false}, {0x8b, false}}, 0, 0, 16, {{0, "f"}})});

  REQUIRE_FALSE(SignatureMatcher::matchAt(db, shortCandidate, 0x1000).has_value());
}

TEST_CASE("SignatureMatcher - matchEntries reports every public name in a "
         "matching module",
         "[sigs][signature-matcher]") {
  const std::vector<std::uint8_t> image = {0x55, 0x8b, 0xec, 0x90, 0x90};

  SignatureDB db;
  db.addSignatures({makeSignature({{0x55, false}, {0x8b, false}, {0xec, false}},
                                  0, 0, 0, {{0x0, "outer"}, {0x3, "inner"}})});

  auto matches = SignatureMatcher::matchEntries(db, image, 0x1000, {0x1000});
  REQUIRE(matches.size() == 2);
  REQUIRE(matches[0].address == 0x1000);
  REQUIRE(matches[0].name == "outer");
  REQUIRE(matches[1].address == 0x1003);
  REQUIRE(matches[1].name == "inner");
}

TEST_CASE("SignatureMatcher - matchEntries only tries addresses within the "
         "image",
         "[sigs][signature-matcher]") {
  const std::vector<std::uint8_t> image = {0x55, 0x8b, 0xec};

  SignatureDB db;
  db.addSignatures({makeSignature({{0x55, false}}, 0, 0, 0, {{0, "f"}})});

  // 0x0FFF is below imageBase, 0x2000 is past the end of the image.
  auto matches = SignatureMatcher::matchEntries(db, image, 0x1000,
                                                {0x0FFF, 0x1000, 0x2000});
  REQUIRE(matches.size() == 1);
  REQUIRE(matches[0].address == 0x1000);
}

// -----------------------------------------------------------------------
// Full pipeline: a .pat text line, parsed, with a CRC this test computes
// itself via the same computeCRC16 the matcher uses - proves parser and
// matcher agree end to end without depending on an external reference file.
// -----------------------------------------------------------------------

TEST_CASE("SignatureMatcher - full pipeline: parse a .pat line then match it",
         "[sigs][signature-matcher][integration]") {
  const std::vector<std::uint8_t> candidate = {0x55, 0x89, 0xe5, 0xaa,
                                               0xbb, 0xcc, 0xc3};
  // Leading pattern: 55 89 e5 (push rbp; mov ebp,esp - as bytes, not
  // actually decoded here, just used as pattern bytes).
  // CRC region: the 3 bytes right after (aa bb cc).
  const auto crc =
          SignatureMatcher::computeCRC16(std::span(candidate).subspan(3, 3));

  const std::string line =
          std::format("5589e5 03 {:04x} {:04x} :0000 known_func", crc,
                     candidate.size());

  auto parsed = dragon::sigs::PatternParser::parseLine(line);
  REQUIRE(parsed.has_value());

  SignatureDB db;
  db.addSignatures({std::move(*parsed)});

  auto match = SignatureMatcher::matchAt(db, candidate, 0x7000);
  REQUIRE(match.has_value());
  REQUIRE(match->name == "known_func");
  REQUIRE(match->address == 0x7000);
}
