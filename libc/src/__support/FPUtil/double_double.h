//===-- Utilities for double-double data type. ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_FPUTIL_DOUBLE_DOUBLE_H
#define LLVM_LIBC_SRC___SUPPORT_FPUTIL_DOUBLE_DOUBLE_H

#include "multiply_add.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/properties/cpu_features.h" // LIBC_TARGET_CPU_HAS_FMA
#include "src/__support/number_pair.h"

namespace LIBC_NAMESPACE_DECL {
namespace fputil {

#define DEFAULT_DOUBLE_SPLIT 27

using DoubleDouble = LIBC_NAMESPACE::NumberPair<double>;

// The output of Dekker's FastTwoSum algorithm is correct, i.e.:
//   r.hi + r.lo = a + b exactly
//   and |r.lo| < eps(r.lo)
// Assumption: |a| >= |b|, or a = 0.
template <bool FAST2SUM = true>
LIBC_INLINE constexpr DoubleDouble exact_add(double a, double b) {
  DoubleDouble r{0.0, 0.0};
  if constexpr (FAST2SUM) {
    r.hi = a + b;
    double t = r.hi - a;
    r.lo = b - t;
  } else {
    r.hi = a + b;
    double t1 = r.hi - a;
    double t2 = r.hi - t1;
    double t3 = b - t1;
    double t4 = a - t2;
    r.lo = t3 + t4;
  }
  return r;
}

// Assumption: |a.hi| >= |b.hi|
LIBC_INLINE constexpr DoubleDouble add(const DoubleDouble &a,
                                       const DoubleDouble &b) {
  DoubleDouble r = exact_add(a.hi, b.hi);
  double lo = a.lo + b.lo;
  return exact_add(r.hi, r.lo + lo);
}

// Assumption: |a.hi| >= |b|
LIBC_INLINE constexpr DoubleDouble add(const DoubleDouble &a, double b) {
  DoubleDouble r = exact_add<false>(a.hi, b);
  return exact_add(r.hi, r.lo + a.lo);
}

// Veltkamp's Splitting for double precision.
// Note: This is proved to be correct for all rounding modes:
//   Zimmermann, P., "Note on the Veltkamp/Dekker Algorithms with Directed
//   Roundings," https://inria.hal.science/hal-04480440.
// Default splitting constant = 2^ceil(prec(double)/2) + 1 = 2^27 + 1.
template <size_t N = DEFAULT_DOUBLE_SPLIT>
LIBC_INLINE constexpr DoubleDouble split(double a) {
  DoubleDouble r{0.0, 0.0};
  // CN = 2^N.
  constexpr double CN = static_cast<double>(1 << N);
  constexpr double C = CN + 1.0;
  double t1 = C * a;
  double t2 = a - t1;
  r.hi = t1 + t2;
  r.lo = a - r.hi;
  return r;
}

// Helper for non-fma exact mult where the first number is already split.
template <size_t SPLIT_B = DEFAULT_DOUBLE_SPLIT>
LIBC_INLINE DoubleDouble exact_mult(const DoubleDouble &as, double a,
                                    double b) {
  DoubleDouble bs = split<SPLIT_B>(b);
  DoubleDouble r{0.0, 0.0};

  r.hi = a * b;
  double t1 = as.hi * bs.hi - r.hi;
  double t2 = as.hi * bs.lo + t1;
  double t3 = as.lo * bs.hi + t2;
  r.lo = as.lo * bs.lo + t3;

  return r;
}

// Note: When FMA instruction is not available, the `exact_mult` function is
// only correct for round-to-nearest mode.  See:
//   Zimmermann, P., "Note on the Veltkamp/Dekker Algorithms with Directed
//   Roundings," https://inria.hal.science/hal-04480440.
// Using Theorem 1 in the paper above, without FMA instruction, if we restrict
// the generated constants to precision <= 51, and splitting it by 2^28 + 1,
// then a * b = r.hi + r.lo is exact for all rounding modes.
template <size_t SPLIT_B = 27>
LIBC_INLINE DoubleDouble exact_mult(double a, double b) {
  DoubleDouble r{0.0, 0.0};

#ifdef LIBC_TARGET_CPU_HAS_FMA
  r.hi = a * b;
  r.lo = fputil::multiply_add(a, b, -r.hi);
#else
  // Dekker's Product.
  DoubleDouble as = split(a);

  r = exact_mult<SPLIT_B>(as, a, b);
#endif // LIBC_TARGET_CPU_HAS_FMA

  return r;
}

LIBC_INLINE DoubleDouble quick_mult(double a, const DoubleDouble &b) {
  DoubleDouble r = exact_mult(a, b.hi);
  r.lo = multiply_add(a, b.lo, r.lo);
  return r;
}

template <size_t SPLIT_B = 27>
LIBC_INLINE DoubleDouble quick_mult(const DoubleDouble &a,
                                    const DoubleDouble &b) {
  DoubleDouble r = exact_mult<SPLIT_B>(a.hi, b.hi);
  double t1 = multiply_add(a.hi, b.lo, r.lo);
  double t2 = multiply_add(a.lo, b.hi, t1);
  r.lo = t2;
  return r;
}

// Assuming |c| >= |a * b|.
template <>
LIBC_INLINE DoubleDouble multiply_add<DoubleDouble>(const DoubleDouble &a,
                                                    const DoubleDouble &b,
                                                    const DoubleDouble &c) {
  return add(c, quick_mult(a, b));
}

// Accurate double-double division, following Karp-Markstein's trick for
// division, implemented in the CORE-MATH project at:
// https://gitlab.inria.fr/core-math/core-math/-/blob/master/src/binary64/tan/tan.c#L1855
//
// Error bounds:
// Let a = ah + al, b = bh + bl.
// Let r = rh + rl be the approximation of (ah + al) / (bh + bl).
// Then:
//   (ah + al) / (bh + bl) - rh =
// = ((ah - bh * rh) + (al - bl * rh)) / (bh + bl)
// = (1 + O(bl/bh)) * ((ah - bh * rh) + (al - bl * rh)) / bh
// Let q = round(1/bh), then the above expressions are approximately:
// = (1 + O(bl / bh)) * (1 + O(2^-52)) * q * ((ah - bh * rh) + (al - bl * rh))
// So we can compute:
//   rl = q * (ah - bh * rh) + q * (al - bl * rh)
// as accurate as possible, then the error is bounded by:
//   |(ah + al) / (bh + bl) - (rh + rl)| < O(bl/bh) * (2^-52 + al/ah + bl/bh)
LIBC_INLINE DoubleDouble div(const DoubleDouble &a, const DoubleDouble &b) {
  DoubleDouble r;
  double q = 1.0 / b.hi;
  r.hi = a.hi * q;

#ifdef LIBC_TARGET_CPU_HAS_FMA
  double e_hi = fputil::multiply_add(b.hi, -r.hi, a.hi);
  double e_lo = fputil::multiply_add(b.lo, -r.hi, a.lo);
#else
  DoubleDouble b_hi_r_hi = fputil::exact_mult(b.hi, -r.hi);
  DoubleDouble b_lo_r_hi = fputil::exact_mult(b.lo, -r.hi);
  double e_hi = (a.hi + b_hi_r_hi.hi) + b_hi_r_hi.lo;
  double e_lo = (a.lo + b_lo_r_hi.hi) + b_lo_r_hi.lo;
#endif // LIBC_TARGET_CPU_HAS_FMA

  r.lo = q * (e_hi + e_lo);
  return r;
}

} // namespace fputil
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_FPUTIL_DOUBLE_DOUBLE_H
