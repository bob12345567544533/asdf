#include "Buffer.hpp"
#include "Menu.hpp"
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

static float percentSetting(char const* key) {
    double value = static_cast<double>(setting<int64_t>(key)) / 100.0;
    return static_cast<float>(std::clamp(value, 0.0, 0.5));
}

void refreshCachedSettings() {
    if (!setting<bool>("enabled")) {
        g_cfg = Config {};
        return;
    }
    g_cfg.shipAssist = percentSetting("ship-assist");
    g_cfg.ufoAssist = percentSetting("ufo-assist");
    g_cfg.robotAssist = percentSetting("robot-assist");
    g_cfg.ledgeTicks = static_cast<int>(std::clamp<int64_t>(setting<int64_t>("ledge-grace"), 0, 12));
    g_cfg.orbTicks = static_cast<int>(std::clamp<int64_t>(setting<int64_t>("late-orbs"), 0, 12));
    g_cfg.hitboxShrink = percentSetting("hitbox-shrink");
    g_cfg.wavePixels = static_cast<int>(std::clamp<int64_t>(setting<int64_t>("wave-assist"), 0, 10));
    g_cfg.slowmo = percentSetting("danger-slowmo");
}

static int trackIndex(PlayLayer* layer, PlayerObject* p) {
    return layer && p == layer->m_player2 ? 1 : 0;
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
    bool applyInput = true; // false: keep the button as it is now (danger checks)
    std::array<bool, 2> held { false, false }; // current button per player, for applyInput = false
    float yOffset = 0.f;    // shift the ghosts up/down (wave nudge test)
};

// Copies the real players onto the hidden ghosts, applies the input after
// `delay` steps, and runs the game's own physics and collision forward.
SimResult simulate(SimSetup const& s, int delay) {
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

        if (s.yOffset != 0.f) {
            auto pos = real->getPosition();
            ghost->setPosition({ pos.x, pos.y + s.yOffset });
        }

        // Ghosts get reused, so set the button to what the real player is doing
        // right now: up if this input is a press, held if it's a release.
        bool hold = s.applyInput ? !s.down : s.held[i];
        if (hold) ghost->pushButton(PlayerButton::Jump);
        else ghost->releaseButton(PlayerButton::Jump);

        ghosts[i] = ghost;
    }

    int total = delay + s.horizon;
    int airborne = 0;
    int grounded = 0;

    for (int step = 0; step < total; ++step) {
        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->m_collisionLogTop->removeAllObjects();
            ghosts[i]->m_collisionLogBottom->removeAllObjects();
            ghosts[i]->m_collisionLogLeft->removeAllObjects();
            ghosts[i]->m_collisionLogRight->removeAllObjects();
            s.layer->checkCollisions(ghosts[i], s.dt, false);
            if (g_simDied) return { true, step };
        }

        // Pressing after the collision pass matches how the game applies a
        // fresh input (it gives the shorter first jump, like the real thing).
        if (s.applyInput && step == delay) {
            for (size_t i = 0; i < s.count; ++i) {
                if (s.down) ghosts[i]->pushButton(PlayerButton::Jump);
                else ghosts[i]->releaseButton(PlayerButton::Jump);
            }
        }

        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->update(s.dt);
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

// ---------------------------------------------------------------------------
// Late click assists
// ---------------------------------------------------------------------------

static bool lateOrbType(GameObjectType type) {
    switch (type) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
            return true;
        default:
            return false;
    }
}

// Window setting is in 240 TPS ticks; convert to steps at the current TPS.
static int windowSteps(int ticks240, PlayLayer* layer) {
    return msToTicks(ticks240 * 1000.0 / 240.0, tickMs(layer));
}

