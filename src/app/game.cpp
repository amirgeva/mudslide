#include "app/game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "gfx/art.h"
#include "gfx/canvas.h"
#include "gfx/font.h"

#ifndef MUD_VERSION
#define MUD_VERSION "DEV"
#endif

namespace mud {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kShowcaseWindow = 25.f;  // seconds of match auditioned for the clip and the cover
const char* const kPlayerNames[kMaxPlayers] = {"RED", "BLUE", "GOLD", "VIOLET"};

SDL_Color rgba(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) { return SDL_Color{r, g, b, a}; }

SDL_Color toColor(RGBA c, float alpha = 1.f) {
    return rgba(Uint8(std::lround(clamp01(c.r) * 255.f)), Uint8(std::lround(clamp01(c.g) * 255.f)),
                Uint8(std::lround(clamp01(c.b) * 255.f)), Uint8(std::lround(clamp01(c.a * alpha) * 255.f)));
}

SDL_Color unpack(uint32_t argb, Uint8 a = 255) {
    return rgba(Uint8((argb >> 16) & 255u), Uint8((argb >> 8) & 255u), Uint8(argb & 255u), a);
}

SDL_Color withAlpha(SDL_Color c, float a) {
    c.a = Uint8(std::lround(clamp01(a) * float(c.a)));
    return c;
}

bool isConfirm(SDL_Keycode k) { return k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE; }

float easeOut(float t) {
    t = clamp01(t);
    return 1.f - (1.f - t) * (1.f - t);
}

uint64_t freshSeed() {
    return (uint64_t(SDL_GetPerformanceCounter()) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(SDL_GetTicks()) << 32);
}

std::string playerName(int i) { return kPlayerNames[std::clamp(i, 0, kMaxPlayers - 1)]; }

}  // namespace

Game::~Game() { shutdown(); }

// ---------------------------------------------------------------- lifecycle

int Game::run(const Options& opt) {
    if (!init(opt)) {
        shutdown();
        return 1;
    }
    if (!opt.screenshot.empty() || !opt.record.empty()) {
        const int rc = opt.record.empty() ? runScreenshot(opt) : runRecord(opt);
        shutdown();
        return rc;
    }
    const double freq = double(SDL_GetPerformanceFrequency());
    Uint64 last = SDL_GetPerformanceCounter();
    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = std::min(0.05f, float(double(now - last) / freq));
        last = now;
        update(dt);
        render();
        SDL_RenderPresent(renderer_);
        if (!vsync_) {
            const double spent = double(SDL_GetPerformanceCounter() - now) / freq;
            if (spent < 1.0 / 120.0) SDL_Delay(Uint32((1.0 / 120.0 - spent) * 1000.0));
        }
    }
    persist();
    shutdown();
    return 0;
}

bool Game::init(const Options& opt) {
    const bool offscreen = !opt.screenshot.empty() || !opt.record.empty();
    persist_ = !offscreen;
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint("SDL_WINDOWS_DPI_SCALING", "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    sdlReady_ = true;

    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (offscreen) flags |= SDL_WINDOW_HIDDEN;
    window_ = SDL_CreateWindow("Mudslide", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.width, opt.height, flags);
    if (!window_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create window: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(window_, 640, 360);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    renderer_ = SDL_CreateRenderer(window_, -1,
                                   SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create renderer: %s", SDL_GetError());
        return false;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer_, &info) == 0) vsync_ = (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    setWindowIcon();
    createStaticAssets();

    settings_ = offscreen ? Settings{} : loadSettings();
    if (!offscreen) audio_.init();
    audio_.setMuted(settings_.muted);

    for (int y = 0; y < kWorldH; ++y)
        rowShade_[size_t(y)] = int(256.f * (1.f - 0.32f * std::pow(float(y) / float(kWorldH), 1.6f)));
    terrainPx_.assign(size_t(kWorldW) * kWorldH, 0u);
    for (int i = 0; i < 6; ++i)
        cloudList_.push_back(Cloud{frand(-100.f, float(kWorldW) + 100.f), frand(30.f, 170.f), frand(0.6f, 1.2f), i % 3});

    startDemo(opt.seed ? opt.seed : freshSeed());
    scene_ = Scene::Menu;
    return true;
}

void Game::shutdown() {
    audio_.shutdown();
    destroyTankSprites();
    SDL_Texture** all[] = {&font_, &sky_, &terrainTex_, &fireball_, &dot_, &ball_, &panel_,
                           &clouds_[0], &clouds_[1], &clouds_[2], &logo_};
    for (SDL_Texture** t : all) {
        if (*t) SDL_DestroyTexture(*t);
        *t = nullptr;
    }
    if (renderer_) SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
    if (window_) SDL_DestroyWindow(window_);
    window_ = nullptr;
    if (sdlReady_) SDL_Quit();
    sdlReady_ = false;
}

void Game::persist() {
    if (persist_) storeSettings(settings_);
}

// Plays a match (CPU players) up to the requested moment and saves one frame
// as a .bmp. Used for README screenshots and as an end-to-end smoke test.
int Game::runScreenshot(const Options& opt) {
    fixedView_ = true;
    viewW_ = opt.width;
    viewH_ = opt.height;
    SDL_Texture* target = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, viewW_, viewH_);
    if (!target) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Render target unavailable: %s", SDL_GetError());
        return 1;
    }
    const float step = 1.f / 60.f;
    const uint64_t seed = opt.seed ? opt.seed : 12345u;
    if (opt.scene == "menu") {
        startDemo(seed);
        for (int i = 0; i < 60 * 8; ++i) update(step);
    } else if (opt.scene == "cover") {
        // The showcase match, caught at the first blast after its mud ball has
        // landed (so the ground is already churned up), or else its first big one.
        configureShowcase();
        Showcase show;
        if (opt.seed) show.seed = opt.seed;
        else show = pickShowcase(kShowcaseWindow);
        startMatch(show.seed);
        auto ready = [&] {
            const bool afterMud = show.mudAt >= 0.f && match_->clock() > show.mudAt + 1.f;
            for (const Flash& f : flashes_)
                if (f.t >= 0.1f && (f.r >= 44.f || (afterMud && f.r >= 25.f))) return true;
            return false;
        };
        for (int i = 0; i < 60 * 300 && !ready(); ++i) update(step);
        cover_ = true;
        floaters_.clear();  // no damage numbers on the cover
    } else {
        settings_.players = std::clamp(opt.players, 2, kMaxPlayers);
        for (int k = 0; k < kMaxPlayers; ++k) settings_.control[size_t(k)] = int(k % 2 ? Control::CpuNormal : Control::CpuHard);
        if (opt.human) settings_.control[0] = int(Control::Human);
        settings_.roundChoice = opt.scene == "over" ? 0 : 1;
        settings_.wind = 2;
        startMatch(seed);
        const std::string& s = opt.scene;
        auto done = [&]() {
            const Match& m = *match_;
            const bool late = opt.human || m.turnInRound() >= opt.turns;
            const Phase ph = m.phase();
            if (s == "aim" || s == "pause" || s == "help")
                return late && ph == Phase::Aiming && (m.humanAiming() || m.phaseTime() > 1.1f);
            if (s == "flight") return late && ph == Phase::Flight && m.phaseTime() > 0.7f;
            if (s == "blast") return late && ph == Phase::Blast && m.phaseTime() > 0.06f;
            if (s == "debris") return late && ph == Phase::Blast && m.phaseTime() > 0.3f && !m.clods().empty();
            if (s == "settle") return late && ph == Phase::Settle && m.phaseTime() > 0.2f;
            if (s == "round") return ph == Phase::RoundOver && m.phaseTime() > 0.4f;
            if (s == "over") return ph == Phase::MatchOver && m.phaseTime() > 0.4f;
            return true;
        };
        for (int i = 0; i < 60 * 1500 && !done(); ++i) update(step);
        if (s == "pause") paused_ = true;
        if (s == "help") help_ = true;
        SDL_Log("screenshot: round %d turn %d phase %d", match_->round(), match_->turnInRound(), int(match_->phase()));
    }

    SDL_SetRenderTarget(renderer_, target);
    render();
    std::vector<uint8_t> px(size_t(viewW_) * size_t(viewH_) * 4);
    const int read = SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888, px.data(), viewW_ * 4);
    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_DestroyTexture(target);
    if (read != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not read pixels: %s", SDL_GetError());
        return 1;
    }
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), viewW_, viewH_, 32, viewW_ * 4, SDL_PIXELFORMAT_ARGB8888);
    const int saved = surf ? SDL_SaveBMP(surf, opt.screenshot.c_str()) : -1;
    if (surf) SDL_FreeSurface(surf);
    if (saved != 0) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not save screenshot: %s", SDL_GetError());
    return saved == 0 ? 0 : 1;
}

