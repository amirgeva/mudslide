#pragma once

#include <SDL.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "app/audio.h"
#include "app/save.h"
#include "core/match.h"

namespace mud {

class Canvas;

struct Options {
    int width = 1280;
    int height = 720;
    std::string screenshot;      // render one frame to this .bmp and exit
    std::string scene = "aim";   // menu | aim | flight | blast | debris | settle | round | over | pause | help | cover
    uint64_t seed = 0;           // 0 = random
    int players = 4;             // screenshot: number of CPU players
    int turns = 3;               // screenshot: turns to play before capturing
    bool human = false;          // screenshot: player 1 is human (captures their first turn)
    std::string record;          // play the showcase clip, raw RGB24 frames to this file ("-" = stdout)
    int fps = 30;                // frame rate for --record
};

class Game {
public:
    ~Game();
    int run(const Options& opt);

private:
    enum class Scene { Menu, Play };
    enum class Item { Start, Players, Slot0, Slot1, Slot2, Slot3, Rounds, Wind, Quit };

    // World pixels to screen pixels.
    struct View {
        float s = 1.f, ox = 0.f, oy = 0.f;
        float x(float wx) const { return ox + wx * s; }
        float y(float wy) const { return oy + wy * s; }
    };
    struct Particle {
        float x, y, vx, vy, life, maxLife, size, grow, gravity;
        SDL_Color color;
        int kind;  // 0 debris chip, 1 smoke puff, 2 spark (additive)
    };
    struct Flash {
        float x, y, r, t, life;
    };
    struct Floater {
        float x, y, t;
        std::string text;
        SDL_Color color;
    };
    struct Trail {
        int id;
        std::vector<SDL_FPoint> pts;
        float fade;  // < 0 while the projectile flies
        bool seen;
    };
    struct Cloud {
        float x, y, scale;
        int variant;
    };

    // A match seed picked for showing the game off, and when its highlights happen.
    struct Showcase {
        uint64_t seed = 1;
        float length = 20.f;   // seconds of match worth recording
        float heavyAt = -1.f;  // match clock of the first heavy blast (-1: none)
        float mudAt = -1.f;    // ... and of the first mud ball landing
    };

    bool init(const Options& opt);
    void shutdown();
    int runScreenshot(const Options& opt);
    int runRecord(const Options& opt);
    void configureShowcase();
    Showcase pickShowcase(float seconds) const;

    // Flow.
    MatchSetup setupFromSettings(uint64_t seed) const;
    void startMatch(uint64_t seed);
    void startDemo(uint64_t seed);
    void goToMenu();
    void handleEvent(const SDL_Event& e);
    void onKeyDown(SDL_Keycode key, bool repeat);
    void menuKey(SDL_Keycode key, bool repeat);
    void playKey(SDL_Keycode key, bool repeat);
    std::vector<Item> menuItems() const;
    void changeItem(Item item, int dir);
    void toggleMute();
    void persist();

    // Simulation and effects.
    void update(float dt);
    void steerHuman(float dt);
    void consumeEvents(Match& m, bool sound);
    void updateEffects(const Match& m, float dt);
    void clearEffects();
    void burst(float x, float y, float r, int weapon, uint8_t material);
    void splat(float x, float y, float r);
    void dust(float x, float y, int count, uint8_t material);
    SDL_Color materialColor(uint8_t material);
    void showCaption(const std::string& text, float seconds);
    float frand();
    float frand(float lo, float hi) { return lo + (hi - lo) * frand(); }

    // Rendering.
    void render();
    View worldView() const;
    int uiScale() const;
    void syncWorld(Match& m);
    void uploadTerrain(const Terrain& t, IRect r);
    void renderWorld(Match& m);
    void renderTanks(const Match& m, const View& v, Uint8 alpha);
    void renderTankOverlays(const Match& m, const View& v);
    void renderShots(const Match& m, const View& v);
    void renderEffects(const View& v);
    void renderHud(const Match& m, const View& v);
    void renderScoreboard(const Match& m);
    void renderPause();
    void renderHelp();
    void renderCover();
    void renderMenu();
    void renderCaption();

    // Assets.
    SDL_Texture* makeTexture(const Canvas& canvas);
    void createStaticAssets();
    void ensureTankSprites(float s);
    void destroyTankSprites();
    void ensureLogo(int cell);
    void drawPanel(int x, int y, int w, int h, int corner);
    void setWindowIcon();

    // Text.
    int textWidth(const std::string& s, int scale) const;
    void drawText(const std::string& s, int x, int y, int scale, SDL_Color color, int align = -1, bool shadow = true);
    void drawTextRaw(const std::string& s, int x, int y, int scale, SDL_Color color);
    void fillRect(float x, float y, float w, float h, SDL_Color c);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool running_ = true;
    bool sdlReady_ = false;
    bool vsync_ = false;
    bool persist_ = true;
    bool fixedView_ = false;
    bool cover_ = false;  // cover image: world fills the frame, title on top, no HUD
    int viewW_ = 0, viewH_ = 0;
    float time_ = 0.f;
    uint32_t rngState_ = 0x9E3779B9u;

    Audio audio_;
    Settings settings_;
    Scene scene_ = Scene::Menu;
    std::unique_ptr<Match> match_;  // the real match
    std::unique_ptr<Match> demo_;   // CPU-only match playing behind the menu
    float demoWait_ = 0.f;
    bool paused_ = false;
    bool help_ = false;  // F1 keyboard legend (also pauses a match)
    int pauseSel_ = 0;
    int menuSel_ = 0;

    // Human aiming: hold timers and fractional steps for the four arrow keys.
    std::array<float, 4> hold_{};
    std::array<float, 4> holdAcc_{};

    // Textures.
    SDL_Texture* font_ = nullptr;
    SDL_Texture* sky_ = nullptr;
    SDL_Texture* terrainTex_ = nullptr;
    SDL_Texture* fireball_ = nullptr;
    SDL_Texture* dot_ = nullptr;
    SDL_Texture* ball_ = nullptr;
    SDL_Texture* panel_ = nullptr;
    SDL_Texture* clouds_[3] = {nullptr, nullptr, nullptr};
    SDL_Texture* logo_ = nullptr;
    int logoCell_ = 0;
    SDL_Texture* body_[kMaxPlayers] = {};
    SDL_Texture* barrel_[kMaxPlayers] = {};
    int spriteKey_ = 0;

    // The world on screen.
    uint32_t terrainGen_ = 0;
    std::array<uint32_t, 256> palette_{};
    std::vector<uint32_t> terrainPx_;
    std::array<int, kWorldH> rowShade_{};
    std::vector<Cloud> cloudList_;

    // Effects.
    std::vector<Particle> particles_;
    std::vector<Flash> flashes_;
    std::vector<Floater> floaters_;
    std::vector<Trail> trails_;
    std::array<std::vector<SDL_FPoint>, kMaxPlayers> lastShot_;  // each player's previous trajectory
    int recording_ = -1;
    float shake_ = 0.f;
    std::string caption_;
    float captionStart_ = 0.f, captionEnd_ = 0.f;
};

}  // namespace mud
