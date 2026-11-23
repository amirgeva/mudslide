// Headless checks of the terrain, ballistics, AI and match flow.
//
// Usage: mud_tests [matches]   (number of full CPU matches to play, default 6)

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/match.h"
#include "core/terrain.h"

using namespace mud;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            ++g_failures;                                      \
            std::printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                          \
            std::printf("\n");                                 \
        }                                                      \
    } while (0)

MatchSetup cpuSetup(int players, uint64_t seed, int wind, int rounds) {
    MatchSetup s;
    const Control skills[4] = {Control::CpuHard, Control::CpuNormal, Control::CpuEasy, Control::CpuHard};
    for (int i = 0; i < players; ++i) s.players.push_back(PlayerSetup{"P" + std::to_string(i), skills[i]});
    s.seed = seed;
    s.wind = wind;
    s.rounds = rounds;
    return s;
}

// No grain hangs in the air, and loose material is at rest on its slope.
int countUnstable(const Terrain& t, bool report = false) {
    int bad = 0;
    auto note = [&](int x, int y, uint8_t c, const char* why) {
        if (report && bad < 5) std::printf("    grain %d at (%d,%d) can %s\n", c, x, y, why);
        ++bad;
    };
    for (int y = 0; y < kWorldH - 1; ++y)
        for (int x = 0; x < kWorldW; ++x) {
            const uint8_t c = t.at(x, y);
            if (!c) continue;
            if (!t.solid(x, y + 1)) {
                note(x, y, c, "fall");
                continue;
            }
            if (!isMud(c) && !isLoose(c)) continue;
            for (int d : {-1, 1}) {
                const int nx = x + d;
                if (nx < 0 || nx >= kWorldW || t.solid(nx, y)) continue;
                if (!t.solid(nx, y + 1)) {
                    note(x, y, c, "slide");
                } else if (isMud(c)) {
                    const int nx2 = x + 2 * d;
                    if (nx2 >= 0 && nx2 < kWorldW && !t.solid(nx2, y) && !t.solid(nx2, y + 1)) note(x, y, c, "slide 2");
                }
            }
        }
    return bad;
}

void testTerrain() {
    std::printf("terrain generation and settling\n");
    for (int theme = 0; theme < 4; ++theme) {
        Terrain a, b;
        a.generate(1234 + uint64_t(theme), theme);
        b.generate(1234 + uint64_t(theme), theme);
        bool same = true;
        for (size_t i = 0; i < size_t(kWorldW) * kWorldH; ++i) same = same && a.cells()[i] == b.cells()[i];
        CHECK(same, "theme %d not deterministic", theme);
        CHECK(a.generation() != b.generation(), "generation ids must be unique");
        for (int x = 0; x < kWorldW; x += 7) {
            const int s = a.surface(x);
            CHECK(s >= kWorldH / 5 && s < kWorldH, "theme %d column %d surface %d", theme, x, s);
        }
        CHECK(countUnstable(a) == 0, "fresh terrain should be at rest");

        // Blow holes, drop mud and loose dirt, and settle: no grain may be
        // lost or created, and everything must come to rest.
        std::vector<Grain> removed;
        const int carved = a.carve(400.f, float(a.surface(400)), 40.f, &removed);
        CHECK(carved == int(removed.size()) && carved > 0, "carve reported %d, listed %d", carved, int(removed.size()));
        a.carve(640.f, float(a.surface(640) + 30), 30.f);  // undercut: the roof must fall in
        const int mud = a.addMud(900.f, float(a.surface(900) - 60), 35.f);
        CHECK(mud > 3000, "mud ball too small: %d", mud);
        int dropped = 0;
        for (int i = 0; i < 40; ++i) dropped += a.deposit(200.f + float(i) * 3.f, 100.f, kLooseFirst + 3, 6);
        CHECK(dropped == 240, "deposit placed %d of 240", dropped);
        const long before = a.countSolid();
        const int steps = a.settleAll();
        CHECK(!a.settling(), "did not settle in %d steps", steps);
        CHECK(a.countSolid() == before, "settling changed grain count %ld -> %ld", before, a.countSolid());
        CHECK(countUnstable(a) == 0, "theme %d: %d grains not at rest", theme, countUnstable(a, true));
    }
}

