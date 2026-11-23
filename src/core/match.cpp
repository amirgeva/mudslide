#include "core/match.h"

#include <algorithm>
#include <cmath>

namespace mud {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.f;
constexpr float kSideMargin = 200.f;   // a shell this far past the edge is gone
constexpr float kBlastTime = 0.40f;    // let a fireball play before dirt falls
constexpr float kBoreDrag = 0.986f;    // speed kept per pixel tunnelled through dirt
constexpr float kBombletRadius = 15.f;
constexpr float kBombletDamage = 20.f;
constexpr float kDeathRadius = 32.f;
constexpr float kDeathDamage = 30.f;
constexpr float kMaxFlight = 20.f;
constexpr int kMaxTurns = 160;         // per round, then it is called a draw
constexpr int kTracesPerTick = 20;     // AI search budget
constexpr float kEjectFraction = 0.85f;  // share of a crater thrown out as clods (the rest is blasted to dust)
constexpr int kMaxClodsPerBlast = 160;
constexpr size_t kMaxClods = 1500;

const WeaponInfo kWeapons[kWeaponCount] = {
    {"SHELL", -1, 22.f, 34.f},
    {"HEAVY SHELL", 2, 44.f, 58.f},
    {"CLUSTER", 2, 24.f, 30.f},  // if it lands before bursting
    {"MUD BALL", 3, 40.f, 10.f},
};
const char* const kControlNames[kControlCount] = {"HUMAN", "CPU EASY", "CPU NORMAL", "CPU HARD"};
const char* const kWindNames[kWindLevels] = {"OFF", "CALM", "BREEZY", "GALE"};
const float kWindMax[kWindLevels] = {0.f, 19.f, 41.f, 71.f};

float boxDistance(const Tank& t, float x, float y) {
    const float cx = std::clamp(x, t.x - kTankHalfW, t.x + kTankHalfW);
    const float cy = std::clamp(y, t.y - kTankH, t.y);
    return std::hypot(x - cx, y - cy);
}

}  // namespace

const WeaponInfo& weaponInfo(int w) { return kWeapons[std::clamp(w, 0, kWeaponCount - 1)]; }
const char* controlName(Control c) { return kControlNames[std::clamp(int(c), 0, kControlCount - 1)]; }
const char* windName(int level) { return kWindNames[std::clamp(level, 0, kWindLevels - 1)]; }

Match::Match(const MatchSetup& setup) : setup_(setup), rng_(setup.seed ^ 0xA5A5F00Dull) {
    if (setup_.players.size() < 2) setup_.players.resize(2);
    if (setup_.players.size() > size_t(kMaxPlayers)) setup_.players.resize(size_t(kMaxPlayers));
    setup_.rounds = std::max(1, setup_.rounds);
    setup_.wind = std::clamp(setup_.wind, 0, kWindLevels - 1);
    const size_t n = setup_.players.size();
    tanks_.resize(n);
    scores_.resize(n);
    memory_.resize(n);
    themeBase_ = rng_.below(4);
    startRound();
}

float Match::maxWind() const { return kWindMax[setup_.wind]; }

int Match::matchWinner() const {
    int best = -1;
    bool tie = false;
    for (int i = 0; i < playerCount(); ++i) {
        if (best < 0) {
            best = i;
            continue;
        }
        const PlayerScore &a = scores_[size_t(i)], &b = scores_[size_t(best)];
        if (a.roundWins > b.roundWins || (a.roundWins == b.roundWins && a.kills > b.kills)) {
            best = i;
            tie = false;
        } else if (a.roundWins == b.roundWins && a.kills == b.kills) {
            tie = true;
        }
    }
    return tie ? -1 : best;
}

bool Match::buried(int tank) const {
    const Tank& t = tanks_[size_t(tank)];
    const float a = t.angle * kDeg;
    const float mx = t.x + std::cos(a) * kBarrelLen, my = t.y - kPivotH - std::sin(a) * kBarrelLen;
    return terrain_.solid(int(std::floor(mx)), int(std::floor(my)));
}

