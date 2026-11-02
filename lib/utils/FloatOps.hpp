#pragma once
#include <dragon/lifters/ignil/IGNIL.hpp>

namespace dragon::ignil::detail {
inline bool floatComparisonOp(Opcode op) {
  return op == Opcode::FLT || op == Opcode::FEQ || op == Opcode::FUNO ||
         op == Opcode::FLT32 || op == Opcode::FEQ32 || op == Opcode::FUNO32;
}
inline bool floatBinaryOp(Opcode op) {
  switch (op) {
    case Opcode::FADD: case Opcode::FSUB: case Opcode::FMUL: case Opcode::FDIV:
    case Opcode::FLT: case Opcode::FEQ: case Opcode::FUNO:
    case Opcode::FADD32: case Opcode::FSUB32: case Opcode::FMUL32: case Opcode::FDIV32:
    case Opcode::FLT32: case Opcode::FEQ32: case Opcode::FUNO32: return true;
    default: return false;
  }
}
inline bool floatUnaryOp(Opcode op) {
  switch (op) {
    case Opcode::SITOFP: case Opcode::FPTOSI: case Opcode::SITOFP32: case Opcode::FPTOSI32:
    case Opcode::FPEXT: case Opcode::FPTRUNC:
    case Opcode::UITOFP: case Opcode::UITOFP32: case Opcode::FPTOUI: case Opcode::FPTOUI32:
    case Opcode::FSQRT: case Opcode::FSQRT32: case Opcode::FFLOOR: case Opcode::FFLOOR32:
    case Opcode::FCEIL: case Opcode::FCEIL32: case Opcode::FTRUNC: case Opcode::FTRUNC32: return true;
    default: return false;
  }
}
inline unsigned floatBits(Opcode op) {
  switch (op) {
    case Opcode::FADD32: case Opcode::FSUB32: case Opcode::FMUL32: case Opcode::FDIV32:
    case Opcode::SITOFP32: case Opcode::FPTOSI32: case Opcode::FLT32: case Opcode::FEQ32:
    case Opcode::FUNO32: case Opcode::UITOFP32: case Opcode::FPTOUI32:
    case Opcode::FSQRT32: case Opcode::FFLOOR32: case Opcode::FCEIL32: case Opcode::FTRUNC32:
    case Opcode::FPEXT: return 32;
    default: return 64;
  }
}
}

#ifdef DRAGON_WITH_Z3
#include <z3++.h>
#include <functional>

