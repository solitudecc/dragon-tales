#include <dragon/lifters/ignil/SymbolicExecutor.hpp>
#include "utils/FloatOps.hpp"

#include <algorithm>
#include <sstream>
#include <tuple>
#include <utility>

namespace dragon::ignil {

namespace {

// ---------------------------------------------------------------------------
// Small Z3 helpers
// ---------------------------------------------------------------------------

/// Bitvector literal. The explicit uint64_t cast matters: z3::context::bv_val
/// is overloaded on int/unsigned/int64_t/uint64_t, and anything else (an
/// `unsigned long long`, say) converts to several of them equally well and is
/// ambiguous.
z3::expr bvVal(z3::context &ctx, std::uint64_t value, Width width) {
  return ctx.bv_val(static_cast<std::uint64_t>(value), width);
}

/// Mask a host value down to `width` bits.
std::uint64_t maskTo(std::uint64_t v, Width width) {
  if (width >= 64)
    return v;
  return v & ((std::uint64_t{1} << width) - 1);
}

Width bvWidth(const z3::expr &e) {
  return static_cast<Width>(e.get_sort().bv_size());
}

/// Zero-extend or truncate `e` to exactly `width` bits.
z3::expr zeroCoerce(const z3::expr &e, Width width) {
  const Width cur = bvWidth(e);
  if (cur == width)
    return e;
  if (cur > width)
    return e.extract(width - 1, 0);
  return z3::zext(e, width - cur);
}

/// Sign-extend or truncate `e` to exactly `width` bits.
z3::expr signCoerce(const z3::expr &e, Width width) {
  const Width cur = bvWidth(e);
  if (cur == width)
    return e;
  if (cur > width)
    return e.extract(width - 1, 0);
  return z3::sext(e, width - cur);
}

/// True for opcodes that read their operands as signed, and so must be
/// sign-extended rather than zero-extended when widened to a common width.
bool isSignedOp(Opcode op) {
  switch (op) {
    case Opcode::SDIV:
    case Opcode::SMOD:
    case Opcode::SLT:
    case Opcode::SLE:
    case Opcode::SGT:
    case Opcode::SGE:
      return true;
    default:
      return false;
  }
}

bool isComparison(Opcode op) {
  switch (op) {
    case Opcode::EQ:
    case Opcode::NEQ:
    case Opcode::ULT:
    case Opcode::ULE:
    case Opcode::UGT:
    case Opcode::UGE:
    case Opcode::SLT:
    case Opcode::SLE:
    case Opcode::SGT:
    case Opcode::SGE:
      return true;
    default:
      return false;
  }
}

/// IEEE754 binary64, the only float format IGNIL models.

std::string hex(std::uint64_t v) {
  std::ostringstream os;
  os << "0x" << std::hex << v;
  return os.str();
}

/// Name of the free variable standing in for a never-written register byte.
std::string lazyRegName(std::uint64_t offset) {
  return "reg[" + std::to_string(offset) + "]";
}

/// Name of the free variable standing in for a never-written memory byte.
std::string lazyMemName(std::uint64_t addr) { return "mem[" + hex(addr) + "]"; }

/// Bytes needed to hold `width` bits.
std::uint64_t byteCount(Width width) { return (width + 7u) / 8u; }

} // namespace

// ---------------------------------------------------------------------------
// Solution
// ---------------------------------------------------------------------------

std::optional<std::uint64_t> Solution::value(std::string_view symbol) const {
  auto it = values_.find(std::string(symbol));
  if (it == values_.end())
    return std::nullopt;
  return it->second;
}

std::vector<std::uint8_t> Solution::bytes(std::string_view region) const {
  const std::string name(region);
  auto it = regionSizes_.find(name);
  if (it == regionSizes_.end())
    return {};

  std::vector<std::uint8_t> out;
  out.reserve(it->second);
  for (std::uint64_t i = 0; i < it->second; ++i) {
    auto v = value(name + "[" + std::to_string(i) + "]");
    out.push_back(static_cast<std::uint8_t>(v.value_or(0) & 0xFF));
  }
  return out;
}

// ---------------------------------------------------------------------------
// SymbolicState
// ---------------------------------------------------------------------------

SymbolicState::SymbolicState(z3::context &ctx, SymbolicOptions opts) :
    ctx_(&ctx), opts_(std::move(opts)) {}

z3::expr SymbolicState::symbol(std::string_view name, Width width) const {
  std::string key(name);
  auto it = symbols_.find(key);
  if (it != symbols_.end())
    return it->second;
  z3::expr s = ctx_->bv_const(key.c_str(), width);
  symbols_.emplace(key, s);
  return s;
}

z3::expr SymbolicState::readByte(const ByteStore &store, std::uint64_t key,
                                 std::string_view kind) const {
  auto it = store.find(key);
  if (it != store.end()) {
    const StoredByte &b = it->second;
    return b.source.extract(b.index * 8 + 7, b.index * 8);
  }
  if (kind == "mem")
    if (auto concrete = imageByte(key))
      return bvVal(*ctx_, *concrete, 8);
  if (!opts_.symbolizeUnmappedReads)
    return bvVal(*ctx_, 0, 8);
  // Deterministic name, so two reads of the same never-written location agree
  // without having to write the placeholder back into the store (which would
  // make isRegisterDefined/isMemoryDefined lie).
  return symbol(kind == "reg" ? lazyRegName(key) : lazyMemName(key), 8);
}

std::optional<std::uint8_t>
SymbolicState::imageByte(std::uint64_t addr) const {
  for (auto it = images_.rbegin(); it != images_.rend(); ++it)
    if (addr >= it->base && addr - it->base < it->bytes->size())
      return (*it->bytes)[addr - it->base];
  return std::nullopt;
}

std::optional<std::uint8_t>
SymbolicState::concreteByte(const ByteStore &store, std::uint64_t key,
                            bool images) const {
  if (auto it = store.find(key); it != store.end()) {
    const StoredByte &b = it->second;
    // get_numeral_uint64 only reads 64 bits; a wider literal (a vector
    // register) goes through the general path instead.
    if (!b.source.is_numeral() || bvWidth(b.source) > 64)
      return std::nullopt;
    return static_cast<std::uint8_t>(
            (b.source.get_numeral_uint64() >> (b.index * 8)) & 0xFF);
  }
  return images ? imageByte(key) : std::nullopt;
}

void SymbolicState::writeBytes(ByteStore &store, std::uint64_t base,
                               const z3::expr &value, Width width) {
  const std::uint64_t n = byteCount(width);
  // A sub-byte value (a 1-bit comparison result written to a flag) is widened
  // to a whole byte first - the stores are byte-granular.
  const z3::expr v = zeroCoerce(value, static_cast<Width>(n * 8));
  for (std::uint64_t i = 0; i < n; ++i)
    store.insert_or_assign(base + i,
                           StoredByte{v, static_cast<unsigned>(i)});
}

z3::expr SymbolicState::readBytes(const ByteStore &store, std::uint64_t base,
                                  Width width, std::string_view kind) const {
  const std::uint64_t n = byteCount(width);

  // Fast path: if every byte in range is a slice of one value, at the right
  // index, and that value is exactly this wide, the location is being read
  // back at the width it was written - so hand back the original expression
  // rather than a concat of extracts Z3 would have to fold.
  if (auto first = store.find(base); first != store.end()) {
    const z3::expr &source = first->second.source;
    bool intact = first->second.index == 0 && bvWidth(source) == n * 8;
    for (std::uint64_t i = 1; intact && i < n; ++i) {
      auto it = store.find(base + i);
      // Z3 hash-conses its AST, so equal ids mean the same expression.
      intact = it != store.end() && it->second.index == i &&
               it->second.source.id() == source.id();
    }
    if (intact)
      return zeroCoerce(source, width);
  }

  // Second fast path: every byte is concrete, so the value is a literal and
  // there is no need to build a concat of extracts only for simplify() to
  // fold it straight back. This is what keeps a concrete run - image bytes,
  // a concrete stack - from paying for Z3 terms it never needed.
  if (n <= 8) {
    const bool images = kind == "mem";
    std::uint64_t literal = 0;
    bool concrete = true;
    for (std::uint64_t i = 0; concrete && i < n; ++i) {
      if (auto b = concreteByte(store, base + i, images))
        literal |= static_cast<std::uint64_t>(*b) << (i * 8);
      else
        concrete = false;
    }
    if (concrete)
      return bvVal(*ctx_, maskTo(literal, width), width);
  }

  // Little-endian: byte `base` is the least significant, so it ends up on the
  // right of the concatenation.
  z3::expr acc = readByte(store, base, kind);
  for (std::uint64_t i = 1; i < n; ++i)
    acc = z3::concat(readByte(store, base + i, kind), acc);
  return zeroCoerce(acc, width).simplify();
}

// -- diffing ----------------------------------------------------------------

std::vector<SymbolicState::Delta>
SymbolicState::diff(const SymbolicState &before) const {
  std::vector<Delta> out;

  // Widest run reported as one value. Width is a uint8_t, so a run can never
  // describe more than 255 bits; 16 bytes covers every register and vector
  // write dragon models today, and anything wider falls back to byte runs.
  constexpr std::uint64_t kMaxRunBytes = 16;

  auto scan = [&](const ByteStore &now, const ByteStore &was, Delta::Kind kind,
                  std::string_view readKind) {
    // Keys whose stored byte changed, in address order (ByteStore is ordered).
    // Z3 hash-conses its AST, so equal ids mean the same expression; a byte is
    // unchanged when it is the same slice of the same written value.
    std::vector<std::uint64_t> changed;
    for (const auto &[key, byte] : now) {
      const auto old = was.find(key);
      if (old == was.end() || old->second.index != byte.index ||
          old->second.source.id() != byte.source.id())
        changed.push_back(key);
    }

    std::size_t i = 0;
    while (i < changed.size()) {
      // Extent of this run of consecutive changed addresses.
      std::size_t runEnd = i + 1;
      while (runEnd < changed.size() &&
             changed[runEnd] == changed[runEnd - 1] + 1)
        ++runEnd;

      while (i < runEnd) {
        const std::uint64_t base = changed[i];
        const StoredByte &first = now.at(base);
        const std::uint64_t srcBytes = byteCount(bvWidth(first.source));

        // If this byte starts a value that was written whole, and every one of
        // its remaining bytes also changed and is still intact, report the
        // whole value - that is exactly readBytes' fast path, so the delta
        // carries the original expression rather than a concat of extracts.
        std::uint64_t n = 1;
        if (first.index == 0 && srcBytes > 1 && srcBytes <= kMaxRunBytes &&
            i + srcBytes <= runEnd) {
          bool intact = true;
          for (std::uint64_t k = 1; intact && k < srcBytes; ++k) {
            const auto it = now.find(base + k);
            intact = it != now.end() && it->second.index == k &&
                     it->second.source.id() == first.source.id();
          }
          if (intact)
            n = srcBytes;
        }

        const auto width = static_cast<Width>(n * 8);
        out.push_back(Delta{kind, base, width,
                            readBytes(now, base, width, readKind)});
        i += n;
      }
    }
  };

  scan(regBytes_, before.regBytes_, Delta::Kind::Register, "reg");
  scan(memBytes_, before.memBytes_, Delta::Kind::Memory, "mem");
  return out;
}

// -- merging ----------------------------------------------------------------

std::vector<SymbolicState::Delta>
SymbolicState::merge(const SymbolicState &incoming, std::string_view tag) {
  std::vector<Delta> out;
  const std::string prefix = "top@" + std::string(tag) + "!";

  // Widest run symbolised as one variable. Kept to 64 bits so every variable
  // a merge makes can be compared against a host integer.
  constexpr std::uint64_t kMaxRunBytes = 8;

  auto join = [&](ByteStore &mine, const ByteStore &theirs, bool images,
                  Delta::Kind kind, char letter) {
    // A byte an earlier merge under this tag already turned into a variable.
    auto isTop = [&](const StoredByte &b) {
      return b.source.is_const() && !b.source.is_numeral() &&
             b.source.decl().name().str().rfind(prefix, 0) == 0;
    };

    auto same = [&](std::uint64_t key) {
      const auto a = mine.find(key);
      const auto b = theirs.find(key);
      // Z3 hash-conses its AST, so equal ids mean the same expression.
      if (a != mine.end() && b != theirs.end() &&
          a->second.index == b->second.index &&
          a->second.source.id() == b->second.source.id())
        return true;
      // Different expressions can still be the same literal - and one side
      // may hold it in the overlay while the other reads it off an image.
      const auto ca = concreteByte(mine, key, images);
      const auto cb = incoming.concreteByte(theirs, key, images);
      return ca && cb && *ca == *cb;
    };

    // Walk the union of both key sets, in address order.
    std::vector<std::uint64_t> differing;
    auto a = mine.begin();
    auto b = theirs.begin();
    while (a != mine.end() || b != theirs.end()) {
      std::uint64_t key;
      if (b == theirs.end() || (a != mine.end() && a->first <= b->first))
        key = a->first;
      else
        key = b->first;
      const bool top = a != mine.end() && a->first == key && isTop(a->second);
      if (a != mine.end() && a->first == key)
        ++a;
      if (b != theirs.end() && b->first == key)
        ++b;
      if (!top && !same(key))
        differing.push_back(key);
    }

    // The whole written value a byte belongs to, as [start, start + n): the
    // unit to symbolise. One differing byte of a 64-bit counter means the
    // counter varies, not that its low byte does - symbolising byte by byte
    // would leave the rest a literal and take one more merge per byte to
    // find out otherwise.
    auto extentOf = [](const ByteStore &store, std::uint64_t key)
            -> std::pair<std::uint64_t, std::uint64_t> {
      const auto it = store.find(key);
      if (it == store.end())
        return {key, 1};
      const std::uint64_t n = byteCount(bvWidth(it->second.source));
      const std::uint64_t start = key - it->second.index;
      if (n <= 1 || n > kMaxRunBytes)
        return {key, 1};
      for (std::uint64_t k = 0; k < n; ++k) {
        const auto b = store.find(start + k);
        if (b == store.end() || b->second.index != k ||
            b->second.source.id() != it->second.source.id())
          return {key, 1};
      }
      return {start, n};
    };

    // Prefer the shape of the incoming value, then of the one it replaces.
    // Extents are taken in address order and one that starts inside an
    // earlier one is dropped, so no byte is symbolised twice.
    std::uint64_t coveredUntil = 0;
    bool any = false;
    for (const std::uint64_t key : differing) {
      if (any && key < coveredUntil)
        continue;
      auto [start, n] = extentOf(theirs, key);
      if (n == 1)
        std::tie(start, n) = extentOf(mine, key);
      if (any && start < coveredUntil) {
        start = key;
        n = 1;
      }

      const auto width = static_cast<Width>(n * 8);
      const z3::expr top =
              symbol(prefix + letter + hex(start) + ":" + std::to_string(width),
                     width);
      writeBytes(mine, start, top, width);
      out.push_back(Delta{kind, start, width, top});
      coveredUntil = start + n;
      any = true;
    }
  };

  join(regBytes_, incoming.regBytes_, /*images=*/false, Delta::Kind::Register,
       'r');
  join(memBytes_, incoming.memBytes_, /*images=*/true, Delta::Kind::Memory,
       'm');

  // Variables the incoming path made are still mentioned by the bytes both
  // sides agree on, so a model has to be able to report them.
  for (const auto &[name, sym] : incoming.symbols_)
    symbols_.emplace(name, sym);
  for (const auto &[name, size] : incoming.regions_)
    regions_.emplace(name, size);

  return out;
}

// -- registers --------------------------------------------------------------

void SymbolicState::writeRegister(const Reg &r, const z3::expr &value) {
  writeBytes(regBytes_, r.offset, zeroCoerce(value, r.width), r.width);

  // x86-64 / AArch64: a 32-bit write clears the upper half of the 64-bit
  // parent. 8- and 16-bit writes merge into the parent on both, which the
  // byte-granular store already gets right for free.
  if (opts_.zeroExtendOn32BitWrite && r.width == 32) {
    for (std::uint64_t i = 4; i < 8; ++i)
      regBytes_.insert_or_assign(r.offset + i,
                                 StoredByte{bvVal(*ctx_, 0, 8), 0});
  }
}

void SymbolicState::writeRegister(const Reg &r, std::uint64_t value) {
  writeRegister(r, bvVal(*ctx_, maskTo(value, r.width), r.width));
}

z3::expr SymbolicState::readRegister(const Reg &r) const {
  return readBytes(regBytes_, r.offset, r.width, "reg");
}

bool SymbolicState::isRegisterDefined(const Reg &r) const {
  const std::uint64_t n = byteCount(r.width);
  for (std::uint64_t i = 0; i < n; ++i)
    if (regBytes_.find(r.offset + i) == regBytes_.end())
      return false;
  return true;
}

z3::expr SymbolicState::symbolizeRegister(const Reg &r, std::string_view name) {
  const std::string chosen =
          name.empty() ? (r.name.empty() ? "reg_" + std::to_string(r.offset)
                                         : r.name)
                       : std::string(name);
  z3::expr s = symbol(chosen, r.width);
  writeRegister(r, s);
  return s;
}

// -- temporaries ------------------------------------------------------------

void SymbolicState::writeTemp(const Temp &t, const z3::expr &value) {
  temps_.insert_or_assign(t.id, zeroCoerce(value, t.width));
}

std::optional<z3::expr> SymbolicState::readTemp(const Temp &t) const {
  auto it = temps_.find(t.id);
  if (it == temps_.end())
    return std::nullopt;
  return it->second;
}

// -- memory -----------------------------------------------------------------

void SymbolicState::writeMemory(std::uint64_t addr, const z3::expr &value) {
  writeBytes(memBytes_, addr, value, bvWidth(value));
}

void SymbolicState::writeMemory(std::uint64_t addr,
                                const std::vector<std::uint8_t> &bytes) {
  if (bytes.size() >= kImageThreshold) {
    // The image replaces whatever was there, overlay included.
    memBytes_.erase(memBytes_.lower_bound(addr),
                    memBytes_.lower_bound(addr + bytes.size()));
    images_.push_back(
            {addr, std::make_shared<const std::vector<std::uint8_t>>(bytes)});
    return;
  }
  for (std::size_t i = 0; i < bytes.size(); ++i)
    memBytes_.insert_or_assign(addr + i,
                               StoredByte{bvVal(*ctx_, bytes[i], 8), 0});
}

z3::expr SymbolicState::readMemory(std::uint64_t addr, Width width) const {
  return readBytes(memBytes_, addr, width, "mem");
}

bool SymbolicState::isMemoryDefined(std::uint64_t addr, Width width) const {
  const std::uint64_t n = byteCount(width);
  for (std::uint64_t i = 0; i < n; ++i)
    if (memBytes_.find(addr + i) == memBytes_.end() && !imageByte(addr + i))
      return false;
  return true;
}

z3::expr SymbolicState::symbolizeMemory(std::uint64_t addr, std::uint64_t size,
                                        std::string_view name) {
  const std::string region(name);
  regions_.insert_or_assign(region, size);

  for (std::uint64_t i = 0; i < size; ++i)
    memBytes_.insert_or_assign(
            addr + i,
            StoredByte{symbol(region + "[" + std::to_string(i) + "]", 8), 0});

  // Handing back the whole region as one value is only meaningful while it
  // fits a bitvector we can compare against a 64-bit constant.
  const Width width =
          static_cast<Width>(std::clamp<std::uint64_t>(size, 1, 8) * 8);
  return readMemory(addr, width);
}

// -- constraints ------------------------------------------------------------

void SymbolicState::addConstraint(const z3::expr &c) {
  constraints_.push_back(c);
}

// ---------------------------------------------------------------------------
// SymbolicExecutor - construction
// ---------------------------------------------------------------------------

SymbolicExecutor::SymbolicExecutor(SymbolicOptions opts) :
  ownedCtx_(std::make_shared<z3::context>()), ctx_(ownedCtx_.get()),
    opts_(std::move(opts)), state_(*ctx_, opts_), solver_(*ctx_) {
  z3::params p(*ctx_);
  p.set("timeout", opts_.solverTimeoutMs);
  solver_.set(p);
}

SymbolicExecutor::SymbolicExecutor(z3::context &ctx, SymbolicOptions opts) :
    ctx_(&ctx), opts_(std::move(opts)), state_(*ctx_, opts_), solver_(*ctx_) {
  z3::params p(*ctx_);
  p.set("timeout", opts_.solverTimeoutMs);
  solver_.set(p);
}

void SymbolicExecutor::reset() {
  state_ = SymbolicState(*ctx_, opts_);
  solver_.reset();
  assertedConstraints_ = 0;
}

void SymbolicExecutor::setState(SymbolicState state) {
  state_ = std::move(state);
  solver_.reset();
  assertedConstraints_ = 0;
}

z3::expr SymbolicExecutor::bv(std::uint64_t value, Width width) const {
  return bvVal(*ctx_, maskTo(value, width), width);
}

// -- machine setup ----------------------------------------------------------

void SymbolicExecutor::setRegister(const Reg &r, std::uint64_t value) {
  state_.writeRegister(r, value);
}

void SymbolicExecutor::setRegister(const Reg &r, const z3::expr &value) {
  state_.writeRegister(r, value);
}

z3::expr SymbolicExecutor::symbolizeRegister(const Reg &r,
                                             std::string_view name) {
  return state_.symbolizeRegister(r, name);
}

void SymbolicExecutor::mapMemory(std::uint64_t addr,
                                 const std::vector<std::uint8_t> &bytes) {
  state_.writeMemory(addr, bytes);
}

void SymbolicExecutor::setMemory(std::uint64_t addr, const z3::expr &value) {
  state_.writeMemory(addr, value);
}

z3::expr SymbolicExecutor::symbolizeMemory(std::uint64_t addr,
                                           std::uint64_t size,
                                           std::string_view name) {
  return state_.symbolizeMemory(addr, size, name);
}

void SymbolicExecutor::assume(const z3::expr &c) { state_.addConstraint(c); }

void SymbolicExecutor::setConcreteOpaque(OpaqueKind kind, std::uint64_t selector,
                                         std::vector<std::uint64_t> results) {
  concreteOpaque_[{kind, selector}] = std::move(results);
}

// -- reading ----------------------------------------------------------------

z3::expr SymbolicExecutor::readRegister(const Reg &r) const {
  return state_.readRegister(r);
}

z3::expr SymbolicExecutor::readMemory(std::uint64_t addr, Width width) const {
  return state_.readMemory(addr, width);
}

std::optional<z3::expr> SymbolicExecutor::readTemp(const Temp &t) const {
  return state_.readTemp(t);
}

// ---------------------------------------------------------------------------
// SymbolicExecutor - expression construction
// ---------------------------------------------------------------------------

z3::expr SymbolicExecutor::eval(const Value &v) const {
  if (const auto *c = std::get_if<Const>(&v))
    return bvVal(*ctx_, maskTo(c->value, c->width), c->width);
  if (const auto *r = std::get_if<Reg>(&v))
    return state_.readRegister(*r);

  const auto &t = std::get<Temp>(v);
  if (auto e = state_.readTemp(t))
    return *e;
  // Reading a Temp that was never defined means the block is not in SSA form
  // (a lifter bug, or a block starting mid-sequence). Treat it the same way
  // as an unwritten register: a fresh free variable, so execution continues
  // and the solver can still say something useful about the rest.
  return state_.symbol("t" + std::to_string(t.id), t.width);
}

z3::expr SymbolicExecutor::coerce(const z3::expr &e, Width width) const {
  return zeroCoerce(e, width);
}

void SymbolicExecutor::writeDst(const Value &dst, const z3::expr &value) {
  if (const auto *t = std::get_if<Temp>(&dst)) {
    state_.writeTemp(*t, value);
    return;
  }
  if (const auto *r = std::get_if<Reg>(&dst)) {
    state_.writeRegister(*r, value);
    return;
  }
  // A Const destination is malformed IGNIL; step() rejects it before we get
  // here, so there is nothing to do.
}

z3::expr SymbolicExecutor::shift(Opcode op, const z3::expr &lhs,
                                 const z3::expr &rhs) const {
  // Do the shift in a width wide enough to hold both operands, so an amount
  // that overshoots the value's width still produces the all-zero (SHL/SHR)
  // or all-sign (SAR) result rather than wrapping. This is exactly the
  // behaviour IGNILSimplify folds constant shifts to.
  const Width w = std::max(bvWidth(lhs), bvWidth(rhs));
  const z3::expr amount = zeroCoerce(rhs, w);

  switch (op) {
    case Opcode::SHL:
      return z3::shl(zeroCoerce(lhs, w), amount);
    case Opcode::SHR:
      return z3::lshr(zeroCoerce(lhs, w), amount);
    default: // SAR
      return z3::ashr(signCoerce(lhs, w), amount);
  }
}

z3::expr SymbolicExecutor::binary(Opcode op, const z3::expr &lhs,
                                  const z3::expr &rhs, Width dstWidth) const {
  if (detail::floatBinaryOp(op))
    return zeroCoerce(detail::floatBinary(op,lhs,rhs),dstWidth);
  if (op == Opcode::SHL || op == Opcode::SHR || op == Opcode::SAR)
    return zeroCoerce(shift(op, lhs, rhs), dstWidth);

  // Widen both operands to a common width before computing, matching how
  // IGNILSimplify folds the same op on constants. For well-formed IGNIL every
  // width here is already equal and this is a no-op.
  const Width w = isComparison(op)
                          ? std::max(bvWidth(lhs), bvWidth(rhs))
                          : std::max({dstWidth, bvWidth(lhs), bvWidth(rhs)});
  const bool sign = isSignedOp(op);
  const z3::expr a = sign ? signCoerce(lhs, w) : zeroCoerce(lhs, w);
  const z3::expr b = sign ? signCoerce(rhs, w) : zeroCoerce(rhs, w);

  const auto boolTo1 = [&](const z3::expr &pred) {
    return z3::ite(pred, bvVal(*ctx_, 1, 1), bvVal(*ctx_, 0, 1));
  };

  z3::expr result = [&]() -> z3::expr {
    switch (op) {
      case Opcode::ADD: return a + b;
      case Opcode::SUB: return a - b;
      case Opcode::MUL: return a * b;
      case Opcode::AND: return a & b;
      case Opcode::OR: return a | b;
      case Opcode::XOR: return a ^ b;
      // Z3's division and remainder are total: x/0 is all-ones for unsigned
      // and -1/1 for signed, x%0 is x. IGNIL leaves division by zero
      // unspecified (IGNILSimplify refuses to fold it), so a block that can
      // divide by zero gets Z3's answer rather than a trap - constrain the
      // divisor if that matters.
      case Opcode::UDIV: return z3::udiv(a, b);
      case Opcode::SDIV: return a / b;
      case Opcode::UMOD: return z3::urem(a, b);
      // srem, not smod: IGNILSimplify folds SMOD with C's %, which truncates
      // toward zero and takes the sign of the dividend.
      case Opcode::SMOD: return z3::srem(a, b);
      case Opcode::EQ: return boolTo1(a == b);
      case Opcode::NEQ: return boolTo1(a != b);
      case Opcode::ULT: return boolTo1(z3::ult(a, b));
      case Opcode::ULE: return boolTo1(z3::ule(a, b));
      case Opcode::UGT: return boolTo1(z3::ugt(a, b));
      case Opcode::UGE: return boolTo1(z3::uge(a, b));
      case Opcode::SLT: return boolTo1(a < b);
      case Opcode::SLE: return boolTo1(a <= b);
      case Opcode::SGT: return boolTo1(a > b);
      default: return boolTo1(a >= b); // SGE
    }
  }();

  return zeroCoerce(result, dstWidth);
}

z3::expr SymbolicExecutor::unary(Opcode op, const z3::expr &src,
                                 Width dstWidth) const {
  if (detail::floatUnaryOp(op))
    return zeroCoerce(detail::floatUnary(op,src,dstWidth),dstWidth);
  switch (op) {
    case Opcode::NEG: {
      const Width w = std::max(dstWidth, bvWidth(src));
      return zeroCoerce(-zeroCoerce(src, w), dstWidth);
    }
    case Opcode::NOT: {
      const Width w = std::max(dstWidth, bvWidth(src));
      return zeroCoerce(~zeroCoerce(src, w), dstWidth);
    }
    case Opcode::SEXT:
      return signCoerce(src, dstWidth);
    default: // COPY, ZEXT, TRUNC
      return zeroCoerce(src, dstWidth);
  }
}

// ---------------------------------------------------------------------------
// SymbolicExecutor - solver
// ---------------------------------------------------------------------------

z3::solver &SymbolicExecutor::solver() {
  const auto &cs = state_.constraints();
  if (cs.size() < assertedConstraints_) {
    solver_.reset();
    assertedConstraints_ = 0;
  }
  for (std::size_t i = assertedConstraints_; i < cs.size(); ++i)
    solver_.add(cs[i]);
  assertedConstraints_ = cs.size();
  return solver_;
}

std::optional<bool> SymbolicExecutor::isSatisfiable(const z3::expr &condition) {
  // A condition that folds to a literal needs no solver: false is false under
  // any constraints, and true is satisfiable whenever there are none to
  // contradict it.
  const z3::expr folded = condition.simplify();
  if (folded.is_false())
    return false;
  if (folded.is_true() && state_.constraints().empty())
    return true;

  z3::solver &s = solver();
  s.push();
  s.add(condition);
  const z3::check_result r = s.check();
  s.pop();

  if (r == z3::unknown)
    return std::nullopt;
  return r == z3::sat;
}

std::optional<bool> SymbolicExecutor::proves(const z3::expr &condition) {
  auto counterexample = isSatisfiable(!condition);
  if (!counterexample)
    return std::nullopt;
  return !*counterexample;
}

std::optional<std::uint64_t> SymbolicExecutor::evaluate(const z3::expr &e) {
  // A literal is its own unique value, whatever the constraints say - unless
  // they are unsatisfiable, and that is rare enough not to pay two solver
  // calls on every concrete register read to find out.
  if (const z3::expr folded = e.simplify();
      folded.is_numeral() && bvWidth(folded) <= 64)
    return folded.get_numeral_uint64();

  z3::solver &s = solver();

  s.push();
  const z3::check_result r = s.check();
  if (r != z3::sat) {
    s.pop();
    return std::nullopt;
  }
  const z3::expr candidate = s.get_model().eval(e, /*model_completion=*/true);
  s.pop();

  if (!candidate.is_numeral())
    return std::nullopt;

  // Unique only if no model disagrees - the same test ValueEnumerator uses to
  // decide an "obfuscated" value is really a constant.
  s.push();
  s.add(e != candidate);
  const z3::check_result other = s.check();
  s.pop();

  if (other != z3::unsat)
    return std::nullopt;
  return candidate.get_numeral_uint64();
}

std::vector<std::uint64_t> SymbolicExecutor::enumerate(const z3::expr &e,
                                                       std::size_t max,
                                                       bool *exhaustive) {
  std::vector<std::uint64_t> out;
  if (exhaustive)
    *exhaustive = false;
  if (max == 0)
    return out;

  z3::solver &s = solver();
  s.push();

  bool ranOut = false;
  while (out.size() < max) {
    const z3::check_result r = s.check();
    if (r == z3::unsat) {
      ranOut = true;
      break;
    }
    if (r == z3::unknown)
      break;

    const z3::expr v = s.get_model().eval(e, /*model_completion=*/true);
    if (!v.is_numeral())
      break;
    out.push_back(v.get_numeral_uint64());
    s.add(e != v);
  }

  // Filling the budget is not proof there is nothing else; one more query
  // settles it, so a set of exactly `max` values is still reported complete.
  if (!ranOut && out.size() == max && s.check() == z3::unsat)
    ranOut = true;

  s.pop();
  if (exhaustive)
    *exhaustive = ranOut;
  return out;
}

std::optional<Solution> SymbolicExecutor::solve() {
  return solve(std::vector<z3::expr>{});
}

std::optional<Solution> SymbolicExecutor::solve(const z3::expr &goal) {
  return solve(std::vector<z3::expr>{goal});
}

std::optional<Solution>
SymbolicExecutor::solve(const std::vector<z3::expr> &goals) {
  z3::solver &s = solver();
  s.push();
  for (const auto &g : goals)
    s.add(g);

  const z3::check_result r = s.check();
  if (r != z3::sat) {
    s.pop();
    return std::nullopt;
  }

  const z3::model m = s.get_model();
  Solution sol;
  for (const auto &[name, sym] : state_.symbols()) {
    const z3::expr v = m.eval(sym, /*model_completion=*/true);
    if (v.is_numeral())
      sol.values_.emplace(name, v.get_numeral_uint64());
  }
  sol.regionSizes_ = state_.regions();

  s.pop();
  return sol;
}

// ---------------------------------------------------------------------------
// SymbolicExecutor - execution
// ---------------------------------------------------------------------------

void SymbolicExecutor::warn(RunResult &result, const Op &op,
                            std::string message) {
  result.warnings.push_back({op.address, std::move(message)});
}

std::optional<std::vector<std::uint64_t>>
SymbolicExecutor::concretize(const z3::expr &addr, std::size_t max,
                             const Op &op, std::string_view what,
                             RunResult &result) {
  if (addr.is_numeral())
    return std::vector<std::uint64_t>{addr.get_numeral_uint64()};

  bool exhaustive = false;
  ++result.stats.solverQueries;
  std::vector<std::uint64_t> values = enumerate(addr, max, &exhaustive);

  if (!exhaustive) {
    warn(result, op,
         std::string(what) + " has more than " + std::to_string(max) +
                 " possible values (or the solver timed out); left unresolved");
    return std::nullopt;
  }

  result.stats.addressesConcretized += values.size();
  return values;
}

ExecStatus SymbolicExecutor::stepMemory(const Op &op, RunResult &result) {
  if (op.opcode == Opcode::LOAD) {
    if (!op.dst || op.srcs.size() != 1)
      return ExecStatus::MalformedOp;

    const Width width = widthOf(*op.dst);
    const z3::expr addr = eval(op.srcs[0]).simplify();

    auto candidates =
            concretize(addr, opts_.maxAddressSolutions, op, "load address",
                       result);
    if (!candidates || candidates->empty()) {
      // Either too symbolic to resolve, or the path is infeasible. Either way
      // the loaded value is unknown; a fresh symbol keeps the run going
      // without inventing a wrong answer.
      const z3::expr fresh = state_.symbol(
              "load@" + hex(op.address) + "_" +
                      std::to_string(result.stats.opsExecuted) +
                      (freshTag_.empty() ? "" : "#" + freshTag_),
              width);
      writeDst(*op.dst, fresh);
      if (!candidates)
        result.unresolved.push_back(
                {/*isStore=*/false, op.address, width, addr, fresh});
      return ExecStatus::Ok;
    }

    // Fold the candidates into a select chain. A single candidate - the
    // common case, and every concrete address - collapses to a plain read.
    z3::expr value = state_.readMemory(candidates->back(), width);
    for (auto it = candidates->rbegin() + 1; it != candidates->rend(); ++it)
      value = z3::ite(addr == bvVal(*ctx_, *it, bvWidth(addr)),
                      state_.readMemory(*it, width), value);

    writeDst(*op.dst, value.simplify());
    return ExecStatus::Ok;
  }

  // STORE: srcs[0] = address, srcs[1] = value, no dst.
  if (op.srcs.size() != 2)
    return ExecStatus::MalformedOp;

  const z3::expr addr = eval(op.srcs[0]).simplify();
  const Width width = widthOf(op.srcs[1]);
  const z3::expr value = zeroCoerce(eval(op.srcs[1]), width);

  auto candidates = concretize(addr, opts_.maxAddressSolutions, op,
                               "store address", result);
  if (!candidates) {
    // Refusing to guess: the store is dropped and the caller is told, rather
    // than silently corrupting an address that only looked plausible.
    result.unresolved.push_back(
            {/*isStore=*/true, op.address, width, addr, value});
    return ExecStatus::Ok;
  }

  if (candidates->size() == 1) {
    state_.writeMemory(candidates->front(), value);
    return ExecStatus::Ok;
  }

  // Several possible addresses: write every one conditionally, so each byte
  // keeps its old value on the paths where this store did not target it.
  const std::uint64_t n = byteCount(width);
  for (std::uint64_t target : *candidates) {
    const z3::expr hits = (addr == bvVal(*ctx_, target, bvWidth(addr)));
    for (std::uint64_t i = 0; i < n; ++i) {
      const z3::expr fresh =
              zeroCoerce(value, static_cast<Width>(n * 8))
                      .extract(static_cast<unsigned>(i * 8 + 7),
                               static_cast<unsigned>(i * 8));
      const z3::expr old = state_.readMemory(target + i, 8);
      state_.writeMemory(target + i, z3::ite(hits, fresh, old).simplify());
    }
  }
  return ExecStatus::Ok;
}

void SymbolicExecutor::addSuccessors(const z3::expr &target,
                                     const z3::expr &guard, bool taken,
                                     const Op &op, RunResult &result) {
  const z3::expr t = target.simplify();

  if (t.is_numeral()) {
    result.successors.push_back({t.get_numeral_uint64(), guard, taken});
    return;
  }

  auto candidates =
          concretize(t, opts_.maxBranchTargets, op, "branch target", result);
  if (!candidates) {
    // The edge exists but we cannot say where it goes. Recorded as a warning
    // so RunResult::complete() reports the successor list as partial.
    return;
  }

  for (std::uint64_t addr : *candidates) {
    const z3::expr reaches = (t == bvVal(*ctx_, addr, bvWidth(t)));
    result.successors.push_back({addr, (guard && reaches).simplify(), taken});
  }
}

z3::expr SymbolicExecutor::opaqueValue(const Op &op, Width dstWidth) {
  const OpaqueKind kind = opaqueKindOf(op.aux);
  const unsigned index = opaqueIndexOf(op.aux);
  const std::string name(opaqueKindName(kind));

  // A caller-supplied concrete result short-circuits the symbol entirely, so a
  // concolic trace never goes symbolic on an anti-tamper CPUID. Match on the
  // kind plus the operands (leaf/subleaf for CPUID), which a concrete trace has
  // as literals; a non-literal operand cannot be matched, so it falls through.
  if (!concreteOpaque_.empty()) {
    std::uint64_t selector = 0;
    bool matchable = true;
    for (std::size_t i = 0; i < op.srcs.size() && i < 2; ++i) {
      const z3::expr s = eval(op.srcs[i]).simplify();
      if (!s.is_numeral() || bvWidth(s) > 64) { matchable = false; break; }
      selector |= (s.get_numeral_uint64() & 0xffffffffULL) << (i == 0 ? 32 : 0);
    }
    if (matchable) {
      const auto it = concreteOpaque_.find({kind, selector});
      if (it != concreteOpaque_.end() && index < it->second.size())
        return bvVal(*ctx_, it->second[index], dstWidth);
    }
  }

  if (!opaqueIsPure(kind)) {
    // Each execution is its own event, so the symbol is keyed on where it
    // happened rather than on the operands: two RDTSCs never compare equal.
    return state_.symbol(
            name + "@" + hex(op.address) + "_" + std::to_string(index),
            dstWidth);
  }

  // Pure: an uninterpreted function of the operands, so two CPUIDs with the
  // same leaf and subleaf build the identical term and Z3 proves them equal.
  z3::expr_vector args(*ctx_);
  z3::sort_vector domain(*ctx_);
  for (const Value &v: op.srcs) {
    args.push_back(eval(v));
    domain.push_back(args.back().get_sort());
  }
  const std::string fn = name + "." + std::to_string(index);
  return ctx_->function(fn.c_str(), domain, ctx_->bv_sort(dstWidth))(args);
}

ExecStatus SymbolicExecutor::stepBranch(const Op &op, RunResult &result) {
  switch (op.opcode) {
    case Opcode::JMP:
    case Opcode::CALL: {
      if (op.srcs.size() != 1)
        return ExecStatus::MalformedOp;
      result.exit = op.opcode == Opcode::JMP ? ExitKind::Jump : ExitKind::Call;
      addSuccessors(eval(op.srcs[0]), ctx_->bool_val(true), true, op, result);
      return ExecStatus::Ok;
    }

    case Opcode::RET:
      result.exit = ExitKind::Return;
      return ExecStatus::Ok;

    default: break; // CJMP
  }

  // CJMP: srcs[0] = condition, srcs[1] = taken target, srcs[2] = fallthrough.
  if (op.srcs.size() != 3)
    return ExecStatus::MalformedOp;

  result.exit = ExitKind::ConditionalJump;

  const z3::expr cond = eval(op.srcs[0]);
  const z3::expr isTrue = (cond != bvVal(*ctx_, 0, bvWidth(cond))).simplify();
  const z3::expr isFalse = (!isTrue).simplify();

  // Each side is only a real edge if the condition can hold there. This is
  // what collapses an opaque predicate to a single successor, and what prunes
  // a branch the accumulated path constraints have already decided.
  const std::pair<const z3::expr &, bool> sides[] = {{isTrue, true},
                                                     {isFalse, false}};
  const Value *targets[] = {&op.srcs[1], &op.srcs[2]};

  for (int i = 0; i < 2; ++i) {
    const auto &[guard, taken] = sides[i];
    // A guard that folded to a literal - the branch of a concrete run - is
    // decided already; isSatisfiable answers it without the solver, so it is
    // not counted as a query either.
    if (guard.is_false())
      continue;
    if (!(guard.is_true() && state_.constraints().empty()))
      ++result.stats.solverQueries;
    const auto feasible = isSatisfiable(guard);

    if (!feasible) {
      // Unknown, not proven infeasible. Keeping the edge is the conservative
      // choice for CFG recovery: a lost edge hides code, a spurious one only
      // costs analysis time.
      warn(result, op,
           std::string("solver could not decide the ") +
                   (taken ? "taken" : "not-taken") +
                   " branch; edge kept without proof");
    } else if (!*feasible) {
      continue;
    }

    addSuccessors(eval(*targets[i]), guard, taken, op, result);
  }

  return ExecStatus::Ok;
}

ExecStatus SymbolicExecutor::step(const Op &op, RunResult &result) {
  switch (op.opcode) {
    case Opcode::LOAD:
    case Opcode::STORE:
      return stepMemory(op, result);

    case Opcode::JMP:
    case Opcode::CJMP:
    case Opcode::CALL:
    case Opcode::RET:
      return stepBranch(op, result);

    case Opcode::OPAQUE: {
      if (!op.dst || std::holds_alternative<Const>(*op.dst))
        return ExecStatus::MalformedOp;
      if (opaqueIndexOf(op.aux) >= opaqueResultCount(opaqueKindOf(op.aux)))
        return ExecStatus::MalformedOp;
      writeDst(*op.dst, opaqueValue(op, widthOf(*op.dst)));
      return ExecStatus::Ok;
    }

    case Opcode::UNDEF: {
      if (!op.dst)
        return ExecStatus::MalformedOp;
      writeDst(*op.dst, state_.symbol("undef@" + hex(op.address) + "_" +
                                              std::to_string(op.address),
                                      widthOf(*op.dst)));
      return ExecStatus::Ok;
    }

    default: break;
  }

  if (!op.dst || std::holds_alternative<Const>(*op.dst))
    return ExecStatus::MalformedOp;

  const Width dstWidth = widthOf(*op.dst);

  switch (op.opcode) {
    // Unary.
    case Opcode::COPY:
    case Opcode::NEG:
    case Opcode::NOT:
    case Opcode::ZEXT:
    case Opcode::SEXT:
    case Opcode::TRUNC:
    case Opcode::SITOFP:
    case Opcode::FPTOSI:
    case Opcode::SITOFP32: case Opcode::FPTOSI32:
    case Opcode::FPEXT: case Opcode::FPTRUNC:
    case Opcode::UITOFP: case Opcode::UITOFP32:
    case Opcode::FPTOUI: case Opcode::FPTOUI32:
    case Opcode::FSQRT: case Opcode::FSQRT32:
    case Opcode::FFLOOR: case Opcode::FFLOOR32:
    case Opcode::FCEIL: case Opcode::FCEIL32:
    case Opcode::FTRUNC: case Opcode::FTRUNC32: {
      if (op.srcs.size() != 1)
        return ExecStatus::MalformedOp;
      const z3::expr src = eval(op.srcs[0]);
      const z3::expr value = unary(op.opcode, src, dstWidth);
      // Z3 builds the term it is asked for and folds nothing until told to,
      // so an op on a literal has to be folded here or a concrete run grows a
      // term per instruction that some later query then has to take apart.
      writeDst(*op.dst, src.is_numeral() ? value.simplify() : value);
      return ExecStatus::Ok;
    }

    // Binary.
    case Opcode::ADD:
    case Opcode::SUB:
    case Opcode::MUL:
    case Opcode::SDIV:
    case Opcode::UDIV:
    case Opcode::SMOD:
    case Opcode::UMOD:
    case Opcode::AND:
    case Opcode::OR:
    case Opcode::XOR:
    case Opcode::SHL:
    case Opcode::SHR:
    case Opcode::SAR:
    case Opcode::EQ:
    case Opcode::NEQ:
    case Opcode::ULT:
    case Opcode::ULE:
    case Opcode::UGT:
    case Opcode::UGE:
    case Opcode::SLT:
    case Opcode::SLE:
    case Opcode::SGT:
    case Opcode::SGE:
    case Opcode::FADD:
    case Opcode::FSUB:
    case Opcode::FMUL:
    case Opcode::FDIV:
    case Opcode::FLT:
    case Opcode::FEQ:
    case Opcode::FUNO:
    case Opcode::FADD32: case Opcode::FSUB32: case Opcode::FMUL32: case Opcode::FDIV32:
    case Opcode::FLT32: case Opcode::FEQ32: case Opcode::FUNO32: {
      if (op.srcs.size() != 2)
        return ExecStatus::MalformedOp;
      const z3::expr lhs = eval(op.srcs[0]);
      const z3::expr rhs = eval(op.srcs[1]);
      const z3::expr value = binary(op.opcode, lhs, rhs, dstWidth);
      writeDst(*op.dst, lhs.is_numeral() && rhs.is_numeral() ? value.simplify()
                                                             : value);
      return ExecStatus::Ok;
    }

    default:
      return ExecStatus::UnsupportedOp;
  }
}

RunResult SymbolicExecutor::run(const Block &block) {
  return run(block.ops());
}

RunResult SymbolicExecutor::run(const std::vector<Op> &ops) {
  RunResult result;
  result.ownedContext_ = ownedCtx_;

  auto failure = [&](ExecStatus status,const std::string &message) {
    result.status=status; result.exit=ExitKind::Error; result.error=message;
    return result;
  };
  std::map<std::uint32_t,std::size_t> labels;
  for (std::size_t i=0;i<ops.size();++i) {
    const auto &op=ops[i];
    if (op.opcode == Opcode::LABEL &&
        (op.dst || !op.srcs.empty() || !labels.emplace(op.aux,i).second))
      return failure(ExecStatus::MalformedOp,"invalid or duplicate local label");
  }
  for (const auto &op : ops)
    if ((op.opcode == Opcode::LOCALJMP || op.opcode == Opcode::LOCALCJMP) &&
        (op.dst || !labels.contains(op.aux) ||
         op.srcs.size() != (op.opcode == Opcode::LOCALCJMP ? 1u : 0u) ||
         (op.opcode == Opcode::LOCALCJMP && widthOf(op.srcs[0]) != 1)))
      return failure(ExecStatus::MalformedOp,"invalid local branch");
  std::size_t localSteps=0;
  for (std::size_t pc=0;pc<ops.size();++pc) {
    const Op &op=ops[pc];
    if (isLocalControl(op.opcode)) {
      ++result.stats.opsExecuted;
      if (++localSteps > 100000)
        return failure(ExecStatus::UnsupportedOp,"local control-flow execution budget exhausted");
      if (op.opcode == Opcode::LABEL) continue;
      bool taken=true;
      if (op.opcode == Opcode::LOCALCJMP) {
        const auto condition=evaluate(eval(op.srcs[0]));
        if (!condition)
          return failure(ExecStatus::UnsupportedOp,"local branch condition is not uniquely determined");
        taken=*condition != 0;
      }
      if (taken) pc=labels.at(op.aux); // next iteration starts after the label
      continue;
    }
    const ExecStatus status = step(op, result);
    ++result.stats.opsExecuted;

    if (status != ExecStatus::Ok) {
      result.status = status;
      result.exit = ExitKind::Error;
      result.error = std::string(status == ExecStatus::MalformedOp
                                         ? "malformed "
                                         : "unsupported ") +
                     std::string(opcodeName(op.opcode)) + " op at " +
                     hex(op.address);
      return result;
    }

    const bool terminator =
            op.opcode == Opcode::JMP || op.opcode == Opcode::CJMP ||
            op.opcode == Opcode::CALL || op.opcode == Opcode::RET;
    if (terminator && opts_.stopAtTerminator)
      break;
  }

  return result;
}

} // namespace dragon::ignil
