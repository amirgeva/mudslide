#pragma once

// Deterministic value noise used for terrain shapes, rock strata and the
// distant hills of the backdrop.

#include <cmath>
#include <cstdint>

#include "core/rng.h"

namespace mud {

// Hash of a lattice point to [-1, 1).
inline float hashUnit(uint64_t seed, int64_t i) {
    uint64_t s = seed ^ (uint64_t(i) * 0xD1B54A32D192ED03ull);
    return float(splitmix64(s) >> 40) * (2.f / 16777216.f) - 1.f;
}

inline float smoothstep01(float t) { return t * t * (3.f - 2.f * t); }

// Smooth 1D value noise in [-1, 1].
inline float noise1(uint64_t seed, float x) {
    const float fx = std::floor(x);
    const int64_t i = int64_t(fx);
    const float t = smoothstep01(x - fx);
    const float a = hashUnit(seed, i), b = hashUnit(seed, i + 1);
    return a + (b - a) * t;
}

// Smooth 2D value noise in [-1, 1].
inline float noise2(uint64_t seed, float x, float y) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int64_t ix = int64_t(fx), iy = int64_t(fy);
    const float tx = smoothstep01(x - fx), ty = smoothstep01(y - fy);
    auto h = [seed](int64_t a, int64_t b) { return hashUnit(seed, a * 0x1F1F1F1Fll + b * 0x3C6EF372ll); };
    const float v00 = h(ix, iy), v10 = h(ix + 1, iy), v01 = h(ix, iy + 1), v11 = h(ix + 1, iy + 1);
    const float a = v00 + (v10 - v00) * tx;
    const float b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

}  // namespace mud
