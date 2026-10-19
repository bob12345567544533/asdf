#include "Buffer.hpp"
#include "Menu.hpp"
#include "Overlay.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include <sstream>

// Shared random number generator, seeded once per game session.
[[maybe_unused]] static std::mt19937& bufRng() {
    static std::mt19937 g{ std::random_device{}() };
    return g;
}

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
    // Speed portals change how fast the player moves, which the ghost can't
    // follow (they're told apart by object ID). Only stop if the speed changes.
    float speed = 0.f;
    switch (object->m_objectID) {
        case 200: speed = 0.7f; break;  // slow
        case 201: speed = 0.9f; break;  // normal
        case 202: speed = 1.1f; break;  // fast
        case 203: speed = 1.3f; break;  // faster
        case 1334: speed = 1.6f; break; // fastest
        default: break;
    }
    if (speed > 0.f) return !ghost || std::abs(ghost->m_playerSpeed - speed) > 0.01f;

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
    // Delayed runs only: once this many steps pass after the input without a
    // death, the delay is safe whatever happens next, so the run can stop.
    int decideAfter = -1;
};

// Ghost runs must not change anything GD counts for its progress check:
// jumps, clicks, attempts and the level's checksum over them. If they change
// behind GD's back, GD shows "New Best" but throws the PB away when you leave
// the level. Snapshot everything before the ghosts run and put it back after.
// Diagnostic: byte-level diff of an object before/after the ghost runs, as
// "0x36cc 0x3a10-0x3a17 ..." (offsets into the object).
struct RawSnapshot {
    uint8_t const* base = nullptr;
    std::vector<uint8_t> bytes;
    void take(void const* object, size_t size) {
        base = static_cast<uint8_t const*>(object);
        bytes.assign(base, base + size);
    }
    std::string diff(size_t maxRanges) const {
        std::string out;
        size_t ranges = 0;
        size_t i = 0;
        while (i < bytes.size() && ranges < maxRanges) {
            if (base[i] == bytes[i]) { ++i; continue; }
            size_t start = i;
            while (i < bytes.size() && (base[i] != bytes[i] || (i + 1 < bytes.size() && base[i + 1] != bytes[i + 1]))) ++i;
            if (!out.empty()) out += ' ';
            out += i - 1 > start ? fmt::format("{:x}-{:x}", start, i - 1) : fmt::format("{:x}", start);
            ++ranges;
        }
        return out;
    }
};

int g_diffReports = 0; // first few clicks only

struct StatsGuard {
    PlayLayer* layer;
    GJGameLevel* level;
    int jumps;
    int uncommittedJumps;
    bool hasJumped;
    int clicks;
    int attempts;
    bool jumping;
    geode::SeedValueRSV levelAttempts;
    geode::SeedValueRSV levelJumps;
    geode::SeedValueRSV levelClicks;
    geode::SeedValueRSV levelAttemptTime;
    int chk = 0;
    bool chkValid = false;
    bool legit = false;
    // Level timers and progress (only checked, for the diagnostic note).
    double totalTime = 0;
    double levelTime = 0;
    unsigned int commandIndex = 0;
    unsigned int progress = 0;
    int currentStep = 0;
    // PlayLayer flags around GD's anti-cheat and progress checks.
    decltype(PlayLayer::m_unk36c8) s_m_unk36c8 {};
    decltype(PlayLayer::m_unk36cc) s_m_unk36cc {};
    decltype(PlayLayer::m_unk36cd) s_m_unk36cd {};
    decltype(PlayLayer::m_unk36ce) s_m_unk36ce {};
    decltype(PlayLayer::m_unk36cf) s_m_unk36cf {};
    decltype(PlayLayer::m_damageVerified) s_m_damageVerified {};
    decltype(PlayLayer::m_passedIntegrity) s_m_passedIntegrity {};
    decltype(PlayLayer::m_isIgnoreDamageEnabled) s_m_isIgnoreDamageEnabled {};
    decltype(PlayLayer::m_unk376d) s_m_unk376d {};
    decltype(PlayLayer::m_unk377c) s_m_unk377c {};
    decltype(PlayLayer::m_unk3788) s_m_unk3788 {};
    decltype(PlayLayer::m_unk378c) s_m_unk378c {};
    decltype(PlayLayer::m_endChecked) s_m_endChecked {};
    decltype(PlayLayer::m_unk37b0) s_m_unk37b0 {};
    decltype(PlayLayer::m_unk37b1) s_m_unk37b1 {};
    decltype(PlayLayer::m_unk37cc) s_m_unk37cc {};
    decltype(PlayLayer::m_unk37e0) s_m_unk37e0 {};
    decltype(PlayLayer::m_hasCompletedLevel) s_m_hasCompletedLevel {};
    decltype(PlayLayer::m_lastAttemptPercent) s_m_lastAttemptPercent {};
    decltype(PlayLayer::m_unk38c8) s_m_unk38c8 {};
    decltype(PlayLayer::m_unk38d0) s_m_unk38d0 {};
    decltype(PlayLayer::m_isTestMode) s_m_isTestMode {};
    decltype(PlayLayer::m_isPracticeMode) s_m_isPracticeMode {};

