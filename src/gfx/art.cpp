#include "gfx/art.h"

#include <cmath>
#include <vector>

#include "core/noise.h"
#include "core/rng.h"
#include "core/terrain.h"
#include "gfx/font.h"

namespace mud::art {
namespace {

constexpr float kPi = 3.14159265f;

struct Theme {
    const char* name;
    uint32_t skyTop, skyHorizon, sun;
    uint32_t far, near;  // distant hill ranges
    uint32_t top;        // grass / sand / snow / moss
    uint32_t strata[6];
    bool night;
};

const Theme kThemes[kThemeCount] = {
    {"GREEN VALLEY", 0x3C86D4, 0xC4E6F6, 0xFFF3C4, 0x8FB4C8, 0x6F9B86, 0x5BAA3A,
     {0x8C5B2F, 0x9E6D3C, 0x7B4C27, 0xAA7A4B, 0x6F4423, 0x94633A}, false},
    {"RED CANYON", 0x3B2C66, 0xF4A35C, 0xFFE0A8, 0xA05E6A, 0x7E4048, 0xE3B577,
     {0xC86A3A, 0xB1572F, 0xDA8B4F, 0x9D4B2B, 0xE2A262, 0xA95F37}, false},
    {"FROST RIDGE", 0x78ADDA, 0xE6F1F8, 0xFFFFF2, 0xBACEDF, 0x98B0C6, 0xF1F5FA,
     {0x77879B, 0x637387, 0x8B9BAD, 0x57667B, 0x9BA9B9, 0x6D7D91}, false},
    {"MOON BOG", 0x0B1532, 0x365384, 0xEEF0FF, 0x2C3D62, 0x1E2C46, 0x5E8E44,
     {0x7A5E42, 0x664E36, 0x8C6E4C, 0x58432E, 0x7E6444, 0x6A5238}, true},
};

const uint32_t kPlayerColors[4] = {0xE04848, 0x3D7BE0, 0xE8B830, 0xA463E0};

const Theme& themeOf(int theme) { return kThemes[((theme % kThemeCount) + kThemeCount) % kThemeCount]; }

RGBA scaled(RGBA c, float f) { return RGBA{clamp01(c.r * f), clamp01(c.g * f), clamp01(c.b * f), c.a}; }

uint32_t pack(RGBA c, float dither = 0.f) {
    auto ch = [dither](float v) { return uint32_t(std::lround(clamp01(v + dither) * 255.f)); };
    return 0xFF000000u | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

bool inCapsule(float x, float y, float ax, float ay, float bx, float by, float r) {
    const float vx = bx - ax, vy = by - ay;
    const float t = std::clamp(((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy), 0.f, 1.f);
    const float dx = x - (ax + vx * t), dy = y - (ay + vy * t);
    return dx * dx + dy * dy <= r * r;
}

}  // namespace

const char* themeName(int theme) { return themeOf(theme).name; }
bool themeIsNight(int theme) { return themeOf(theme).night; }

RGBA playerColor(int player) { return rgb(kPlayerColors[((player % 4) + 4) % 4]); }

// ---------------------------------------------------------------- world

std::array<uint32_t, 256> terrainPalette(int theme) {
    const Theme& t = themeOf(theme);
    std::array<uint32_t, 256> pal{};
    RGBA average{0, 0, 0, 1};
    for (int i = 0; i < 6; ++i) {
        const RGBA c = rgb(t.strata[i]);
        average.r += c.r / 6.f;
        average.g += c.g / 6.f;
        average.b += c.b / 6.f;
    }
    for (int b = 0; b < kStrataBands; ++b) {
        // Strata are kept close to the average so the bands read as subtle layers.
        RGBA base = mix(rgb(t.strata[b % 6]), average, 0.45f);
        if (b >= 6) base = scaled(base, 0.95f);
        for (int k = 0; k < kShades; ++k) pal[size_t(kStrataFirst + b * kShades + k)] = pack(scaled(base, 0.82f + 0.05f * float(k)));
    }
    // Loose earth is a blend of the strata, a touch darker (it is churned up).
    const RGBA looseBase = scaled(average, 0.86f);
    const RGBA top = rgb(t.top);
    const RGBA looseTop = mix(top, looseBase, 0.35f);
    const RGBA mud = rgb(0x5E3B1E);
    for (int k = 0; k < kShades; ++k) {
        const float f = float(k);
        pal[size_t(kTopFirst + k)] = pack(scaled(top, 0.84f + 0.045f * f));
        pal[size_t(kLooseFirst + k)] = pack(scaled(looseBase, 0.78f + 0.06f * f));
        pal[size_t(kLooseTopFirst + k)] = pack(scaled(looseTop, 0.80f + 0.05f * f));
        RGBA m = scaled(mud, 0.74f + 0.075f * f);
        if (k >= 6) m = mix(m, rgb(0xA88462), 0.25f * float(k - 5));  // wet sheen
        pal[size_t(kMudFirst + k)] = pack(m);
    }
    for (size_t i = 1; i < pal.size(); ++i)
        if (!pal[i]) pal[i] = pal[kStrataFirst];
    return pal;
}

void backdrop(uint32_t* px, int w, int h, int theme, uint64_t seed) {
    const Theme& t = themeOf(theme);
    Rng rng(seed ^ 0x5EEDBACC0FFEEull);
    const RGBA top = rgb(t.skyTop), hor = rgb(t.skyHorizon), sun = rgb(t.sun);
    const float fw = float(w), fh = float(h);
    const float sunX = fw * rng.uniform(0.12f, 0.88f);
    const float sunY = fh * (theme == 1 ? rng.uniform(0.28f, 0.36f) : rng.uniform(0.10f, 0.22f));
    const float sunR = fh * (t.night ? 0.032f : (theme == 1 ? 0.07f : 0.048f));
    const uint64_t ns = (uint64_t(rng.next()) << 32) | rng.next();

    std::vector<float> far(static_cast<size_t>(w)), near(static_cast<size_t>(w));
    for (int x = 0; x < w; ++x) {
        const float fx = float(x) * 1280.f / fw;
        float f = fh * (0.50f + 0.08f * noise1(ns, fx / 170.f) + 0.03f * noise1(ns + 1, fx / 55.f));
        float n = fh * (0.61f + 0.06f * noise1(ns + 2, fx / 120.f) + 0.02f * noise1(ns + 3, fx / 40.f));
        if (theme == 1) {  // mesas
            const float step = fh * 0.06f;
            auto terrace = [step](float v) {
                const float q = v / step, fr = q - std::floor(q);
                return (std::floor(q) + smoothstep01(smoothstep01(fr))) * step;
            };
            f = terrace(f);
            n = terrace(n);
        } else if (theme == 2) {  // jagged peaks
            const float s = 1.f - std::fabs(noise1(ns + 4, fx / 90.f));
            f -= fh * 0.14f * s * s * s;
            const float s2 = 1.f - std::fabs(noise1(ns + 5, fx / 70.f));
            n -= fh * 0.06f * s2 * s2;
        }
        far[size_t(x)] = f;
        near[size_t(x)] = n;
    }
    const RGBA farC = mix(rgb(t.far), hor, 0.2f), nearC = rgb(t.near);

    for (int y = 0; y < h; ++y) {
        const float fy = float(y);
        const float ty = clamp01(fy / (fh * 0.72f));
        const RGBA sky = mix(top, hor, std::pow(ty, 1.15f));
        for (int x = 0; x < w; ++x) {
            const float fx = float(x);
            RGBA c = sky;
            if (t.night && fy < far[size_t(x)]) {
                uint64_t s = (uint64_t(y) * 1315423911ull + uint64_t(x) * 2654435761ull) ^ seed;
                const uint64_t hsh = splitmix64(s);
                if ((hsh & 1023u) < 2u) c = mix(c, rgb(0xFFFFFF), (0.35f + float((hsh >> 10) & 255u) / 400.f) * (1.f - ty));
            }
            const float dx = fx - sunX, dy = fy - sunY;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float glow = (t.night ? 0.10f : 0.30f) * std::exp(-d / (sunR * 2.5f));
            c.r = clamp01(c.r + sun.r * glow);
            c.g = clamp01(c.g + sun.g * glow);
            c.b = clamp01(c.b + sun.b * glow);
            const float disk = clamp01(sunR - d + 0.5f);
            if (disk > 0.f) {
                RGBA s = sun;
                if (t.night) s = scaled(s, 0.92f + 0.08f * noise2(ns + 6, fx / 9.f, fy / 9.f));  // moon blotches
                c = mix(c, s, disk);
            }
            const float hf = far[size_t(x)];
            const float cf = clamp01(fy + 1.f - hf);
            if (cf > 0.f) {
                const float depth = clamp01((fy - hf) / (fh * 0.3f));
                c = mix(c, mix(farC, scaled(farC, 0.9f), depth), cf);
            }
            const float hn = near[size_t(x)];
            const float cn = clamp01(fy + 1.f - hn);
            if (cn > 0.f) {
                const float depth = clamp01((fy - hn) / (fh * 0.3f));
                c = mix(c, mix(nearC, scaled(nearC, 0.8f), depth), cn);
            }
            const float dither = (float(((uint32_t(x) * 73u + uint32_t(y) * 151u) * 2654435761u) >> 24) / 255.f - 0.5f) / 255.f;
            px[size_t(y) * size_t(w) + size_t(x)] = pack(c, dither);
        }
    }
}

// ---------------------------------------------------------------- sprites

void tankBody(const Pen& p, RGBA color) {
    const RGBA dark = mix(color, rgb(0x000000), 0.45f);
    const RGBA light = mix(color, rgb(0xFFFFFF), 0.35f);
    // Treads and road wheels.
    p.roundRect(1.f, 9.f, 25.f, 14.f, 2.5f, solid(rgb(0x202020)));
    p.roundRect(1.8f, 9.6f, 24.2f, 13.3f, 2.f, solid(rgb(0x3A3A3A)));
    for (int i = 0; i < 5; ++i) {
        const float cx = 4.6f + float(i) * 4.2f;
        p.circle(cx, 11.5f, 1.7f, solid(rgb(0x7C7C7C)));
        p.circle(cx, 11.5f, 0.6f, solid(rgb(0x2A2A2A)));
    }
    // Hull: a trapezoid, wider at the bottom.
    auto hull = [](float x, float y) {
        if (y < 5.4f || y > 9.9f) return false;
        const float t = (y - 5.4f) / 4.5f;
        return x >= 6.f - 4.5f * t && x <= 20.f + 4.5f * t;
    };
    p.fill(1.f, 5.f, 25.f, 10.f, hull, VGrad{5.4f, 9.9f, light, dark});
    // Dome turret.
    p.fill(7.f, 0.5f, 19.f, 6.f, [](float x, float y) { return y <= 5.8f && inEllipse(x, y, 13.f, 5.8f, 5.6f, 4.9f); },
           Radial{11.2f, 2.4f, 6.5f, light, color});
    p.rect(6.2f, 5.4f, 19.8f, 6.1f, solid(rgb(0xFFFFFF, 0.28f)));
}

void barrel(const Pen& p, RGBA color) {
    p.roundRect(0.f, 0.7f, 15.5f, 3.3f, 1.2f, VGrad{0.7f, 3.3f, rgb(0x9CA1A8), rgb(0x3A3D42)});
    p.rect(11.6f, 0.7f, 13.2f, 3.3f, solid(mix(color, rgb(0x000000), 0.15f)));
}

void fireball(const Pen& p) {
    p.circle(0.5f, 0.5f, 0.5f, [](float x, float y) {
        const float d = std::min(1.f, std::hypot(x - 0.5f, y - 0.5f) / 0.5f);
        RGBA c = d < 0.3f ? mix(rgb(0xFFFBE0), rgb(0xFFC850), d / 0.3f) : mix(rgb(0xFFC850), rgb(0xE04A10), (d - 0.3f) / 0.7f);
        c.a = std::pow(1.f - d, 1.2f);
        return c;
    });
}

void dot(const Pen& p) {
    p.circle(0.5f, 0.5f, 0.5f, [](float x, float y) {
        const float d = std::min(1.f, std::hypot(x - 0.5f, y - 0.5f) / 0.5f);
        return RGBA{1.f, 1.f, 1.f, (1.f - d) * (1.f - d)};
    });
}

void ball(const Pen& p) {
    p.circle(0.5f, 0.5f, 0.46f, Radial{0.36f, 0.34f, 0.62f, rgb(0xFFFFFF), rgb(0x8C8C8C)});
}

void cloud(const Pen& p, int variant) {
    Rng rng(uint64_t(variant) * 977u + 13u);
    const int puffs = 6 + variant;
    p.roundRect(0.14f, 0.27f, 0.86f, 0.40f, 0.06f, solid(rgb(0xDCE3EC, 0.92f)));
    for (int i = 0; i < puffs; ++i) {
        const float u = float(i) / float(puffs - 1);
        const float cx = 0.2f + 0.6f * u + rng.uniform(-0.03f, 0.03f);
        const float bulge = std::sin(u * kPi);
        const float r = 0.08f + 0.09f * bulge + rng.uniform(0.f, 0.02f);
        const float cy = 0.36f - r * 0.75f + rng.uniform(-0.01f, 0.01f);
        p.circle(cx, cy, r, Radial{cx - r * 0.35f, cy - r * 0.45f, r * 1.5f, rgb(0xFFFFFF, 0.97f), rgb(0xD6DEE8, 0.92f)});
    }
}

// ---------------------------------------------------------------- title art

void logo(const Pen& p) {
    const char* text = "MUDSLIDE";
    const int letters = 8;
    auto lit = [&](int gx, int gy) {
        if (gy < 0 || gy >= font::kGlyphH || gx < 0) return false;
        const int ch = gx / font::kAdvance, cx = gx % font::kAdvance;
        if (ch >= letters || cx >= font::kGlyphW) return false;
        const char* const* rows = font::glyph(text[ch]);
        return rows && rows[gy][cx] == '#';
    };
    const int cols = letters * font::kAdvance - 1;
    const float ox = 1.f, oy = 1.f;
    auto dripLen = [](int gx) { return ((gx * 37 + 11) % 5) == 0 ? 0.9f + float((gx * 13) % 7) * 0.33f : 0.f; };
    const RGBA outline = rgb(0x24140A), hi = rgb(0xC08A58), lo = rgb(0x5A3418);

    // Lit font pixels in a grid with a one-cell empty border.
    constexpr int GW = kLogoW, GH = font::kGlyphH + 2;
    bool grid[GH][GW] = {};
    for (int gy = 0; gy < font::kGlyphH; ++gy)
        for (int gx = 0; gx < cols; ++gx) grid[gy + 1][gx + 1] = lit(gx, gy);
    auto on = [&](int cx, int cy) { return cx >= 0 && cy >= 0 && cx < GW && cy < GH && grid[cy][cx]; };

    // Outline: everything within `pad` of a lit cell (rounds itself).
    const float pad = 0.26f;
    auto nearLit = [&](float x, float y) {
        const int cx = int(std::floor(x)), cy = int(std::floor(y));
        for (int j = cy - 1; j <= cy + 1; ++j)
            for (int i = cx - 1; i <= cx + 1; ++i) {
                if (!on(i, j)) continue;
                const float dx = std::max({float(i) - x, 0.f, x - float(i + 1)});
                const float dy = std::max({float(j) - y, 0.f, y - float(j + 1)});
                if (dx * dx + dy * dy <= pad * pad) return true;
            }
        return false;
    };
    // Body: the union of lit cells, with outward corners rounded.
    const float rc = 0.26f;
    auto inBody = [&](float x, float y) {
        const int cx = int(std::floor(x)), cy = int(std::floor(y));
        if (!on(cx, cy)) return false;
        const float fx = x - float(cx), fy = y - float(cy);
        const bool l = on(cx - 1, cy), r = on(cx + 1, cy), u = on(cx, cy - 1), d = on(cx, cy + 1);
        auto cut = [&](float px, float py) { return (fx - px) * (fx - px) + (fy - py) * (fy - py) > rc * rc; };
        if (!l && !u && fx < rc && fy < rc && cut(rc, rc)) return false;
        if (!r && !u && fx > 1.f - rc && fy < rc && cut(1.f - rc, rc)) return false;
        if (!l && !d && fx < rc && fy > 1.f - rc && cut(rc, 1.f - rc)) return false;
        if (!r && !d && fx > 1.f - rc && fy > 1.f - rc && cut(1.f - rc, 1.f - rc)) return false;
        return true;
    };
    const Pen g = p.sub(ox - 1.f, oy - 1.f, 1.f);  // grid coordinates
    g.fill(0.f, 0.f, float(GW), float(GH), nearLit, solid(outline));
    for (int gx = 0; gx < cols; ++gx) {
        const float len = dripLen(gx);
        if (len <= 0.f || !lit(gx, font::kGlyphH - 1)) continue;
        const float x = ox + float(gx), y = oy + float(font::kGlyphH);
        p.roundRect(x + 0.16f, y - 0.5f, x + 0.84f, y + len, 0.32f, solid(outline));
        p.circle(x + 0.5f, y + len, 0.48f, solid(outline));
    }
    g.fill(0.f, 0.f, float(GW), float(GH), inBody, VGrad{1.f, 1.f + float(font::kGlyphH), hi, lo});
    for (int gx = 0; gx < cols; ++gx) {
        const float len = dripLen(gx);
        if (len <= 0.f || !lit(gx, font::kGlyphH - 1)) continue;
        const float x = ox + float(gx), y = oy + float(font::kGlyphH);
        p.roundRect(x + 0.3f, y - 0.6f, x + 0.7f, y + len, 0.2f, solid(lo));
        p.circle(x + 0.5f, y + len, 0.32f, solid(lo));
        p.circle(x + 0.42f, y + len - 0.1f, 0.1f, solid(rgb(0xE8C8A0, 0.6f)));
    }
    // A glossy highlight along each run of top edges.
    for (int gy = 0; gy < font::kGlyphH; ++gy)
        for (int gx = 0; gx < cols;) {
            if (!lit(gx, gy) || lit(gx, gy - 1)) {
                ++gx;
                continue;
            }
            int end = gx;
            while (end + 1 < cols && lit(end + 1, gy) && !lit(end + 1, gy - 1)) ++end;
            p.roundRect(ox + float(gx) + 0.28f, oy + float(gy) + 0.16f, ox + float(end) + 0.72f, oy + float(gy) + 0.36f, 0.1f,
                        solid(rgb(0xF6DCB4, 0.6f)));
            gx = end + 1;
        }
}

void icon(const Pen& p, bool background) {
    auto inside = [background](float x, float y) { return !background || inRoundRect(x, y, 0.f, 0.f, 1.f, 1.f, 0.2f); };
    if (background) p.roundRect(0.f, 0.f, 1.f, 1.f, 0.2f, VGrad{0.f, 1.f, rgb(0x3C86D4), rgb(0xC4E6F6)});
    auto ground = [](float x) { return 0.70f - 0.10f * std::sin((x + 0.1f) * 3.2f); };
    p.fill(0.f, 0.4f, 1.f, 1.f, [&](float x, float y) { return y >= ground(x) && inside(x, y); },
           VGrad{0.55f, 1.f, rgb(0x9E6D3C), rgb(0x4E2C14)});
    p.fill(0.f, 0.4f, 1.f, 1.f, [&](float x, float y) { return y >= ground(x) && y < ground(x) + 0.05f && inside(x, y); },
           solid(rgb(0x5BAA3A)));
    // Tank on the left hill, barrel raised to the right.
    const float tx = 0.27f, ty = ground(tx) + 0.01f;
    const float s = 0.40f / 26.f;
    const float px = tx, py = ty - 9.f * s;
    const float a = 50.f * kPi / 180.f;
    p.fill(px - 0.02f, py - 0.25f, px + 0.25f, py + 0.02f,
           [&](float x, float y) { return inCapsule(x, y, px, py, px + std::cos(a) * 15.f * s, py - std::sin(a) * 15.f * s, 1.4f * s); },
           solid(rgb(0x4A4E54)));
    tankBody(p.sub(tx - 13.f * s, ty - 14.f * s, s), playerColor(0));
    // Dotted trajectory to a splash of mud on the right.
    const float ex = 0.86f, ey = ground(ex) - 0.01f;
    const float sx = px + std::cos(a) * 15.f * s, sy = py - std::sin(a) * 15.f * s;
    for (int i = 1; i <= 6; ++i) {
        const float t = float(i) / 7.f;
        const float x = sx + (ex - sx) * t;
        const float y = sy + (ey - sy) * t - 0.55f * t * (1.f - t);
        p.circle(x, y, 0.022f, solid(rgb(0xFFFFFF, 0.9f)));
    }
    p.circle(ex, ey, 0.07f, solid(rgb(0x5E3B1E)));
    p.circle(ex - 0.07f, ey - 0.07f, 0.03f, solid(rgb(0x6E4626)));
    p.circle(ex + 0.06f, ey - 0.09f, 0.025f, solid(rgb(0x6E4626)));
    p.circle(ex + 0.01f, ey - 0.13f, 0.02f, solid(rgb(0x6E4626)));
}

}  // namespace mud::art
