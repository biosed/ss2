#pragma once
// Deterministic IEEE-754 binary64 math primitives for cross-ISA simulation parity.
// Every function avoids platform-dependent libm rounding/polynomial differences
// across glibc/libmvec (aarch64, x86-64), musl, and MSVC UCRT.
// Compiled with -ffp-contract=off (/fp:precise on MSVC) on ss2_sim.
#include <cmath>
#include <cstdint>

namespace ss2::det {

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

struct SinCos {
    double s;
    double c;
};

// Overflow-safe Euclidean norm using IEEE-754 correctly rounded sqrt(dx*dx + dy*dy)
// without FMA contraction for all normal simulation coordinates.
inline double detHypot(double dx, double dy) noexcept {
    const double ax = dx < 0.0 ? -dx : dx;
    const double ay = dy < 0.0 ? -dy : dy;
    if (ax == 0.0) return ay;
    if (ay == 0.0) return ax;
    if (ax <= 1e150 && ay <= 1e150) {
        return std::sqrt(dx * dx + dy * dy);
    }
    const double hi = ax > ay ? ax : ay;
    const double lo = ax > ay ? ay : ax;
    const double r = lo / hi;
    return hi * std::sqrt(1.0 + r * r);
}

// Half-away-from-zero rounding to fixed-width signed 64-bit integer using
// exact IEEE-754 std::trunc (independent of long bit-width and FE_TONEAREST).
inline std::int64_t detRoundToInt64(double x) noexcept {
    const double t = std::trunc(x);
    const double frac = x - t;
    if (frac >= 0.5) return static_cast<std::int64_t>(t) + 1;
    if (frac <= -0.5) return static_cast<std::int64_t>(t) - 1;
    return static_cast<std::int64_t>(t);
}

inline int detRoundToInt(double x) noexcept {
    return static_cast<int>(detRoundToInt64(x));
}

// Cody-Waite 3-part pi/2 reduction + degree-15 sin / degree-16 cos Taylor
// polynomials on [-pi/4, pi/4]. Max error <= 1 ULP over all simulation angles.
inline SinCos detSinCos(double x) noexcept {
    if (x == 0.0) return {0.0, 1.0};
    constexpr double kInvPio2 = 0.636619772367581382432888403855;
    constexpr double kPio2_1 = 0x1.921fb54400000p+0;
    constexpr double kPio2_2 = 0x1.0b4611a600000p-34;
    constexpr double kPio2_3 = 0x1.3198a2e037073p-69;

    const std::int64_t k = detRoundToInt64(x * kInvPio2);
    const double kd = static_cast<double>(k);
    const double r = ((x - kd * kPio2_1) - kd * kPio2_2) - kd * kPio2_3;
    const double r2 = r * r;

    double ps = -1.0 / 1307674368000.0;
    ps = 1.0 / 6227020800.0 + r2 * ps;
    ps = -1.0 / 39916800.0 + r2 * ps;
    ps = 1.0 / 362880.0 + r2 * ps;
    ps = -1.0 / 5040.0 + r2 * ps;
    ps = 1.0 / 120.0 + r2 * ps;
    ps = -1.0 / 6.0 + r2 * ps;
    const double sv = r + r * r2 * ps;

    double pc = 1.0 / 20922789888000.0;
    pc = -1.0 / 87178291200.0 + r2 * pc;
    pc = 1.0 / 479001600.0 + r2 * pc;
    pc = -1.0 / 3628800.0 + r2 * pc;
    pc = 1.0 / 40320.0 + r2 * pc;
    pc = -1.0 / 720.0 + r2 * pc;
    pc = 1.0 / 24.0 + r2 * pc;
    pc = -0.5 + r2 * pc;
    const double cv = 1.0 + r2 * pc;

    switch (static_cast<std::uint64_t>(k) & 3u) {
    case 0u: return {sv, cv};
    case 1u: return {cv, -sv};
    case 2u: return {-sv, -cv};
    default: return {-cv, sv};
    }
}

inline double detSin(double x) noexcept { return detSinCos(x).s; }
inline double detCos(double x) noexcept { return detSinCos(x).c; }

// Exact integer 8-direction quantization matching FUN_10021400
// (0=+U, 1=+U+V, 2=+V, 3=-U+V, 4=-U, 5=-U-V, 6=-V, 7=+U-V).
inline std::uint8_t detDir8(int du, int dv) noexcept {
    if (du == 0 && dv == 0) return 0u;
    const std::int64_t sdu = du;
    const std::int64_t sdv = dv;
    const std::int64_t au = sdu < 0 ? -sdu : sdu;
    const std::int64_t av = sdv < 0 ? -sdv : sdv;
    if (av * av + 2 * au * av < au * au) return du > 0 ? 0u : 4u;
    if (au * au + 2 * au * av < av * av) return dv > 0 ? 2u : 6u;
    if (du > 0) return dv > 0 ? 1u : 7u;
    return dv > 0 ? 3u : 5u;
}

// Exact 32-direction sector quantization using fixed IEEE-754 tan(k * pi / 32)
// boundaries (avoids cross-libm std::atan2 drift).
inline unsigned detDir32(double du, double dv) noexcept {
    if (du == 0.0 && dv == 0.0) return 0u;
    constexpr double kTan1 = 0.09849140335716425;
    constexpr double kTan3 = 0.3033466836073424;
    constexpr double kTan5 = 0.5345111359507916;
    constexpr double kTan7 = 0.8206787908286602;

    const double au = du < 0.0 ? -du : du;
    const double av = dv < 0.0 ? -dv : dv;
    const bool swap = av > au;
    const double x = swap ? av : au;
    const double y = swap ? au : av;

    unsigned s = 4u;
    if (y < x * kTan1) s = 0u;
    else if (y < x * kTan3) s = 1u;
    else if (y < x * kTan5) s = 2u;
    else if (y < x * kTan7) s = 3u;
    if (swap) s = 8u - s;

    if (du < 0.0) {
        s = (dv < 0.0) ? (16u + s) : (16u - s);
    } else if (dv < 0.0) {
        s = 32u - s;
    }
    return s & 31u;
}

}  // namespace ss2::det