    RawSnapshot rawLayer;
    RawSnapshot rawLevel;
    RawSnapshot rawSpike; // GD's hidden anti-cheat spike object

    explicit StatsGuard(PlayLayer* l) : layer(l), level(l ? l->m_level : nullptr) {
        if (g_diffReports < 6) {
            rawLayer.take(layer, sizeof(PlayLayer));
            if (level) rawLevel.take(level, sizeof(GJGameLevel));
            if (layer->m_anticheatSpike) rawSpike.take(layer->m_anticheatSpike, sizeof(GameObject));
        }
        jumps = layer->m_jumps;
        uncommittedJumps = layer->m_uncommittedJumps;
        hasJumped = layer->m_hasJumped;
        clicks = layer->m_clicks;
        attempts = layer->m_attempts;
        jumping = layer->m_jumping;
        totalTime = layer->m_gameState.m_totalTime;
        levelTime = layer->m_gameState.m_levelTime;
        commandIndex = layer->m_gameState.m_commandIndex;
        progress = layer->m_gameState.m_currentProgress;
        currentStep = layer->m_currentStep;
        s_m_unk36c8 = layer->m_unk36c8;
        s_m_unk36cc = layer->m_unk36cc;
        s_m_unk36cd = layer->m_unk36cd;
        s_m_unk36ce = layer->m_unk36ce;
        s_m_unk36cf = layer->m_unk36cf;
        s_m_damageVerified = layer->m_damageVerified;
        s_m_passedIntegrity = layer->m_passedIntegrity;
        s_m_isIgnoreDamageEnabled = layer->m_isIgnoreDamageEnabled;
        s_m_unk376d = layer->m_unk376d;
        s_m_unk377c = layer->m_unk377c;
        s_m_unk3788 = layer->m_unk3788;
        s_m_unk378c = layer->m_unk378c;
        s_m_endChecked = layer->m_endChecked;
        s_m_unk37b0 = layer->m_unk37b0;
        s_m_unk37b1 = layer->m_unk37b1;
        s_m_unk37cc = layer->m_unk37cc;
        s_m_unk37e0 = layer->m_unk37e0;
        s_m_hasCompletedLevel = layer->m_hasCompletedLevel;
        s_m_lastAttemptPercent = layer->m_lastAttemptPercent;
        s_m_unk38c8 = layer->m_unk38c8;
        s_m_unk38d0 = layer->m_unk38d0;
        s_m_isTestMode = layer->m_isTestMode;
        s_m_isPracticeMode = layer->m_isPracticeMode;
        if (level) {
            levelAttempts = level->m_attempts;
            levelJumps = level->m_jumps;
            levelClicks = level->m_clicks;
            levelAttemptTime = level->m_attemptTime;
            chk = level->m_chk;
            chkValid = level->m_isChkValid;
            legit = level->m_isCompletionLegitimate;
        }
    }