std::vector<Event> Match::takeEvents() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

// ---------------------------------------------------------------- flow

void Match::update(float dt) {
    acc_ += std::clamp(dt, 0.f, 0.25f);
    int n = 0;
    while (acc_ >= kTick && n < 60) {
        acc_ -= kTick;
        tick();
        ++n;
    }
}

void Match::setPhase(Phase p) {
    phase_ = p;
    phaseTime_ = 0.f;
    if (p == Phase::Settle) {
        settleSpeed_ = 90.f;
        settleAcc_ = 0.f;
    }
}

void Match::tick() {
    clock_ += kTick;
    phaseTime_ += kTick;
    updateTanks();
    stepClods();
    switch (phase_) {
        case Phase::Aiming:
            if (isCpu(active_)) cpuAim();
            break;
        case Phase::Flight:
            stepFlight();
            break;
        case Phase::Blast:
            if (clock_ - lastBlast_ >= kBlastTime) setPhase(Phase::Settle);
            break;
        case Phase::Settle:
            stepSettle();
            break;
        case Phase::Dying:
            if (phaseTime_ >= 0.35f) killNext();
            break;
        case Phase::Between:
            if (phaseTime_ >= betweenWait_) {
                if (roundEnding_) {
                    finishRound();
                } else {
                    if (advance_) active_ = nextAlive(active_);
                    advance_ = false;
                    beginTurn();
                }
            }
            break;
        case Phase::RoundOver:
        case Phase::MatchOver:
            break;
    }
}

void Match::startRound() {
    ++round_;
    theme_ = int((themeBase_ + uint32_t(round_ - 1)) % 4u);
    terrain_.generate(hash64(setup_.seed, uint64_t(round_)), theme_);
    projectiles_.clear();
    clods_.clear();

    // Spread the tanks out, in a random order, each on a small level pad.
    const int n = playerCount();
    std::vector<int> order(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) order[size_t(i)] = i;
    for (int i = n - 1; i > 0; --i) std::swap(order[size_t(i)], order[size_t(rng_.below(uint32_t(i + 1)))]);
    const float margin = 55.f;
    const float span = (float(kWorldW) - 2.f * margin) / float(n);
    for (int s = 0; s < n; ++s) {
        const int p = order[size_t(s)];
        Tank& t = tanks_[size_t(p)];
        t = Tank{};
        t.player = p;
        t.x = std::round(margin + span * (float(s) + 0.5f) + rng_.uniform(-0.22f, 0.22f) * span);
        int ys[25];
        for (int d = -12; d <= 12; ++d) ys[d + 12] = terrain_.surface(int(t.x) + d);
        std::nth_element(ys, ys + 12, ys + 25);
        const int y = std::clamp(ys[12], 60, kWorldH - 20);
        terrain_.flattenPad(int(t.x) - 14, int(t.x) + 14, y);
        t.y = float(y);
        t.angle = t.x < float(kWorldW) / 2.f ? 60.f : 120.f;
        for (int w = 0; w < kWeaponCount; ++w) t.ammo[size_t(w)] = kWeapons[w].ammo;
    }

    wind_ = rng_.uniform(-1.f, 1.f) * maxWind();
    turnInRound_ = 0;
    roundWinner_ = -1;
    roundEnding_ = false;
    advance_ = false;
    shooter_ = -1;
    active_ = (round_ - 1) % n;
    betweenWait_ = 0.9f;
    setPhase(Phase::Between);
}

void Match::nextRound() {
    if (phase_ == Phase::RoundOver) startRound();
}

