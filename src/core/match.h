#pragma once

// Match rules and ballistics: tanks, turns, projectiles, explosions, falling
// tanks and rounds. Runs in fixed 1/120 s ticks and has no SDL dependency, so
// whole matches can be simulated headlessly (see tests/test_core.cpp).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/ai.h"
#include "core/rng.h"
#include "core/terrain.h"

namespace mud {

// ---------------------------------------------------------------- setup

enum class Weapon : int { Shell, Heavy, Cluster, Mud };
constexpr int kWeaponCount = 4;

struct WeaponInfo {
    const char* name;
    int ammo;      // per round; -1 = unlimited
    float radius;  // blast radius (or mud ball radius), world pixels
    float damage;  // damage at the centre of the blast
};
const WeaponInfo& weaponInfo(int weapon);

enum class Control : int { Human, CpuEasy, CpuNormal, CpuHard };
constexpr int kControlCount = 4;
const char* controlName(Control c);

constexpr int kMaxPlayers = 4;
constexpr int kWindLevels = 4;  // off, calm, breezy, gale
const char* windName(int level);

struct PlayerSetup {
    std::string name;
    Control control = Control::Human;
};

struct MatchSetup {
    std::vector<PlayerSetup> players;  // 2..4
    int rounds = 3;
    int wind = 2;
    uint64_t seed = 1;
};

// ---------------------------------------------------------------- physics

constexpr float kGravity = 225.f;    // px/s^2
constexpr float kMaxSpeed = 540.f;   // muzzle speed at power 100, px/s
constexpr float kTick = 1.f / 120.f;
constexpr float kBarrelLen = 14.f;
constexpr float kPivotH = 9.f;       // barrel pivot height above the ground row
constexpr float kTankHalfW = 11.f;   // hit box half width
constexpr float kTankH = 13.f;       // hit box height
constexpr float kTreadHalfW = 8.f;   // a tank stands if ground is under its treads
constexpr float kMaxHealth = 100.f;
constexpr float kMinPower = 5.f;

struct Tank {
    int player = 0;
    float x = 0.f, y = 0.f;  // centre of the treads; y is the ground row they rest on
    float angle = 90.f;      // degrees: 0 = right, 90 = up, 180 = left
    float power = 55.f;      // 0..100
    float health = kMaxHealth;
    bool alive = true;
    bool falling = false;
    float fallSpeed = 0.f, fallAcc = 0.f, fallFrom = 0.f;
    int weapon = 0;
    std::array<int, kWeaponCount> ammo{};
    int lastHitBy = -1;  // who to credit if this tank dies
};

struct Projectile {
    int id = 0;
    int weapon = 0;
    int owner = -1;
    float x = 0.f, y = 0.f, vx = 0.f, vy = 0.f;
    float age = 0.f;
    bool bomblet = false;  // cluster fragment
    bool boring = false;   // fired from inside dirt: tunnels out, losing speed
    float boreX = 0.f, boreY = 0.f;
};

// A clump of dirt (or mud) blown out of a crater. It flies ballistically and,
// where it lands, its grains are added back to the terrain as loose soil that
// slides down slopes and can bury tanks.
struct Clod {
    float x = 0.f, y = 0.f, vx = 0.f, vy = 0.f;
    uint8_t color = 0;  // terrain cell value it will deposit
    uint8_t cells = 1;  // grains it carries
};

enum class Phase {
    Aiming,     // the active tank aims (keyboard or CPU)
    Flight,     // projectiles in the air
    Blast,      // explosions play out
    Settle,     // dirt and tanks fall
    Dying,      // destroyed tanks blow up, one at a time
    Between,    // short pause before the next turn
    RoundOver,  // waiting for nextRound()
    MatchOver,
};

enum class EventType { TurnStart, Fire, Bore, Split, Explode, MudSplat, TankHit, TankLand, TankDeath, RoundOver, MatchOver };

// Things the front end may want to show or play.
struct Event {
    EventType type = EventType::TurnStart;
    float x = 0.f, y = 0.f, r = 0.f;
    float amount = 0.f;    // damage dealt (TankHit, TankLand), cells carved (Explode); fall height in r for TankLand
    int tank = -1;         // tank involved (shooter for Fire/Explode, victim for hits)
    int other = -1;        // killer (TankDeath)
    int weapon = 0;        // -1 for a tank blowing up
    uint8_t material = 0;  // terrain cell at an impact, for debris colour
};

struct ShotTrace {
    bool impact = false;
    float x = 0.f, y = 0.f;
    int tankHit = -1;
    float time = 0.f;
};

struct PlayerScore {
    int roundWins = 0;
    int kills = 0;
};

class Match {
public:
    explicit Match(const MatchSetup& setup);

