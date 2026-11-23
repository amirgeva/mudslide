#pragma once

// Destructible pixel terrain. Every cell is either air or a coloured grain of
// earth. Explosions carve holes; anything left hanging then falls straight
// down, and wet mud also slumps sideways down slopes like a landslide.

#include <cstdint>
#include <vector>

namespace mud {

constexpr int kWorldW = 960;
constexpr int kWorldH = 540;

// Cell values. 0 is air; anything else is solid and doubles as an index into
// the terrain palette of the round's theme (gfx/art.h).
constexpr uint8_t kAir = 0;
constexpr int kShades = 8;
constexpr int kStrataBands = 12;
constexpr uint8_t kStrataFirst = 1;  // 12 bands x 8 shades: 1..96
constexpr uint8_t kTopFirst = 104;   // topsoil (grass, sand, snow, moss): 104..111
constexpr uint8_t kMudFirst = 128;   // wet mud: 128..135, slides down anything steeper than ~27 deg
constexpr uint8_t kMudLast = kMudFirst + kShades - 1;
constexpr uint8_t kLooseFirst = 144;     // loose earth thrown out by blasts: 144..151, slides at 45 deg
constexpr uint8_t kLooseTopFirst = 152;  // loose topsoil: 152..159
constexpr uint8_t kLooseLast = kLooseTopFirst + kShades - 1;

inline bool isMud(uint8_t c) { return c >= kMudFirst && c <= kMudLast; }
inline bool isLoose(uint8_t c) { return c >= kLooseFirst && c <= kLooseLast; }
inline bool isTop(uint8_t c) { return c >= kTopFirst && c < kTopFirst + kShades; }

// The loose material a cell turns into when it is blasted into the air.
inline uint8_t looseOf(uint8_t c) {
    if (isMud(c) || isLoose(c)) return c;
    if (isTop(c)) return uint8_t(kLooseTopFirst + (c - kTopFirst));
    return uint8_t(kLooseFirst + (c - kStrataFirst) % kShades);
}

// One cell removed by carve().
struct Grain {
    int16_t x, y;
    uint8_t color;
};

// Inclusive pixel rectangle; empty when x1 < x0.
struct IRect {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

    bool empty() const { return x1 < x0 || y1 < y0; }
    void add(int x, int y) {
        if (empty()) {
            x0 = x1 = x;
            y0 = y1 = y;
            return;
        }
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    void add(const IRect& r) {
        if (r.empty()) return;
        add(r.x0, r.y0);
        add(r.x1, r.y1);
    }
};

class Terrain {
public:
    Terrain();

    // Fresh hills for one of the visual themes (0..3). Deterministic per seed.
    void generate(uint64_t seed, int theme);

    // Outside the world there is air, except below the bottom edge, which is
    // solid bedrock.
    uint8_t at(int x, int y) const {
        if (y >= kWorldH) return kStrataFirst;
        if (x < 0 || x >= kWorldW || y < 0) return kAir;
        return cells_[size_t(y) * kWorldW + size_t(x)];
    }
    bool solid(int x, int y) const { return at(x, y) != kAir; }
    int surface(int x) const;  // first solid row of column x (kWorldH if none)

    // Explosion crater; returns the number of cells removed, and lists them
    // in `removed` when given (for the dirt thrown into the air).
    int carve(float cx, float cy, float r, std::vector<Grain>* removed = nullptr);
    void carveLine(float x0, float y0, float x1, float y1, float r);  // a bored tunnel
    int addMud(float cx, float cy, float r);                          // fills air in a disc; returns cells added
    // Drops `cells` grains of `color` into the air pockets nearest (x, y),
    // e.g. where a flying clod of dirt lands. Returns the number placed.
    int deposit(float x, float y, uint8_t color, int cells);
    void flattenPad(int x0, int x1, int y);  // level ground at row y for columns [x0, x1]

    // Settling: after a change, unsupported grains fall one pixel per step.
    // Loose earth also slides down slopes steeper than 45 degrees and wet mud
    // down anything steeper than about 27 degrees.
    bool settling() const { return wakeHi_ >= wakeLo_; }
    bool settleStep();  // returns true if anything moved
    int settleAll(int maxSteps = 5000);

    long countSolid() const;
    const uint8_t* cells() const { return cells_.data(); }

    // Area changed since the last call (for the renderer).
    IRect takeDirty();
    // Unique per generate() across all terrains, so renderers can tell maps apart.
    uint32_t generation() const { return generation_; }
    uint64_t seed() const { return seed_; }
    int theme() const { return theme_; }

private:
    uint8_t earthColor(int x, int y) const;
    uint8_t topColor(int x, int y) const;
    uint8_t mudColor(int x, int y) const;
    void put(int x, int y, uint8_t v) {
        cells_[size_t(y) * kWorldW + size_t(x)] = v;
        dirty_.add(x, y);
    }
    void wake(int x0, int x1);

    std::vector<uint8_t> cells_;
    std::vector<uint8_t> wake_, nextWake_;  // per column: needs another settle pass
    int wakeLo_ = 1, wakeHi_ = 0;
    uint32_t pass_ = 0;
    IRect dirty_;
    uint32_t generation_ = 0;
    uint64_t seed_ = 0;
    int theme_ = 0;
    float bandPhase_ = 0.f;
    uint64_t noiseSeed_ = 0;
};

}  // namespace mud
