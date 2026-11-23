#include "core/terrain.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/noise.h"
#include "core/rng.h"

namespace mud {
namespace {

constexpr int W = kWorldW;
constexpr int H = kWorldH;
constexpr float kTwoPi = 6.28318530718f;

uint32_t nextGeneration() {
    static uint32_t counter = 0;
    return ++counter;
}

}  // namespace

Terrain::Terrain()
    : cells_(size_t(W) * size_t(H), kAir), wake_(size_t(W), 0), nextWake_(size_t(W), 0), generation_(nextGeneration()) {}

// ---------------------------------------------------------------- generation

void Terrain::generate(uint64_t seed, int theme) {
    Rng rng(seed);
    seed_ = seed;
    theme_ = theme;
    generation_ = nextGeneration();
    noiseSeed_ = (uint64_t(rng.next()) << 32) | rng.next();
    bandPhase_ = rng.uniform(0.f, kTwoPi);

    const float fh = float(H);
    struct Wave {
        float amp, freq, phase;
    };
    const float base = fh * rng.uniform(0.56f, 0.66f);
    const Wave waves[3] = {
        {fh * rng.uniform(0.07f, 0.14f), rng.uniform(0.7f, 1.6f), rng.uniform(0.f, kTwoPi)},
        {fh * rng.uniform(0.03f, 0.07f), rng.uniform(2.0f, 3.8f), rng.uniform(0.f, kTwoPi)},
        {fh * rng.uniform(0.008f, 0.02f), rng.uniform(6.f, 11.f), rng.uniform(0.f, kTwoPi)},
    };
    const float ridgeFreq = rng.uniform(2.5f, 4.5f), ridgePhase = rng.uniform(0.f, kTwoPi);

    std::vector<float> h(static_cast<size_t>(W));
    for (int x = 0; x < W; ++x) {
        const float u = float(x) / float(W);
        float v = base;
        for (const Wave& w : waves) v += w.amp * std::sin(u * w.freq * kTwoPi + w.phase);
        const float nx = u * 1280.f;  // noise scale independent of the world size
        v += fh * 0.035f * (noise1(noiseSeed_, nx / 90.f) * 0.7f + noise1(noiseSeed_ + 1, nx / 30.f) * 0.3f);
        switch (theme) {
            case 1: {  // canyon: stepped mesas with steep cliffs
                const float step = fh * 0.075f;
                const float t = v / step;
                const float f = t - std::floor(t);
                v = (std::floor(t) + smoothstep01(smoothstep01(f))) * step;
                break;
            }
            case 2: {  // frost: sharp ridges
                const float s = 1.f - std::fabs(std::sin(u * ridgeFreq * kTwoPi * 0.5f + ridgePhase));
                v -= fh * 0.10f * s * s;
                break;
            }
            case 3:  // bog: lower, softer ground
                v = v * 0.8f + fh * 0.16f;
                break;
            default:
                break;
        }
        h[size_t(x)] = std::clamp(v, fh * 0.26f, fh * 0.9f);
    }

    std::fill(cells_.begin(), cells_.end(), kAir);
    for (int x = 0; x < W; ++x) {
        const int top = int(std::ceil(h[size_t(x)]));
        const int topDepth = 4 + int(2.5f * (noise1(noiseSeed_ + 2, float(x) / 18.f) + 1.f));
        for (int y = top; y < H; ++y)
            cells_[size_t(y) * W + size_t(x)] = (y - top) < topDepth ? topColor(x, y) : earthColor(x, y);
    }

    std::fill(wake_.begin(), wake_.end(), uint8_t(0));
    std::fill(nextWake_.begin(), nextWake_.end(), uint8_t(0));
    wakeLo_ = 1;
    wakeHi_ = 0;
    dirty_ = IRect{0, 0, W - 1, H - 1};
}

uint8_t Terrain::earthColor(int x, int y) const {
    const float fx = float(x), fy = float(y);
    // Gently waving horizontal rock strata.
    const float by = fy + 12.f * std::sin(fx * 0.0105f + bandPhase_) + 10.f * noise1(noiseSeed_ + 3, fx / 55.f);
    int band = int(std::floor(by / 24.f)) % kStrataBands;
    if (band < 0) band += kStrataBands;
    const float n = noise2(noiseSeed_ + 7, fx / 7.f, fy / 5.f) * 2.4f + noise2(noiseSeed_ + 9, fx / 2.3f, fy / 2.3f) * 1.2f;
    const int shade = std::clamp(int(std::lround(3.5f + n)), 0, kShades - 1);
    return uint8_t(kStrataFirst + band * kShades + shade);
}

uint8_t Terrain::topColor(int x, int y) const {
    const float n = noise2(noiseSeed_ + 11, float(x) / 3.f, float(y) / 2.f) * 2.5f;
    return uint8_t(kTopFirst + std::clamp(int(std::lround(3.5f + n)), 0, kShades - 1));
}

uint8_t Terrain::mudColor(int x, int y) const {
    const float n = noise2(noiseSeed_ + 13, float(x) / 4.f, float(y) / 4.f) * 2.6f +
                    hashUnit(noiseSeed_ + 17, int64_t(y) * W + x) * 0.9f;
    return uint8_t(kMudFirst + std::clamp(int(std::lround(3.5f + n)), 0, kShades - 1));
}

int Terrain::surface(int x) const {
    if (x < 0 || x >= W) return H;
    for (int y = 0; y < H; ++y)
        if (cells_[size_t(y) * W + size_t(x)]) return y;
    return H;
}

// ---------------------------------------------------------------- editing

void Terrain::wake(int x0, int x1) {
    x0 = std::max(0, x0);
    x1 = std::min(W - 1, x1);
    if (x1 < x0) return;
    for (int x = x0; x <= x1; ++x) wake_[size_t(x)] = 1;
    if (!settling()) {
        wakeLo_ = x0;
        wakeHi_ = x1;
    } else {
        wakeLo_ = std::min(wakeLo_, x0);
        wakeHi_ = std::max(wakeHi_, x1);
    }
}

int Terrain::carve(float cx, float cy, float r, std::vector<Grain>* removed) {
    const int x0 = std::max(0, int(std::floor(cx - r))), x1 = std::min(W - 1, int(std::ceil(cx + r)));
    const int y0 = std::max(0, int(std::floor(cy - r))), y1 = std::min(H - 1, int(std::ceil(cy + r)));
    const float r2 = r * r;
    int count = 0;
    for (int y = y0; y <= y1; ++y) {
        const float dy = float(y) + 0.5f - cy;
        for (int x = x0; x <= x1; ++x) {
            const float dx = float(x) + 0.5f - cx;
            const uint8_t v = cells_[size_t(y) * W + size_t(x)];
            if (dx * dx + dy * dy > r2 || !v) continue;
            if (removed) removed->push_back(Grain{int16_t(x), int16_t(y), v});
            put(x, y, kAir);
            ++count;
        }
    }
    if (x1 >= x0) wake(x0 - 2, x1 + 2);
    return count;
}

int Terrain::deposit(float x, float y, uint8_t color, int cells) {
    // Fill the air cells nearest the landing point (lower rows first among
    // equals, so the clump sits on the ground) within a radius of 9, which
    // holds even the biggest clod; settling then spreads the pile.
    static const std::vector<std::pair<int8_t, int8_t>> kOffsets = [] {
        std::vector<std::pair<int8_t, int8_t>> o;
        for (int dy = -9; dy <= 2; ++dy)
            for (int dx = -9; dx <= 9; ++dx)
                if (dx * dx + dy * dy <= 81) o.push_back({int8_t(dx), int8_t(dy)});
        std::stable_sort(o.begin(), o.end(), [](const auto& a, const auto& b) {
            const int da = a.first * a.first + a.second * a.second, db = b.first * b.first + b.second * b.second;
            return da != db ? da < db : a.second > b.second;
        });
        return o;
    }();
    const int bx = int(std::floor(x)), by = int(std::floor(y));
    int placed = 0;
    for (const auto& o : kOffsets) {
        if (placed >= cells) break;
        const int px = bx + o.first, py = by + o.second;
        if (px < 0 || px >= W || py < 0 || py >= H) continue;
        if (cells_[size_t(py) * W + size_t(px)]) continue;
        put(px, py, color);
        ++placed;
    }
    if (placed) wake(bx - 11, bx + 11);
    return placed;
}

void Terrain::carveLine(float x0, float y0, float x1, float y1, float r) {
    const float len = std::hypot(x1 - x0, y1 - y0);
    const int steps = std::max(1, int(std::ceil(len)));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        carve(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r);
    }
}

