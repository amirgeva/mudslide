#include "app/save.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mud {
namespace {

std::string settingsPath() {
    char* base = SDL_GetPrefPath("Mudslide", "Mudslide");
    if (!base) return {};
    std::string path = std::string(base) + "settings.ini";
    SDL_free(base);
    return path;
}

}  // namespace

Settings loadSettings() {
    Settings s;
    const std::string path = settingsPath();
    if (path.empty()) return s;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return s;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const int value = std::atoi(eq + 1);
        if (!std::strcmp(line, "players")) s.players = value;
        else if (!std::strcmp(line, "rounds")) s.roundChoice = value;
        else if (!std::strcmp(line, "wind")) s.wind = value;
        else if (!std::strcmp(line, "muted")) s.muted = value != 0;
        else if (!std::strncmp(line, "control", 7)) {
            const int i = std::atoi(line + 7);
            if (i >= 0 && i < kMaxPlayers) s.control[size_t(i)] = value;
        }
    }
    std::fclose(f);
    s.players = std::clamp(s.players, 2, kMaxPlayers);
    s.roundChoice = std::clamp(s.roundChoice, 0, kRoundChoices - 1);
    s.wind = std::clamp(s.wind, 0, kWindLevels - 1);
    for (int& c : s.control) c = std::clamp(c, 0, kControlCount - 1);
    return s;
}

void storeSettings(const Settings& s) {
    const std::string path = settingsPath();
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        SDL_Log("Could not write %s", path.c_str());
        return;
    }
    std::fprintf(f, "players=%d\nrounds=%d\nwind=%d\nmuted=%d\n", s.players, s.roundChoice, s.wind, s.muted ? 1 : 0);
    for (int i = 0; i < kMaxPlayers; ++i) std::fprintf(f, "control%d=%d\n", i, s.control[size_t(i)]);
    std::fclose(f);
}

}  // namespace mud
