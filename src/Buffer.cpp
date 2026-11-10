#include "Buffer.hpp"

#include "Coach.hpp"
#include "Config.hpp"
#include "Decide.hpp"
#include "Ghost.hpp"
#include "Popup.hpp"
#include "Window.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

using namespace geode::prelude;

namespace pb::buffer {

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// An input waiting to be handed to GD.
struct Pending {
    bool active = false;
    bool down = true;
    int stepsLeft = 0;    // whole steps to wait
    float frac = 0.f;     // then this far into the next step (finer timing)
    int elapsed = 0;      // steps since it was held back
    int lastChange = -1;  // `elapsed` when you last changed the button during the wait
    float ticks = 0.f;    // how long it's held back in all
};

// The opposite input of a quick tap made while a press was held back: it fires
// as long after the press as you actually held the button.
struct Flip {
    bool active = false;
    bool down = false;
    int stepsLeft = 0;
};

// The last time the buffer changed one of your inputs (for the death check).
struct LastHelp {
    bool active = false;
    long long step = 0;
    double ticks = 0.0;
    int kind = 0; // 0 click, 1 release, 2 hold fix
};

Pending g_pending;
Flip g_flip;
LastHelp g_lastHelp;
bool g_feeding = false;
bool g_held = false;         // the button as you hold it
bool g_fed = false;          // the button as GD last got it
bool g_isP1 = true;
bool g_splitArmed = false;   // the update hook is about to fire the pending input mid-step
bool g_autoReleased = false; // hold fix let go for you while you still hold the button
int g_autoSince = 0;
int g_autoLimit = 0;
int g_sinceInput = 999;      // steps since your last input (capped)
int g_burst = 0;             // quick inputs in a row
int g_attempt = -1;
bool g_newAttempt = true;    // the next step is the first of an attempt
PlayLayer* g_layer = nullptr;

// Where in time the game is. A physics step can run in parts (Click Between
// Frames splits it at your click), so "now" is a step plus how far into it.
long long g_step = 0;     // whole steps of player 1 started so far
double g_clock = 0.0;     // step time so far (for moving objects)
float g_phase = 0.f;      // how much of the current step has run
float g_sum = 0.f;        // the same, measured up to the game's own end of step
bool g_inStep = false;    // inside player 1's update
float g_lastDt = 0.25f;   // the last update call
float g_recent[16] {};    // recent update calls
int g_recentAt = 0;
float g_seenDt = 0.25f;   // the largest of them (a split step's parts are smaller)
float g_exactDt = 0.25f;  // a whole step, measured between two of the game's end-of-step checks
bool g_exact = false;     // ... which is known
int g_exactAge = 0;

constexpr int SNAP_EVERY = 24; // steps between coach snapshots

float fullDt() {
    float dt = g_exact ? g_exactDt : g_seenDt;
    return (dt > 0.001f && dt < 2.f) ? dt : 0.25f;
}

// Step size for the ghost: exactly what the real player gets.
float stepDt() {
    if (config().steadyTiming) return fullDt();
    return (g_lastDt > 0.001f && g_lastDt < 2.f) ? g_lastDt : 0.25f;
}

// When an input handed to GD right now takes effect, in steps.
double inputTime() {
    if (g_phase > 0.f) return static_cast<double>(g_step) + std::clamp(g_phase / fullDt(), 0.f, 0.999f);
    return static_cast<double>(g_inStep ? g_step : g_step + 1);
}

int modeOf(PlayerObject* p) {
    if (p->m_isShip) return SHIP;
    if (p->m_isBall) return BALL;
    if (p->m_isBird) return UFO;
    if (p->m_isDart) return WAVE;
    if (p->m_isRobot) return ROBOT;
    if (p->m_isSpider) return SPIDER;
    if (p->m_isSwing) return SWING;
    return CUBE;
}

int percentOf(PlayLayer* layer) {
    return static_cast<int>(std::clamp(layer->getCurrentPercent(), 0.f, 100.f));
}

// Largest delay for the players' current gamemode(s).
int limitFor(PlayerObject* p1, PlayerObject* p2) {
    int limit = config().limit(modeOf(p1));
    if (p2) limit = std::min(limit, config().limit(modeOf(p2)));
    return limit;
}

std::string tickText(double ticks) {
    if (std::abs(ticks - std::round(ticks)) < 1e-6) {
        int t = static_cast<int>(std::lround(ticks));
        return fmt::format("+{} tick{}", t, t == 1 ? "" : "s");
    }
    return fmt::format("+{:.1f} ticks", ticks);
}

std::mt19937& rng() {
    static std::mt19937 gen { std::random_device {}() };
    return gen;
}

// ---------------------------------------------------------------------------
// Your taps
// ---------------------------------------------------------------------------

// Your taps (steps from press to release) per mode, newest last, and the press being timed.
std::vector<int> g_taps[MODES];
long long g_pressAt = -1;
int g_pressMode = CUBE;
bool g_tapsLoaded = false;

bool tapMode(int mode) { return mode != WAVE && mode != SHIP; } // hold length varies too much there

void loadTaps() {
    if (g_tapsLoaded) return;
    g_tapsLoaded = true;
    auto mod = Mod::get();
    for (int m = 0; m < MODES; ++m) {
        auto key = fmt::format("tap-{}", m);
        if (!mod->hasSavedValue(key)) continue;
        int tap = static_cast<int>(mod->getSavedValue<int64_t>(key));
        if (tap > 0) g_taps[m].assign(4, tap); // last session's usual tap to start from
    }
}

// Your taps in a mode at a given point of their spread (0.5 = the usual one), 0 = not known yet.
int tapAt(int mode, double where) {
    if (mode < 0 || mode >= MODES) return 0;
    auto taps = g_taps[mode];
    if (taps.size() < 4) return 0;
    std::sort(taps.begin(), taps.end());
    size_t i = std::min(taps.size() - 1, static_cast<size_t>(std::lround(where * (taps.size() - 1))));
    return std::clamp(taps[i], 1, 120);
}
int usualTap(int mode) { return tapAt(mode, 0.5); }

// Times your real presses and releases (whatever the buffer does with them).
void noteInput(PlayerObject* p1, bool down) {
    if (!p1) return;
    if (down) {
        g_pressAt = g_step;
        g_pressMode = modeOf(p1);
        return;
    }
    if (g_pressAt < 0) return;
    int tap = static_cast<int>(g_step - g_pressAt);
    g_pressAt = -1;
    if (tap < 1 || tap > 120 || !tapMode(g_pressMode)) return;
    auto& taps = g_taps[g_pressMode];
    taps.push_back(tap);
    if (taps.size() > 16) taps.erase(taps.begin());
}

void saveTaps() {
    auto mod = Mod::get();
    for (int m = 0; m < MODES; ++m) {
        if (int tap = usualTap(m)) mod->setSavedValue<int64_t>(fmt::format("tap-{}", m), tap);
    }
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------

// Where the ghost said player 1 would be for the first steps after an input,
// compared with where it really is. Done for every input the ghost looks at,
// held back or not.
constexpr float CHECK_TOLERANCE = 0.5f; // units (a block is 30)
constexpr int CHECK_STEPS = 8;
struct Check {
    bool armed = false;  // the input is still held back
    bool active = false; // it's in: comparing step by step
    bool holdMatters = false; // robot / wave / ship: your own release changes the path
    std::vector<CCPoint> predicted;
    long long firstEnd = 0; // predicted[0] is where you are when this step ends
    float worst = 0.f;
    bool tags[static_cast<int>(Feature::Count)] {};
};
Check g_check;
int g_checks = 0;
int g_hits = 0;
int g_tagChecks[static_cast<int>(Feature::Count)] {};
int g_tagMisses[static_cast<int>(Feature::Count)] {};

// What the ghost said happens after an input if you do nothing else: you die at
// a certain step, or you're fine up to one. Scored once it's known.
struct Call {
    bool armed = false;  // waits for the held-back input to fire (`at` counts from there)
    bool active = false;
    bool dies = false;
    long long at = 0;    // dies: the step; fine: the last step it vouches for
    bool holdMatters = false;
};
Call g_call;

void finishCheck(bool miss) {
    g_check.armed = g_check.active = false;
    g_checks++;
    if (!miss) g_hits++;
    if (miss) log::info("self-check: prediction missed by {:.2f} units", g_check.worst);
    for (int f = 0; f < static_cast<int>(Feature::Count); ++f) {
        if (!g_check.tags[f]) continue;
        g_tagChecks[f]++;
        if (miss) g_tagMisses[f]++;
        // a feature whose predictions keep missing turns itself off until the game restarts
        if (g_tagMisses[f] >= 3 && g_tagMisses[f] * 10 >= g_tagChecks[f] * 4) {
            auto feature = static_cast<Feature>(f);
            disableForSession(feature);
            auto line = fmt::format("{} off for this session: it missed {} of {}", featureName(feature), g_tagMisses[f],
                g_tagChecks[f]);
            log::warn("{}", line);
            window::note(line, window::Bad);
            g_tagMisses[f] = 0;
            g_tagChecks[f] = 0;
        }
    }
    window::accuracy(g_checks, g_hits);
}

void finishCall(bool right) {
    g_call = {};
    coach::called(right);
}

// Called at the start of every whole step: the step before it is done.
void stepChecks(PlayerObject* p1) {
    long long done = g_step - 1;
    if (g_check.active && p1) {
        long long i = done - g_check.firstEnd;
        if (i >= static_cast<long long>(g_check.predicted.size())) {
            finishCheck(g_check.worst > CHECK_TOLERANCE);
        }
        else if (i >= 0) {
            auto real = p1->getPosition();
            auto want = g_check.predicted[static_cast<size_t>(i)];
            g_check.worst = std::max({ g_check.worst, std::abs(real.x - want.x), std::abs(real.y - want.y) });
            if (g_check.worst > CHECK_TOLERANCE) finishCheck(true);
            else if (i + 1 >= static_cast<long long>(g_check.predicted.size())) finishCheck(false);
        }
    }
    if (g_call.active) {
        if (g_call.dies && done > g_call.at + 2) finishCall(false);  // still alive: it was wrong
        else if (!g_call.dies && done >= g_call.at) finishCall(true); // made it as far as it said
    }
}

// Your own input changed the path: the comparison can't continue.
void dropChecks(bool down) {
    if (g_check.active && (down || g_check.holdMatters)) g_check = {};
    if (g_call.active && (down || g_call.holdMatters)) g_call = {};
}

void tagCheck(ghost::Setup const& setup, ghost::RunInfo const& info, bool fraction) {
    g_check.tags[static_cast<int>(Feature::Finer)] = fraction;
    g_check.tags[static_cast<int>(Feature::Remainder)] = setup.remainder > 0.f;
    g_check.tags[static_cast<int>(Feature::Moving)] = info.moved;
    g_check.tags[static_cast<int>(Feature::Pads)] = info.pad;
    g_check.tags[static_cast<int>(Feature::Orbs)] = info.orb;
}

bool holdMatters(PlayerObject* p1) {
    return p1->m_isRobot || p1->m_isDart || p1->m_isShip;
}

// How many steps of a run can be compared before your own next input changes the path anyway.
int checkLength(ghost::Setup const& setup, bool down) {
    int length = CHECK_STEPS;
    if (down && setup.p1->m_isRobot && setup.tapSteps > 0) length = std::min(length, setup.tapSteps);
    return length;
}

// An input went through as it is: check what the ghost said about it.
void watchPassed(ghost::Setup const& setup, ghost::Result const& now, ghost::RunInfo const& info, std::vector<CCPoint>& path,
                 long long firstEnd, int horizon) {
    g_check = {};
    g_call = {};
    if (now.hitStop) return;
    bool matters = holdMatters(setup.p1);
    if (path.size() >= 3) {
        g_check.predicted = std::move(path);
        g_check.firstEnd = firstEnd;
        g_check.holdMatters = matters;
        g_check.active = true;
        tagCheck(setup, info, false);
    }
    if (now.cleared) return; // it stopped looking once the jump landed
    g_call.active = true;
    g_call.holdMatters = matters;
    g_call.dies = now.survived < horizon;
    g_call.at = g_call.dies ? firstEnd + now.survived : firstEnd + horizon - 1;
}

// An input is held back: run the chosen delay once more and note where player 1
// should be for the first steps after it, and how the run ends.
void armChecks(PlayLayer* layer, ghost::Setup const& setup, double delay, bool down, int horizon) {
    g_check = {};
    g_call = {};
    auto record = setup;
    record.record = &g_check.predicted;
    record.recordMax = checkLength(setup, down);
    auto r = ghost::run(layer, record, delay, down, horizon);
    if (r.hitStop) {
        g_check = {};
        return;
    }
    bool matters = holdMatters(setup.p1);
    if (g_check.predicted.size() >= 3) {
        g_check.holdMatters = matters;
        g_check.armed = true;
        tagCheck(setup, ghost::lastRun(), std::abs(delay - std::floor(delay + 1e-6)) > 0.01);
    }
    else {
        g_check = {};
    }
    if (r.cleared || r.diedBeforeInput) return;
    g_call.armed = true;
    g_call.holdMatters = matters;
    g_call.dies = r.survived < horizon;
    g_call.at = g_call.dies ? r.survived : horizon - 1; // counted from the step it fires in
}

// ---------------------------------------------------------------------------
// Handing inputs to GD
// ---------------------------------------------------------------------------

void feed(PlayLayer* layer, bool down, double time, float moved = 0.f) {
    g_feeding = true;
    g_fed = down;
    coach::logInput(time, down, moved);
    layer->handleButton(down, static_cast<int>(PlayerButton::Jump), g_isP1);
    g_feeding = false;
}

void cancel() {
    g_pending = {};
    g_flip = {};
    g_splitArmed = false;
    g_autoReleased = false;
    g_check.armed = false;
    g_call.armed = false;
}

void remember(double ticks, int kind) {
    g_lastHelp = { true, g_step, ticks, kind };
}

void firePending(PlayLayer* layer, double time) {
    feed(layer, g_pending.down, time, g_pending.ticks);
    g_pending.active = false;
    if (g_check.armed) {
        g_check.armed = false;
        g_check.active = true;
        g_check.firstEnd = g_step;
    }
    if (g_call.armed) {
        g_call.armed = false;
        g_call.active = true;
        g_call.at += g_step;
    }
    if (g_held != g_pending.down) {
        // you changed the button during the wait: keep the tap as long as you made it
        int tap = std::max(1, g_pending.lastChange);
        g_flip = { true, g_held, tap };
    }
}

void report(PlayLayer* layer, PlayerObject* p1, decide::Choice const& c, bool down, int limit) {
    double ticks = c.ticks(); // real delay: the rest of this step + whole steps
    double ms = ticks * TICK_MS;
    log::info("held {} back {} ({:.1f} ms): {} -> {} steps survived [limit {}]{}{}{}{}",
        down ? "press" : "release", tickText(ticks), ms, c.base, c.result, limit,
        c.random ? ", random" : "", c.loose ? ", loose" : "", c.better ? ", best fix" : "", c.sure ? ", sure" : "");
    if (config().popup) {
        popup::show(fmt::format("{} {:.1f} ms early ({})", down ? "click" : "release", ms, tickText(ticks)));
    }
    window::saved({ static_cast<int>(std::lround(ticks)), ms, modeOf(p1), !down, c.random, percentOf(layer) });
    remember(ticks, down ? 0 : 1);
}

// An input the ghost says dies, with no safe delay within the limit: say why.
void reportMiss(PlayLayer* layer, decide::Choice const& c, bool down, int limit) {
    char const* what = down ? "click" : "release";
    int percent = percentOf(layer);
    std::string line;
    if (c.needed > 0) {
        line = fmt::format("{}% {} {:.0f} ms early: needed +{}, limit {}", percent, what, c.needed * TICK_MS, c.needed, limit);
    }
    else if (c.why == decide::Why::KnifeEdge) {
        line = fmt::format("{}% {} let through: fix too tight to trust", percent, what);
    }
    else if (c.why == decide::Why::LongTap) {
        line = fmt::format("{}% {} let through: not safe with long taps", percent, what);
    }
    else {
        line = fmt::format("{}% {} let through: no delay saves it", percent, what);
    }
    log::info("{}", line);
    window::note(line, c.needed > 0 ? window::Early : window::Plain);
    coach::missed(g_step, down, c.needed, limit);
}

// Hold fix (wave, and ship when it's on): you're holding and holding on kills you
// within the delay limit, but letting go right now is safe. Let go for you; your
// own release then lands on an already released button.
bool holdFix(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, double time) {
    auto const& cfg = config();
    if (!cfg.holdFix || g_autoReleased || !g_held || !g_fed || p2 || g_pending.active || g_flip.active) return false;
    if (cfg.practiceOnly && !layer->m_isPracticeMode) return false;
    bool wave = p1->m_isDart;
    bool ship = p1->m_isShip && cfg.ship;
    if (!wave && !ship) return false;
    int limit = cfg.limit(modeOf(p1));
    if (limit <= 0 || p1->m_isDead) return false;

    ghost::Setup setup;
    setup.p1 = p1;
    setup.dt = stepDt();
    setup.flying = true;
    setup.fresh = cfg.freshGhost;
    setup.heldBefore = true;

    auto hold = ghost::coast(layer, setup, limit + 1);
    if (hold.hitStop || hold.survived >= limit) return false; // holding on is fine (or can't tell)
    bool mini = p1->m_vehicleSize < 1.f;
    int horizon = wave ? (mini ? 28 : 38) : 48;
    auto letGo = ghost::run(layer, setup, ghost::NOW, false, horizon);
    if (letGo.hitStop || letGo.survived < horizon) return false; // letting go doesn't save you either

    g_check = {};
    g_call = {};
    double ticks = hold.survived + 1;
    feed(layer, false, time, static_cast<float>(ticks));
    g_autoReleased = true;
    g_autoSince = 0;
    g_autoLimit = limit;
    log::info("hold fix: let go for you ({} steps before holding on would hit)", hold.survived + 1);
    if (cfg.popup) popup::show(fmt::format("let go for you ({:.1f} ms early)", ticks * TICK_MS));
    window::saved({ static_cast<int>(ticks), ticks * TICK_MS, modeOf(p1), true, false, percentOf(layer) });
    remember(ticks, 2);
    return true;
}

// Drops whatever is held back (a death or a restart).
void dropHeld() {
    cancel();
    g_check = {};
    g_call = {};
    g_fed = false;
    g_burst = 0;
    g_phase = 0.f;
    g_sum = 0.f;
}

} // namespace

bool feeding() {
    return g_feeding;
}

bool take(PlayLayer* layer, bool down, bool isPlayer1) {
    // second player of a two player level: never touched
    if (layer->m_level && layer->m_level->m_twoPlayerMode && !isPlayer1) return false;

    auto const& cfg = config();
    auto p1 = layer->m_player1;
    if (p1 && p1->m_isDead) coach::lateInput(down); // a click that came after the death

    g_isP1 = isPlayer1;
    g_held = down;
    double when = inputTime();
    auto pass = [&] {
        g_fed = down;
        coach::logInput(when, down, 0.f);
        return false;
    };

    noteInput(p1, down);
    // a new click (or a release where holding matters) changes the path: stop comparing
    dropChecks(down);

    // Hold fix already let go for you: this is your own release arriving.
    if (g_autoReleased) {
        g_autoReleased = false;
        if (!down) {
            g_sinceInput = 0;
            return true; // GD has it released already
        }
    }

    PlayerObject* p2 = layer->m_gameState.m_isDualMode ? layer->m_player2 : nullptr;
    if (!cfg.enabled || cfg.maxTicks <= 0 || !p1 || p1->m_isDead || layer->m_isPlatformer || !ghost::ready()
        || (cfg.practiceOnly && !layer->m_isPracticeMode)) {
        cancel();
        return pass();
    }
    bool ship = p1->m_isShip || (p2 && p2->m_isShip);
    if (ship && !cfg.ship) {
        cancel();
        return pass();
    }

    // Something is already held back: this is part of the same tap.
    if (g_pending.active) {
        g_pending.lastChange = g_pending.elapsed;
        // you changed the button before it even fired: nothing to compare any more
        if (down || g_check.holdMatters) g_check = {};
        if (down || g_call.holdMatters) g_call = {};
        g_sinceInput = 0;
        return true;
    }
    // The end of the last tap hasn't fired yet: fire it first, then handle this one.
    if (g_flip.active) {
        g_flip.active = false;
        if (g_flip.down != g_fed) feed(layer, g_flip.down, when);
    }

    bool wave = p1->m_isDart || (p2 && p2->m_isDart);
    bool miniWave = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

    // Fast tapping in wave: let it through untouched.
    if (g_sinceInput < 8) {
        int before = g_burst++;
        g_sinceInput = 0;
        if (before >= 2 && wave) return pass();
    }
    else {
        g_sinceInput = 0;
        g_burst = 0;
    }

    if (!down) {
        if (!cfg.releases) return pass();
        // letting go early only matters in wave and robot (and ship when it's on)
        bool robot = p1->m_isRobot || (p2 && p2->m_isRobot);
        if (!wave && !robot && !(ship && cfg.ship)) return pass();
    }

    bool flying = p1->m_isDart || p1->m_isBird || p1->m_isSwing || (p2 && (p2->m_isDart || p2->m_isBird || p2->m_isSwing));
    int horizon = miniWave ? 28 : wave ? 38 : (cfg.smartSafety && !flying) ? 64 : 48;
    int limit = limitFor(p1, p2);
    if (limit <= 0) return pass();

    ghost::Setup setup;
    setup.p1 = p1;
    setup.p2 = p2;
    setup.dt = stepDt();
    setup.flying = flying;
    setup.fresh = cfg.freshGhost;
    setup.heldBefore = g_fed;
    // Click Between Frames hands GD your click partway through a step: the rest of
    // that step still runs before any whole step does
    float full = fullDt();
    bool aligned = true;               // the ghost's steps line up with the real ones
    long long firstEnd = g_step + 1;   // the step the input lands in
    if (cfg.steadyTiming && g_phase > 0.f) {
        float left = std::clamp(1.f - g_phase / full, 0.f, 1.f);
        if (left < 0.01f) {
            // as good as the end of this step
        }
        else if (cfg.remainderModel && (g_exact || g_phase < 0.9f * full)) {
            setup.remainder = left;
            firstEnd = g_step;
        }
        else {
            aligned = false;
        }
    }
    // a press: the ghost lets go when you usually do (cube-like modes), checked with
    // your shorter taps and, if they're clearly different, your longer ones too
    int longTap = 0;
    if (down && cfg.tapModel && tapMode(modeOf(p1)) && (!p2 || tapMode(modeOf(p2)))) {
        int shortTap = tapAt(modeOf(p1), 0.25);
        int longerTap = tapAt(modeOf(p1), 0.75);
        setup.tapSteps = shortTap;
        if (shortTap > 0 && longerTap > shortTap + 1) longTap = longerTap;
    }

    // the input as it is (its path is kept for the self-check)
    std::vector<CCPoint> path;
    bool comparable = cfg.selfCheck && !p2 && aligned;
    auto first = setup;
    if (comparable) {
        first.record = &path;
        first.recordMax = checkLength(setup, down);
    }
    auto now = ghost::run(layer, first, ghost::NOW, down, horizon);
    auto nowInfo = ghost::lastRun();
    auto watch = [&] {
        if (comparable) watchPassed(setup, now, nowInfo, path, firstEnd, horizon);
    };

    // fine as it is, or something ahead the ghost can't judge (an orb, a mode portal)
    if (now.hitStop || now.survived >= horizon) {
        watch();
        return pass();
    }

    decide::Params params;
    params.wave = wave;
    params.smart = cfg.smartSafety && !flying;
    params.dual = p2 != nullptr;
    params.horizon = horizon;
    params.limit = limit;
    params.base = now.survived;
    params.remainder = setup.remainder;
    params.randomize = cfg.randomize;
    params.bestFix = cfg.bestFix;
    params.finer = cfg.finerTiming;
    params.confidence = cfg.confidence;
    params.longTap = longTap;
    auto sim = [&](double delay, int steps, int tap) {
        auto s = setup;
        if (tap > 0) s.tapSteps = tap;
        auto r = ghost::run(layer, s, delay, down, steps);
        return decide::Outcome { r.survived, r.diedBeforeInput, r.hitStop, r.cleared };
    };
    auto pick = [](int count) { return std::uniform_int_distribution<int>(0, count - 1)(rng()); };
    auto c = decide::choose(params, sim, pick);
    if (!c.ok) {
        reportMiss(layer, c, down, limit);
        watch();
        return pass();
    }

    g_pending = {};
    g_pending.active = true;
    g_pending.down = down;
    g_pending.stepsLeft = static_cast<int>(std::floor(c.delay + 1e-6));
    g_pending.frac = static_cast<float>(c.delay - g_pending.stepsLeft);
    if (g_pending.frac < 0.01f) g_pending.frac = 0.f;
    g_pending.ticks = static_cast<float>(c.ticks());
    report(layer, p1, c, down, limit);
    if (cfg.selfCheck) armChecks(layer, setup, c.delay, down, horizon);
    return true;
}

float beforeStep(PlayLayer* layer, float dt) {
    g_splitArmed = false;
    auto const& cfg = config();
    if (layer->m_attempts != g_attempt) {
        g_attempt = layer->m_attempts;
        window::attempt(g_attempt, false);
        g_phase = 0.f;
        g_sum = 0.f;
        g_newAttempt = true;
    }

    g_lastDt = dt;
    g_recent[g_recentAt] = dt;
    g_recentAt = (g_recentAt + 1) % 16;
    g_seenDt = *std::max_element(std::begin(g_recent), std::end(g_recent));
    if (g_exact && ++g_exactAge > 8) g_exact = false; // the game's end-of-step check stopped showing up

    // Click Between Frames runs a step in parts. Only the call that starts a step
    // counts (and fires inputs), so waits are whole steps long.
    float full = fullDt();
    if (g_exact && g_phase > 0.f && g_phase + dt > 1.01f * full) {
        // more than a whole step without the game's end-of-step check: that step is over all the same
        g_phase = 0.f;
        g_sum = 0.f;
        g_exact = false;
    }
    bool counts = !cfg.steadyTiming || g_phase <= 0.f;
    double now; // when an input fired from here takes effect
    if (counts) {
        g_step++;
        now = static_cast<double>(g_step);
    }
    else {
        now = static_cast<double>(g_step) + std::clamp(g_phase / full, 0.f, 0.999f);
    }
    g_phase = cfg.steadyTiming ? g_phase + dt : 0.f;
    g_sum += dt;
    g_inStep = true;

    auto p1 = layer->m_player1;
    auto p2 = layer->m_gameState.m_isDualMode ? layer->m_player2 : nullptr;
    if (counts) {
        if (g_sinceInput < 999) g_sinceInput++;
        stepChecks(p1);
        // the coach keeps a copy of you every few steps to play a death back from
        if (cfg.coach && p1 && !p2 && !p1->m_isDead && !layer->m_isPlatformer && ghost::snapshotsReady() && !coach::busy()
            && (g_newAttempt || g_step % SNAP_EVERY == 0)) {
            ghost::takeSnapshot(p1, g_step, g_fed, coach::inputCount(), stepDt());
        }
        g_newAttempt = false;
    }
    if (!cfg.enabled) return 0.f;

    bool shipBlocked = !cfg.ship && p1 && (p1->m_isShip || (p2 && p2->m_isShip));
    if (shipBlocked) {
        cancel();
        if (g_held != g_fed) feed(layer, g_held, now);
        return 0.f;
    }
    if (!counts) return 0.f;

    if (g_pending.active) {
        if (g_pending.stepsLeft > 0) {
            g_pending.stepsLeft--;
            g_pending.elapsed++;
            return 0.f;
        }
        if (g_pending.frac > 0.f) {
            // finer timing: fire it this far into the step (split step, see the update hook)
            if (cfg.finerTiming && !p2 && dt >= 0.9f * full && p1 && ghost::splittable(p1)) {
                g_splitArmed = true;
                return g_pending.frac;
            }
            // can't split this step (the ghost knew): fire at the start of the next one
            g_pending.frac = 0.f;
            g_pending.elapsed++;
            return 0.f;
        }
        firePending(layer, now);
        return 0.f;
    }
    if (g_flip.active) {
        if (--g_flip.stepsLeft > 0) return 0.f;
        g_flip.active = false;
        if (g_flip.down != g_fed) feed(layer, g_flip.down, now);
        return 0.f;
    }

    if (g_autoReleased) {
        // still holding long after hold fix let go: you meant to hold, give it back
        if (g_held && ++g_autoSince > g_autoLimit) {
            g_autoReleased = false;
            feed(layer, true, now);
        }
        else if (!g_held) {
            g_autoReleased = false;
        }
        return 0.f;
    }
    if (p1 && holdFix(layer, p1, p2, now)) return 0.f;

    // keep GD in step with the button (after anything that skipped GD)
    if (g_held != g_fed) feed(layer, g_held, now);
    return 0.f;
}

void fireSplit(PlayLayer* layer) {
    if (!g_splitArmed) return;
    g_splitArmed = false;
    if (g_pending.active) firePending(layer, static_cast<double>(g_step) + g_pending.frac);
}

void afterStep(PlayLayer* layer, float dt) {
    g_inStep = false;
    // Without the game's own end-of-step signal (stepEnded), a step counts as done
    // once nearly all of it has run.
    if (!config().steadyTiming || (!g_exact && g_phase >= 0.9f * fullDt())) g_phase = 0.f;
    g_clock += dt;
}

void stepEnded() {
    // everything player 1's update got since the last end of step is one whole step
    // (more than the largest single call would be several steps: not trusted)
    if (g_sum > 0.001f && g_sum < 2.f && g_sum <= g_seenDt * 1.05f) {
        g_exactDt = g_sum;
        g_exact = true;
        g_exactAge = 0;
    }
    g_sum = 0.f;
    g_phase = 0.f;
}

double clock() {
    return g_clock;
}

void playerDied(PlayLayer* layer, bool player1) {
    auto const& cfg = config();
    auto p1 = layer->m_player1;
    if (player1 && p1) {
        // the self-check: was this death what the ghost said would happen?
        bool expected = g_call.active && g_call.dies && std::llabs(g_step - g_call.at) <= 2;
        if (g_check.active) finishCheck(!expected);
        if (g_call.active) finishCall(g_call.dies ? expected : false);

        if (cfg.deathCheck && g_lastHelp.active) {
            long long since = g_step - g_lastHelp.step;
            if (since <= 144) { // more than 0.6 s ago is a later obstacle, not that input
                std::string what = g_lastHelp.kind == 2 ? "hold fix let go"
                    : fmt::format("a held-back {} ({})", g_lastHelp.kind == 1 ? "release" : "click", tickText(g_lastHelp.ticks));
                auto line = fmt::format("died {:.2f}s after {}", since / 240.0, what);
                log::info("death check: {}", line);
                if (cfg.popup) popup::show(line, false);
                window::note(line, window::Bad);
            }
        }
        g_lastHelp.active = false;

        coach::Death death;
        death.step = g_step;
        death.percent = percentOf(layer);
        death.mode = modeOf(p1);
        death.held = g_fed;
        death.dual = layer->m_gameState.m_isDualMode;
        death.tap = usualTap(death.mode);
        death.limit = cfg.enabled ? cfg.limit(death.mode) : 0;
        coach::died(layer, death);
    }
    dropHeld();
}

void levelStarted(PlayLayer* layer) {
    g_layer = layer;
    loadTaps();
    g_pressAt = -1;
    ghost::create(layer, config().coach);
    coach::levelStarted(layer);
    dropHeld();
    g_held = false;
    g_isP1 = true;
    g_sinceInput = 999;
    g_lastHelp = {};
    g_attempt = layer->m_attempts;
    g_newAttempt = true;
    g_exact = false;
    popup::attach(layer);
    window::attempt(g_attempt, true);
}

void levelEnded(bool layerAlive) {
    saveTaps();
    coach::levelEnded();
    ghost::destroy(layerAlive);
    dropHeld();
    g_feeding = false;
    g_layer = nullptr;
}

void reset() {
    dropHeld();
    g_newAttempt = true;
    coach::reset();
    ghost::clearTracks();
}

void flush() {
    auto layer = PlayLayer::get();
    if (!layer || layer != g_layer) {
        cancel();
        return;
    }
    if (g_pending.active) {
        g_pending.active = false;
        feed(layer, g_pending.down, inputTime(), g_pending.ticks);
    }
    cancel();
    // don't leave the button stuck the wrong way
    if (g_held != g_fed) feed(layer, g_held, inputTime());
}

} // namespace pb::buffer