namespace dragon::ignil::detail {
inline z3::expr fpBits(const z3::expr &value, unsigned width) {
  const auto bits = value.get_sort().bv_size();
  if (bits == width) return value;
  return bits < width ? z3::zext(value,width-bits) : value.extract(width-1,0);
}
inline z3::sort floatSort(z3::context &ctx, unsigned bits) {
  return bits == 32 ? ctx.fpa_sort(8,24) : ctx.fpa_sort(11,53);
}
using FreshFloatBits=std::function<z3::expr(unsigned)>;
inline z3::expr ieeeResult(const z3::expr &value,unsigned bits,const FreshFloatBits &fresh={}) {
  auto &ctx=value.ctx();
  const auto nan=z3::to_expr(ctx,Z3_mk_fpa_is_nan(ctx,value)).simplify();
  if (nan.is_false()) return value.mk_to_ieee_bv();
  // fp.to_ieee_bv(NaN) is one underspecified SMT term for every NaN. Using
  // that term directly correlates independent payloads and can prove a false
  // integer XOR=0. Keep the result a quiet NaN with an independent payload.
  const auto payload=fresh ? fresh(bits) : z3::to_expr(ctx,Z3_mk_fresh_const(ctx,"nan_bits",ctx.bv_sort(bits)));
  const uint64_t variable=bits==32 ? 0x803fffffull : 0x8007ffffffffffffull;
  const uint64_t fixed=bits==32 ? 0x7fc00000ull : 0x7ff8000000000000ull;
  const auto pattern=(payload & ctx.bv_val(variable,bits)) | ctx.bv_val(fixed,bits);
  return nan.is_true() ? pattern : z3::ite(nan,pattern,value.mk_to_ieee_bv());
}
inline z3::expr floatBinary(Opcode op, const z3::expr &left, const z3::expr &right,const FreshFloatBits &fresh={}) {
  auto &ctx = left.ctx();
  const auto bits = floatBits(op);
  auto sort = floatSort(ctx,bits);
  auto a = fpBits(left,bits).mk_from_ieee_bv(sort);
  auto b = fpBits(right,bits).mk_from_ieee_bv(sort);
  auto rne = z3::to_expr(ctx,Z3_mk_fpa_round_nearest_ties_to_even(ctx));
  auto predicate = [&](Z3_ast value) {
    return z3::ite(z3::to_expr(ctx,value),ctx.bv_val(1,1),ctx.bv_val(0,1));
  };
  switch (op) {
    case Opcode::FLT: case Opcode::FLT32: return predicate(Z3_mk_fpa_lt(ctx,a,b));
    case Opcode::FEQ: case Opcode::FEQ32: return predicate(Z3_mk_fpa_eq(ctx,a,b));
    case Opcode::FUNO: case Opcode::FUNO32:
      return z3::ite(z3::to_expr(ctx,Z3_mk_fpa_is_nan(ctx,a)) ||
                     z3::to_expr(ctx,Z3_mk_fpa_is_nan(ctx,b)),ctx.bv_val(1,1),ctx.bv_val(0,1));
    default: break;
  }
  Z3_ast result;
  switch (op) {
    case Opcode::FADD: case Opcode::FADD32: result = Z3_mk_fpa_add(ctx,rne,a,b); break;
    case Opcode::FSUB: case Opcode::FSUB32: result = Z3_mk_fpa_sub(ctx,rne,a,b); break;
    case Opcode::FMUL: case Opcode::FMUL32: result = Z3_mk_fpa_mul(ctx,rne,a,b); break;
    default: result = Z3_mk_fpa_div(ctx,rne,a,b); break;
  }
  return ieeeResult(z3::to_expr(ctx,result),bits,fresh);
}
inline z3::expr floatUnary(Opcode op, const z3::expr &source, unsigned destinationWidth,const FreshFloatBits &fresh={}) {
  auto &ctx = source.ctx();
  const auto bits = floatBits(op);
  auto sort = floatSort(ctx,bits);
  auto rne = z3::to_expr(ctx,Z3_mk_fpa_round_nearest_ties_to_even(ctx));
  auto rtz = z3::to_expr(ctx,Z3_mk_fpa_round_toward_zero(ctx));
  switch (op) {
    case Opcode::SITOFP: case Opcode::SITOFP32:
      return z3::to_expr(ctx,Z3_mk_fpa_to_fp_signed(ctx,rne,source,sort)).mk_to_ieee_bv();
    case Opcode::UITOFP: case Opcode::UITOFP32:
      return z3::to_expr(ctx,Z3_mk_fpa_to_fp_unsigned(ctx,rne,source,sort)).mk_to_ieee_bv();
    default: break;
  }
  auto value = fpBits(source,bits).mk_from_ieee_bv(sort);
  switch (op) {
    case Opcode::FPTOSI: case Opcode::FPTOSI32:
      return z3::to_expr(ctx,Z3_mk_fpa_to_sbv(ctx,rtz,value,destinationWidth));
    case Opcode::FPTOUI: case Opcode::FPTOUI32:
      return z3::to_expr(ctx,Z3_mk_fpa_to_ubv(ctx,rtz,value,destinationWidth));
    case Opcode::FPEXT: case Opcode::FPTRUNC:
      return ieeeResult(z3::to_expr(ctx,Z3_mk_fpa_to_fp_float(ctx,rne,value,
                          floatSort(ctx,op == Opcode::FPEXT ? 64 : 32))),op == Opcode::FPEXT ? 64 : 32,fresh);
    case Opcode::FSQRT: case Opcode::FSQRT32:
      return ieeeResult(z3::to_expr(ctx,Z3_mk_fpa_sqrt(ctx,rne,value)),bits,fresh);
    default: break;
  }
  auto rounding = op == Opcode::FFLOOR || op == Opcode::FFLOOR32 ? z3::to_expr(ctx,Z3_mk_fpa_round_toward_negative(ctx))
                  : op == Opcode::FCEIL || op == Opcode::FCEIL32 ? z3::to_expr(ctx,Z3_mk_fpa_round_toward_positive(ctx))
                                                               : rtz;
  return ieeeResult(z3::to_expr(ctx,Z3_mk_fpa_round_to_integral(ctx,rounding,value)),bits,fresh);
}
}
#endif