// Four CPU tanks, three rounds, a breeze: the match used for the gameplay
// clip and the cover image.
void Game::configureShowcase() {
    settings_ = Settings{};
    settings_.players = 4;
    settings_.control = {int(Control::CpuHard), int(Control::CpuNormal), int(Control::CpuHard), int(Control::CpuNormal)};
    settings_.roundChoice = 1;
    settings_.wind = 2;
}

// Matches are deterministic, so seeds can be auditioned headlessly: the one
// whose first `seconds` pack in the most (a heavy blast, a cluster burst, a
// mud ball, a tank falling, a shot drilling out of dirt, a tank destroyed)
// is the one the recording will replay exactly.
Game::Showcase Game::pickShowcase(float seconds) const {
    Showcase best;
    float bestScore = -1e9f;
    for (uint64_t seed = 1; seed <= 160; ++seed) {
        Match m(setupFromSettings(seed));
        Showcase s;
        s.seed = seed;
        float last = 0.f, split = -1.f, death = -1.f, fall = -1.f, bore = -1.f;
        int hits = 0;
        while (m.clock() < seconds && m.phase() != Phase::RoundOver) {
            m.update(1.f / 30.f);
            for (const Event& e : m.takeEvents()) {
                auto first = [&](float& at) {
                    if (at < 0.f) last = std::max(last, at = m.clock());
                };
                switch (e.type) {
                    case EventType::Explode:
                        if (e.weapon == int(Weapon::Heavy)) first(s.heavyAt);
                        break;
                    case EventType::MudSplat: first(s.mudAt); break;
                    case EventType::Split: first(split); break;
                    case EventType::TankDeath: first(death); break;
                    case EventType::TankLand:
                        if (e.r >= 20.f) first(fall);
                        break;
                    case EventType::Bore: first(bore); break;
                    case EventType::TankHit: ++hits; break;
                    default: break;
                }
            }
        }
        auto has = [](float at) { return at >= 0.f ? 1.f : 0.f; };
        // Bright daytime maps show the game off best.
        const float themeBonus = m.theme() == 0 || m.theme() == 1 ? 4.5f : m.theme() == 2 ? 1.5f : 0.f;
        const float score = 6.f * has(s.mudAt) + 2.f * has(s.heavyAt) + 2.f * has(split) + 4.f * has(death) +
                            has(fall) + has(bore) + 0.2f * float(std::min(hits, 10)) - 0.04f * last + themeBonus;
        if (score > bestScore) {
            bestScore = score;
            s.length = std::max(14.f, last + 3.5f);
            best = s;
        }
    }
    std::fprintf(stderr, "showcase: seed %llu, %.1f s, heavy at %.1f, mud at %.1f\n", (unsigned long long)best.seed,
                 double(best.length), double(best.heavyAt), double(best.mudAt));
    return best;
}

// Plays the showcase clip at a fixed frame rate and writes every frame as raw
// RGB24 (pipe it into ffmpeg; see tools/make_media.sh): a moment on the menu,
// then the showcase match with a caption on its first heavy blast and mud ball.
int Game::runRecord(const Options& opt) {
    fixedView_ = true;
    viewW_ = opt.width;
    viewH_ = opt.height;
    configureShowcase();
    Showcase show;
    if (opt.seed) {
        show.seed = opt.seed;
        show.length = kShowcaseWindow;
    } else {
        show = pickShowcase(kShowcaseWindow);
    }

    FILE* out = nullptr;
    if (opt.record == "-") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        out = stdout;
    } else {
        out = std::fopen(opt.record.c_str(), "wb");
    }
    if (!out) {
        std::fprintf(stderr, "cannot open %s for writing\n", opt.record.c_str());
        return 1;
    }
    SDL_Texture* target = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, viewW_, viewH_);
    if (!target) {
        std::fprintf(stderr, "render target unavailable: %s\n", SDL_GetError());
        if (out != stdout) std::fclose(out);
        return 1;
    }

    const int fps = std::clamp(opt.fps, 10, 60);
    const float dt = 1.f / float(fps);
    std::vector<uint8_t> px(size_t(viewW_) * size_t(viewH_) * 3);
    int frames = 0;
    bool ok = true;
    auto frame = [&] {
        update(dt);
        SDL_SetRenderTarget(renderer_, target);
        render();
        if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGB24, px.data(), viewW_ * 3) != 0) ok = false;
        SDL_SetRenderTarget(renderer_, nullptr);
        if (std::fwrite(px.data(), 1, px.size(), out) != px.size()) ok = false;
        ++frames;
    };

    // The menu, over a demo match that has already got going.
    startDemo(show.seed + 1000u);
    scene_ = Scene::Menu;
    menuSel_ = 0;
    for (int i = 0; i < fps * 3; ++i) update(dt);
    for (int i = 0; i < int(2.4f * float(fps)) && ok; ++i) frame();

    // The match itself, with captions on its highlights.
    startMatch(show.seed);
    struct Cue {
        float at;
        const char* text;
        bool shown;
    };
    Cue cues[] = {{show.heavyAt, "BLASTS THROW THE DIRT AROUND", false},
                  {show.mudAt, "MUD SLIDES DOWNHILL AND BURIES TANKS", false}};
    while (ok && match_->clock() < show.length) {
        for (Cue& c : cues)
            if (!c.shown && c.at >= 0.f && match_->clock() >= c.at) {
                showCaption(c.text, 2.6f);
                c.shown = true;
            }
        frame();
    }

    SDL_DestroyTexture(target);
    std::fflush(out);
    if (out != stdout) std::fclose(out);
    std::fprintf(stderr, "recorded %d frames (%.1f s at %d fps, %dx%d)%s\n", frames, double(frames) / fps, fps, viewW_,
                 viewH_, ok ? "" : " - WRITE ERRORS");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- flow

MatchSetup Game::setupFromSettings(uint64_t seed) const {
    MatchSetup s;
    for (int i = 0; i < settings_.players; ++i)
        s.players.push_back(PlayerSetup{playerName(i), Control(settings_.control[size_t(i)])});
    s.rounds = kRoundOptions[settings_.roundChoice];
    s.wind = settings_.wind;
    s.seed = seed;
    return s;
}

void Game::startMatch(uint64_t seed) {
    match_ = std::make_unique<Match>(setupFromSettings(seed));
    scene_ = Scene::Play;
    paused_ = false;
    clearEffects();
    for (auto& shot : lastShot_) shot.clear();
    hold_.fill(0.f);
    holdAcc_.fill(0.f);
}

void Game::startDemo(uint64_t seed) {
    MatchSetup s;
    const Control skills[4] = {Control::CpuNormal, Control::CpuHard, Control::CpuEasy, Control::CpuNormal};
    for (int i = 0; i < 4; ++i) s.players.push_back(PlayerSetup{playerName(i), skills[i]});
    s.rounds = 99;
    s.wind = 2;
    s.seed = seed;
    demo_ = std::make_unique<Match>(s);
    demoWait_ = 0.f;
}

void Game::goToMenu() {
    scene_ = Scene::Menu;
    paused_ = false;
    match_.reset();
    clearEffects();
    persist();
}

void Game::toggleMute() {
    settings_.muted = !settings_.muted;
    audio_.setMuted(settings_.muted);
    showCaption(settings_.muted ? "SOUND OFF" : "SOUND ON", 1.f);
    persist();
}

void Game::showCaption(const std::string& text, float seconds) {
    caption_ = text;
    captionStart_ = time_;
    captionEnd_ = time_ + seconds;
}

// ---------------------------------------------------------------- input

void Game::handleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT:
            running_ = false;
            break;
        case SDL_KEYDOWN:
            onKeyDown(e.key.keysym.sym, e.key.repeat != 0);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                hold_.fill(0.f);
                holdAcc_.fill(0.f);
            }
            break;
        default:
            break;
    }
}

