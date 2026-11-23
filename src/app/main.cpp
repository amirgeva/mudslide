#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/game.h"

namespace {

void usage() {
    std::printf(
        "Mudslide - turn-based artillery on slippery ground.\n\n"
        "  --size WxH             window size (default 1280x720)\n"
        "  --screenshot FILE.bmp  play CPU turns offscreen, save one frame and exit\n"
        "  --scene NAME           what to capture: menu | aim | flight | blast | debris |\n"
        "                         settle | round | over | pause (default aim)\n"
        "  --seed N               match seed for --screenshot (default 12345)\n"
        "  --players N            CPU players for --screenshot (2-4, default 4)\n"
        "  --turns N              turns to play before capturing (default 3)\n"
        "  --human                make player 1 human (captures their first turn)\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    mud::Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--size" && next) {
            int w = 0, h = 0;
            if (std::sscanf(next, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
                opt.width = w;
                opt.height = h;
            }
            ++i;
        } else if (a == "--screenshot" && next) {
            opt.screenshot = next;
            ++i;
        } else if (a == "--scene" && next) {
            opt.scene = next;
            ++i;
        } else if (a == "--seed" && next) {
            opt.seed = std::strtoull(next, nullptr, 10);
            ++i;
        } else if (a == "--players" && next) {
            opt.players = std::atoi(next);
            ++i;
        } else if (a == "--turns" && next) {
            opt.turns = std::atoi(next);
            ++i;
        } else if (a == "--human") {
            opt.human = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        }
    }
    mud::Game game;
    return game.run(opt);
}