    void update(float dt);  // advances in fixed ticks

    // Input for a human player; ignored unless a human is aiming.
    bool humanAiming() const { return phase_ == Phase::Aiming && !isCpu(active_); }
    void adjustAim(float dAngle, float dPower);
    void selectWeapon(int weapon);
    void cycleWeapon(int dir);
    void fire();

    void nextRound();  // from Phase::RoundOver
    void setAiNoise(bool on) { aiNoise_ = on; }

    const MatchSetup& setup() const { return setup_; }
    int playerCount() const { return int(tanks_.size()); }
    Phase phase() const { return phase_; }
    float phaseTime() const { return phaseTime_; }
    float clock() const { return clock_; }
    int round() const { return round_; }
    int rounds() const { return setup_.rounds; }
    int turnInRound() const { return turnInRound_; }
    int active() const { return active_; }
    int theme() const { return theme_; }
    float wind() const { return wind_; }
    float maxWind() const;
    Control control(int tank) const { return setup_.players[size_t(tank)].control; }
    bool isCpu(int tank) const { return control(tank) != Control::Human; }

    const Terrain& terrain() const { return terrain_; }
    Terrain& terrain() { return terrain_; }
    const std::vector<Tank>& tanks() const { return tanks_; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<Clod>& clods() const { return clods_; }
    const std::vector<PlayerScore>& scores() const { return scores_; }
    int roundWinner() const { return roundWinner_; }  // -1: nobody survived
    int matchWinner() const;                           // -1: tie
    bool buried(int tank) const;                       // muzzle inside dirt

    std::vector<Event> takeEvents();

    // Ballistics shared by real shots and the AI's search.
    Projectile makeProjectile(int tank, float angle, float power, int weapon) const;
    ShotTrace traceShot(int tank, float angle, float power, std::vector<float>* path = nullptr) const;

    // Test hook.
    Tank& tankForTest(int i) { return tanks_[size_t(i)]; }

private:
    enum class Step { Flying, Impact, Lost };
    struct Hit {
        float x = 0.f, y = 0.f;
        int tank = -1;
    };

    void tick();
    void setPhase(Phase p);
    void push(const Event& e) { events_.push_back(e); }
    void startRound();
    void beginTurn();
    void finishRound();
    void launch();
    void cpuAim();
    void stepFlight();
    void stepSettle();
    void afterSettle();
    void endShot();
    void killNext();
    void updateTanks();
    bool supported(const Tank& t) const;
    void land(int tank);
    Step stepProjectile(Projectile& p, Hit& hit) const;
    void explodeProjectile(const Projectile& p, const Hit& hit);
    void split(const Projectile& p, std::vector<Projectile>& out);
    void blast(float x, float y, float r, float damage, int owner, int direct, int weapon);
    void throwDirt(float cx, float r, const std::vector<Grain>& grains, int total, float vmin, float vmax);
    void stepClods();
    void damage(int tank, float amount, int owner);
    uint8_t sampleMaterial(float x, float y) const;
    int nextAlive(int from) const;
    int aliveCount() const;

    MatchSetup setup_;
    Rng rng_;
    Terrain terrain_;
    std::vector<Tank> tanks_;
    std::vector<Projectile> projectiles_;
    std::vector<Clod> clods_;
    std::vector<Grain> grains_;  // scratch for carve()
    std::vector<PlayerScore> scores_;
    std::vector<AiMemory> memory_;
    std::vector<Event> events_;

    Phase phase_ = Phase::Between;
    float phaseTime_ = 0.f;
    float clock_ = 0.f;
    float acc_ = 0.f;
    int round_ = 0;
    int theme_ = 0;
    uint32_t themeBase_ = 0;
    int turnInRound_ = 0;
    int active_ = 0;
    int shooter_ = -1;
    float wind_ = 0.f;
    int roundWinner_ = -1;
    int nextId_ = 1;
    float lastBlast_ = -10.f;
    float settleSpeed_ = 0.f, settleAcc_ = 0.f;
    float betweenWait_ = 0.f;
    bool roundEnding_ = false;
    bool advance_ = false;

    Planner planner_;
    Plan plan_;
    bool planReady_ = false;
    float cpuSettledAt_ = -1.f;
    bool aiNoise_ = true;
};

}  // namespace mud
