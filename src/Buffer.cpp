#include "Buffer.hpp"
#include "Menu.hpp"
#include "Overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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

// Level % for the log, worked out from the player's position. Doesn't call
// GD's own percent function, which isn't safe to call mid-step.
int levelPercent(PlayLayer* layer) {
    if (!layer || !layer->m_player1 || layer->m_levelLength <= 0.f) return -1;
    float pct = layer->m_player1->getPositionX() / layer->m_levelLength * 100.f;
    return static_cast<int>(std::clamp(pct, 0.f, 100.f));
}

// Ghost runs must leave GD exactly as it was. GD's collision code writes
// bookkeeping into the level layer when a player touches blocks, and if that
// sticks GD shows "New Best" but won't save it. Copy the layer's memory before
// the ghosts run and put back every value they changed. Pointers are left
// alone (lists GD grew or moved), only plain values are restored.
struct Watch {
    char const* name;
    uint64_t* base = nullptr;
    std::vector<uint64_t> saved;
    void take(void* object, size_t size) {
        if (!object) return;
        base = static_cast<uint64_t*>(object);
        saved.assign(base, base + size / 8);
    }
};

struct LayerRestore {
    PlayLayer* layer;
    std::vector<uint64_t> saved;
    std::array<Watch, 5> watches { Watch{"P1"}, Watch{"LV"}, Watch{"GM"}, Watch{"GS"}, Watch{"P2"} };
    static inline int reports = 0;
    explicit LayerRestore(PlayLayer* l) : layer(l) {
        auto words = reinterpret_cast<uint64_t const*>(layer);
        saved.assign(words, words + sizeof(PlayLayer) / 8);
        if (reports < 3) {
            watches[0].take(layer->m_player1, sizeof(PlayerObject));
            watches[1].take(layer->m_level, sizeof(GJGameLevel));
            watches[2].take(GameManager::get(), sizeof(GameManager));
            watches[3].take(GameStatsManager::get(), sizeof(GameStatsManager));
            watches[4].take(layer->m_player2, sizeof(PlayerObject));
        }
    }
    static bool looksLikePointer(uint64_t v) {
        return v >= 0x10000 && v <= 0x7fffffffffffull;
    }
    ~LayerRestore() {
        auto words = reinterpret_cast<uint64_t*>(layer);
        int restored = 0;
        std::string where, ptrs;
        int ptrCount = 0;
        for (size_t i = 0; i < saved.size(); ++i) {
            uint64_t now = words[i];
            uint64_t was = saved[i];
            if (now == was) continue;
            if (looksLikePointer(now) && looksLikePointer(was)) {
                if (ptrCount++ < 6) ptrs += fmt::format(" {:x}", i * 8);
                continue;
            }
            words[i] = was;
            if (restored++ < 6) where += fmt::format(" {:x}", i * 8);
        }
        if (reports < 3 && (restored || ptrCount || true)) {
            std::string other;
            for (auto& w : watches) {
                if (!w.base) continue;
                int n = 0;
                std::string at;
                for (size_t i = 0; i < w.saved.size(); ++i) {
                    if (w.base[i] != w.saved[i] && n++ < 5) at += fmt::format(" {:x}", i * 8);
                }
                if (n) other += fmt::format(" {}:{}{}", w.name, n, at);
            }
            if (restored || ptrCount || !other.empty()) {
                reports++;
                menu::log(fmt::format("fix {}:{} ptr {}:{}", restored, where, ptrCount, ptrs));
                if (!other.empty()) menu::log("chg" + other);
            }
        }
    }
};

void setHeld(PlayerObject* ghost, int index, bool held) {
    if (held) ghost->pushButton(PlayerButton::Jump);
    else ghost->releaseButton(PlayerButton::Jump);
    g_simHeld[index] = held;
}

// Copies a value at a raw byte offset of a PlayerObject. These are the exact
// fields (by GD memory position) the original Predictive Input Buffer copies
// onto its ghost, so the setup matches it byte for byte.
template <class T>
void copyRaw(PlayerObject* to, PlayerObject* from, size_t offset) {
    std::memcpy(reinterpret_cast<char*>(to) + offset, reinterpret_cast<char const*>(from) + offset, sizeof(T));
}