int Terrain::addMud(float cx, float cy, float r) {
    const int x0 = std::max(0, int(std::floor(cx - r))), x1 = std::min(W - 1, int(std::ceil(cx + r)));
    const int y0 = std::max(0, int(std::floor(cy - r))), y1 = std::min(H - 1, int(std::ceil(cy + r)));
    const float r2 = r * r;
    int added = 0;
    for (int y = y0; y <= y1; ++y) {
        const float dy = float(y) + 0.5f - cy;
        for (int x = x0; x <= x1; ++x) {
            const float dx = float(x) + 0.5f - cx;
            if (dx * dx + dy * dy <= r2 && !cells_[size_t(y) * W + size_t(x)]) {
                put(x, y, mudColor(x, y));
                ++added;
            }
        }
    }
    wake(x0 - 2, x1 + 2);
    return added;
}

void Terrain::flattenPad(int x0, int x1, int y) {
    x0 = std::max(0, x0);
    x1 = std::min(W - 1, x1);
    y = std::clamp(y, 1, H - 1);
    for (int x = x0; x <= x1; ++x) {
        for (int yy = 0; yy < y; ++yy)
            if (cells_[size_t(yy) * W + size_t(x)]) put(x, yy, kAir);
        for (int yy = y; yy < H; ++yy) {
            if (yy < y + 4) put(x, yy, topColor(x, yy));
            else if (!cells_[size_t(yy) * W + size_t(x)]) put(x, yy, earthColor(x, yy));
        }
    }
}