void Game::onKeyDown(SDL_Keycode key, bool repeat) {
    if (!repeat && key == SDLK_m) {
        toggleMute();
        return;
    }
    if (!repeat && (key == SDLK_F11 || (key == SDLK_RETURN && (SDL_GetModState() & KMOD_ALT)))) {
        const bool full = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        SDL_SetWindowFullscreen(window_, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    if (!repeat && key == SDLK_F1) {
        help_ = !help_;
        audio_.play(Sfx::Tick);
        return;
    }
    if (help_) {  // the legend swallows other keys until it is closed
        if (!repeat && key == SDLK_ESCAPE) help_ = false;
        return;
    }
    if (scene_ == Scene::Menu) menuKey(key, repeat);
    else playKey(key, repeat);
}

std::vector<Game::Item> Game::menuItems() const {
    std::vector<Item> items = {Item::Start, Item::Players};
    for (int i = 0; i < settings_.players; ++i) items.push_back(Item(int(Item::Slot0) + i));
    items.push_back(Item::Rounds);
    items.push_back(Item::Wind);
    items.push_back(Item::Quit);
    return items;
}

void Game::changeItem(Item item, int dir) {
    switch (item) {
        case Item::Players:
            settings_.players = std::clamp(settings_.players + dir, 2, kMaxPlayers);
            break;
        case Item::Slot0:
        case Item::Slot1:
        case Item::Slot2:
        case Item::Slot3: {
            int& c = settings_.control[size_t(int(item) - int(Item::Slot0))];
            c = (c + dir + kControlCount) % kControlCount;
            break;
        }
        case Item::Rounds:
            settings_.roundChoice = (settings_.roundChoice + dir + kRoundChoices) % kRoundChoices;
            break;
        case Item::Wind:
            settings_.wind = (settings_.wind + dir + kWindLevels) % kWindLevels;
            break;
        default:
            return;
    }
    audio_.play(Sfx::Tick);
    persist();
}

void Game::menuKey(SDL_Keycode key, bool repeat) {
    const std::vector<Item> items = menuItems();
    menuSel_ = std::clamp(menuSel_, 0, int(items.size()) - 1);
    const Item item = items[size_t(menuSel_)];
    switch (key) {
        case SDLK_UP:
        case SDLK_w:
            menuSel_ = (menuSel_ + int(items.size()) - 1) % int(items.size());
            audio_.play(Sfx::Tick);
            break;
        case SDLK_DOWN:
        case SDLK_s:
            menuSel_ = (menuSel_ + 1) % int(items.size());
            audio_.play(Sfx::Tick);
            break;
        case SDLK_LEFT:
        case SDLK_a:
            changeItem(item, -1);
            break;
        case SDLK_RIGHT:
        case SDLK_d:
            changeItem(item, 1);
            break;
        case SDLK_ESCAPE:
            if (!repeat) running_ = false;
            break;
        default:
            if (isConfirm(key) && !repeat) {
                if (item == Item::Start) startMatch(freshSeed());
                else if (item == Item::Quit) running_ = false;
                else changeItem(item, 1);
            }
            break;
    }
}

void Game::playKey(SDL_Keycode key, bool repeat) {
    if (!match_) return;
    if (paused_) {
        if (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_w || key == SDLK_s) {
            pauseSel_ = 1 - pauseSel_;
            audio_.play(Sfx::Tick);
        } else if (key == SDLK_ESCAPE && !repeat) {
            paused_ = false;
        } else if (isConfirm(key) && !repeat) {
            if (pauseSel_ == 0) paused_ = false;
            else goToMenu();
        }
        return;
    }
    Match& m = *match_;
    const Phase ph = m.phase();
    if (key == SDLK_ESCAPE && !repeat) {
        if (ph == Phase::MatchOver) {
            goToMenu();
        } else {
            paused_ = true;
            pauseSel_ = 0;
        }
        return;
    }
    if (ph == Phase::RoundOver) {
        if (isConfirm(key) && !repeat && m.phaseTime() > 0.5f) {
            m.nextRound();
            clearEffects();
            for (auto& shot : lastShot_) shot.clear();
        }
        return;
    }
    if (ph == Phase::MatchOver) {
        if (isConfirm(key) && !repeat && m.phaseTime() > 0.5f) startMatch(freshSeed());
        return;
    }
    if (!m.humanAiming()) return;
    const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
    switch (key) {
        case SDLK_LEFT:
        case SDLK_a:
            if (!repeat) m.adjustAim(1.f, 0.f), hold_[0] = holdAcc_[0] = 0.f;
            break;
        case SDLK_RIGHT:
        case SDLK_d:
            if (!repeat) m.adjustAim(-1.f, 0.f), hold_[1] = holdAcc_[1] = 0.f;
            break;
        case SDLK_UP:
        case SDLK_w:
            if (!repeat) m.adjustAim(0.f, 1.f), hold_[2] = holdAcc_[2] = 0.f;
            break;
        case SDLK_DOWN:
        case SDLK_s:
            if (!repeat) m.adjustAim(0.f, -1.f), hold_[3] = holdAcc_[3] = 0.f;
            break;
        case SDLK_TAB:
            m.cycleWeapon(shift ? -1 : 1);
            audio_.play(Sfx::Tick);
            break;
        case SDLK_q:
            m.cycleWeapon(-1);
            audio_.play(Sfx::Tick);
            break;
        case SDLK_e:
            m.cycleWeapon(1);
            audio_.play(Sfx::Tick);
            break;
        case SDLK_1:
        case SDLK_2:
        case SDLK_3:
        case SDLK_4:
            m.selectWeapon(int(key - SDLK_1));
            audio_.play(Sfx::Tick);
            break;
        default:
            if (isConfirm(key) && !repeat) m.fire();
            break;
    }
}

// Holding an aiming key keeps turning (or powering) at a rate that speeds up
// the longer it is held; Shift slows it down for fine adjustment.
void Game::steerHuman(float dt) {
    if (!match_ || !match_->humanAiming()) {
        hold_.fill(0.f);
        holdAcc_.fill(0.f);
        return;
    }
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    const bool fine = (SDL_GetModState() & KMOD_SHIFT) != 0;
    const SDL_Scancode keys[4][2] = {{SDL_SCANCODE_LEFT, SDL_SCANCODE_A},
                                     {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D},
                                     {SDL_SCANCODE_UP, SDL_SCANCODE_W},
                                     {SDL_SCANCODE_DOWN, SDL_SCANCODE_S}};
    for (size_t k = 0; k < 4; ++k) {
        if (!ks[keys[k][0]] && !ks[keys[k][1]]) {
            hold_[k] = holdAcc_[k] = 0.f;
            continue;
        }
        hold_[k] += dt;
        if (hold_[k] < 0.25f) continue;
        const float rate = fine ? 5.f : (hold_[k] > 1.1f ? 60.f : 22.f);
        holdAcc_[k] += rate * dt;
        while (holdAcc_[k] >= 1.f) {
            holdAcc_[k] -= 1.f;
            const float sign = (k == 0 || k == 2) ? 1.f : -1.f;
            if (k < 2) match_->adjustAim(sign, 0.f);
            else match_->adjustAim(0.f, sign);
        }
    }
}

// ---------------------------------------------------------------- simulation

float Game::frand() {
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 17;
    rngState_ ^= rngState_ << 5;
    return float(rngState_ >> 8) / float(1u << 24);
}

void Game::update(float dt) {
    time_ += dt;
    if (scene_ == Scene::Menu) {
        demo_->update(dt);
        const Phase ph = demo_->phase();
        if (ph == Phase::RoundOver || ph == Phase::MatchOver) {
            demoWait_ += dt;
            if (demoWait_ > 2.5f) {
                demoWait_ = 0.f;
                if (ph == Phase::RoundOver) demo_->nextRound();
                else startDemo(freshSeed());
                clearEffects();
            }
        }
        consumeEvents(*demo_, false);
        updateEffects(*demo_, dt);
    } else if (match_) {
        const bool frozen = paused_ || help_;
        if (!frozen) {
            steerHuman(dt);
            match_->update(dt);
        }
        consumeEvents(*match_, true);
        updateEffects(*match_, frozen ? 0.f : dt);
    }
}

void Game::consumeEvents(Match& m, bool sound) {
    auto play = [&](Sfx s, float volume = 1.f) {
        if (sound) audio_.play(s, volume);
    };
    for (const Event& e : m.takeEvents()) {
        switch (e.type) {
            case EventType::TurnStart:
                if (!m.isCpu(e.tank)) play(Sfx::Chime);
                if (sound && m.turnInRound() == 1)
                    showCaption("ROUND " + std::to_string(m.round()) + " - " + art::themeName(m.theme()), 2.2f);
                break;
            case EventType::Fire: {
                play(Sfx::Fire);
                const float a = e.r * kPi / 180.f;
                const float dx = std::cos(a), dy = -std::sin(a);
                for (int i = 0; i < 12; ++i) {
                    const float sp = frand(120.f, 320.f);
                    particles_.push_back(Particle{e.x, e.y, dx * sp + frand(-40.f, 40.f), dy * sp + frand(-40.f, 40.f), 0.f,
                                                  frand(0.12f, 0.28f), frand(2.f, 3.5f), 0.f, 0.f,
                                                  rgba(255, Uint8(frand(160.f, 230.f)), 80), 2});
                }
                for (int i = 0; i < 6; ++i)
                    particles_.push_back(Particle{e.x, e.y, dx * frand(20.f, 60.f) + frand(-10.f, 10.f),
                                                  dy * frand(20.f, 60.f) - 10.f, 0.f, frand(0.8f, 1.4f), frand(4.f, 7.f),
                                                  9.f, -8.f, rgba(200, 200, 200, 150), 1});
                recording_ = e.tank;
                lastShot_[size_t(e.tank)].clear();
                break;
            }
            case EventType::Bore:
                play(Sfx::Bore);
                dust(e.x, e.y, 16, e.material);
                break;
            case EventType::Split:
                play(Sfx::Split);
                for (int i = 0; i < 14; ++i)
                    particles_.push_back(Particle{e.x, e.y, frand(-160.f, 160.f), frand(-160.f, 160.f), 0.f,
                                                  frand(0.15f, 0.35f), frand(1.5f, 3.f), 0.f, 100.f,
                                                  rgba(255, Uint8(frand(180.f, 240.f)), 90), 2});
                break;
            case EventType::Explode:
                if (e.weapon < 0 || e.r >= 40.f) play(Sfx::BigBoom);
                else if (e.r < 18.f) play(Sfx::Crack);
                else play(Sfx::Boom);
                burst(e.x, e.y, e.r, e.weapon, e.material);
                break;
            case EventType::MudSplat:
                play(Sfx::Splat);
                splat(e.x, e.y, e.r);
                break;
            case EventType::TankHit:
                if (e.amount >= 0.5f)
                    floaters_.push_back(Floater{e.x, e.y - 14.f, 0.f, "-" + std::to_string(int(std::lround(e.amount))),
                                                rgba(255, 226, 120)});
                break;
            case EventType::TankLand:
                if (e.r >= 10.f) {
                    play(Sfx::Thud, std::min(1.f, e.r / 40.f));
                    dust(e.x, e.y, 4 + int(e.r / 5.f), m.terrain().at(int(e.x), int(e.y)));
                }
                if (e.amount >= 0.5f)
                    floaters_.push_back(Floater{e.x, e.y - 30.f, 0.f, "-" + std::to_string(int(std::lround(e.amount))),
                                                rgba(255, 190, 120)});
                break;
            case EventType::TankDeath:
                if (sound) {
                    const std::string victim = playerName(e.tank);
                    showCaption(e.other >= 0 && e.other != e.tank ? playerName(e.other) + " DESTROYED " + victim
                                                                  : victim + " BLEW ITSELF UP",
                                2.f);
                }
                break;
            case EventType::RoundOver:
                play(e.tank >= 0 ? Sfx::Fanfare : Sfx::Boom);
                break;
            case EventType::MatchOver:
                break;
        }
    }
}

SDL_Color Game::materialColor(uint8_t material) {
    if (material == kAir) return rgba(120, 90, 60);
    // Pick a random shade from the same 8-colour family.
    const int start = material >= kTopFirst ? (material & ~7) : 1 + ((material - 1) & ~7);
    return unpack(palette_[size_t(start + int(frand() * 7.99f))]);
}

void Game::burst(float x, float y, float r, int weapon, uint8_t material) {
    flashes_.push_back(Flash{x, y, r * (weapon < 0 ? 1.4f : 1.15f), 0.f, weapon < 0 ? 0.6f : 0.42f});
    shake_ = std::max(shake_, r / 7.f);
    const float power = std::sqrt(r / 22.f);
    // Fine grit (cosmetic; the clods the match throws are the real dirt).
    for (int i = 0, n = int(r * 0.6f); i < n; ++i) {
        const float a = frand(-kPi, 0.f), sp = frand(60.f, 220.f) * power;
        particles_.push_back(Particle{x, y, std::cos(a) * sp, std::sin(a) * sp, 0.f, frand(0.4f, 0.9f), frand(1.f, 2.f),
                                      0.f, 420.f, materialColor(material), 0});
    }
    for (int i = 0, n = int(r * 0.7f); i < n; ++i) {
        const float a = frand(-kPi, kPi), sp = frand(100.f, 340.f) * power;
        particles_.push_back(Particle{x, y, std::cos(a) * sp, std::sin(a) * sp - 60.f, 0.f, frand(0.2f, 0.5f),
                                      frand(2.f, 3.5f), 0.f, 250.f, rgba(255, Uint8(frand(140.f, 220.f)), 60), 2});
    }
    for (int i = 0, n = int(r / 3.5f); i < n; ++i)
        particles_.push_back(Particle{x + frand(-r, r) * 0.5f, y + frand(-r, r * 0.5f) * 0.5f, frand(-20.f, 20.f),
                                      frand(-40.f, -15.f), 0.f, frand(1.f, 2.f), r * frand(0.5f, 0.8f), r * 0.6f, -5.f,
                                      rgba(70, 64, 58, 150), 1});
}

void Game::splat(float x, float y, float r) {
    shake_ = std::max(shake_, r / 12.f);
    for (int i = 0; i < 30; ++i) {
        const float a = frand(-kPi, 0.f), sp = frand(60.f, 240.f);
        particles_.push_back(Particle{x, y, std::cos(a) * sp, std::sin(a) * sp, 0.f, frand(0.4f, 0.9f), frand(2.f, 4.f),
                                      0.f, 450.f, unpack(palette_[size_t(kMudFirst + int(frand() * 7.99f))]), 0});
    }
}

void Game::dust(float x, float y, int count, uint8_t material) {
    SDL_Color c = materialColor(material);
    c = rgba(Uint8((c.r + 200) / 2), Uint8((c.g + 190) / 2), Uint8((c.b + 170) / 2), 140);
    for (int i = 0; i < count; ++i)
        particles_.push_back(Particle{x + frand(-8.f, 8.f), y - frand(0.f, 4.f), frand(-35.f, 35.f), frand(-30.f, -5.f), 0.f,
                                      frand(0.6f, 1.2f), frand(3.f, 6.f), 6.f, 10.f, c, 1});
}

void Game::clearEffects() {
    particles_.clear();
    flashes_.clear();
    floaters_.clear();
    trails_.clear();
    recording_ = -1;
    shake_ = 0.f;
}

void Game::updateEffects(const Match& m, float dt) {
    if (dt <= 0.f) return;
    const Terrain& terrain = m.terrain();
    for (Particle& p : particles_) {
        p.life += dt;
        p.vy += p.gravity * dt;
        if (p.kind == 1) p.vx += (m.wind() * 0.25f - p.vx * 0.5f) * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.size += p.grow * dt;
        if (p.kind != 1 && terrain.solid(int(p.x), int(p.y))) p.life = p.maxLife;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(), [](const Particle& p) { return p.life >= p.maxLife; }),
                     particles_.end());
    if (particles_.size() > 2500) particles_.erase(particles_.begin(), particles_.begin() + long(particles_.size() - 2500));

    for (Flash& f : flashes_) f.t += dt;
    flashes_.erase(std::remove_if(flashes_.begin(), flashes_.end(), [](const Flash& f) { return f.t >= f.life; }), flashes_.end());
    for (Floater& f : floaters_) {
        f.t += dt;
        f.y -= 18.f * dt;
    }
    floaters_.erase(std::remove_if(floaters_.begin(), floaters_.end(), [](const Floater& f) { return f.t >= 1.3f; }),
                    floaters_.end());
    shake_ = std::max(0.f, shake_ - dt * (6.f + shake_ * 5.f));

    for (Cloud& c : cloudList_) {
        c.x += (m.wind() * 0.5f + 3.f) * dt;
        if (c.x > float(kWorldW) + 130.f) c.x = -130.f;
        if (c.x < -130.f) c.x = float(kWorldW) + 130.f;
    }

    // Smoke trails behind projectiles.
    for (Trail& t : trails_) t.seen = false;
    for (const Projectile& p : m.projectiles()) {
        auto it = std::find_if(trails_.begin(), trails_.end(), [&](const Trail& t) { return t.id == p.id; });
        if (it == trails_.end()) {
            trails_.push_back(Trail{p.id, {}, -1.f, false});
            it = trails_.end() - 1;
        }
        it->seen = true;
        const SDL_FPoint pt{p.x, p.y};
        if (it->pts.empty() || std::hypot(pt.x - it->pts.back().x, pt.y - it->pts.back().y) >= 3.f) {
            it->pts.push_back(pt);
            if (it->pts.size() > 220) it->pts.erase(it->pts.begin());
        }
        if (recording_ >= 0 && p.owner == recording_ && !p.bomblet) {
            auto& shot = lastShot_[size_t(recording_)];
            if (shot.empty() || std::hypot(pt.x - shot.back().x, pt.y - shot.back().y) >= 8.f) shot.push_back(pt);
        }
    }
    for (Trail& t : trails_)
        if (!t.seen) t.fade = std::max(0.f, t.fade) + dt;
    trails_.erase(std::remove_if(trails_.begin(), trails_.end(), [](const Trail& t) { return t.fade >= 0.7f; }), trails_.end());
    if (m.phase() != Phase::Flight) recording_ = -1;
}

// ---------------------------------------------------------------- assets

SDL_Texture* Game::makeTexture(const Canvas& canvas) {
    const std::vector<uint8_t> px = canvas.toRGBA8();
    SDL_Texture* t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, canvas.width(), canvas.height());
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, px.data(), canvas.width() * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

void Game::setWindowIcon() {
    Canvas c(64, 64);
    art::icon(Pen(c), true);
    std::vector<uint8_t> px = c.toRGBA8();
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32);
    if (s) {
        SDL_SetWindowIcon(window_, s);
        SDL_FreeSurface(s);
    }
}

