#ifdef DRAGON_WITH_Z3

#include <catch2/catch_test_macros.hpp>

#include <dragon/analysis/SliceExpr.hpp>
#include <dragon/analysis/SliceExprZ3.hpp>
#include <dragon/analysis/ValueEnumerator.hpp>
#include <dragon/lifters/ignil/x86/x86IGNILRegs.hpp>

#include <memory>

namespace x86 = dragon::x86;
using namespace dragon::slice;

// Shorthand helpers to keep test bodies readable.
static std::shared_ptr<Expr> cst(std::uint64_t v, dragon::ignil::Width w = 64) {
    return std::make_shared<Expr>(Const{v, w});
}
static std::shared_ptr<Expr> sym(const dragon::ignil::Reg &r) {
    return std::make_shared<Expr>(SymReg{r});
}

// ---------------------------------------------------------------------------
// Test 1: lower() on a Const node - Z3 bitvector equals that value
// ---------------------------------------------------------------------------

TEST_CASE("SliceExprZ3 - lower Const node", "[z3][lowerer]") {
    z3::context ctx;
    SliceExprZ3 lowerer{ctx};

    Expr e{Const{42, 64}};
    z3::expr z = lowerer.lower(e);

    z3::solver s{ctx};
    s.add(z == ctx.bv_val(static_cast<uint64_t>(42), 64u));
    REQUIRE(s.check() == z3::sat);

    // And it is NOT equal to 43.
    z3::solver s2{ctx};
    s2.add(z == ctx.bv_val(static_cast<uint64_t>(43), 64u));
    REQUIRE(s2.check() == z3::unsat);
}

// ---------------------------------------------------------------------------
// Test 2: enumeratedValues on ADD(Const, Const) → single concrete result
//
// Exercises the case that the backward slicer produces when it sees:
//   mov rax, 0x3000 ; add rax, 8 ; jmp rax
// The slicer returns BinOp{ADD, Const{0x3000}, Const{8}}, not a Const, so
// the fast path misses it.  Z3 resolves it to the single value 0x3008.
// ---------------------------------------------------------------------------

TEST_CASE("SliceExprZ3 - enumeratedValues collapses constant ADD",
          "[z3][enumerator]") {
    z3::context ctx;
    Expr e{BinOp{dragon::ignil::Opcode::ADD, cst(0x3000), cst(8)}};

    auto vals = enumeratedValues(e, ctx);

    REQUIRE(vals.size() == 1);
    REQUIRE(vals[0] == 0x3008);
}

// ---------------------------------------------------------------------------
// Test 3: enumeratedValues on a SymReg with an OR constraint
//
// Models a jump table where rax is known to take one of three values.
// The constraint is built using the same Z3 symbol that SliceExprZ3 produces
// for RAX: ctx.bv_const("RAX:64", 64).
// ---------------------------------------------------------------------------

TEST_CASE("SliceExprZ3 - enumeratedValues with OR constraint on SymReg",
          "[z3][enumerator]") {
    z3::context ctx;
    Expr e{SymReg{x86::RAX}};

    // SliceExprZ3 names RAX as "RAX:64" (see symFor in SliceExprZ3.cpp).
    z3::expr rax = ctx.bv_const("RAX:64", 64);
    z3::expr constraint =
            (rax == ctx.bv_val(static_cast<uint64_t>(0x1000), 64u)) ||
            (rax == ctx.bv_val(static_cast<uint64_t>(0x2000), 64u)) ||
            (rax == ctx.bv_val(static_cast<uint64_t>(0x3000), 64u));

    auto vals = enumeratedValues(e, ctx, {constraint});

    // All three targets must be found (sorted=true by default).
    REQUIRE(vals.size() == 3);
    REQUIRE(vals[0] == 0x1000);
    REQUIRE(vals[1] == 0x2000);
    REQUIRE(vals[2] == 0x3000);
}

// ---------------------------------------------------------------------------
// Test 4: enumeratedValues on ADD(SymReg{RAX}, Const{8}) with a bound
//
// Models: mov rbx, rax ; add rbx, 8 ; jmp rbx  where rax == 0x1000.
// ---------------------------------------------------------------------------

TEST_CASE("SliceExprZ3 - enumeratedValues on ADD(SymReg, Const) with bound",
          "[z3][enumerator]") {
    z3::context ctx;
    Expr e{BinOp{dragon::ignil::Opcode::ADD, sym(x86::RAX), cst(8)}};

    z3::expr rax = ctx.bv_const("RAX:64", 64);
    z3::expr constraint = (rax == ctx.bv_val(static_cast<uint64_t>(0x1000), 64u));

    auto vals = enumeratedValues(e, ctx, {constraint});

    REQUIRE(vals.size() == 1);
    REQUIRE(vals[0] == 0x1008);
}

// ---------------------------------------------------------------------------
// Test 5: solveUnique - constant expression has a unique solution
// ---------------------------------------------------------------------------

