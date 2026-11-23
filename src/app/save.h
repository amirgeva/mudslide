#pragma once

#include <array>

#include "core/match.h"

namespace mud {

constexpr int kRoundChoices = 4;
constexpr int kRoundOptions[kRoundChoices] = {1, 3, 5, 10};

// Match setup chosen in the menu, kept between sessions in the per-user
// preferences folder that SDL picks for each platform (e.g.
// %APPDATA%\Mudslide\Mudslide on Windows, ~/.local/share/Mudslide/Mudslide on
// Linux, ~/Library/Application Support/Mudslide/Mudslide on macOS).
struct Settings {
    int players = 2;
    std::array<int, kMaxPlayers> control{{int(Control::Human), int(Control::CpuNormal), int(Control::CpuNormal),
                                          int(Control::CpuHard)}};
    int roundChoice = 1;  // index into kRoundOptions
    int wind = 2;         // 0 off .. 3 gale
    bool muted = false;
};

Settings loadSettings();
void storeSettings(const Settings& s);

}  // namespace mud