void Game::createStaticAssets() {
    {  // Font atlas: one 6x7 cell per ASCII code, white pixels, nearest filtering.
        const int w = 128 * font::kAdvance, h = font::kGlyphH;
        std::vector<uint32_t> px(size_t(w) * size_t(h), 0);
        for (int ch = 0; ch < 128; ++ch) {
            const char* const* rows = font::glyph(char(ch));
            if (!rows) continue;
            for (int y = 0; y < font::kGlyphH; ++y)
                for (int x = 0; x < font::kGlyphW; ++x)
                    if (rows[y][x] == '#') px[size_t(y * w + ch * font::kAdvance + x)] = 0xFFFFFFFFu;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        font_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        if (font_) {
            SDL_UpdateTexture(font_, nullptr, px.data(), w * 4);
            SDL_SetTextureBlendMode(font_, SDL_BLENDMODE_BLEND);
        }
    }
    auto bake = [&](int w, int h, auto draw) {
        Canvas c(w, h);
        draw(c);
        return makeTexture(c);
    };
    fireball_ = bake(128, 128, [](Canvas& c) { art::fireball(Pen(c)); });
    dot_ = bake(32, 32, [](Canvas& c) { art::dot(Pen(c)); });
    ball_ = bake(32, 32, [](Canvas& c) { art::ball(Pen(c)); });
    for (int i = 0; i < 3; ++i) clouds_[i] = bake(256, 128, [i](Canvas& c) { art::cloud(Pen(c), i); });
    panel_ = bake(48, 48, [](Canvas& c) {
        const Pen p(c, 0.f, 0.f, 1.f);
        p.roundRect(0.f, 0.f, 48.f, 48.f, 14.f, solid(rgb(0xB08458, 0.95f)));
        p.roundRect(3.f, 3.f, 45.f, 45.f, 11.f, VGrad{0.f, 48.f, rgb(0x3E2C1F, 0.95f), rgb(0x1E150F, 0.95f)});
    });
    sky_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, kWorldW, kWorldH);
    terrainTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kWorldW, kWorldH);
    if (terrainTex_) SDL_SetTextureBlendMode(terrainTex_, SDL_BLENDMODE_BLEND);
}

