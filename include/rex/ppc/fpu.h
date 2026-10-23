/**
 * @file        ppc/fpu.h
 * @brief       Scalar FPU arithmetic for generated code, with the guest's rounding mode
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

#include <rex/platform/fpscr.h>

// Codegen emits scalar FPU instructions (fadd, fmul, fmadd, frsp, fcfid and so
// on) through these macros. Natively they are the plain expressions: FPSCR's
// rounding mode is mirrored into the host's (MXCSR, FPCR), which every
// operation honors. WebAssembly has no rounding control, so there they check
// the emulated mode: round-to-nearest is the plain expression behind one
// predictable branch, and the directed modes (which titles set around some
// math: Blade lays out text under round-up and round-down) correct the nearest
// result by one ulp from its exact error. Results match x86 bit for bit,
// signed zeros and overflow included. VMX instructions round to nearest
// regardless of FPSCR, so vector code doesn't go through these.

#if defined(__wasm__)

namespace rex::ppc::fpu {

enum class Rounding { kNearest, kTowardZero, kUp, kDown };

inline Rounding CurrentRounding() {
  using Platform = rex::platform::FPSCRPlatform;
  switch (Platform::getcsr() & Platform::RoundMaskVal) {
    case SIMDE_MM_ROUND_TOWARD_ZERO:
      return Rounding::kTowardZero;
    case SIMDE_MM_ROUND_UP:
      return Rounding::kUp;
    case SIMDE_MM_ROUND_DOWN:
      return Rounding::kDown;
    default:
      return Rounding::kNearest;
  }
}

namespace detail {

// s + err == a + b exactly (Knuth's TwoSum; no ordering requirement).
inline void TwoSum(double a, double b, double& s, double& err) {
  s = a + b;
  double bb = s - a;
  err = (a - (s - bb)) + (b - bb);
}

// p + err == a * b exactly unless the product under- or overflows
// (Dekker/Veltkamp splitting, as wasm has no fused multiply-add instruction).
inline void TwoProduct(double a, double b, double& p, double& err) {
  constexpr double kSplit = 134217729.0;  // 2^27 + 1
  p = a * b;
  double ca = kSplit * a, cb = kSplit * b;
  double ahi = ca - (ca - a), alo = a - ahi;
  double bhi = cb - (cb - b), blo = b - bhi;
  err = ((ahi * bhi - p) + ahi * blo + alo * bhi) + alo * blo;
}

// a * b == (p + err) * 2^k exactly for finite nonzero a and b. The mantissas
// are multiplied, so neither the splitting nor the error term can over- or
// underflow whatever the exponents.
inline void ScaledProduct(double a, double b, double& p, double& err, int& k) {
  int ea, eb;
  double ma = std::frexp(a, &ea), mb = std::frexp(b, &eb);
  TwoProduct(ma, mb, p, err);
  k = ea + eb;
}

// Sign (-1, 0, 1) of the exact sum of the terms, via Shewchuk's expansion
// growth with zero elimination: the last component has the sum's sign.
inline int ExactSign(std::initializer_list<double> terms) {
  double e[8];
  int n = 0;
  for (double t : terms) {
    double q = t;
    int m = 0;
    for (int i = 0; i < n; ++i) {
      double s, err;
      TwoSum(q, e[i], s, err);
      if (err != 0.0) {
        e[m++] = err;
      }
      q = s;
    }
    if (q != 0.0) {
      e[m++] = q;
    }
    n = m;
  }
  return n == 0 ? 0 : (e[n - 1] > 0.0 ? 1 : -1);
}

// Moves the round-to-nearest result r of a finite exact value one step in the
// rounding direction when the exact value lies on that side (residual_sign is
// the sign of exact - r).
template <typename T>
inline T Step(T r, int residual_sign, Rounding mode) {
  constexpr T kInf = std::numeric_limits<T>::infinity();
  switch (mode) {
    case Rounding::kUp:
      return residual_sign > 0 ? std::nextafter(r, kInf) : r;
    case Rounding::kDown:
      return residual_sign < 0 ? std::nextafter(r, -kInf) : r;
    case Rounding::kTowardZero:
      return (residual_sign > 0 && r < 0) || (residual_sign < 0 && r > 0) ? std::nextafter(r, T(0))
                                                                          : r;
    default:
      return r;
  }
}

// Overflow: nearest gives an infinity, but the directed modes that round
// toward zero for its sign give the largest finite value instead.
template <typename T>
inline T Overflow(T r, Rounding mode) {
  constexpr T kMax = std::numeric_limits<T>::max();
  bool toward_zero = mode == Rounding::kTowardZero || (r > 0 && mode == Rounding::kDown) ||
                     (r < 0 && mode == Rounding::kUp);
  return toward_zero ? std::copysign(kMax, r) : r;
}

// An exact zero sum of nonzero or differently signed operands is +0 in every
// mode but round-down, where it is -0.
inline double ZeroSum(double r, double a, double b, Rounding mode) {
  if (r == 0.0 && mode == Rounding::kDown &&
      !(a == 0.0 && b == 0.0 && !std::signbit(a) && !std::signbit(b))) {
    return -0.0;
  }
  return r;
}

[[gnu::noinline]] inline double AddDirected(double a, double b, Rounding mode) {
  double r = a + b;
  if (!std::isfinite(r)) {
    return std::isinf(r) && std::isfinite(a) && std::isfinite(b) ? Overflow(r, mode) : r;
  }
  double s, err;
  TwoSum(a, b, s, err);
  if (err == 0.0) {
    return ZeroSum(r, a, b, mode);
  }
  return Step(r, err > 0.0 ? 1 : -1, mode);
}

[[gnu::noinline]] inline double MulDirected(double a, double b, Rounding mode) {
  double r = a * b;
  if (!std::isfinite(r)) {
    return std::isinf(r) && std::isfinite(a) && std::isfinite(b) ? Overflow(r, mode) : r;
  }
  if (a == 0.0 || b == 0.0) {
    return r;
  }
  // r scaled by 2^-k is a mantissa-sized value, so the scaling is exact even
  // when r is denormal or underflowed to zero.
  double p, err;
  int k;
  ScaledProduct(a, b, p, err, k);
  return Step(r, ExactSign({p, err, -std::ldexp(r, -k)}), mode);
}

[[gnu::noinline]] inline double DivDirected(double a, double b, Rounding mode) {
  double r = a / b;
  if (!std::isfinite(a) || !std::isfinite(b) || b == 0.0) {
    return r;  // infinities, NaNs and division by zero are exact
  }
  if (!std::isfinite(r)) {
    return Overflow(r, mode);
  }
  if (r == 0.0) {
    // Zero dividend is exact; otherwise the quotient underflowed, and its
    // sign is the signs' product (r is the zero of that sign).
    return a == 0.0 ? r : Step(r, std::signbit(r) ? -1 : 1, mode);
  }
  // sign(a/b - r) == sign(a - r*b) * sign(b), all scaled by 2^-k.
  double p, err;
  int k;
  ScaledProduct(r, b, p, err, k);
  int sign = ExactSign({std::ldexp(a, -k), -p, -err});
  return Step(r, b > 0.0 ? sign : -sign, mode);
}

[[gnu::noinline]] inline double SqrtDirected(double a, Rounding mode) {
  double r = std::sqrt(a);
  if (!std::isfinite(r) || r == 0.0) {
    return r;
  }
  // sign(sqrt(a) - r) == sign(a - r*r), scaled by 2^-k.
  double p, err;
  int k;
  ScaledProduct(r, r, p, err, k);
  return Step(r, ExactSign({std::ldexp(a, -k), -p, -err}), mode);
}

[[gnu::noinline]] inline double FmaDirected(double a, double b, double c, Rounding mode) {
  double r = std::fma(a, b, c);
  if (!std::isfinite(r)) {
    return std::isinf(r) && std::isfinite(a) && std::isfinite(b) && std::isfinite(c)
               ? Overflow(r, mode)
               : r;
  }
  if (a == 0.0 || b == 0.0) {
    return ZeroSum(r, a * b, c, mode);  // exactly 0 + c
  }
  // exact - r == (p + err + c' - r') * 2^k with c' and r' scaled by 2^-k.
  // A c too small to survive the scaling only breaks ties; one too large to
  // be scaled dwarfs the product, which then only breaks ties itself.
  double p, err;
  int k;
  ScaledProduct(a, b, p, err, k);
  double cs = std::ldexp(c, -k), rs = std::ldexp(r, -k);
  int sign;
  if (!std::isfinite(cs) || !std::isfinite(rs)) {
    sign = ExactSign({c, -r});
    if (sign == 0) {
      sign = p > 0.0 ? 1 : -1;
    }
  } else {
    sign = ExactSign({p, err, cs, -rs});
    if (sign == 0 && cs == 0.0 && c != 0.0) {
      sign = c > 0.0 ? 1 : -1;
    }
  }
  if (sign == 0 && r == 0.0) {
    return ZeroSum(r, p, c, mode);
  }
  return Step(r, sign, mode);
}

[[gnu::noinline]] inline double ToSingleDirected(double d, Rounding mode) {
  float f = static_cast<float>(d);
  if (std::isnan(d)) {
    return f;
  }
  if (std::isinf(f)) {
    return std::isfinite(d) ? Overflow(f, mode) : f;
  }
  // f is d's nearest float, close enough for d - f to be exact.
  double residual = d - static_cast<double>(f);
  return residual == 0.0 ? f : Step(f, residual > 0.0 ? 1 : -1, mode);
}

[[gnu::noinline]] inline double FromInt64Directed(int64_t x, Rounding mode) {
  double r = static_cast<double>(x);
  int sign;
  if (r >= 0x1p63) {
    sign = -1;  // x < 2^63 <= r
  } else {
    int64_t back = static_cast<int64_t>(r);
    sign = x > back ? 1 : (x < back ? -1 : 0);
  }
  return Step(r, sign, mode);
}

}  // namespace detail

inline double Add(double a, double b) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return a + b;
  }
  return detail::AddDirected(a, b, mode);
}

inline double Sub(double a, double b) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return a - b;
  }
  return detail::AddDirected(a, -b, mode);
}

inline double Mul(double a, double b) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return a * b;
  }
  return detail::MulDirected(a, b, mode);
}

inline double Div(double a, double b) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return a / b;
  }
  return detail::DivDirected(a, b, mode);
}

inline double Sqrt(double a) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return std::sqrt(a);
  }
  return detail::SqrtDirected(a, mode);
}

inline double Fma(double a, double b, double c) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return std::fma(a, b, c);
  }
  return detail::FmaDirected(a, b, c, mode);
}

inline double ToSingle(double d) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return static_cast<double>(static_cast<float>(d));
  }
  return detail::ToSingleDirected(d, mode);
}

inline double FromInt64(int64_t x) {
  Rounding mode = CurrentRounding();
  if (mode == Rounding::kNearest) [[likely]] {
    return static_cast<double>(x);
  }
  return detail::FromInt64Directed(x, mode);
}

}  // namespace rex::ppc::fpu

#define REX_FADD(a, b) ::rex::ppc::fpu::Add(a, b)
#define REX_FSUB(a, b) ::rex::ppc::fpu::Sub(a, b)
#define REX_FMUL(a, b) ::rex::ppc::fpu::Mul(a, b)
#define REX_FDIV(a, b) ::rex::ppc::fpu::Div(a, b)
#define REX_FSQRT(a) ::rex::ppc::fpu::Sqrt(a)
#define REX_FMA(a, b, c) ::rex::ppc::fpu::Fma(a, b, c)
#define REX_FSINGLE(a) ::rex::ppc::fpu::ToSingle(a)
#define REX_FCFID(a) ::rex::ppc::fpu::FromInt64(a)

#else

#define REX_FADD(a, b) ((a) + (b))
#define REX_FSUB(a, b) ((a) - (b))
#define REX_FMUL(a, b) ((a) * (b))
#define REX_FDIV(a, b) ((a) / (b))
#define REX_FSQRT(a) sqrt(a)
#define REX_FMA(a, b, c) std::fma(a, b, c)
#define REX_FSINGLE(a) double(float(a))
#define REX_FCFID(a) double(a)

#endif