// Copies the real players onto the hidden ghosts, applies the input after
// `delay` steps, and runs the game's own physics and collision forward.
// Mirrors the original mod's simulation step for step: same setup calls and
// copied fields, and per step: input, update, rotation, collision check.
// `budget` is decreased by the number of steps used.
SimResult simulate(SimSetup const& s, int delay, int& budget) {
    SimGuard guard;

    std::array<PlayerObject*, 2> ghosts { nullptr, nullptr };
    for (size_t i = 0; i < s.count; ++i) {
        auto ghost = g.ghosts[i];
        auto real = s.reals[i];

        ghost->setVisible(false);
        ghost->m_isDead = false;
        ghost->copyAttributes(real);
        ghost->setRotation(real->getRotation());
        ghost->setPosition(real->getPosition());
        copyRaw<float>(ghost, real, 0x2fc);
        copyRaw<float>(ghost, real, 0x300);
        copyRaw<float>(ghost, real, 0xb84);
        copyRaw<bool>(ghost, real, 0x9c1);
        copyRaw<bool>(ghost, real, 0x9bf);
        copyRaw<float>(ghost, real, 0x9f0);
        copyRaw<float>(ghost, real, 0x9f4);

        // Delayed runs: the button starts the way the real player has it now
        // (up before a press, held before a release).
        if (delay > 0) setHeld(ghost, static_cast<int>(i), !s.down);
        else g_simHeld[i] = !s.down;

        ghosts[i] = ghost;
    }

    int total = delay + s.horizon;
    int airborne = 0;
    int grounded = 0;

    for (int step = 0; step < total; ++step) {
        // Out of time: count it as a death so an unchecked delay never gets used.
        if (--budget < 0) return { true, false, -1 };

        if (step == delay) {
            for (size_t i = 0; i < s.count; ++i) setHeld(ghosts[i], static_cast<int>(i), s.down);
        }

        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->update(s.dt);
            ghosts[i]->updateRotation(s.dt);
        }
        if (g_simDied) return { true, false, step };

        for (size_t i = 0; i < s.count; ++i) {
            ghosts[i]->m_collisionLogTop->removeAllObjects();
            ghosts[i]->m_collisionLogBottom->removeAllObjects();
            ghosts[i]->m_collisionLogLeft->removeAllObjects();
            ghosts[i]->m_collisionLogRight->removeAllObjects();
            if (g_testMode != 'D') s.layer->checkCollisions(ghosts[i], s.dt, false);
            if (g_simDied) return { true, false, step };
            if (g_simStopped) return { false, true, step };
        }

        // Far enough past the input that the verdict can't change any more.
        if (s.decideAfter >= 0 && delay > 0 && step - delay >= s.decideAfter) return { false, false, step };

        // Jumped, then landed and stayed on the ground: that's a safe outcome.
        if (s.earlyExit && step >= delay) {
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

    // PB bug hunt. A: no prediction at all. C: fixed 2-tick delay on presses, no prediction.
    auto testMode = Mod::get()->getSettingValue<std::string>("pb-test");
    g_testMode = testMode.empty() ? 'N' : testMode[0];
    if (g_testMode == 'D' || g_testMode == 'E' || g_testMode == 'G') testMode = "B"; // sim variants, never delay
    if (testMode == "A") return false;
    if (testMode == "C") {
        if (!down) return false;
        ch.shift = 2;
        ch.queue.push_back({ down, isPlayer1, g.tick + 1 + 2 });
        menu::saved(2, 2 * (1000.0 / 240.0), 0, false, false, levelPercent(layer));
        return true;
    }

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
    LayerRestore restore(layer); // undoes whatever the ghost runs changed, on return
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

    if (testMode == "B") {
        menu::log(fmt::format("B: would delay +{}", chosen));
        return false;
    }

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
    menu::saved(chosen, earlyMs, static_cast<int>(mainMode), !down, randomPick, levelPercent(layer));
    return true;
}

} // namespace pb