void Game::destroyTankSprites() {
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (body_[i]) SDL_DestroyTexture(body_[i]);
        if (barrel_[i]) SDL_DestroyTexture(barrel_[i]);
        body_[i] = barrel_[i] = nullptr;
    }
    spriteKey_ = 0;
}

// Tanks are drawn at exactly their on-screen size so they stay crisp.
void Game::ensureTankSprites(float s) {
    const int key = int(std::lround(s * 100.f));
    if (key == spriteKey_ && body_[0]) return;
    destroyTankSprites();
    spriteKey_ = key;
    const int bw = std::max(4, int(std::ceil(26.f * s))), bh = std::max(2, int(std::ceil(14.f * s)));
    const int rw = std::max(4, int(std::ceil(16.f * s))), rh = std::max(2, int(std::ceil(4.f * s)));
    for (int i = 0; i < kMaxPlayers; ++i) {
        Canvas body(bw, bh);
        art::tankBody(Pen(body, 0.f, 0.f, float(bw) / 26.f), art::playerColor(i));
        body_[i] = makeTexture(body);
        Canvas barrel(rw, rh);
        art::barrel(Pen(barrel, 0.f, 0.f, float(rw) / 16.f), art::playerColor(i));
        barrel_[i] = makeTexture(barrel);
    }
}

void Game::ensureLogo(int cell) {
    if (logo_ && logoCell_ == cell) return;
    if (logo_) SDL_DestroyTexture(logo_);
    Canvas c(art::kLogoW * cell, art::kLogoH * cell);
    art::logo(Pen(c, 0.f, 0.f, float(cell)));
    logo_ = makeTexture(c);
    logoCell_ = cell;
}

// Nine-slice panel: fixed corners, stretched edges and middle.
void Game::drawPanel(int x, int y, int w, int h, int corner) {
    if (!panel_) return;
    const int s = 16, S = 48;
    corner = std::min(corner, std::min(w, h) / 2);
    const int sx[4] = {0, s, S - s, S}, dx[4] = {x, x + corner, x + w - corner, x + w};
    const int sy[4] = {0, s, S - s, S}, dy[4] = {y, y + corner, y + h - corner, y + h};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const SDL_Rect src{sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j]};
            const SDL_Rect dst{dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j]};
            SDL_RenderCopy(renderer_, panel_, &src, &dst);
        }
}

// ---------------------------------------------------------------- text

int Game::textWidth(const std::string& s, int scale) const {
    return s.empty() ? 0 : int(s.size()) * font::kAdvance * scale - scale;
}

void Game::drawText(const std::string& s, int x, int y, int scale, SDL_Color color, int align, bool shadow) {
    const int w = textWidth(s, scale);
    if (align == 0) x -= w / 2;
    else if (align > 0) x -= w;
    if (shadow) {
        const int o = std::max(1, scale / 2);
        drawTextRaw(s, x + o, y + o, scale, rgba(10, 6, 3, Uint8(float(color.a) * 0.7f)));
    }
    drawTextRaw(s, x, y, scale, color);
}

void Game::drawTextRaw(const std::string& s, int x, int y, int scale, SDL_Color color) {
    if (!font_) return;
    SDL_SetTextureColorMod(font_, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(font_, color.a);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch >= 'a' && ch <= 'z') ch = static_cast<unsigned char>(ch - 'a' + 'A');
        if (ch >= 128 || ch == ' ') continue;
        const SDL_Rect src{int(ch) * font::kAdvance, 0, font::kGlyphW, font::kGlyphH};
        const SDL_Rect dst{x + int(i) * font::kAdvance * scale, y, font::kGlyphW * scale, font::kGlyphH * scale};
        SDL_RenderCopy(renderer_, font_, &src, &dst);
    }
}

void Game::fillRect(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    const SDL_FRect r{x, y, w, h};
    SDL_RenderFillRectF(renderer_, &r);
}

// ---------------------------------------------------------------- rendering

int Game::uiScale() const { return std::max(1, std::min(viewW_ / 560, viewH_ / 330)); }

Game::View Game::worldView() const {
    View v;
    const float sx = float(viewW_) / float(kWorldW), sy = float(viewH_) / float(kWorldH);
    v.s = cover_ ? std::max(sx, sy) : std::min(sx, sy);  // the cover fills the frame, cropping the edges
    v.ox = std::floor((float(viewW_) - float(kWorldW) * v.s) * 0.5f);
    v.oy = std::floor((float(viewH_) - float(kWorldH) * v.s) * 0.5f);
    return v;
}

void Game::render() {
    if (!fixedView_) SDL_GetRendererOutputSize(renderer_, &viewW_, &viewH_);
    SDL_SetRenderDrawColor(renderer_, 14, 10, 8, 255);
    SDL_RenderClear(renderer_);
    Match* m = scene_ == Scene::Menu ? demo_.get() : match_.get();
    if (cover_) {
        if (m) renderWorld(*m);
        renderCover();
        return;
    }
    if (m) renderWorld(*m);
    if (scene_ == Scene::Menu) {
        renderMenu();
    } else if (match_) {
        renderHud(*match_, worldView());
        const Phase ph = match_->phase();
        if (ph == Phase::RoundOver || ph == Phase::MatchOver) renderScoreboard(*match_);
        if (paused_) renderPause();
    }
    renderCaption();
    if (help_) renderHelp();
}

void Game::syncWorld(Match& m) {
    Terrain& t = m.terrain();
    if (t.generation() != terrainGen_) {
        terrainGen_ = t.generation();
        palette_ = art::terrainPalette(t.theme());
        std::vector<uint32_t> sky(size_t(kWorldW) * kWorldH);
        art::backdrop(sky.data(), kWorldW, kWorldH, t.theme(), t.seed());
        if (sky_) SDL_UpdateTexture(sky_, nullptr, sky.data(), kWorldW * 4);
        t.takeDirty();
        uploadTerrain(t, IRect{0, 0, kWorldW - 1, kWorldH - 1});
        return;
    }
    const IRect r = t.takeDirty();
    if (!r.empty()) uploadTerrain(t, r);
}

// Colours changed terrain cells, with a lit rim on exposed tops, shadowed
// undersides and darker rock deeper down.
void Game::uploadTerrain(const Terrain& t, IRect r) {
    if (!terrainTex_) return;
    const int W = kWorldW, H = kWorldH;
    const int x0 = std::max(0, r.x0), x1 = std::min(W - 1, r.x1);
    const int y0 = std::max(0, r.y0 - 1), y1 = std::min(H - 1, r.y1 + 2);
    if (x1 < x0 || y1 < y0) return;
    const uint8_t* c = t.cells();
    for (int y = y0; y <= y1; ++y) {
        const int shade = rowShade_[size_t(y)];
        for (int x = x0; x <= x1; ++x) {
            const size_t i = size_t(y) * size_t(W) + size_t(x);
            const uint8_t v = c[i];
            if (!v) {
                terrainPx_[i] = 0u;
                continue;
            }
            int f = shade;
            if (y == 0 || !c[i - size_t(W)]) f = f * 300 / 256;
            else if (y < 2 || !c[i - 2 * size_t(W)]) f = f * 276 / 256;
            if (y + 1 < H && !c[i + size_t(W)]) f = f * 190 / 256;
            const uint32_t col = palette_[v];
            const uint32_t rr = std::min(255u, (((col >> 16) & 255u) * uint32_t(f)) >> 8);
            const uint32_t gg = std::min(255u, (((col >> 8) & 255u) * uint32_t(f)) >> 8);
            const uint32_t bb = std::min(255u, ((col & 255u) * uint32_t(f)) >> 8);
            terrainPx_[i] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        }
    }
    const SDL_Rect rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    SDL_UpdateTexture(terrainTex_, &rect, &terrainPx_[size_t(y0) * size_t(W) + size_t(x0)], W * 4);
}