void testBallistics() {
    std::printf("ballistics\n");
    Match m(cpuSetup(2, 7, 0, 1));
    CHECK(m.wind() == 0.f, "wind should be off");
    for (int i = 0; i < 2; ++i) {
        const Tank& t = m.tanks()[size_t(i)];
        // Straight up comes straight back down on the shooter.
        std::vector<float> path;
        const ShotTrace up = m.traceShot(i, 90.f, 50.f, &path);
        CHECK(up.impact && up.tankHit == i, "tank %d vertical shot: impact=%d tank=%d", i, up.impact, up.tankHit);
        float top = 1e9f;
        for (size_t k = 1; k < path.size(); k += 2) top = std::min(top, path[k]);
        const float v = 0.5f * kMaxSpeed;
        const float expected = t.y - kPivotH - kBarrelLen - v * v / (2.f * kGravity);
        CHECK(std::fabs(top - expected) < 3.f, "apex %.1f, expected %.1f", top, expected);
        // A 45 degree lob toward the other tank lands inside the world, and
        // in still air a mirrored shot from the same spot flies the same way.
        const Tank& other = m.tanks()[size_t(1 - i)];
        const float lobAngle = other.x > t.x ? 45.f : 135.f;
        std::vector<float> a, b;
        const ShotTrace lob = m.traceShot(i, lobAngle, 60.f, &a);
        m.traceShot(i, 180.f - lobAngle, 60.f, &b);
        CHECK(lob.impact, "45 degree shot from tank %d should land", i);
        bool mirrored = true;
        for (size_t k = 0; k + 1 < std::min(a.size(), b.size()) && k < 60; k += 2)
            mirrored = mirrored && std::fabs((a[k] - t.x) + (b[k] - t.x)) < 0.01f && std::fabs(a[k + 1] - b[k + 1]) < 0.01f;
        CHECK(mirrored, "mirrored shots should be symmetric");
    }
}

void testAi() {
    std::printf("ai accuracy\n");
    int good = 0, total = 0;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        MatchSetup s = cpuSetup(2, seed, int(seed % 4), 1);
        s.players[0].control = Control::CpuHard;
        Match m(s);
        m.setAiNoise(false);
        // Run until the first CPU has fired; check where the shell lands.
        bool fired = false;
        int shooter = -1;
        ShotTrace predicted;
        for (int i = 0; i < 20000 && !fired; ++i) {
            if (m.phase() == Phase::Aiming) {
                shooter = m.active();
                const Tank& t = m.tanks()[size_t(shooter)];
                predicted = m.traceShot(shooter, t.angle, t.power);
            }
            m.update(kTick);
            for (const Event& e : m.takeEvents())
                if (e.type == EventType::Fire) fired = true;
        }
        CHECK(fired, "seed %llu: CPU never fired", (unsigned long long)seed);
        if (!fired) continue;
        const Tank& enemy = m.tanks()[size_t(1 - shooter)];
        const float d = std::hypot(predicted.x - enemy.x, predicted.y - (enemy.y - 6.f));
        ++total;
        if (predicted.impact && (predicted.tankHit == 1 - shooter || d < 22.f)) ++good;
    }
    std::printf("  %d of %d first shots on target with no aiming error\n", good, total);
    CHECK(good * 100 >= total * 85, "only %d of %d on target", good, total);
}

