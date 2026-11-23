#pragma once

#include <array>
#include <cstdint>

#include "gfx/canvas.h"

// Procedural art. Nothing here touches SDL; everything is drawn into a
// Canvas (through a Pen) or straight into an ARGB pixel buffer.
namespace mud::art {

constexpr int kThemeCount = 4;
const char* themeName(int theme);
bool themeIsNight(int theme);

// ARGB colour of every terrain cell value (0 = transparent air).
std::array<uint32_t, 256> terrainPalette(int theme);

// Sky, sun or moon, stars and two ranges of distant hills: w*h opaque ARGB.
void backdrop(uint32_t* px, int w, int h, int theme, uint64_t seed);

RGBA playerColor(int player);

// Pens below use world pixels as units unless noted.
void tankBody(const Pen& p, RGBA color);  // 26 x 14 box; treads rest on y = 14, turret pivot at (13, 5)
void barrel(const Pen& p, RGBA color);    // 16 x 4 box pointing right; pivot at (0, 2)
void fireball(const Pen& p);              // unit square: white-hot core fading to red
void dot(const Pen& p);                   // unit square: soft white puff
void ball(const Pen& p);                  // unit square: shaded white ball (tinted per use)
void cloud(const Pen& p, int variant);    // unit width, half a unit tall

// The "MUDSLIDE" title in dripping mud letters. Units are font pixels; the
// logo fills kLogoW x kLogoH of them.
constexpr int kLogoW = 49;
constexpr int kLogoH = 12;
void logo(const Pen& p);

void icon(const Pen& p, bool background);  // unit square

}  // namespace mud::art
