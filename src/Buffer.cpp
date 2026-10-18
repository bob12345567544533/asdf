#include "Buffer.hpp"
#include "Overlay.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace pb {

// Hard cap on how far an input can be pushed back, whatever the settings say.
static constexpr int MAX_DELAY_TICKS = 24;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

Mode modeOf(PlayerObject* p) {
    if (p->m_isShip) return Mode::Ship;
    if (p->m_isBird) return Mode::Ufo;
    if (p->m_isBall) return Mode::Ball;
    if (p->m_isDart) return Mode::Wave;
    if (p->m_isRobot) return Mode::Robot;
    if (p->m_isSpider) return Mode::Spider;
    if (p->m_isSwing) return Mode::Swing;
    return Mode::Cube;
}

char const* modeName(Mode mode) {
    switch (mode) {
        case Mode::Cube: return "cube";
        case Mode::Ship: return "ship";
        case Mode::Ball: return "ball";
        case Mode::Ufo: return "ufo";
        case Mode::Wave: return "wave";
        case Mode::Robot: return "robot";
        case Mode::Spider: return "spider";
        case Mode::Swing: return "swing";
    }
    return "?";
}

bool isFlyingMode(Mode mode) {
    return mode == Mode::Ship || mode == Mode::Ufo || mode == Mode::Wave || mode == Mode::Swing;
}

// Only these modes care about when you let go.
static bool releaseMatters(Mode mode) {
    return mode == Mode::Wave || mode == Mode::Ship || mode == Mode::Robot;
}

template <class T>
static T setting(char const* key) {
    return Mod::get()->getSettingValue<T>(key);
}

bool isGhost(PlayerObject* player) {
    return player && (player == g.ghosts[0] || player == g.ghosts[1]);
}

void setGhosts(PlayerObject* a, PlayerObject* b) {
    g.ghosts = { a, b };
}

void clearGhosts(PlayerObject* a, PlayerObject* b) {
    // Only clear if these are still the active ghosts (a new level may already
    // have replaced them).
    if (g.ghosts[0] == a) g.ghosts[0] = nullptr;
    if (g.ghosts[1] == b) g.ghosts[1] = nullptr;
}

void clearQueues() {
    for (auto& ch : g.channels) {
        ch.queue.clear();
        ch.shift = 0;
    }
}

double tickMs(PlayLayer* layer) {
    float dt = g.lastStepDt;
    if (!(dt > 0.0001f && dt < 2.f)) dt = 0.25f;

    // The step dt already has the level's time warp applied. Undo it so the
    // result is in real time, which is what your hands feel.
    float warp = layer ? layer->m_gameState.m_timeWarp : 1.f;
    if (!(warp > 0.01f && warp < 10.f)) warp = 1.f;

    return (dt / warp) * (1000.0 / 60.0);
}

static int msToTicks(double ms, double perTick) {
    if (ms <= 0.0 || perTick <= 0.0) return 0;
    return static_cast<int>(std::lround(ms / perTick));
}