TEST_CASE("SliceExprZ3 - solveUnique on Const returns that value",
          "[z3][enumerator]") {
    z3::context ctx;
    Expr e{Const{0xdeadbeef, 64}};

    auto result = solveUnique(e, ctx);

    REQUIRE(result.has_value());
    REQUIRE(*result == 0xdeadbeef);
}

// ---------------------------------------------------------------------------
// Test 6: solveUnique - multiple solutions → nullopt
// ---------------------------------------------------------------------------

TEST_CASE("SliceExprZ3 - solveUnique returns nullopt when not unique",
          "[z3][enumerator]") {
    z3::context ctx;
    Expr e{SymReg{x86::RAX}};

    // Constrain RAX to two values → not uniquely determined.
    z3::expr rax = ctx.bv_const("RAX:64", 64);
    z3::expr constraint =
            (rax == ctx.bv_val(static_cast<uint64_t>(0x1), 64u)) ||
            (rax == ctx.bv_val(static_cast<uint64_t>(0x2), 64u));

    auto result = solveUnique(e, ctx, {constraint});

    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Test 7: solveUnique - single-value constraint → that value
// ---------------------------------------------------------------------------

TEST_CASE("SliceExprZ3 - solveUnique finds the one constrained value",
          "[z3][enumerator]") {
    z3::context ctx;
    Expr e{SymReg{x86::RAX}};

    z3::expr rax = ctx.bv_const("RAX:64", 64);
    z3::expr constraint = (rax == ctx.bv_val(static_cast<uint64_t>(0x5), 64u));

    auto result = solveUnique(e, ctx, {constraint});

    REQUIRE(result.has_value());
    REQUIRE(*result == 0x5);
}

// ---------------------------------------------------------------------------
// Undef: unrelated unknowns are independent, one unknown equals itself.
//
// Every Undef used to lower to the same Z3 constant "undef", so two unrelated
// unknowns (two different unlifted instructions) were equal and
// SUB(undef, undef) solved uniquely to 0 - an indirect jump through such a
// value "resolved" to a constant.
// ---------------------------------------------------------------------------

static std::shared_ptr<Expr> sub(std::shared_ptr<Expr> a,
                                 std::shared_ptr<Expr> b) {
    return std::make_shared<Expr>(
            BinOp{dragon::ignil::Opcode::SUB, std::move(a), std::move(b)});
}

TEST_CASE("SliceExprZ3 - unrelated Undefs are independent", "[z3][undef]") {
    z3::context ctx;
    // No known origin.
    Expr anon = *sub(std::make_shared<Expr>(Undef{64}),
                     std::make_shared<Expr>(Undef{64}));
    CHECK_FALSE(solveUnique(anon, ctx).has_value());
    // Two different producing ops.
    Expr diff = *sub(std::make_shared<Expr>(Undef{64, 0x1000, 3}),
                     std::make_shared<Expr>(Undef{64, 0x1000, 4}));
    CHECK_FALSE(solveUnique(diff, ctx).has_value());
}

TEST_CASE("SliceExprZ3 - one producing op is one value", "[z3][undef]") {
    z3::context ctx;
    Expr same = *sub(std::make_shared<Expr>(Undef{64, 0x1000, 3}),
                     std::make_shared<Expr>(Undef{64, 0x1000, 3}));
    auto v = solveUnique(same, ctx);
    REQUIRE(v.has_value());
    CHECK(*v == 0);
}

#include <dragon/analysis/BackwardSlicer.hpp>
#include <dragon/disassembler/Graph.hpp>

TEST_CASE("SliceExprZ3 - sliced UNDEF ops keep their identity", "[z3][undef]") {
    namespace ignil = dragon::ignil;
    auto slice = [](bool sameSource) {
        // RAX = UNDEF ; RCX = UNDEF or COPY(RAX) ; RAX = SUB(RAX, RCX)
        ignil::Block b{0x1000};
        b.append({ignil::Opcode::UNDEF, ignil::Value{x86::RAX}, {}, 0x1000});
        if (sameSource)
            b.append({ignil::Opcode::COPY, ignil::Value{x86::RCX},
                      {ignil::Value{x86::RAX}}, 0x1004});
        else
            b.append({ignil::Opcode::UNDEF, ignil::Value{x86::RCX}, {}, 0x1004});
        b.append({ignil::Opcode::SUB, ignil::Value{x86::RAX},
                  {ignil::Value{x86::RAX}, ignil::Value{x86::RCX}}, 0x1008});
        b.append({ignil::Opcode::RET, std::nullopt, {}, 0x100b});
        dragon::Graph g{dragon::Architecture::X86_64, dragon::Configuration{}};
        g.addIGNILBlock(std::move(b));
        g.addBlock(0x1000);
        return dragon::BackwardSlicer{}.slice(ignil::Value{x86::RAX}, 0x1000, g);
    };
    z3::context ctx;
    CHECK_FALSE(solveUnique(slice(false), ctx).has_value()); // was 0
    auto v = solveUnique(slice(true), ctx);
    REQUIRE(v.has_value());
    CHECK(*v == 0);
}

#endif // DRAGON_WITH_Z3
