#pragma once

// Built-in 5x7 pixel font (upper case, digits, punctuation, arrows), so the
// game needs no font files or SDL_ttf.
namespace mud::font {

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kAdvance = 6;

// Arrow glyphs, usable inside string literals: "MOVE " MUD_ARROW_UP
#define MUD_ARROW_UP "\001"
#define MUD_ARROW_DOWN "\002"
#define MUD_ARROW_LEFT "\003"
#define MUD_ARROW_RIGHT "\004"
#define MUD_DEGREE "\005"

// Seven rows of five characters ('#' = lit) or nullptr if the glyph is blank
// or unknown. Lower case letters map to upper case.
const char* const* glyph(char c);

}  // namespace mud::font