void Game::renderWorld(Match& m) {
    syncWorld(m);
    const View base = worldView();
    View v = base;
    if (shake_ > 0.05f) {
        v.ox += frand(-1.f, 1.f) * shake_ * v.s;
        v.oy += frand(-1.f, 1.f) * shake_ * v.s;
    }
    const SDL_FRect world{v.ox, v.oy, float(kWorldW) * v.s, float(kWorldH) * v.s};
    if (sky_) SDL_RenderCopyF(renderer_, sky_, nullptr, &world);

    const bool night = art::themeIsNight(m.theme());
    for (const Cloud& c : cloudList_) {
        SDL_Texture* tex = clouds_[c.variant];
        if (!tex) continue;
        const float w = 170.f * c.scale, h = w * 0.5f;
        const SDL_FRect dst{v.x(c.x - w * 0.5f), v.y(c.y - h * 0.5f), w * v.s, h * v.s};
        SDL_SetTextureColorMod(tex, night ? 80 : 255, night ? 92 : 255, night ? 130 : 255);
        SDL_SetTextureAlphaMod(tex, night ? 100 : 215);
        SDL_RenderCopyF(renderer_, tex, nullptr, &dst);
    }

    renderTanks(m, v, 255);
    if (terrainTex_) SDL_RenderCopyF(renderer_, terrainTex_, nullptr, &world);
    renderShots(m, v);
    renderEffects(v);
    renderTankOverlays(m, v);

    // Letterbox bars (also hide anything shaken past the edge).
    const SDL_Color bar = rgba(14, 10, 8);
    const float wx = base.ox, wy = base.oy, ww = float(kWorldW) * base.s, wh = float(kWorldH) * base.s;
    if (wy > 0.f) fillRect(0.f, 0.f, float(viewW_), wy + 1.f, bar);
    if (wy + wh < float(viewH_)) fillRect(0.f, wy + wh, float(viewW_), float(viewH_) - wy - wh, bar);
    if (wx > 0.f) fillRect(0.f, 0.f, wx, float(viewH_), bar);
    if (wx + ww < float(viewW_)) fillRect(wx + ww, 0.f, float(viewW_) - wx - ww, float(viewH_), bar);
}

void Game::renderTanks(const Match& m, const View& v, Uint8 alpha) {
    ensureTankSprites(v.s);
    const auto& tanks = m.tanks();
    for (size_t i = 0; i < tanks.size(); ++i) {
        const Tank& t = tanks[i];
        const Uint8 tint = t.alive ? 255 : 70;
        if (barrel_[i]) {
            SDL_SetTextureColorMod(barrel_[i], tint, tint, tint);
            SDL_SetTextureAlphaMod(barrel_[i], alpha);
            const SDL_FRect dst{v.x(t.x), v.y(t.y - kPivotH - 2.f), 16.f * v.s, 4.f * v.s};
            const SDL_FPoint pivot{0.f, 2.f * v.s};
            const float angle = t.alive ? t.angle : (t.angle < 90.f ? -15.f : 195.f);  // wrecks droop
            SDL_RenderCopyExF(renderer_, barrel_[i], nullptr, &dst, double(-angle), &pivot, SDL_FLIP_NONE);
        }
        if (body_[i]) {
            SDL_SetTextureColorMod(body_[i], tint, Uint8(tint * (t.alive ? 1.f : 0.92f)), Uint8(tint * (t.alive ? 1.f : 0.85f)));
            SDL_SetTextureAlphaMod(body_[i], alpha);
            const SDL_FRect dst{v.x(t.x - 13.f), v.y(t.y - 14.f), 26.f * v.s, 14.f * v.s};
            SDL_RenderCopyF(renderer_, body_[i], nullptr, &dst);
        }
    }
}

void Game::renderTankOverlays(const Match& m, const View& v) {
    const int u = uiScale();
    const auto& tanks = m.tanks();
    for (size_t i = 0; i < tanks.size(); ++i) {
        const Tank& t = tanks[i];
        if (!t.alive) continue;
        // A buried tank shows through the dirt as a ghost.
        const bool buried = m.buried(int(i)) || m.terrain().solid(int(t.x), int(t.y - 7.f));
        if (buried) {
            const SDL_FRect dst{v.x(t.x - 13.f), v.y(t.y - 14.f), 26.f * v.s, 14.f * v.s};
            SDL_SetTextureAlphaMod(body_[i], 90);
            SDL_RenderCopyF(renderer_, body_[i], nullptr, &dst);
            SDL_SetTextureAlphaMod(body_[i], 255);
            drawText("BURIED", int(v.x(t.x)), int(v.y(t.y + 3.f)), std::max(1, u - 1), rgba(255, 240, 220, 200), 0);
        }
        const float bw = 26.f * v.s, bh = std::max(2.f, 3.5f * v.s);
        const float bx = v.x(t.x - 13.f), by = v.y(t.y - 25.f);
        fillRect(bx - 1.f, by - 1.f, bw + 2.f, bh + 2.f, rgba(0, 0, 0, 150));
        const float frac = clamp01(t.health / kMaxHealth);
        const SDL_Color hc = frac > 0.6f ? rgba(96, 214, 96) : frac > 0.3f ? rgba(240, 200, 60) : rgba(232, 72, 60);
        fillRect(bx, by, bw * frac, bh, hc);
        if (m.phase() == Phase::Aiming && int(i) == m.active()) {
            const int bob = int(std::lround((3.f + 3.f * std::sin(time_ * 6.f)) * float(u)));
            drawText(MUD_ARROW_DOWN, int(v.x(t.x)), int(by) - 10 * u - bob, 2 * u, toColor(art::playerColor(int(i))), 0);
        }
    }
}