// ---------------------------------------------------------------- settling

bool Terrain::settleStep() {
    if (!settling()) return false;
    ++pass_;
    int lo = W, hi = -1;
    auto mark = [&](int x) {
        if (x < 0 || x >= W) return;
        nextWake_[size_t(x)] = 1;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    };
    uint8_t* c = cells_.data();
    bool any = false;
    for (int x = wakeLo_; x <= wakeHi_; ++x) {
        if (!wake_[size_t(x)]) continue;
        wake_[size_t(x)] = 0;
        bool moved = false;
        // Bottom-up, so a whole hanging column drops one pixel per pass.
        for (int y = H - 2; y >= 0; --y) {
            const size_t i = size_t(y) * W + size_t(x);
            const uint8_t v = c[i];
            if (!v) continue;
            if (!c[i + W]) {
                c[i + W] = v;
                c[i] = kAir;
                dirty_.add(x, y);
                dirty_.add(x, y + 1);
                moved = true;
                continue;
            }
            const bool mud = isMud(v);
            if (!mud && !isLoose(v)) continue;
            // Loose material resting on something slides diagonally if there
            // is room: loose earth one pixel sideways (45 degrees), wet mud
            // up to two (an angle of repose of ~27 degrees).
            const int first = ((uint32_t(x) * 7u + uint32_t(y) * 13u + pass_) & 1u) ? 1 : -1;
            for (int k = 0; k < 2; ++k) {
                const int d = k ? -first : first;
                const int nx = x + d;
                if (nx < 0 || nx >= W) continue;
                const size_t side = size_t(y) * W + size_t(nx);
                if (c[side]) continue;
                int dest = -1;
                if (!c[side + W]) {
                    dest = nx;
                } else if (mud) {
                    const int nx2 = x + 2 * d;
                    if (nx2 >= 0 && nx2 < W) {
                        const size_t side2 = size_t(y) * W + size_t(nx2);
                        if (!c[side2] && !c[side2 + W]) dest = nx2;
                    }
                }
                if (dest < 0) continue;
                c[size_t(y + 1) * W + size_t(dest)] = v;
                c[i] = kAir;
                dirty_.add(x, y);
                dirty_.add(dest, y + 1);
                for (int k2 = -2; k2 <= 2; ++k2) mark(dest + k2);
                moved = true;
                break;
            }
        }
        // Mud reaches two columns sideways, so a change here can free grains
        // up to two columns away.
        if (moved) {
            for (int k2 = -2; k2 <= 2; ++k2) mark(x + k2);
            any = true;
        }
    }
    wake_.swap(nextWake_);
    if (hi >= lo) {
        wakeLo_ = lo;
        wakeHi_ = hi;
    } else {
        wakeLo_ = 1;
        wakeHi_ = 0;
    }
    return any;
}

int Terrain::settleAll(int maxSteps) {
    int steps = 0;
    while (settling() && steps < maxSteps) {
        settleStep();
        ++steps;
    }
    return steps;
}

long Terrain::countSolid() const {
    long n = 0;
    for (uint8_t v : cells_) n += v ? 1 : 0;
    return n;
}

IRect Terrain::takeDirty() {
    const IRect r = dirty_;
    dirty_ = IRect{};
    return r;
}

}  // namespace mud
