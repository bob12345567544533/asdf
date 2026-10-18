#include "Buffer.hpp"
#include "Menu.hpp"
#include "Overlay.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace pb {

// Hard cap on how far an input can be pushed back, whatever the settings say.
static constexpr int MAX_DELAY_TICKS = 24;

// Most ghost physics steps one click may cost. Keeps a worst-case click (one
// that dies no matter what) from causing a frame hitch.
static constexpr int STEP_BUDGET = 2400;

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

static bool isFlyingMode(Mode mode) {
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
    return ghostIndex(player) >= 0;
}

int ghostIndex(PlayerObject* player) {
    if (!player) return -1;
    if (player == g.ghosts[0]) return 0;
    if (player == g.ghosts[1]) return 1;
    return -1;
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

bool stopsSimulation(GameObject* object, PlayerObject* ghost) {
    switch (object->m_objectType) {
        // Portals, pads and teleports act on contact.
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::GravityTogglePortal:
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::SpiderPad:
            return true;

        // Orbs only do something while the button is held.
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb: {
            int i = ghostIndex(ghost);
            return i >= 0 && g_simHeld[i];
        }

        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

namespace {

struct SimGuard {
    SimGuard() {
        g_simulating = true;
        g_simDied = false;
        g_simStopped = false;
    }
    ~SimGuard() {
        g_simulating = false;
        g_simDied = false;
        g_simStopped = false;
    }
};

struct SimResult {
    bool died = false;
    bool stopped = false; // reached something the sim can't model, counts as alive
    int steps = 0;        // steps survived (the whole run if it didn't die)
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

void setHeld(PlayerObject* ghost, int index, bool held) {
    if (held) ghost->pushButton(PlayerButton::Jump);
    else ghost->releaseButton(PlayerButton::Jump);
    g_simHeld[index] = held;
}

// Copies the real players onto the hidden ghosts, applies the input after
// `delay` steps, and runs the game's own physics and collision forward.
// `budget` is decreased by the number of steps used.
SimResult simulate(SimSetup const& s, int delay, int& budget) {
    SimGuard guard;

    std::array<PlayerObject*, 2> ghosts { nullptr, nullptr };
    for (size_t i = 0; i < s.count; ++i) {
        auto ghost = g.ghosts[i];
        auto real = s.reals[i];

        // Copy the state with plain field writes. Helper functions like
        // spawnFromPlayer also run gravity-flip, velocity-change and visibility
        // logic, which can leave traces on the real level.
        ghost->copyAttributes(real);
        ghost->setPosition(real->getPosition());
        ghost->setRotation(real->getRotation());
        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isOnSlope = real->m_isOnSlope;
        ghost->m_wasOnSlope = real->m_wasOnSlope;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isPlatformer = real->m_isPlatformer;
        ghost->m_holdingLeft = real->m_holdingLeft;
        ghost->m_holdingRight = real->m_holdingRight;
        ghost->m_platformerXVelocity = real->m_platformerXVelocity;

        // Ghosts get reused, so set the button to what the real player is doing
        // right now: up if this input is a press, held if it's a release.
        setHeld(ghost, static_cast<int>(i), !s.down);

        ghosts[i] = ghost;
    }

    int total = delay + s.horizon;
    int airborne = 0;
    int grounded = 0;

    for (int step = 0; step < total; ++step) {
        // Out of time: count it as a death so an unchecked delay never gets used.
        if (--budget < 0) return { true, false, -1 };

        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->m_collisionLogTop->removeAllObjects();
            ghosts[i]->m_collisionLogBottom->removeAllObjects();
            ghosts[i]->m_collisionLogLeft->removeAllObjects();
            ghosts[i]->m_collisionLogRight->removeAllObjects();
            s.layer->checkCollisions(ghosts[i], s.dt, false);
            if (g_simDied) return { true, false, step };
            if (g_simStopped) return { false, true, step };
        }

        // Pressing after the collision pass matches how the game applies a
        // fresh input (it gives the shorter first jump, like the real thing).
        if (step == delay) {
            for (size_t i = 0; i < s.count; ++i) setHeld(ghosts[i], static_cast<int>(i), s.down);
        }

        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->update(s.dt);
            ghosts[i]->updateRotation(s.dt);
        }
        if (g_simDied) return { true, false, step };

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
                return { false, false, total };
            }
        }
    }
    return { false, false, total };
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
    int budget = STEP_BUDGET;

    // Would this input kill you if it went through right now? If the ghost
    // survives, or reaches a portal/pad/orb first, let it through untouched.
    auto base = simulate(s, 0, budget);
    if (!base.died) return false;

    auto survives = [&](SimResult const& r) {
        return !r.died || r.steps - base.steps >= minGain;
    };

    // Find the smallest delay that clearly survives longer.
    for (int delay = 1; delay <= limit && budget > 0; ++delay) {
        auto result = simulate(s, delay, budget);
        if (!survives(result)) continue;

        // The first working delay sits right at the edge where a hair earlier
        // still dies. If one more step also works, use that for some margin.
        int chosen = delay;
        if (delay + 1 <= limit && budget > 0 && survives(simulate(s, delay + 1, budget))) {
            chosen = delay + 1;
        }

        ch.shift = chosen;
        ch.queue.push_back({ down, isPlayer1, g.tick + 1 + chosen });

        double earlyMs = chosen * perTick;
        if (debug) {
            log::info(
                "{} buffered by {} steps ({:.2f}ms, {}): dies after {} steps now, {} after delay",
                down ? "Press" : "Release", chosen, earlyMs, modeName(mainMode), base.steps,
                result.died ? fmt::format("{}", result.steps) : std::string(result.stopped ? "reaches a portal/pad/orb" : "survives")
            );
        }
        if (auto overlay = BufferOverlay::get(layer)) {
            overlay->showBuffered(earlyMs, chosen, mainMode, !down);
        }
        menu::log(fmt::format(
            "+{} tick{} ({:.1f} ms)  {} {}",
            chosen, chosen == 1 ? "" : "s", earlyMs, modeName(mainMode), down ? "press" : "release"
        ));
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