void Match::beginTurn() {
    ++turnInRound_;
    if (turnInRound_ > 1) wind_ = std::clamp(wind_ + rng_.gauss() * 0.2f * maxWind(), -maxWind(), maxWind());
    Tank& t = tanks_[size_t(active_)];
    if (t.ammo[size_t(t.weapon)] == 0) t.weapon = 0;
    shooter_ = active_;
    planReady_ = false;
    cpuSettledAt_ = -1.f;
    if (isCpu(active_)) planner_.start(*this, active_, rng_, memory_[size_t(active_)], aiNoise_);
    setPhase(Phase::Aiming);
    Event e;
    e.type = EventType::TurnStart;
    e.tank = active_;
    e.x = t.x;
    e.y = t.y;
    push(e);
}

void Match::endShot() {
    roundEnding_ = aliveCount() <= 1 || turnInRound_ >= kMaxTurns;
    betweenWait_ = roundEnding_ ? 1.6f : 0.45f;
    advance_ = true;
    setPhase(Phase::Between);
}

void Match::finishRound() {
    int winner = -1;
    for (int i = 0; i < playerCount(); ++i)
        if (tanks_[size_t(i)].alive) winner = i;
    if (aliveCount() != 1) winner = -1;
    roundWinner_ = winner;
    if (winner >= 0) ++scores_[size_t(winner)].roundWins;
    Event e;
    e.type = EventType::RoundOver;
    e.tank = winner;
    push(e);
    if (round_ >= setup_.rounds) {
        setPhase(Phase::MatchOver);
        Event m;
        m.type = EventType::MatchOver;
        m.tank = matchWinner();
        push(m);
    } else {
        setPhase(Phase::RoundOver);
    }
}

int Match::nextAlive(int from) const {
    const int n = playerCount();
    for (int k = 1; k <= n; ++k) {
        const int j = (from + k) % n;
        if (tanks_[size_t(j)].alive) return j;
    }
    return from;
}

