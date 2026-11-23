#include "core/ai.h"

#include <algorithm>
#include <cmath>

#include "core/match.h"

namespace mud {
namespace {

struct Skill {
    float angleSigma, powerSigma;  // aiming error on a first shot
    float minFactor;               // how far that error can shrink with practice
};

Skill skillFor(Control c) {
    switch (c) {
        case Control::CpuEasy: return {7.f, 9.f, 0.55f};
        case Control::CpuNormal: return {3.2f, 4.5f, 0.3f};
        case Control::CpuHard: return {1.3f, 1.8f, 0.2f};
        default: return {0.f, 0.f, 0.f};
    }
}

float centreDistance(const Tank& t, float x, float y) { return std::hypot(x - t.x, y - (t.y - 6.f)); }

}  // namespace

void Planner::start(const Match& m, int tank, Rng& rng, AiMemory& memory, bool noise) {
    tank_ = tank;
    const std::vector<Tank>& tanks = m.tanks();
    const Tank& me = tanks[size_t(tank)];
    const Control skill = m.control(tank);
    plan_ = Plan{};
    plan_.angle = me.angle;
    plan_.power = me.power;

    // Target: Easy picks anyone, Normal the nearest, Hard the most hurt.
    int target = -1;
    float bestScore = 1e30f;
    for (size_t k = 0; k < tanks.size(); ++k) {
        const Tank& t = tanks[k];
        if (int(k) == tank || !t.alive) continue;
        const float dx = std::fabs(t.x - me.x);
        const float hp = std::max(0.f, t.health);
        float s;
        switch (skill) {
            case Control::CpuEasy: s = rng.uniform() * 1000.f; break;
            case Control::CpuNormal: s = dx + hp * 1.5f; break;
            default: s = hp * 2.5f + dx * 0.4f; break;
        }
        if (s < bestScore) {
            bestScore = s;
            target = int(k);
        }
    }
    plan_.target = target;
    if (target < 0) {
        stage_ = Stage::Done;
        return;
    }
    const Tank& tgt = tanks[size_t(target)];

    // Weapon.
    auto has = [&](Weapon w) { return me.ammo[size_t(w)] != 0; };
    int crowd = 0;  // other enemies close to the target
    for (size_t k = 0; k < tanks.size(); ++k)
        if (int(k) != tank && int(k) != target && tanks[k].alive && std::fabs(tanks[k].x - tgt.x) < 110.f) ++crowd;
    Weapon w = Weapon::Shell;
    const float r = rng.uniform();
    if (skill == Control::CpuEasy) {
        if (r < 0.15f && has(Weapon::Heavy)) w = Weapon::Heavy;
        else if (r < 0.27f && has(Weapon::Cluster)) w = Weapon::Cluster;
        else if (r < 0.36f && has(Weapon::Mud)) w = Weapon::Mud;
    } else if (tgt.health > 36.f) {
        const bool hard = skill == Control::CpuHard;
        if (crowd > 0 && has(Weapon::Cluster)) w = Weapon::Cluster;
        else if (has(Weapon::Heavy) && r < (hard ? 0.65f : 0.4f)) w = Weapon::Heavy;
        else if (has(Weapon::Cluster) && r < (hard ? 0.85f : 0.6f)) w = Weapon::Cluster;
        else if (has(Weapon::Mud) && tgt.y > me.y + 50.f && r < 0.95f) w = Weapon::Mud;  // bury it from above
    }
    plan_.weapon = int(w);
    safeRadius_ = weaponInfo(int(w)).radius + 18.f;
    if (w == Weapon::Cluster) safeRadius_ = 70.f;

    // Aiming error, shrinking with consecutive shots at the same target.
    const Skill sk = skillFor(skill);
    float factor = 1.f;
    if (memory.target == target) {
        factor = std::max(sk.minFactor, std::pow(0.6f, float(memory.shots)));
        ++memory.shots;
    } else {
        memory.target = target;
        memory.shots = 1;
    }
    const float ga = rng.gauss(), gp = rng.gauss();
    noiseAngle_ = noise ? ga * sk.angleSigma * factor : 0.f;
    noisePower_ = noise ? gp * sk.powerSigma * factor : 0.f;

    // Coarse grid over the half-plane facing the target (plus high lobs).
    queue_.clear();
    best_.clear();
    next_ = 0;
    bestRefined_ = Cand{me.angle, me.power, 1e30f};
    const bool right = tgt.x > me.x;
    const int a0 = right ? 4 : 40, a1 = right ? 140 : 176;
    for (int a = a0; a <= a1; a += 4)
        for (int p = 12; p <= 100; p += 4) queue_.push_back(Cand{float(a), float(p), 0.f});
    stage_ = Stage::Coarse;
}

bool Planner::work(const Match& m, int budget) {
    while (budget-- > 0 && stage_ != Stage::Done) {
        if (next_ >= queue_.size()) {
            advance();
            continue;
        }
        Cand& c = queue_[next_++];
        c.score = evaluate(m, c.angle, c.power);
        consider(c);
    }
    return stage_ == Stage::Done;
}

float Planner::evaluate(const Match& m, float angle, float power) const {
    const ShotTrace tr = m.traceShot(tank_, angle, power);
    if (!tr.impact) return 1e6f;
    const Tank& me = m.tanks()[size_t(tank_)];
    const Tank& tgt = m.tanks()[size_t(plan_.target)];
    float score;
    if (tr.tankHit == plan_.target) {
        score = 0.f;
    } else {
        score = centreDistance(tgt, tr.x, tr.y);
        if (tr.tankHit >= 0 && tr.tankHit != tank_) score = std::min(score, 30.f);  // another enemy will do
    }
    if (tr.tankHit == tank_ || centreDistance(me, tr.x, tr.y) < safeRadius_) score += 5000.f;  // not myself
    return score + tr.time * 1.5f;  // prefer quick shots
}

void Planner::consider(const Cand& c) {
    if (stage_ == Stage::Refine) {
        if (c.score < bestRefined_.score) bestRefined_ = c;
        return;
    }
    // Keep up to four good shots with clearly different angles.
    for (Cand& b : best_) {
        if (std::fabs(b.angle - c.angle) < 6.f) {
            if (c.score < b.score) b = c;
            std::sort(best_.begin(), best_.end(), [](const Cand& a, const Cand& b2) { return a.score < b2.score; });
            return;
        }
    }
    best_.push_back(c);
    std::sort(best_.begin(), best_.end(), [](const Cand& a, const Cand& b2) { return a.score < b2.score; });
    if (best_.size() > 4) best_.pop_back();
}

void Planner::advance() {
    if (stage_ == Stage::Coarse) {
        queue_.clear();
        next_ = 0;
        if (!best_.empty()) bestRefined_ = best_.front();
        for (const Cand& b : best_)
            for (int da = -3; da <= 3; ++da)
                for (int dp = -3; dp <= 3; ++dp)
                    queue_.push_back(Cand{std::clamp(b.angle + float(da), 0.f, 180.f),
                                          std::clamp(b.power + float(dp), kMinPower, 100.f), 0.f});
        stage_ = Stage::Refine;
        return;
    }
    plan_.angle = std::clamp(bestRefined_.angle + noiseAngle_, 0.f, 180.f);
    plan_.power = std::clamp(bestRefined_.power + noisePower_, kMinPower, 100.f);
    plan_.score = bestRefined_.score;
    stage_ = Stage::Done;
}

}  // namespace mud