void Game::renderShots(const Match& m, const View& v) {
    // The aiming human's previous shot, as faint dots.
    if (m.phase() == Phase::Aiming && !m.isCpu(m.active())) {
        SDL_SetTextureBlendMode(dot_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(dot_, 255, 255, 255);
        SDL_SetTextureAlphaMod(dot_, 110);
        const float d = 4.f * v.s;
        for (const SDL_FPoint& p : lastShot_[size_t(m.active())]) {
            const SDL_FRect dst{v.x(p.x) - d * 0.5f, v.y(p.y) - d * 0.5f, d, d};
            SDL_RenderCopyF(renderer_, dot_, nullptr, &dst);
        }
    }
    // Smoke trails.
    SDL_SetTextureBlendMode(dot_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(dot_, 235, 232, 225);
    for (const Trail& t : trails_) {
        const float fade = t.fade < 0.f ? 1.f : 1.f - t.fade / 0.7f;
        const size_t n = t.pts.size();
        for (size_t j = 0; j < n; ++j) {
            const float k = float(j + 1) / float(n);
            SDL_SetTextureAlphaMod(dot_, Uint8(std::lround(150.f * k * fade)));
            const float d = (2.f + 3.f * (1.f - k)) * v.s;
            const SDL_FRect dst{v.x(t.pts[j].x) - d * 0.5f, v.y(t.pts[j].y) - d * 0.5f, d, d};
            SDL_RenderCopyF(renderer_, dot_, nullptr, &dst);
        }
    }
    // Flying clods of dirt.
    for (const Clod& c : m.clods()) {
        const float size = (1.2f + std::sqrt(float(c.cells)) * 0.7f) * v.s;
        fillRect(v.x(c.x) - size * 0.5f, v.y(c.y) - size * 0.5f, size, size, unpack(palette_[c.color]));
    }
    // Projectiles.
    const int u = uiScale();
    for (const Projectile& p : m.projectiles()) {
        if (p.y < -3.f) {  // above the screen: a marker at the top edge
            drawText(MUD_ARROW_UP, int(v.x(p.x)), int(v.oy) + 15 * u, u, rgba(255, 255, 255, 220), 0);
            continue;
        }
        const bool mud = p.weapon == int(Weapon::Mud);
        const float size = mud ? 9.f : p.bomblet ? 4.5f : p.weapon == int(Weapon::Heavy) ? 8.f : p.weapon == int(Weapon::Cluster) ? 7.f : 6.f;
        if (!mud) {
            const float g = size * 2.8f * v.s;
            SDL_SetTextureBlendMode(dot_, SDL_BLENDMODE_ADD);
            SDL_SetTextureColorMod(dot_, 255, 150, 60);
            SDL_SetTextureAlphaMod(dot_, 150);
            const SDL_FRect glow{v.x(p.x) - g * 0.5f, v.y(p.y) - g * 0.5f, g, g};
            SDL_RenderCopyF(renderer_, dot_, nullptr, &glow);
        }
        if (ball_) {
            const float s = size * v.s;
            if (mud) SDL_SetTextureColorMod(ball_, 140, 92, 52);
            else SDL_SetTextureColorMod(ball_, 70, 68, 64);
            const SDL_FRect dst{v.x(p.x) - s * 0.5f, v.y(p.y) - s * 0.5f, s, s};
            SDL_RenderCopyF(renderer_, ball_, nullptr, &dst);
        }
    }
}

void Game::renderEffects(const View& v) {
    if (fireball_) {
        SDL_SetTextureBlendMode(fireball_, SDL_BLENDMODE_ADD);
        for (const Flash& f : flashes_) {
            const float k = f.t / f.life;
            const float r = f.r * (0.55f + 0.6f * easeOut(f.t / 0.1f));
            SDL_SetTextureAlphaMod(fireball_, Uint8(std::lround(255.f * std::pow(1.f - k, 1.5f))));
            const SDL_FRect dst{v.x(f.x - r), v.y(f.y - r), 2.f * r * v.s, 2.f * r * v.s};
            SDL_RenderCopyF(renderer_, fireball_, nullptr, &dst);
        }
    }
    for (const Particle& p : particles_) {
        const float a = 1.f - p.life / p.maxLife;
        const float size = p.size * v.s;
        if (p.kind == 0) {
            fillRect(v.x(p.x) - size * 0.5f, v.y(p.y) - size * 0.5f, size, size, withAlpha(p.color, std::min(1.f, a * 2.f)));
            continue;
        }
        SDL_SetTextureBlendMode(dot_, p.kind == 2 ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(dot_, p.color.r, p.color.g, p.color.b);
        SDL_SetTextureAlphaMod(dot_, Uint8(std::lround(float(p.color.a) * a)));
        const SDL_FRect dst{v.x(p.x) - size * 0.5f, v.y(p.y) - size * 0.5f, size, size};
        SDL_RenderCopyF(renderer_, dot_, nullptr, &dst);
    }
    SDL_SetTextureBlendMode(dot_, SDL_BLENDMODE_BLEND);
    const int u = uiScale();
    for (const Floater& f : floaters_)
        drawText(f.text, int(v.x(f.x)), int(v.y(f.y)), u + 1, withAlpha(f.color, 1.f - f.t / 1.3f), 0);
}

void Game::renderHud(const Match& m, const View& v) {
    const int u = uiScale();
    const float left = v.ox, top = v.oy, width = float(kWorldW) * v.s, height = float(kWorldH) * v.s;
    const int a = m.active();
    const Tank& t = m.tanks()[size_t(a)];
    const SDL_Color pc = toColor(art::playerColor(a));
    const SDL_Color white = rgba(240, 236, 228), grey = rgba(170, 160, 150);

    fillRect(left, top, width, float(13 * u), rgba(10, 8, 6, 150));
    const int ty = int(top) + 3 * u;
    int x = int(left) + 6 * u;
    fillRect(float(x), float(ty), float(7 * u), float(7 * u), pc);
    x += 11 * u;
    const std::string name = playerName(a);
    drawText(name, x, ty, u, pc);
    drawText("HP " + std::to_string(std::max(0, int(std::ceil(t.health)))), x + textWidth("VIOLET", u) + 8 * u, ty, u, white);

    drawText("ANGLE " + std::to_string(int(std::lround(t.angle))) + MUD_DEGREE, int(left + width * 0.27f), ty, u, white);

    const std::string power = "POWER " + std::to_string(int(std::lround(t.power)));
    const int px = int(left + width * 0.42f);
    drawText(power, px, ty, u, white);
    const float barX = float(px + textWidth("POWER 100", u) + 5 * u), barW = float(36 * u);
    fillRect(barX - 1.f, float(ty) - 1.f, barW + 2.f, float(7 * u) + 2.f, rgba(0, 0, 0, 160));
    fillRect(barX, float(ty), barW * t.power / 100.f, float(7 * u), pc);

    const WeaponInfo& wi = weaponInfo(t.weapon);
    std::string weapon = std::to_string(t.weapon + 1) + " " + wi.name;
    const int ammo = t.ammo[size_t(t.weapon)];
    if (ammo >= 0) weapon += " (" + std::to_string(ammo) + ")";
    drawText(weapon, int(left + width * 0.62f), ty, u, rgba(255, 214, 140));

    std::string wind;
    const float w = m.wind();
    if (m.maxWind() <= 0.f) {
        wind = "WIND OFF";
    } else {
        const int strength = int(std::lround(std::fabs(w) / 4.f));
        const int arrows = strength == 0 ? 0 : std::clamp(int(std::ceil(std::fabs(w) / 18.f)), 1, 4);
        if (strength == 0) wind = "WIND 0";
        else if (w > 0.f) wind = "WIND " + std::to_string(strength) + " " + std::string(size_t(arrows), '>');
        else wind = "WIND " + std::string(size_t(arrows), '<') + " " + std::to_string(strength);
    }
    drawText(wind, int(left + width) - 6 * u, ty, u, rgba(190, 225, 255), 1);

    // Bottom strip: controls or who is aiming, and the round.
    const int by = int(top + height) - 10 * u;
    fillRect(left, float(by - 2 * u), width, float(10 * u), rgba(10, 8, 6, 120));
    std::string help;
    if (m.phase() == Phase::Aiming) {
        if (m.isCpu(a))
            help = name + " (" + controlName(m.control(a)) + ") IS AIMING...";
        else
            help = MUD_ARROW_LEFT MUD_ARROW_RIGHT " AIM   " MUD_ARROW_UP MUD_ARROW_DOWN
                   " POWER   SHIFT FINE   TAB WEAPON   SPACE FIRE   ESC PAUSE   F1 HELP";
    }
    if (!help.empty()) drawText(help, int(left) + 6 * u, by, u, grey);
    drawText("ROUND " + std::to_string(m.round()) + "/" + std::to_string(m.rounds()), int(left + width) - 6 * u, by, u, grey, 1);
}

void Game::renderScoreboard(const Match& m) {
    const bool final = m.phase() == Phase::MatchOver;
    const int u = uiScale();
    const int n = m.playerCount();
    const int w = 232 * u, h = (66 + 12 * n) * u;
    const int x = (viewW_ - w) / 2, y = (viewH_ - h) / 2;
    const float appear = easeOut(m.phaseTime() / 0.3f);
    fillRect(0.f, 0.f, float(viewW_), float(viewH_), rgba(0, 0, 0, Uint8(90.f * appear)));
    drawPanel(x, y, w, h, 7 * u);
    const int cx = x + w / 2;
    drawText(final ? "MATCH OVER" : "ROUND " + std::to_string(m.round()) + " OF " + std::to_string(m.rounds()), cx, y + 7 * u,
             2 * u, rgba(255, 226, 160), 0);
    const int winner = final ? m.matchWinner() : m.roundWinner();
    std::string headline;
    SDL_Color hc = rgba(230, 225, 215);
    if (winner >= 0) {
        headline = playerName(winner) + (final ? " WINS THE MATCH!" : " WINS THE ROUND");
        hc = toColor(art::playerColor(winner));
    } else {
        headline = final ? "IT'S A DRAW!" : "NOBODY SURVIVED";
    }
    drawText(headline, cx, y + 25 * u, u, hc, 0);

    const int colName = x + 14 * u, colRounds = x + w - 74 * u, colKills = x + w - 14 * u;
    int ry = y + 39 * u;
    const SDL_Color grey = rgba(160, 150, 140);
    drawText("PLAYER", colName, ry, u, grey);
    drawText("ROUNDS", colRounds, ry, u, grey, 0);
    drawText("KILLS", colKills, ry, u, grey, 1);
    for (int i = 0; i < n; ++i) {
        ry += 12 * u;
        const SDL_Color pc = toColor(art::playerColor(i));
        fillRect(float(colName), float(ry), float(7 * u), float(7 * u), pc);
        drawText(playerName(i), colName + 11 * u, ry, u, pc);
        drawText(m.isCpu(i) ? controlName(m.control(i)) : "", colName + 11 * u + textWidth("VIOLET ", u), ry, u, grey);
        const PlayerScore& sc = m.scores()[size_t(i)];
        drawText(std::to_string(sc.roundWins), colRounds, ry, u, rgba(240, 236, 228), 0);
        drawText(std::to_string(sc.kills), colKills, ry, u, rgba(240, 236, 228), 1);
    }
    if (m.phaseTime() > 0.5f) {
        const float blink = 0.6f + 0.4f * std::sin(time_ * 4.f);
        drawText(final ? "ENTER - REMATCH     ESC - MENU" : "ENTER - NEXT ROUND", cx, y + h - 11 * u, u,
                 withAlpha(rgba(255, 214, 140), blink), 0);
    }
}

void Game::renderPause() {
    const int u = uiScale();
    fillRect(0.f, 0.f, float(viewW_), float(viewH_), rgba(0, 0, 0, 120));
    const int w = 130 * u, h = 62 * u;
    const int x = (viewW_ - w) / 2, y = (viewH_ - h) / 2;
    drawPanel(x, y, w, h, 7 * u);
    drawText("PAUSED", x + w / 2, y + 8 * u, 2 * u, rgba(255, 226, 160), 0);
    const char* items[2] = {"RESUME", "QUIT TO MENU"};
    for (int i = 0; i < 2; ++i) {
        const bool sel = i == pauseSel_;
        const std::string label = sel ? std::string("> ") + items[i] + " <" : items[i];
        drawText(label, x + w / 2, y + (30 + 13 * i) * u, u, sel ? rgba(255, 214, 140) : rgba(220, 214, 205), 0);
    }
}

// F1: a short legend of every key.
void Game::renderHelp() {
    struct Row {
        const char* keys;
        const char* action;
    };
    static const Row kRows[] = {
        {MUD_ARROW_LEFT MUD_ARROW_RIGHT "  A D", "TURN THE BARREL"},
        {MUD_ARROW_UP MUD_ARROW_DOWN "  W S", "SHOT POWER"},
        {"SHIFT", "FINE ADJUSTMENT"},
        {"TAB  Q E  1-4", "CHOOSE WEAPON"},
        {"SPACE  ENTER", "FIRE"},
        {"ESC", "PAUSE / BACK"},
        {"M", "SOUND ON / OFF"},
        {"F11  ALT+ENTER", "FULLSCREEN"},
        {"F1", "SHOW / HIDE THIS HELP"},
    };
    const int n = int(sizeof(kRows) / sizeof(kRows[0]));
    const int u = uiScale();
    int keyW = 0, actionW = 0;
    for (const Row& r : kRows) {
        keyW = std::max(keyW, textWidth(r.keys, u));
        actionW = std::max(actionW, textWidth(r.action, u));
    }
    const int gap = 12 * u, margin = 16 * u;
    const int w = margin * 2 + keyW + gap + actionW, h = (36 + 11 * n + 16) * u;
    const int x = (viewW_ - w) / 2, y = (viewH_ - h) / 2;
    fillRect(0.f, 0.f, float(viewW_), float(viewH_), rgba(0, 0, 0, 110));
    drawPanel(x, y, w, h, 7 * u);
    drawText("KEYS", x + w / 2, y + 8 * u, 2 * u, rgba(255, 226, 160), 0);
    int ry = y + 32 * u;
    for (const Row& r : kRows) {
        drawText(r.keys, x + margin + keyW, ry, u, rgba(255, 214, 140), 1);
        drawText(r.action, x + margin + keyW + gap, ry, u, rgba(236, 230, 220));
        ry += 11 * u;
    }
    drawText("F1 OR ESC TO CLOSE", x + w / 2, y + h - 12 * u, u, rgba(160, 150, 140), 0);
}

// Cover image: the logo and a tagline over the action, the sky darkened
// behind them so they read.
void Game::renderCover() {
    const int fade = int(float(viewH_) * 0.62f);
    for (int y = 0; y < fade; ++y) {
        const float t = float(y) / float(fade);
        fillRect(0.f, float(y), float(viewW_), 1.f, rgba(10, 7, 5, Uint8(std::lround(165.f * std::pow(1.f - t, 1.6f)))));
    }
    // Small covers (itch's 630x500, shown at 315x250 in listings) need a
    // bigger logo and tagline relative to the frame to stay readable.
    const bool small = viewW_ < 900;
    const int cell = std::max(2, int(float(viewW_) * (small ? 0.8f : 0.6f) / float(art::kLogoW)));
    ensureLogo(cell);
    const int lw = art::kLogoW * cell, lh = art::kLogoH * cell;
    const int ly = int(float(viewH_) * 0.07f);
    if (logo_) {
        const SDL_Rect dst{(viewW_ - lw) / 2, ly, lw, lh};
        SDL_RenderCopy(renderer_, logo_, nullptr, &dst);
    }
    const std::string tagline = "TURN-BASED ARTILLERY ON SLIPPERY GROUND";
    // At least 14 px tall, and as wide as fits in 85% of the frame.
    const int scale = std::max(small ? 2 : 1, std::min(viewW_ / 430, int(float(viewW_) * 0.85f) / textWidth(tagline, 1)));
    const int tw = textWidth(tagline, scale), ty = ly + lh + scale * 3;
    fillRect(float((viewW_ - tw) / 2 - 6 * scale), float(ty - 3 * scale), float(tw + 12 * scale), float(13 * scale),
             rgba(18, 11, 6, 170));
    drawText(tagline, viewW_ / 2, ty, scale, rgba(250, 230, 196), 0);
}

void Game::renderMenu() {
    const int u = uiScale();
    fillRect(0.f, 0.f, float(viewW_), float(viewH_), rgba(6, 8, 14, 140));
    const int cell = std::max(2, std::min(int(float(viewW_) * 0.6f / float(art::kLogoW)), int(float(viewH_) * 0.2f / float(art::kLogoH))));
    ensureLogo(cell);
    const int lw = art::kLogoW * cell, lh = art::kLogoH * cell;
    const int ly = std::max(4 * u, int(float(viewH_) * 0.05f));
    if (logo_) {
        const SDL_Rect dst{(viewW_ - lw) / 2, ly, lw, lh};
        SDL_RenderCopy(renderer_, logo_, nullptr, &dst);
    }
    const int cx = viewW_ / 2;
    drawText("ARTILLERY ON SLIPPERY GROUND", cx, ly + lh + 2 * u, u, rgba(235, 205, 160), 0);

    const std::vector<Item> items = menuItems();
    menuSel_ = std::clamp(menuSel_, 0, int(items.size()) - 1);
    const int rowH = 13 * u;
    int y = ly + lh + 18 * u;
    const int boxW = 230 * u;
    fillRect(float(cx - boxW / 2), float(y - 6 * u), float(boxW), float(rowH * int(items.size()) + 8 * u), rgba(10, 7, 5, 150));
    for (size_t i = 0; i < items.size(); ++i, y += rowH) {
        const Item item = items[i];
        const bool sel = int(i) == menuSel_;
        if (sel) fillRect(float(cx - boxW / 2), float(y - 3 * u), float(boxW), float(rowH), rgba(255, 220, 160, 38));
        const SDL_Color normal = rgba(232, 226, 216), hi = rgba(255, 214, 140);
        std::string label, value;
        SDL_Color labelColor = sel ? hi : normal;
        switch (item) {
            case Item::Start:
                drawText(sel ? "> START MATCH <" : "START MATCH", cx, y, u, labelColor, 0);
                continue;
            case Item::Quit:
                drawText(sel ? "> QUIT <" : "QUIT", cx, y, u, labelColor, 0);
                continue;
            case Item::Players:
                label = "PLAYERS";
                value = std::to_string(settings_.players);
                break;
            case Item::Rounds:
                label = "ROUNDS";
                value = std::to_string(kRoundOptions[settings_.roundChoice]);
                break;
            case Item::Wind:
                label = "WIND";
                value = windName(settings_.wind);
                break;
            default: {
                const int slot = int(item) - int(Item::Slot0);
                label = playerName(slot);
                labelColor = toColor(art::playerColor(slot));
                value = controlName(Control(settings_.control[size_t(slot)]));
                break;
            }
        }
        drawText(label, cx - 8 * u, y, u, labelColor, 1);
        drawText(sel ? std::string(MUD_ARROW_LEFT " ") + value + " " MUD_ARROW_RIGHT : value, cx + 8 * u, y, u, sel ? hi : normal);
    }
    const std::string footer = MUD_ARROW_UP MUD_ARROW_DOWN " SELECT   " MUD_ARROW_LEFT MUD_ARROW_RIGHT
                               " CHANGE   ENTER START   F1 HELP";
    drawText(footer, cx, viewH_ - 12 * u, u, rgba(190, 180, 168), 0);
    if (settings_.muted) drawText("SOUND OFF", viewW_ - 6 * u, 6 * u, u, rgba(190, 180, 168), 1);
    drawText("V" MUD_VERSION, 6 * u, viewH_ - 12 * u, u, rgba(150, 140, 128));
}

void Game::renderCaption() {
    if (caption_.empty() || time_ >= captionEnd_) return;
    const int u = uiScale();
    const float a = std::min({1.f, (time_ - captionStart_) / 0.2f, (captionEnd_ - time_) / 0.4f});
    drawText(caption_, viewW_ / 2, int(float(viewH_) * 0.2f), 2 * u, withAlpha(rgba(255, 240, 210), a), 0);
}

}  // namespace mud