static bool touchingRing(PlayerObject* p) {
    return p->m_touchingRings && p->m_touchingRings->count() > 0;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

namespace {

struct SimGuard {
    SimGuard() {
        g_simulating = true;
        g_simDied = false;
    }
    ~SimGuard() {
        g_simulating = false;
        g_simDied = false;
    }
};

struct SimResult {
    bool died = false;
    int steps = 0; // steps survived (the whole run if it didn't die)
};

struct SimSetup {
    PlayLayer* layer = nullptr;
    std::array<PlayerObject*, 2> reals { nullptr, nullptr };
    size_t count = 0;
    bool down = true;
    int horizon = 48;
    float dt = 0.25f;
    bool earlyExit = false; // grounded modes: a safe landing counts as survived
};

// Copies the real players onto the hidden ghosts, applies the input after
// `delay` steps, and runs the game's own physics and collision forward.
SimResult simulate(SimSetup const& s, int delay) {
    SimGuard guard;

    std::array<PlayerObject*, 2> ghosts { nullptr, nullptr };
    for (size_t i = 0; i < s.count; ++i) {
        auto ghost = g.ghosts[i];
        auto real = s.reals[i];

        ghost->spawnFromPlayer(real, false);
        ghost->setVisible(false);
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isPlatformer = real->m_isPlatformer;
        ghost->m_holdingLeft = real->m_holdingLeft;
        ghost->m_holdingRight = real->m_holdingRight;
        ghost->m_platformerXVelocity = real->m_platformerXVelocity;

        // Ghosts get reused, so set the button to what the real player is doing
        // right now: up if this input is a press, held if it's a release.
        if (s.down) ghost->releaseButton(PlayerButton::Jump);
        else ghost->pushButton(PlayerButton::Jump);

        ghosts[i] = ghost;
    }

    int total = delay + s.horizon;
    int airborne = 0;
    int grounded = 0;

    for (int step = 0; step < total; ++step) {
        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->resetCollisionLog(false);
            s.layer->checkCollisions(ghosts[i], s.dt, false);
            if (g_simDied) return { true, step };
        }

        // Pressing after the collision pass matches how the game applies a
        // fresh input (it gives the shorter first jump, like the real thing).
        if (step == delay) {
            for (size_t i = 0; i < s.count; ++i) {
                if (s.down) ghosts[i]->pushButton(PlayerButton::Jump);
                else ghosts[i]->releaseButton(PlayerButton::Jump);
            }
        }

        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->update(s.dt);
            ghosts[i]->updateSpecial(s.dt);
            ghosts[i]->updateRotation(s.dt);
        }
        if (g_simDied) return { true, step };

        // Jumped, then landed and stayed on the ground: that's a safe outcome.
        if (s.earlyExit && step > delay) {
            bool allGrounded = true;
            for (size_t i = 0; i < s.count; ++i) {
                if (!ghosts[i]->m_isOnGround) allGrounded = false;
            }
            if (!allGrounded) {
                airborne++;
                grounded = 0;
            }
            else if (airborne >= 6 && ++grounded >= 3) {
                return { false, total };
            }
        }
    }
    return { false, total };
}

void fire(PlayLayer* layer, QueuedInput const& input) {
    g_bypass = true;
    layer->handleButton(input.down, static_cast<int>(PlayerButton::Jump), input.isPlayer1);
    g_bypass = false;
}

} // namespace

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void flushChannel(PlayLayer* layer, int channel) {
    auto& ch = g.channels[channel];
    // Copy first: firing can call back into handleButton.
    auto pending = std::move(ch.queue);
    ch.queue.clear();
    ch.shift = 0;
    for (auto const& input : pending) fire(layer, input);
}

void onStep(PlayLayer* layer, float dt) {
    // The game sometimes splits a step in two around an input. Use the biggest
    // recent step as the "full" step size so a half step doesn't skew anything.
    g.recentDts[g.recentDtIndex] = dt;
    g.recentDtIndex = (g.recentDtIndex + 1) % g.recentDts.size();
    g.lastStepDt = *std::max_element(g.recentDts.begin(), g.recentDts.end());
    g.tick++;

    for (int c = 0; c < 2; ++c) {
        auto& ch = g.channels[c];
        while (!ch.queue.empty() && ch.queue.front().fireAt <= g.tick) {
            auto input = ch.queue.front();
            ch.queue.pop_front();
            fire(layer, input);
        }
        if (ch.queue.empty()) ch.shift = 0;
    }
}