static bool tryLateOrb(PlayLayer* layer, SimSetup const& s, bool isPlayer1) {
    if (g_cfg.orbTicks <= 0) return false;
    int window = std::max(1, windowSteps(g_cfg.orbTicks, layer));

    std::array<std::pair<PlayerObject*, RingObject*>, 2> hits {};
    size_t count = 0;
    int late = 0;
    for (size_t i = 0; i < s.count; ++i) {
        auto p = s.reals[i];
        if (!p || p->m_isDead || p->m_isOnGround || touchingRing(p)) continue;
        auto& t = g.track[trackIndex(layer, p)];
        if (!t.ring) continue;
        int age = g.tick - t.ringTick;
        if (age <= 0 || age > window) continue;
        hits[count++] = { p, t.ring };
        late = std::max(late, age);
    }
    if (count == 0) return false;

    // Let the click through first so the game sees the button as held.
    g_bypass = true;
    layer->handleButton(true, static_cast<int>(PlayerButton::Jump), isPlayer1);
    g_bypass = false;

    for (size_t i = 0; i < count; ++i) {
        hits[i].first->ringJump(hits[i].second, true);
        g.track[trackIndex(layer, hits[i].first)].ring = nullptr;
    }
    menu::log(fmt::format("late orb  +{} tick{}", late, late == 1 ? "" : "s"));
    return true;
}

