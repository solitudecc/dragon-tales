#pragma once
#include <dragon/lifters/ignil/IGNIL.hpp>
#include <cstdint>

namespace dragon::analysis::detail {
// Equality of operand snapshots, never equality of abstract Unknown values.
// A defined Temp or a register is read once conceptually at this operation.
// Different spellings of the same register bytes do not change its identity.
template<typename DefinedTemp>
bool sameSnapshot(const ignil::Value &a,const ignil::Value &b,DefinedTemp defined) {
  const auto width=ignil::widthOf(a);
  if(!width || width>128 || width!=ignil::widthOf(b))return false;
  if(const auto *left=std::get_if<ignil::Reg>(&a)) {
    const auto *right=std::get_if<ignil::Reg>(&b);
    return right && left->offset==right->offset &&
        left->offset<=UINT32_MAX-(unsigned(width)+7)/8;
  }
  if(const auto *left=std::get_if<ignil::Temp>(&a)) {
    const auto *right=std::get_if<ignil::Temp>(&b);
    return right && left->id==right->id && defined(left->id,width);
  }
  return false;
}
}