    ~StatsGuard() {
        if (g_diffReports < 6 && rawLayer.base) {
            ++g_diffReports;
            auto pl = rawLayer.diff(12);
            auto lv = rawLevel.base ? rawLevel.diff(8) : std::string();
            log::info("[PB diag] PlayLayer changed: {} | Level changed: {}", pl, lv);
            // Split into window-sized lines.
            std::string line = "PL:";
            std::istringstream words(pl.empty() ? std::string("none") : pl);
            for (std::string w; words >> w;) {
                if (line.size() + w.size() > 40) { menu::log(line); line = "PL:"; }
                line += " " + w;
            }
            menu::log(line);
            menu::log("LV: " + (lv.empty() ? std::string("none") : lv));
            auto sp = rawSpike.base ? rawSpike.diff(8) : std::string("no spike");
            menu::log("SP: " + (sp.empty() ? std::string("none") : sp));
        }
        static bool reported = false;
        if (!reported) {
            bool changed = layer->m_jumps != jumps || layer->m_uncommittedJumps != uncommittedJumps
                || layer->m_clicks != clicks || layer->m_attempts != attempts
                || (level && (level->m_jumps.value() != levelJumps.value() || level->m_clicks.value() != levelClicks.value()
                    || level->m_attempts.value() != levelAttempts.value() || level->m_chk != chk));
            auto const& now = layer->m_gameState;
            char const* what = nullptr;
            if (changed) what = "jump/click counters";
            else if (now.m_totalTime != totalTime) what = "total time";
            else if (now.m_levelTime != levelTime) what = "level time";
            else if (now.m_commandIndex != commandIndex) what = "command index";
            else if (now.m_currentProgress != progress) what = "progress";
            else if (layer->m_currentStep != currentStep) what = "step counter";
            else if (layer->m_unk36c8 != s_m_unk36c8) what = "m_unk36c8";
            else if (layer->m_unk36cc != s_m_unk36cc) what = "m_unk36cc";
            else if (layer->m_unk36cd != s_m_unk36cd) what = "m_unk36cd";
            else if (layer->m_unk36ce != s_m_unk36ce) what = "m_unk36ce";
            else if (layer->m_unk36cf != s_m_unk36cf) what = "m_unk36cf";
            else if (layer->m_damageVerified != s_m_damageVerified) what = "m_damageVerified";
            else if (layer->m_passedIntegrity != s_m_passedIntegrity) what = "m_passedIntegrity";
            else if (layer->m_isIgnoreDamageEnabled != s_m_isIgnoreDamageEnabled) what = "m_isIgnoreDamageEnabled";
            else if (layer->m_unk376d != s_m_unk376d) what = "m_unk376d";
            else if (layer->m_unk377c != s_m_unk377c) what = "m_unk377c";
            else if (layer->m_unk3788 != s_m_unk3788) what = "m_unk3788";
            else if (layer->m_unk378c != s_m_unk378c) what = "m_unk378c";
            else if (layer->m_endChecked != s_m_endChecked) what = "m_endChecked";
            else if (layer->m_unk37b0 != s_m_unk37b0) what = "m_unk37b0";
            else if (layer->m_unk37b1 != s_m_unk37b1) what = "m_unk37b1";
            else if (layer->m_unk37cc != s_m_unk37cc) what = "m_unk37cc";
            else if (layer->m_unk37e0 != s_m_unk37e0) what = "m_unk37e0";
            else if (layer->m_hasCompletedLevel != s_m_hasCompletedLevel) what = "m_hasCompletedLevel";
            else if (layer->m_lastAttemptPercent != s_m_lastAttemptPercent) what = "m_lastAttemptPercent";
            else if (layer->m_unk38c8 != s_m_unk38c8) what = "m_unk38c8";
            else if (layer->m_unk38d0 != s_m_unk38d0) what = "m_unk38d0";
            else if (layer->m_isTestMode != s_m_isTestMode) what = "m_isTestMode";
            else if (layer->m_isPracticeMode != s_m_isPracticeMode) what = "m_isPracticeMode";
            if (what) {
                reported = true;
                menu::log(fmt::format("ghosts changed: {}", what));
            }
        }
        layer->m_jumps = jumps;
        layer->m_uncommittedJumps = uncommittedJumps;
        layer->m_hasJumped = hasJumped;
        layer->m_clicks = clicks;
        layer->m_attempts = attempts;
        layer->m_jumping = jumping;
        layer->m_unk36c8 = s_m_unk36c8;
        layer->m_unk36cc = s_m_unk36cc;
        layer->m_unk36cd = s_m_unk36cd;
        layer->m_unk36ce = s_m_unk36ce;
        layer->m_unk36cf = s_m_unk36cf;
        layer->m_damageVerified = s_m_damageVerified;
        layer->m_passedIntegrity = s_m_passedIntegrity;
        layer->m_isIgnoreDamageEnabled = s_m_isIgnoreDamageEnabled;
        layer->m_unk376d = s_m_unk376d;
        layer->m_unk377c = s_m_unk377c;
        layer->m_unk3788 = s_m_unk3788;
        layer->m_unk378c = s_m_unk378c;
        layer->m_endChecked = s_m_endChecked;
        layer->m_unk37b0 = s_m_unk37b0;
        layer->m_unk37b1 = s_m_unk37b1;
        layer->m_unk37cc = s_m_unk37cc;
        layer->m_unk37e0 = s_m_unk37e0;
        layer->m_hasCompletedLevel = s_m_hasCompletedLevel;
        layer->m_lastAttemptPercent = s_m_lastAttemptPercent;
        layer->m_unk38c8 = s_m_unk38c8;
        layer->m_unk38d0 = s_m_unk38d0;
        layer->m_isTestMode = s_m_isTestMode;
        layer->m_isPracticeMode = s_m_isPracticeMode;
        if (level) {
            level->m_attempts = levelAttempts;
            level->m_jumps = levelJumps;
            level->m_clicks = levelClicks;
            level->m_attemptTime = levelAttemptTime;
            level->m_chk = chk;
            level->m_isChkValid = chkValid;
            level->m_isCompletionLegitimate = legit;
        }
    }
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