static void tryLedgeGrace(PlayLayer* layer, SimSetup const& s) {
    if (g_cfg.ledgeTicks <= 0) return;
    int window = std::max(1, windowSteps(g_cfg.ledgeTicks, layer));

    for (size_t i = 0; i < s.count; ++i) {
        auto p = s.reals[i];
        if (!p || p->m_isDead || p->m_isOnGround) continue;
        Mode mode = modeOf(p);
        if (mode != Mode::Cube && mode != Mode::Robot && mode != Mode::Ball && mode != Mode::Spider) continue;

        auto& t = g.track[trackIndex(layer, p)];
        int age = g.tick - t.leftGroundTick;
        if (t.leftByJump || age <= 0 || age > window) continue;

        // Count as still standing on the ledge for this click.
        p->m_isOnGround = true;
        t.leftByJump = true; // only once per ledge
        menu::log(fmt::format("ledge grace  +{} tick{}", age, age == 1 ? "" : "s"));
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

    // Late clicks: an orb you just passed, or a ledge you just ran off.
    if (down && tryLateOrb(layer, s, isPlayer1)) return true;
    if (down) tryLedgeGrace(layer, s);

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
        menu::log(fmt::format(
            "+{} tick{} ({:.1f} ms)  {} {}",
            delay, delay == 1 ? "" : "s", earlyMs, modeName(mainMode), down ? "press" : "release"
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

// ---------------------------------------------------------------------------
// Assist tracking and per-frame checks
// ---------------------------------------------------------------------------

void resetTracks() {
    g.track = {};
    g.slowFactor = 1.f;
}

void noteHeld(PlayLayer* layer, bool down, int button, bool isPlayer1) {
    if (g_simulating || button != static_cast<int>(PlayerButton::Jump)) return;
    bool twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    if (twoPlayer) {
        g.track[isPlayer1 ? 0 : 1].held = down;
    }
    else {
        g.track[0].held = down;
        g.track[1].held = down;
    }
}

void beforeRealUpdate(PlayLayer* layer, PlayerObject* p) {
    auto& t = g.track[trackIndex(layer, p)];
    bool now = p->m_isOnGround;
    if (t.prevOnGround && !now) {
        // Just left the ground. Moving up fast means it was a jump.
        double up = p->m_isUpsideDown ? -p->m_yVelocity : p->m_yVelocity;
        t.leftGroundTick = g.tick;
        t.leftByJump = up > 1.0;
    }
    t.prevOnGround = now;
}

float assistFor(PlayLayer* layer, PlayerObject* p) {
    if (!layer || p->m_gameLayer != layer || p->m_isDead || p->m_isSwing) return 0.f;
    if (p->m_isShip) return g_cfg.shipAssist;
    if (p->m_isBird) return g_cfg.ufoAssist;
    if (p->m_isRobot) return g_cfg.robotAssist;
    return 0.f;
}

void noteRingTouched(PlayLayer* layer, PlayerObject* p, RingObject* ring) {
    if (g_simulating || !ring || !lateOrbType(ring->m_objectType)) return;
    if (p != layer->m_player1 && p != layer->m_player2) return;
    auto& t = g.track[trackIndex(layer, p)];
    t.ring = ring;
    t.ringTick = g.tick;
}

void noteRingUsed(PlayLayer* layer, PlayerObject* p) {
    if (g_simulating) return;
    if (p != layer->m_player1 && p != layer->m_player2) return;
    g.track[trackIndex(layer, p)].ring = nullptr;
}

bool hitboxForgives(PlayLayer* layer, PlayerObject* p, GameObject* object) {
    if (g_cfg.hitboxShrink <= 0.f || !layer || !p || !object) return false;
    // Never touch the game's own anti-cheat check.
    if (object == layer->m_anticheatSpike) return false;
    auto type = object->m_objectType;
    if (type != GameObjectType::Hazard && type != GameObjectType::AnimatedHazard) return false;

    CCRect box = p->getObjectRect();
    float keep = 1.f - g_cfg.hitboxShrink;
    float w = box.size.width * keep;
    float h = box.size.height * keep;
    CCRect inner(box.getMidX() - w / 2.f, box.getMidY() - h / 2.f, w, h);
    CCRect hazard = object->getObjectRect();
    return !inner.intersectsRect(hazard);
}

void onFrameEnd(PlayLayer* layer) {
    g.slowFactor = 1.f;
    if (g_cfg.slowmo <= 0.f && g_cfg.wavePixels <= 0) return;
    if (layer->m_levelEndAnimationStarted || layer->m_isPaused) return;
    if (!g.ghosts[0] || !g.ghosts[1]) return;
    // A queued input is about to fire; the check can't know about it.
    for (auto& ch : g.channels) {
        if (!ch.queue.empty()) return;
    }

    auto p1 = layer->m_player1;
    auto p2 = layer->m_player2;
    if (!p1 || p1->m_isDead) return;

    SimSetup s;
    s.layer = layer;
    s.applyInput = false;
    s.reals[s.count++] = p1;
    if (layer->m_gameState.m_isDualMode && p2 && !p2->m_isDead) s.reals[s.count++] = p2;
    s.held = { g.track[0].held, g.track[1].held };
    s.dt = g.lastStepDt > 0.0001f && g.lastStepDt < 2.f ? g.lastStepDt : 0.25f;

    bool anyWave = false;
    bool anyFlying = false;
    for (size_t i = 0; i < s.count; ++i) {
        if (s.reals[i]->m_isDashing) return;
        Mode mode = modeOf(s.reals[i]);
        anyWave |= mode == Mode::Wave;
        anyFlying |= isFlyingMode(mode);
    }
    s.earlyExit = !anyFlying;
    double perTick = tickMs(layer);

    // Wave nudge: about to clip something in the next few steps? See if a
    // pixel or two up or down avoids it, and if so, move there.
    if (g_cfg.wavePixels > 0 && anyWave && g.tick - g.track[0].lastNudgeTick >= 6) {
        s.horizon = 10;
        if (simulate(s, 0).died) {
            for (int px = 1; px <= g_cfg.wavePixels; ++px) {
                bool done = false;
                for (float sign : { 1.f, -1.f }) {
                    s.yOffset = sign * px;
                    if (simulate(s, 0).died) continue;
                    for (size_t i = 0; i < s.count; ++i) {
                        auto pos = s.reals[i]->getPosition();
                        s.reals[i]->setPosition({ pos.x, pos.y + s.yOffset });
                    }
                    g.track[0].lastNudgeTick = g.tick;
                    menu::log(fmt::format("wave nudge  {}{}px", sign > 0 ? "+" : "-", px));
                    done = true;
                    break;
                }
                if (done) break;
            }
            s.yOffset = 0.f;
        }
    }

    // Danger slow-mo: if the current path dies soon, slow the next frame down.
    if (g_cfg.slowmo > 0.f) {
        s.horizon = std::max(4, msToTicks(150.0, perTick));
        if (simulate(s, 0).died) g.slowFactor = 1.f - g_cfg.slowmo;
    }
}

} // namespace pb