// A human turn through the input calls: aim a mud ball onto the enemy and
// check it lands, uses ammo and buries the target.
void testHumanMud() {
    std::printf("human turn: mud ball buries the enemy\n");
    MatchSetup s;
    s.players = {PlayerSetup{"A", Control::Human}, PlayerSetup{"B", Control::Human}};
    s.wind = 0;
    s.rounds = 1;
    s.seed = 21;
    Match m(s);
    for (int i = 0; i < 2000 && !m.humanAiming(); ++i) m.update(kTick);
    CHECK(m.humanAiming(), "no human turn");
    const int me = m.active(), other = 1 - me;
    const Tank& target = m.tanks()[size_t(other)];
    float bestA = 90.f, bestP = 50.f, bestD = 1e9f;
    for (float a = 1.f; a < 180.f; a += 1.f)
        for (float p = 10.f; p <= 100.f; p += 0.5f) {
            const ShotTrace tr = m.traceShot(me, a, p);
            if (!tr.impact) continue;
            const float d = tr.tankHit == other ? 0.f : std::hypot(tr.x - target.x, tr.y - target.y);
            if (d < bestD) {
                bestD = d;
                bestA = a;
                bestP = p;
            }
        }
    CHECK(bestD < 10.f, "no shot reaches the enemy (best miss %.1f)", bestD);
    const Tank& t = m.tanks()[size_t(me)];
    m.adjustAim(bestA - t.angle, bestP - t.power);
    m.selectWeapon(int(Weapon::Mud));
    CHECK(t.weapon == int(Weapon::Mud), "mud ball not selected");
    const int ammo = t.ammo[size_t(Weapon::Mud)];
    m.fire();
    bool splat = false;
    int clodsSeen = 0;
    for (int i = 0; i < 6000 && !(m.phase() == Phase::Aiming && m.active() == other); ++i) {
        m.update(kTick);
        clodsSeen = std::max(clodsSeen, int(m.clods().size()));
        for (const Event& e : m.takeEvents()) splat = splat || e.type == EventType::MudSplat;
    }
    CHECK(splat, "mud ball never landed");
    CHECK(clodsSeen > 0, "mud ball should splash clods around");
    CHECK(m.tanks()[size_t(me)].ammo[size_t(Weapon::Mud)] == ammo - 1, "mud ammo not used");
    CHECK(m.phase() == Phase::Aiming && m.active() == other, "turn did not pass to the other player");
    const bool covered = m.buried(other) || m.terrain().solid(int(target.x), int(target.y - 7.f));
    CHECK(covered, "target should be buried in mud");
    CHECK(countUnstable(m.terrain()) == 0, "terrain should be at rest after the turn");
}

void testMatches(int count) {
    std::printf("full CPU matches\n");
    for (int i = 0; i < count; ++i) {
        const uint64_t seed = 100 + uint64_t(i);
        const int players = 2 + i % 3;
        Match m(cpuSetup(players, seed, 1 + i % 3, 3));
        const auto start = std::chrono::steady_clock::now();
        float simTime = 0.f;
        int shots = 0, explosions = 0, splats = 0;
        long maxLoose = 0;
        while (m.phase() != Phase::MatchOver && simTime < 4000.f) {
            m.update(1.f / 30.f);
            simTime += 1.f / 30.f;
            for (const Event& e : m.takeEvents()) {
                shots += e.type == EventType::Fire;
                explosions += e.type == EventType::Explode;
                splats += e.type == EventType::MudSplat;
            }
            if (m.phase() == Phase::RoundOver) {
                long loose = 0;
                const uint8_t* c = m.terrain().cells();
                for (size_t k = 0; k < size_t(kWorldW) * kWorldH; ++k) loose += isLoose(c[k]) ? 1 : 0;
                maxLoose = std::max(maxLoose, loose);
                m.nextRound();
            }
            for (const Tank& t : m.tanks())
                CHECK(std::isfinite(t.x) && t.y >= 0.f && t.y <= float(kWorldH), "tank out of the world");
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        int wins = 0;
        for (const PlayerScore& sc : m.scores()) wins += sc.roundWins;
        std::printf("  seed %llu, %d players: %d shots, %d blasts, %d mud, %.0f s of play in %.2f s, winner %d, loose dirt %ld\n",
                    (unsigned long long)seed, players, shots, explosions, splats, simTime, secs, m.matchWinner(),
                    maxLoose);
        CHECK(m.phase() == Phase::MatchOver, "seed %llu did not finish", (unsigned long long)seed);
        CHECK(wins <= m.rounds(), "%d round wins in %d rounds", wins, m.rounds());
        CHECK(explosions > 0 && maxLoose > 0, "blasts should leave loose dirt around");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int matches = argc > 1 ? std::atoi(argv[1]) : 6;
    testTerrain();
    testBallistics();
    testAi();
    testHumanMud();
    testMatches(matches);
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
