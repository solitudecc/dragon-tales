#pragma once

#include <dragon/lifters/ignil/IGNIL.hpp>

#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace dragon::slice {

/// Forward declaration needed by BinOp, UnaryOp, and Load,
/// which hold shared_ptr<Expr> children.
struct Expr;

/// A value's producing operation, or a block-entry state (opIndex == -1).
/// context distinguishes bounded visits around a loop within one query.
struct ValueOrigin {
  std::uint64_t blockAddress;
  int opIndex;
  int context;
  bool operator==(const ValueOrigin &) const = default;
};

/// A constant integer value.
struct Const {
  std::uint64_t value;
  ignil::Width width;
};

/// A register whose definition could not be traced further.
/// Treated as a symbolic / unknown input by the Z3 lowerer.
struct SymReg {
  ignil::Reg reg; ///< The register will depend on the architecture
                  ///< but for ignil the register is architecture agnostic.
  std::optional<ValueOrigin> origin{};
};

/// Result of a binary operation (same opcodes as ignil::Opcode).
struct BinOp {
  ignil::Opcode op; ///< Binary operation
  std::shared_ptr<Expr> left; ///< Left expression
  std::shared_ptr<Expr> right; ///< Right expression
};

/// Result of a unary operation (NEG, NOT, ZEXT, SEXT, TRUNC).
struct UnaryOp {
  ignil::Opcode op; ///< Unary operation
  std::shared_ptr<Expr> operand; ///< Operand for unary operation
  ignil::Width resultWidth{
          0}; ///< Target width for ZEXT/SEXT/TRUNC; 0 = same as operand
};

/// Value loaded from memory at a computed address.
struct Load {
  std::shared_ptr<Expr>
          addr; ///< Keep for the address an expression that can be solved
  ignil::Width width; ///< width for that address in bits
  // Identity of the producing LOAD within one sliced graph. Equal addresses
  // alone do not identify equal values across stores/calls. An absent origin
  // denotes an independent read, as with an originless Undef.
  std::uint64_t blockAddress{0};
  int opIndex{-1};
  int context{0};
  // A proven memory snapshot. Stores/calls change it; joins introduce a
  // new snapshot. Equal-address reads may agree only in the same snapshot.
  std::optional<ValueOrigin> memoryState{};
};

/// Slicer could not determine the value (UNDEF source, depth exceeded,
/// or a join node was reached).
///
/// Two Undefs are the same unknown value only when they come from the same
/// IGNIL op - `opIndex` within the block at `blockAddress`, as the slicer
/// records for an op it cannot model (UNDEF, OPAQUE, ...). An Undef with
/// `opIndex < 0` has no known origin and is independent of every other one.
/// SliceExprZ3 lowers them accordingly, so SUB(undef_a, undef_b) of two
/// unrelated unknowns is not solved to 0.
struct Undef {
  ignil::Width width{64};
  std::uint64_t blockAddress{0};
  int opIndex{-1};
};

/// Neutral symbolic expression produced by backward slicing.
/// Use the Z3 lowerer to turn it into a solver formula.
struct Expr {
  using Var = std::variant<Const, SymReg, BinOp, UnaryOp, Load, Undef>;
  Var data; ///< Data represents any of the possible expressions with ignil

  template<typename T>
  explicit Expr(T &&v) : data(std::forward<T>(v)) {}

  template<typename Visitor>
  decltype(auto) visit(Visitor &&vis) const {
    return std::visit(std::forward<Visitor>(vis), data);
  }
};

/// Human-readable rendering for debugging.
[[nodiscard]] std::string toString(const Expr &e);

/// Structural value identity, including conversion widths and producing ops.
/// Originless loads/undefs cannot prove equality. Register labels are cosmetic.
[[nodiscard]] bool sameValue(const Expr &a, const Expr &b);

} // namespace dragon::slice
