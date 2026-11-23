#pragma once

// Computer players. A CPU tank picks a target and a weapon, then searches
// angle/power pairs by simulating each shot with the real ballistics (wind
// included). The search runs a slice at a time while the turret turns, so it
// never stalls a frame. Skill decides how much aiming error is added; the
// error shrinks with each shot at the same target, as if correcting.

#include <vector>

#include "core/rng.h"

namespace mud {

class Match;

struct AiMemory {
    int target = -1;  // tank aimed at last time
    int shots = 0;    // consecutive shots at it
};

struct Plan {
    int weapon = 0;
    float angle = 90.f;
    float power = 50.f;
    int target = -1;
    float score = 0.f;  // predicted miss distance (lower is better)
};

class Planner {
public:
    void start(const Match& m, int tank, Rng& rng, AiMemory& memory, bool noise);
    bool work(const Match& m, int budget);  // evaluates up to `budget` shots; true once done
    bool done() const { return stage_ == Stage::Done; }
    const Plan& plan() const { return plan_; }

private:
    enum class Stage { Idle, Coarse, Refine, Done };
    struct Cand {
        float angle, power, score;
    };

    float evaluate(const Match& m, float angle, float power) const;
    void consider(const Cand& c);
    void advance();

    Stage stage_ = Stage::Idle;
    int tank_ = -1;
    float safeRadius_ = 40.f;
    std::vector<Cand> queue_;
    size_t next_ = 0;
    std::vector<Cand> best_;  // a few distinct good shots from the coarse pass
    Cand bestRefined_{90.f, 50.f, 1e30f};
    float noiseAngle_ = 0.f, noisePower_ = 0.f;
    Plan plan_;
};

}  // namespace mud