        // Far enough past the input that the verdict can't change any more.
        if (s.decideAfter >= 0 && delay > 0 && step - delay >= s.decideAfter) return { false, false, step };

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

void resetTiming() {
    g.tick = 0;
    g.recentDts.fill(0.f);
    g.recentDtIndex = 0;
    g.lastStepDt = 0.25f;
    g.lastWarp = 0.f; // re-read on the next step
}

void onStep(PlayLayer* layer, float dt) {
    // A time warp change changes the step size: forget the old sizes right away
    // instead of carrying them for the next 16 steps.
    float warp = layer->m_gameState.m_timeWarp;
    if (warp != g.lastWarp) {
        g.recentDts.fill(0.f);
        g.recentDtIndex = 0;
        g.lastWarp = warp;
    }

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

    // Smallest delay allowed (the left handle of the range slider).
    double minMs = static_cast<double>(setting<int64_t>("buffer-min-ticks")) * 1000.0 / 240.0;
    int minDelay = std::clamp(msToTicks(minMs, perTick), 1, std::max(1, limit));

    bool anyWave = false;
    bool anyMini = false;
    bool anyFlying = false;
    Mode mainMode = modeOf(s.reals[0]);

    bool releaseCares = false;
    for (size_t i = 0; i < s.count; ++i) {
        auto p = s.reals[i];
        Mode mode = modeOf(p);
        if (mode == Mode::Ship && !bufferShip) return false;
        releaseCares |= releaseMatters(mode);
        anyWave |= mode == Mode::Wave;
        anyMini |= p->m_vehicleSize < 1.f;
        anyFlying |= isFlyingMode(mode);
    }
    // Releases: only when some player's mode cares when you let go (in dual,
    // wave + cube still counts because of the wave).
    if (!down && (!bufferReleases || !releaseCares)) return false;
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
    StatsGuard stats(layer); // restores GD's counters when onButton returns
    auto base = simulate(s, 0, budget);
    if (!base.died) return false;

    // A delay is safe when, counted from the moment the input goes in, the
    // ghost lives through the look-ahead, reaches a portal/pad/orb no sooner
    // than it would have died, or dies at least Min Gain later than it does
    // now. (Counting from the start of the run would give every delay free
    // credit for itself, so a delay that only postpones the death would pass.)
    auto survives = [&](SimResult const& r, int delay) {
        int afterPress = r.steps - delay;
        if (r.stopped) return afterPress > 0 && afterPress >= base.steps;
        if (!r.died) return true;
        return afterPress - base.steps >= minGain;
    };
    s.decideAfter = base.steps + minGain;

    // --- delay search ---
    // 0 = old deterministic behavior. >0 = randomly pick a safe delay within
    // this many ticks above the smallest one.
    int slider = Mod::get()->getSettingValue<int>("randomize");

    // `budget` is by-reference and drains across calls, exactly like the old
    // loop. Guard on it and keep the same call order so the budget state ends
    // up where the old code left it. Named `simAt` because the outer `survives`
    // already exists and a lambda named `survives` would shadow it.
    auto simAt = [&](int delaySteps, SimResult& out) -> bool {
        if (budget <= 0) return false;
        out = simulate(s, delaySteps, budget);
        return survives(out, delaySteps);
    };

    // Ascending scan, collecting every safe delay. `safe` stays sorted and
    // starts at `first`, so the old "+1 margin" pick is just safe[1].
    std::vector<int> safe;
    std::vector<SimResult> safeResults; // parallel to `safe`, avoids re-simulating
    int first = -1;

    for (int d = minDelay; d <= limit && budget > 0; ++d) {
        SimResult r;
        bool ok = simAt(d, r);

        if (ok) {
            if (first < 0) first = d;
            safe.push_back(d);
            safeResults.push_back(r);
        }
        else if ((r.died && r.steps < d) || (r.stopped && r.steps <= d)) {
            // The run ended before its input even went in. Every longer delay
            // follows the same path up to here and ends the same way.
            break;
        }

        if (first >= 0) {
            // Stop as soon as the window we care about is covered: one step for
            // the deterministic pick, `slider` steps for the randomized one.
            int reach = (slider > 0) ? (first + slider) : (first + 1);
            if (d >= reach) break;
        }
    }

    if (first < 0) {
        // nothing works -> pass the input through, exactly like before
        if (debug) {
            log::info(
                "{} would die after {} steps and no delay up to {} steps helps",
                down ? "Press" : "Release", base.steps, limit
            );
        }
        return false;
    }

    // old deterministic pick: smallest safe delay + 1 step of margin
    auto oldPick = [&]() -> int {
        if (first + 1 <= limit && safe.size() >= 2 && safe[1] == first + 1) return first + 1;
        return first;
    };

    int chosen;
    bool randomPick = false; // true when Randomize chose the delay
    if (slider <= 0 || safe.size() <= 1) {
        chosen = oldPick();
    } else {
        // Keep the same 1-step safety margin as the normal pick: skip the
        // smallest safe delay when the one right above it is safe too.
        size_t lo = (safe.size() >= 2 && safe[1] == first + 1) ? 1 : 0;
        std::uniform_int_distribution<size_t> dist(lo, safe.size() - 1);
        chosen = safe[dist(bufRng())];
        randomPick = safe.size() - lo > 1; // more than one option to pick from
    }

    SimResult const& result =
        safeResults[static_cast<size_t>(std::find(safe.begin(), safe.end(), chosen) - safe.begin())];

    ch.shift = chosen;
    ch.queue.push_back({ down, isPlayer1, g.tick + 1 + chosen });

    double earlyMs = chosen * perTick;
    if (debug) {
        log::info(
            "{} buffered by {} steps ({:.2f}ms, {}): dies after {} steps now, {} after delay",
            down ? "Press" : "Release", chosen, earlyMs, modeName(mainMode), base.steps,
            result.died ? fmt::format("{}", result.steps) : std::string(result.stopped ? "reaches a portal/pad/orb" : "is safe")
        );
    }
    if (auto overlay = BufferOverlay::get(layer)) {
        overlay->showBuffered(earlyMs, chosen, mainMode, !down, randomPick);
    }
    menu::saved(chosen, earlyMs, static_cast<int>(mainMode), !down, randomPick, static_cast<int>(layer->getCurrentPercent()));
    return true;
}

} // namespace pb