bool onButton(PlayLayer* layer, bool down, int button, bool isPlayer1) {
    if (g_bypass || g_simulating) return false;
    if (button != static_cast<int>(PlayerButton::Jump)) return false;
    if (!setting<bool>("enabled")) return false;

    bool twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    int channel = (twoPlayer && !isPlayer1) ? 1 : 0;
    auto& ch = g.channels[channel];

    // An input is already being held back on this channel. Push this one back by
    // the same amount so the gap between them stays exactly how you played it.
    if (!ch.queue.empty()) {
        int fireAt = std::max(g.tick + 1 + ch.shift, ch.queue.back().fireAt + 1);
        ch.queue.push_back({ down, isPlayer1, fireAt });
        return true;
    }

    if (layer->m_levelEndAnimationStarted) return false;

    // Which players does this input move?
    auto p1 = layer->m_player1;
    auto p2 = layer->m_player2;
    bool dual = layer->m_gameState.m_isDualMode;

    SimSetup s;
    s.layer = layer;
    s.down = down;
    if (twoPlayer) {
        if (channel == 0) s.reals[s.count++] = p1;
        else if (dual) s.reals[s.count++] = p2;
        else return false;
    }
    else {
        s.reals[s.count++] = p1;
        if (dual) s.reals[s.count++] = p2;
    }

    for (size_t i = 0; i < s.count; ++i) {
        auto p = s.reals[i];
        if (!p || !g.ghosts[i]) return false;
        if (p->m_isDead || p->m_isDashing || touchingRing(p)) return false;
    }

    // Work out the allowed delay. In dual with two different modes, the
    // stricter one wins.
    double perTick = tickMs(layer);
    bool bufferReleases = setting<bool>("buffer-releases");
    bool bufferShip = setting<bool>("buffer-ship");

    // The setting is in 240 TPS ticks. Turn it into real time so it means the
    // same thing if you change TPS.
    double maxMs = static_cast<double>(setting<int64_t>("buffer-ticks")) * 1000.0 / 240.0;
    int limit = std::min(MAX_DELAY_TICKS, msToTicks(maxMs, perTick));
    bool anyWave = false;
    bool anyMini = false;
    bool anyFlying = false;
    Mode mainMode = modeOf(s.reals[0]);

    for (size_t i = 0; i < s.count; ++i) {
        auto p = s.reals[i];
        Mode mode = modeOf(p);
        if (!down && (!bufferReleases || !releaseMatters(mode))) return false;

        if (mode == Mode::Ship && !bufferShip) return false;
        anyWave |= mode == Mode::Wave;
        anyMini |= p->m_vehicleSize < 1.f;
        anyFlying |= isFlyingMode(mode);
    }
    if (limit <= 0) return false;

    double lookAheadMs = anyWave ? setting<double>("wave-look-ahead-ms") : setting<double>("look-ahead-ms");
    if (anyMini) lookAheadMs *= 0.7; // mini moves faster vertically, keep it tight
    s.horizon = std::clamp(msToTicks(lookAheadMs, perTick), 8, 200);
    s.dt = g.lastStepDt > 0.0001f && g.lastStepDt < 2.f ? g.lastStepDt : 0.25f;
    s.earlyExit = !anyFlying;

    int minGain = std::max(1, msToTicks(setting<double>("min-gain-ms"), perTick));
    bool debug = setting<bool>("debug-log");

    // Would this input kill you if it went through right now?
    auto base = simulate(s, 0);
    if (!base.died) return false;

    // Find the smallest delay that clearly survives longer.
    for (int delay = 1; delay <= limit; ++delay) {
        auto result = simulate(s, delay);
        bool ok = !result.died || result.steps - base.steps >= minGain;
        if (!ok) continue;

        ch.shift = delay;
        ch.queue.push_back({ down, isPlayer1, g.tick + 1 + delay });

        double earlyMs = delay * perTick;
        g.stats.attemptCount++;
        g.stats.sessionCount++;
        g.stats.sessionEarlyMs += earlyMs;

        if (debug) {
            log::info(
                "{} buffered by {} steps ({:.2f}ms, {}): dies after {} steps now, {} after delay",
                down ? "Press" : "Release", delay, earlyMs, modeName(mainMode),
                base.steps, result.died ? fmt::format("{}", result.steps) : std::string("survives")
            );
        }
        if (auto overlay = BufferOverlay::get(layer)) {
            overlay->showBuffered(earlyMs, delay, mainMode, !down);
        }
        return true;
    }

    if (debug) {
        log::info(
            "{} would die after {} steps and no delay up to {} steps helps",
            down ? "Press" : "Release", base.steps, limit
        );
    }
    return false;
}

} // namespace pb