int Match::aliveCount() const {
    int n = 0;
    for (const Tank& t : tanks_) n += t.alive ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------- aiming

void Match::adjustAim(float dAngle, float dPower) {
    if (!humanAiming()) return;
    Tank& t = tanks_[size_t(active_)];
    t.angle = std::clamp(t.angle + dAngle, 0.f, 180.f);
    t.power = std::clamp(t.power + dPower, kMinPower, 100.f);
}

void Match::selectWeapon(int weapon) {
    if (!humanAiming() || weapon < 0 || weapon >= kWeaponCount) return;
    Tank& t = tanks_[size_t(active_)];
    if (t.ammo[size_t(weapon)] != 0) t.weapon = weapon;
}

void Match::cycleWeapon(int dir) {
    if (!humanAiming()) return;
    Tank& t = tanks_[size_t(active_)];
    for (int k = 1; k <= kWeaponCount; ++k) {
        const int w = ((t.weapon + dir * k) % kWeaponCount + kWeaponCount) % kWeaponCount;
        if (t.ammo[size_t(w)] != 0) {
            t.weapon = w;
            return;
        }
    }
}

void Match::fire() {
    if (humanAiming()) launch();
}

void Match::cpuAim() {
    Tank& t = tanks_[size_t(active_)];
    if (!planner_.done()) {
        planner_.work(*this, kTracesPerTick);
        return;
    }
    if (!planReady_) {
        plan_ = planner_.plan();
        planReady_ = true;
        if (t.ammo[size_t(plan_.weapon)] != 0) t.weapon = plan_.weapon;
    }
    if (phaseTime_ < 0.5f) return;
    // Swing the turret and power over to the plan, then fire.
    const float da = plan_.angle - t.angle, dp = plan_.power - t.power;
    const float sa = 80.f * kTick, sp = 60.f * kTick;
    t.angle += std::clamp(da, -sa, sa);
    t.power += std::clamp(dp, -sp, sp);
    if (std::fabs(da) <= sa && std::fabs(dp) <= sp) {
        t.angle = plan_.angle;
        t.power = plan_.power;
        if (cpuSettledAt_ < 0.f) cpuSettledAt_ = phaseTime_;
        else if (phaseTime_ - cpuSettledAt_ >= 0.35f) launch();
    }
}

void Match::launch() {
    Tank& t = tanks_[size_t(active_)];
    int w = t.weapon;
    if (t.ammo[size_t(w)] == 0) w = t.weapon = 0;
    if (t.ammo[size_t(w)] > 0) --t.ammo[size_t(w)];
    Projectile p = makeProjectile(active_, t.angle, t.power, w);
    p.id = nextId_++;
    projectiles_.push_back(p);
    shooter_ = active_;
    Event e;
    e.type = EventType::Fire;
    e.x = p.x;
    e.y = p.y;
    e.tank = active_;
    e.weapon = w;
    e.r = t.angle;
    push(e);
    setPhase(Phase::Flight);
}

// ---------------------------------------------------------------- ballistics

Projectile Match::makeProjectile(int tank, float angle, float power, int weapon) const {
    const Tank& t = tanks_[size_t(tank)];
    const float a = angle * kDeg;
    const float dx = std::cos(a), dy = -std::sin(a);
    Projectile p;
    p.weapon = weapon;
    p.owner = tank;
    p.x = t.x + dx * kBarrelLen;
    p.y = t.y - kPivotH + dy * kBarrelLen;
    const float v = std::clamp(power, 0.f, 100.f) / 100.f * kMaxSpeed;
    p.vx = dx * v;
    p.vy = dy * v;
    p.boring = terrain_.solid(int(std::floor(p.x)), int(std::floor(p.y)));
    p.boreX = p.x;
    p.boreY = p.y;
    return p;
}

// One tick of flight, checked pixel by pixel so nothing tunnels through thin
// ground or a tank.
Match::Step Match::stepProjectile(Projectile& p, Hit& hit) const {
    p.vx += wind_ * kTick;
    p.vy += kGravity * kTick;
    float dx = p.vx * kTick, dy = p.vy * kTick;
    const int n = std::max(1, int(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
    dx /= float(n);
    dy /= float(n);
    p.age += kTick;
    for (int i = 0; i < n; ++i) {
        p.x += dx;
        p.y += dy;
        if (p.x < -kSideMargin || p.x > float(kWorldW) + kSideMargin || p.y > float(kWorldH) + 40.f) return Step::Lost;
        if (terrain_.solid(int(std::floor(p.x)), int(std::floor(p.y)))) {
            if (p.boring) {
                // Fired from inside dirt: drill out, losing speed.
                p.vx *= kBoreDrag;
                p.vy *= kBoreDrag;
                dx *= kBoreDrag;
                dy *= kBoreDrag;
                if (p.vx * p.vx + p.vy * p.vy > 40.f * 40.f) continue;
            }
            hit = Hit{p.x, p.y, -1};
            return Step::Impact;
        }
        p.boring = false;
        for (size_t k = 0; k < tanks_.size(); ++k) {
            const Tank& t = tanks_[k];
            if (!t.alive) continue;
            if (int(k) == p.owner && p.age < 0.25f) continue;
            if (p.x >= t.x - kTankHalfW && p.x <= t.x + kTankHalfW && p.y >= t.y - kTankH && p.y < t.y) {
                hit = Hit{p.x, p.y, int(k)};
                return Step::Impact;
            }
        }
    }
    return Step::Flying;
}

ShotTrace Match::traceShot(int tank, float angle, float power, std::vector<float>* path) const {
    Projectile p = makeProjectile(tank, angle, power, 0);
    ShotTrace r;
    const int maxTicks = int(kMaxFlight / kTick);
    for (int i = 0; i < maxTicks; ++i) {
        Hit hit;
        const Step s = stepProjectile(p, hit);
        if (path) {
            path->push_back(p.x);
            path->push_back(p.y);
        }
        if (s == Step::Impact) {
            r.impact = true;
            r.x = hit.x;
            r.y = hit.y;
            r.tankHit = hit.tank;
            r.time = float(i + 1) * kTick;
            return r;
        }
        if (s == Step::Lost) return r;
    }
    return r;
}

void Match::stepFlight() {
    std::vector<Projectile> spawned;
    for (size_t i = 0; i < projectiles_.size();) {
        Projectile& p = projectiles_[i];
        const bool wasBoring = p.boring;
        Hit hit;
        const Step s = stepProjectile(p, hit);
        if (wasBoring && (!p.boring || s != Step::Flying)) {
            terrain_.carveLine(p.boreX, p.boreY, p.x, p.y, 4.f);
            Event e;
            e.type = EventType::Bore;
            e.x = p.x;
            e.y = p.y;
            e.tank = p.owner;
            e.material = sampleMaterial(p.x, p.y);
            push(e);
        }
        bool remove = false;
        if (s == Step::Impact) {
            explodeProjectile(p, hit);
            remove = true;
        } else if (s == Step::Lost || p.age > kMaxFlight) {
            remove = true;
        } else if (p.weapon == int(Weapon::Cluster) && !p.bomblet && p.vy > 0.f && p.age > 0.4f) {
            split(p, spawned);
            remove = true;
        }
        if (remove) projectiles_.erase(projectiles_.begin() + long(i));
        else ++i;
    }
    projectiles_.insert(projectiles_.end(), spawned.begin(), spawned.end());
    if (projectiles_.empty()) setPhase(Phase::Blast);
}

void Match::split(const Projectile& p, std::vector<Projectile>& out) {
    static const float kSpreadX[5] = {-95.f, -48.f, 0.f, 48.f, 95.f};
    static const float kSpreadY[5] = {-30.f, -12.f, 0.f, -12.f, -30.f};
    for (int k = 0; k < 5; ++k) {
        Projectile b = p;
        b.id = nextId_++;
        b.bomblet = true;
        b.boring = false;
        b.vx += kSpreadX[k];
        b.vy += kSpreadY[k];
        out.push_back(b);
    }
    Event e;
    e.type = EventType::Split;
    e.x = p.x;
    e.y = p.y;
    e.tank = p.owner;
    push(e);
}

// ---------------------------------------------------------------- explosions

uint8_t Match::sampleMaterial(float x, float y) const {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    for (int dy = 0; dy <= 8; dy += 2)
        for (int dx : {0, -3, 3}) {
            const uint8_t c = terrain_.at(ix + dx, iy + dy);
            if (c != kAir) return c;
        }
    return kAir;
}

void Match::explodeProjectile(const Projectile& p, const Hit& hit) {
    const WeaponInfo& wi = kWeapons[p.weapon];
    const float r = p.bomblet ? kBombletRadius : wi.radius;
    const float dmg = p.bomblet ? kBombletDamage : wi.damage;
    if (p.weapon != int(Weapon::Mud)) {
        blast(hit.x, hit.y, r, dmg, p.owner, hit.tank, p.weapon);
        return;
    }
    // Mud ball: most of it lands as a heap, the rest splashes around.
    Event e;
    e.type = EventType::MudSplat;
    e.x = hit.x;
    e.y = hit.y;
    e.r = r;
    e.tank = p.owner;
    e.weapon = p.weapon;
    e.material = kMudFirst + 4;
    push(e);
    const float heap = r * 0.9f;
    const int added = terrain_.addMud(hit.x, hit.y, heap);
    // The whole ball always lands: whatever did not fit in the heap (the part
    // that would be underground) splashes out around it, and at least a
    // quarter splashes anyway.
    const int volume = int(kPi * r * r * 0.85f);
    grains_.clear();
    for (int k = 0; k < 64; ++k) {
        const float a = rng_.uniform(-kPi + 0.25f, -0.25f);
        const float d = heap + 1.5f;
        grains_.push_back(Grain{int16_t(std::floor(hit.x + std::cos(a) * d)), int16_t(std::floor(hit.y + std::sin(a) * d)),
                                uint8_t(kMudFirst + rng_.below(kShades))});
    }
    throwDirt(hit.x, r, grains_, std::max(volume - added, volume / 4), 55.f, 190.f);
    if (hit.tank >= 0) damage(hit.tank, dmg, p.owner);
    lastBlast_ = clock_;
}

void Match::blast(float x, float y, float r, float dmg, int owner, int direct, int weapon) {
    Event e;
    e.type = EventType::Explode;
    e.x = x;
    e.y = y;
    e.r = r;
    e.tank = owner;
    e.weapon = weapon;
    e.material = sampleMaterial(x, y);
    grains_.clear();
    const int removed = terrain_.carve(x, y, r, &grains_);
    // Slowest clods just clear the crater (range ~1.3 r); most land within a
    // couple of radii as a raised rim, a few fly far.
    const float vmin = std::sqrt(1.3f * r * kGravity);
    throwDirt(x, r, grains_, int(float(removed) * kEjectFraction), vmin, vmin * 2.6f);
    e.amount = float(removed);
    push(e);
    lastBlast_ = clock_;
    for (size_t k = 0; k < tanks_.size(); ++k) {
        const Tank& t = tanks_[k];
        if (!t.alive) continue;
        const float d = int(k) == direct ? 0.f : boxDistance(t, x, y);
        if (d >= r) continue;
        damage(int(k), dmg * (1.f - 0.6f * d / r), owner);
    }
}

// Throws `total` grains out of a crater (or a mud splash) as clods, launched
// up and away from the centre line: steep near the middle, flatter toward
// the edge. Speeds are skewed low, so most of the dirt lands close by.
void Match::throwDirt(float cx, float r, const std::vector<Grain>& grains, int total, float vmin, float vmax) {
    if (grains.empty() || total <= 0 || clods_.size() >= kMaxClods) return;
    const int count = std::clamp(total / 12, 1, kMaxClodsPerBlast);
    const int base = std::min(total / count, 254), extra = total - base * count;
    for (int i = 0; i < count && clods_.size() < kMaxClods; ++i) {
        const Grain& g = grains[rng_.below(uint32_t(grains.size()))];
        const float gx = float(g.x) + 0.5f, gy = float(g.y) + 0.5f;
        const float dx = gx - cx;
        const float side = dx > 0.5f ? 1.f : dx < -0.5f ? -1.f : (rng_.uniform() < 0.5f ? -1.f : 1.f);
        const float reach = std::min(1.f, std::fabs(dx) / std::max(1.f, r));
        const float elev = (72.f - 37.f * reach + rng_.uniform(-12.f, 12.f)) * kDeg;
        const float u = rng_.uniform();
        const float v = vmin + (vmax - vmin) * u * u;
        Clod c;
        c.x = gx;
        c.y = gy;
        c.vx = side * std::cos(elev) * v;
        c.vy = -std::sin(elev) * v;
        c.color = looseOf(g.color);
        c.cells = uint8_t(std::min(255, base + (i < extra ? 1 : 0)));
        clods_.push_back(c);
    }
}

// Clods fly like light shells; where one hits the ground its grains are
// dropped there as loose soil, which then settles and slides.
void Match::stepClods() {
    for (size_t i = 0; i < clods_.size();) {
        Clod& c = clods_[i];
        c.vx += wind_ * 0.35f * kTick;
        c.vy += kGravity * kTick;
        float dx = c.vx * kTick, dy = c.vy * kTick;
        const int n = std::max(1, int(std::ceil(std::max(std::fabs(dx), std::fabs(dy)))));
        dx /= float(n);
        dy /= float(n);
        bool done = false;
        for (int s = 0; s < n && !done; ++s) {
            const float nx = c.x + dx, ny = c.y + dy;
            if (nx < -2.f || nx > float(kWorldW) + 2.f) {
                done = true;  // off the side of the world
            } else if (terrain_.solid(int(std::floor(nx)), int(std::floor(ny)))) {
                terrain_.deposit(c.x, c.y, c.color, c.cells);
                done = true;
            } else {
                c.x = nx;
                c.y = ny;
            }
        }
        if (done) {
            clods_[i] = clods_.back();
            clods_.pop_back();
        } else {
            ++i;
        }
    }
}

void Match::damage(int tank, float amount, int owner) {
    if (amount <= 0.f) return;
    Tank& t = tanks_[size_t(tank)];
    t.health -= amount;
    t.lastHitBy = owner;
    Event e;
    e.type = EventType::TankHit;
    e.x = t.x;
    e.y = t.y - kTankH;
    e.amount = amount;
    e.tank = tank;
    e.other = owner;
    push(e);
}

// ---------------------------------------------------------------- settling

void Match::stepSettle() {
    settleSpeed_ = std::min(settleSpeed_ + 1200.f * kTick, 1100.f);
    settleAcc_ += settleSpeed_ * kTick;
    int passes = 0;
    while (settleAcc_ >= 1.f && terrain_.settling() && passes < 16) {
        terrain_.settleStep();
        settleAcc_ -= 1.f;
        ++passes;
    }
    if (terrain_.settling() || !clods_.empty()) return;
    settleAcc_ = 0.f;
    for (const Tank& t : tanks_)
        if (t.falling) return;
    if (phaseTime_ < 0.1f) return;
    afterSettle();
}

void Match::afterSettle() {
    for (const Tank& t : tanks_)
        if (t.alive && t.health <= 0.f) {
            setPhase(Phase::Dying);
            return;
        }
    endShot();
}

void Match::killNext() {
    for (int k = 0; k < playerCount(); ++k) {
        Tank& t = tanks_[size_t(k)];
        if (!t.alive || t.health > 0.f) continue;
        t.alive = false;
        t.health = 0.f;
        const int killer = t.lastHitBy;
        if (killer >= 0 && killer != k) ++scores_[size_t(killer)].kills;
        Event e;
        e.type = EventType::TankDeath;
        e.tank = k;
        e.other = killer;
        e.x = t.x;
        e.y = t.y - 6.f;
        push(e);
        blast(t.x, t.y - 6.f, kDeathRadius, kDeathDamage, killer >= 0 ? killer : k, -1, -1);
        setPhase(Phase::Blast);
        return;
    }
    endShot();
}

bool Match::supported(const Tank& t) const {
    const int y = int(t.y);
    if (y >= kWorldH) return true;
    const int x0 = int(std::floor(t.x - kTreadHalfW)), x1 = int(std::floor(t.x + kTreadHalfW));
    for (int x = x0; x <= x1; ++x)
        if (terrain_.solid(x, y)) return true;
    return false;
}

void Match::updateTanks() {
    for (size_t i = 0; i < tanks_.size(); ++i) {
        Tank& t = tanks_[i];
        if (supported(t)) {
            if (t.falling) land(int(i));
            continue;
        }
        if (!t.falling) {
            t.falling = true;
            t.fallFrom = t.y;
            t.fallSpeed = 0.f;
            t.fallAcc = 0.f;
        }
        t.fallSpeed = std::min(t.fallSpeed + 700.f * kTick, 700.f);
        t.fallAcc += t.fallSpeed * kTick;
        while (t.fallAcc >= 1.f && !supported(t)) {
            t.fallAcc -= 1.f;
            t.y += 1.f;
        }
        if (supported(t)) land(int(i));
    }
}

void Match::land(int tank) {
    Tank& t = tanks_[size_t(tank)];
    t.falling = false;
    t.fallSpeed = t.fallAcc = 0.f;
    const float dist = t.y - t.fallFrom;
    Event e;
    e.type = EventType::TankLand;
    e.x = t.x;
    e.y = t.y;
    e.r = dist;
    e.tank = tank;
    if (t.alive && dist > 14.f) {
        const float dmg = std::min(50.f, (dist - 14.f) * 0.5f);
        t.health -= dmg;
        e.amount = dmg;
        if (shooter_ >= 0) t.lastHitBy = shooter_;
    }
    if (dist >= 3.f) push(e);
}

}  // namespace mud
