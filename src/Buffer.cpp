#include "Buffer.hpp"

#include "Config.hpp"
#include "Ghost.hpp"
#include "Popup.hpp"
#include "Window.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace geode::prelude;

namespace pb::buffer {

namespace {

// An input waiting to be handed to GD.
struct Pending {
    bool active = false;
    bool down = true;
    int stepsLeft = 0;    // whole steps to wait
    float frac = 0.f;     // then this far into the next step (finer timing)
    int elapsed = 0;      // steps since it was held back
    int lastChange = -1;  // `elapsed` when you last changed the button during the wait
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
bool g_held = false;       // the button as you hold it
bool g_fed = false;        // the button as GD last got it
bool g_isP1 = true;
bool g_splitArmed = false; // the update hook is about to fire the pending input mid-step
bool g_autoReleased = false; // hold fix let go for you while you still hold the button
int g_autoSince = 0;
int g_autoLimit = 0;
int g_sinceInput = 999;    // steps since your last input (capped)
int g_burst = 0;           // quick inputs in a row
long long g_step = 0;      // physics steps of player 1 so far
double g_clock = 0.0;      // step time so far (for moving objects)
float g_dt = 0.25f;        // last real step, time warp taken out
float g_recent[16] {};     // recent step sizes
int g_recentAt = 0;
float g_fullDt = 0.25f;    // largest recent step (a Click Between Frames split step is smaller)
float g_accum = 0.f;       // split steps adding up to a full one
int g_attempt = -1;
PlayLayer* g_layer = nullptr;

std::mt19937& rng() {
    static std::mt19937 gen { std::random_device {}() };
    return gen;
}

void feed(PlayLayer* layer, bool down) {
    g_feeding = true;
    g_fed = down;
    layer->handleButton(down, static_cast<int>(PlayerButton::Jump), g_isP1);
    g_feeding = false;
}

void cancel() {
    g_pending = {};
    g_flip = {};
    g_splitArmed = false;
    g_autoReleased = false;
}

int percentOf(PlayLayer* layer) {
    return static_cast<int>(std::clamp(layer->getCurrentPercent(), 0.f, 100.f));
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

// Largest delay for the players' current gamemode(s).
int limitFor(PlayerObject* p1, PlayerObject* p2) {
    int limit = config().limit(modeOf(p1));
    if (p2) limit = std::min(limit, config().limit(modeOf(p2)));
    return limit;
}

// Step size for the ghost.
float ghostDt(PlayLayer* layer) {
    float dt = (g_dt > 0.001f && g_dt < 2.f) ? g_dt : 0.25f;
    if (config().steadyTiming) {
        // a Click Between Frames split step is shorter than a real one: use the full step
        float warp = layer->m_gameState.m_timeWarp;
        float full = warp > 0.f ? g_fullDt / warp : g_fullDt;
        if (full > 0.001f && full < 2.f) dt = full;
    }
    return dt;
}

std::string tickText(double ticks) {
    if (std::abs(ticks - std::round(ticks)) < 1e-6) {
        int t = static_cast<int>(std::lround(ticks));
        return fmt::format("+{} tick{}", t, t == 1 ? "" : "s");
    }
    return fmt::format("+{:g} ticks", ticks);
}

struct Choice {
    double delay = 0.0; // 0 = nothing safe, let the input through
    int base = 0;       // steps survived with no delay
    int result = 0;     // steps survived with the chosen delay
    bool random = false;
    bool loose = false;  // only passed the looser rule
    bool better = false; // best fix picked a later delay that lasts longer
    bool sure = false;   // passed the confidence check
    char const* why = "no safe delay within the limit"; // when there's no fix
};

// Finds the delay for an input that would kill you as it is.
Choice choose(PlayLayer* layer, ghost::Setup const& setup, bool down, bool wave, int horizon, int limit, int base) {
    auto const& cfg = config();
    bool smart = cfg.smartSafety && !setup.flying;
    bool dual = setup.p2 != nullptr;

    // The original mod's rule: the delayed input has to live clearly longer
    // (wave gets stricter the longer the wait).
    auto loose = [&](double d, ghost::Result const& r) {
        if (r.hitStop) return r.survived > 0 && r.survived >= base;
        if (wave) {
            int need = base + 10 + static_cast<int>(std::ceil(2 * (d - 1)));
            int gain = 6 + static_cast<int>(std::ceil(2 * (d - 1)));
            return r.survived < horizon ? r.survived >= need : (r.survived - base >= gain || r.survived >= need);
        }
        return r.survived >= std::min(base + 12, horizon);
    };
    // Smart safety (cube-like modes): the jump has to clear everything ahead or land.
    auto strict = [&](double d, ghost::Result const& r) {
        if (!smart) return loose(d, r);
        if (r.hitStop) return r.survived > 0 && r.survived >= base;
        return r.survived >= horizon;
    };

    bool strictOk[MAX_TICKS + 1] {};
    bool looseOk[MAX_TICKS + 1] {};
    bool tested[MAX_TICKS + 1] {};
    int survived[MAX_TICKS + 1] {};
    int firstStrict = 0;
    int firstLoose = 0;
    int window = std::max(cfg.randomize, cfg.bestFix ? 2 : 0);
    for (int d = 1; d <= limit; ++d) {
        auto r = ghost::run(layer, setup, d, down, horizon);
        if (r.diedBeforeInput) break; // waiting any longer only dies sooner
        tested[d] = true;
        survived[d] = r.survived;
        strictOk[d] = strict(d, r);
        looseOk[d] = loose(d, r);
        if (strictOk[d] && !firstStrict) firstStrict = d;
        if (looseOk[d] && !firstLoose) firstLoose = d;
        // with a safe delay found, only a few ticks above it still matter
        if (firstStrict && d >= firstStrict + window) break;
    }

    Choice c;
    c.base = base;
    bool useStrict = firstStrict != 0;
    bool* pool = useStrict ? strictOk : looseOk;
    int first = useStrict ? firstStrict : firstLoose;
    if (!first) return c;
    c.loose = !useStrict;
    auto passes = [&](double d, ghost::Result const& r) {
        return !r.diedBeforeInput && (useStrict ? strict(d, r) : loose(d, r));
    };

    // the safe delays close above the smallest one
    std::vector<int> candidates;
    for (int d = first; d <= std::min(limit, first + window); ++d) {
        if (pool[d]) candidates.push_back(d);
    }

    // Best fix: look further ahead and keep only the delays that last longest
    // (surviving this jump just to die on the next one isn't a fix).
    std::vector<int> best = candidates;
    if (cfg.bestFix && candidates.size() > 1) {
        int deep = horizon + horizon / 2;
        std::vector<int> scores;
        int top = -1;
        for (int d : candidates) {
            auto r = ghost::run(layer, setup, d, down, deep);
            int score = r.cleared ? deep + 1 : r.hitStop ? deep : r.survived;
            scores.push_back(score);
            top = std::max(top, score);
        }
        best.clear();
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (scores[i] == top) best.push_back(candidates[i]);
        }
    }
    c.better = best.front() != first;

    // Randomize: any of the best delays from the smallest one up to `randomize` above it.
    int pick = best.front();
    if (cfg.randomize > 0) {
        std::vector<int> options;
        for (int d : best) {
            if (d <= best.front() + cfg.randomize) options.push_back(d);
        }
        if (options.size() > 1) pick = options[std::uniform_int_distribution<size_t>(0, options.size() - 1)(rng())];
    }
    c.random = pick != best.front();
    double chosen = pick;

    // Finer timing: when the tick before isn't safe, find the smallest safe delay
    // to a quarter of a tick, so your click moves only as much as it has to.
    if (cfg.finerTiming && !dual && !c.random) {
        bool beforeUnsafe = pick == 1 || (tested[pick - 1] && !pool[pick - 1]);
        if (beforeUnsafe) {
            double lo = pick - 1;
            double hi = pick;
            for (int i = 0; i < 2; ++i) {
                double mid = (lo + hi) / 2;
                if (passes(mid, ghost::run(layer, setup, mid, down, horizon))) hi = mid;
                else lo = mid;
            }
            chosen = hi;
        }
    }

    // Confidence: only help when a hair later and slightly different steps agree.
    // If they don't, try the next safe delays; if none holds up, leave the input alone.
    if (cfg.confidence) {
        // a quarter tick later has to work too, so the fix isn't sitting on a knife edge
        auto sure = [&](double d) { return passes(d, ghost::run(layer, setup, d + 0.25, down, horizon)); };
        std::vector<double> order { chosen };
        if (chosen != pick) order.push_back(pick);
        for (int d : best) {
            if (d > pick && order.size() < 4) order.push_back(d);
        }
        for (int d : candidates) {
            if (d > pick && order.size() < 4 && std::find(order.begin(), order.end(), static_cast<double>(d)) == order.end()) {
                order.push_back(d);
            }
        }
        bool found = false;
        for (double d : order) {
            if (d <= limit && sure(d)) {
                chosen = d;
                found = true;
                break;
            }
        }
        if (!found) return Choice { .base = base, .why = "not sure (only works on a knife edge)" };
        c.sure = true;
    }

    c.delay = chosen;
    int whole = std::clamp(static_cast<int>(std::ceil(chosen - 1e-6)), 1, MAX_TICKS);
    c.result = survived[whole];
    return c;
}

void remember(double ticks, int kind) {
    g_lastHelp = { true, g_step, ticks, kind };
}

void report(PlayLayer* layer, PlayerObject* p1, Choice const& c, bool down, int limit) {
    double ms = c.delay * TICK_MS;
    log::info("held {} back {} ({:.1f} ms): {} -> {} steps survived [limit {}]{}{}{}{}",
        down ? "press" : "release", tickText(c.delay), ms, c.base, c.result, limit,
        c.random ? ", random" : "", c.loose ? ", loose" : "", c.better ? ", best fix" : "", c.sure ? ", sure" : "");
    if (config().popup) {
        popup::show(fmt::format("{} {:.1f} ms early ({})", down ? "click" : "release", ms, tickText(c.delay)));
    }
    window::saved({ static_cast<int>(std::lround(c.delay)), ms, modeOf(p1), !down, c.random, percentOf(layer) });
    remember(c.delay, down ? 0 : 1);
}

void firePending(PlayLayer* layer) {
    feed(layer, g_pending.down);
    g_pending.active = false;
    if (g_held != g_pending.down) {
        // you changed the button during the wait: keep the tap as long as you made it
        int tap = std::max(1, g_pending.lastChange);
        g_flip = { true, g_held, tap };
    }
}

// Hold fix (wave, and ship when it's on): you're holding and holding on kills you
// within the delay limit, but letting go right now is safe. Let go for you; your
// own release then lands on an already released button.
bool holdFix(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2) {
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
    setup.dt = ghostDt(layer);
    setup.flying = true;
    setup.fresh = cfg.freshGhost;

    auto hold = ghost::run(layer, setup, 0, true, limit + 1);
    if (hold.hitStop || hold.survived >= limit) return false; // holding on is fine (or can't tell)
    bool mini = p1->m_vehicleSize < 1.f;
    int horizon = wave ? (mini ? 28 : 38) : 48;
    auto letGo = ghost::run(layer, setup, 0, false, horizon);
    if (letGo.hitStop || letGo.survived < horizon) return false; // letting go doesn't save you either

    feed(layer, false);
    g_autoReleased = true;
    g_autoSince = 0;
    g_autoLimit = limit;
    double ticks = hold.survived + 1;
    log::info("hold fix: let go for you ({} steps before holding on would hit)", hold.survived + 1);
    if (cfg.popup) popup::show(fmt::format("let go for you ({:.1f} ms early)", ticks * TICK_MS));
    window::saved({ static_cast<int>(ticks), ticks * TICK_MS, modeOf(p1), true, false, percentOf(layer) });
    remember(ticks, 2);
    return true;
}

} // namespace

bool feeding() {
    return g_feeding;
}

bool take(PlayLayer* layer, bool down, bool isPlayer1) {
    // second player of a two player level: never touched
    if (layer->m_level && layer->m_level->m_twoPlayerMode && !isPlayer1) return false;

    auto const& cfg = config();
    g_isP1 = isPlayer1;
    g_held = down;
    auto pass = [&] {
        g_fed = down;
        return false;
    };

    // Hold fix already let go for you: this is your own release arriving.
    if (g_autoReleased) {
        g_autoReleased = false;
        if (!down) {
            g_sinceInput = 0;
            return true; // GD has it released already
        }
    }

    auto p1 = layer->m_player1;
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
        g_sinceInput = 0;
        return true;
    }
    // The end of the last tap hasn't fired yet: fire it first, then handle this one.
    if (g_flip.active) {
        g_flip.active = false;
        if (g_flip.down != g_fed) feed(layer, g_flip.down);
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
    setup.dt = ghostDt(layer);
    setup.flying = flying;
    setup.fresh = cfg.freshGhost;

    auto now = ghost::run(layer, setup, 0, down, horizon);
    // fine as it is, or something ahead the ghost can't judge (an orb, a mode portal)
    if (now.hitStop || now.survived >= horizon) return pass();

    auto c = choose(layer, setup, down, wave, horizon, limit, now.survived);
    if (c.delay <= 0.0) {
        auto line = fmt::format("{}% {} would hit something: let through ({}, limit {})", percentOf(layer),
            down ? "click" : "release", c.why, limit);
        log::info("{}", line);
        window::note(line);
        return pass();
    }

    g_pending = {};
    g_pending.active = true;
    g_pending.down = down;
    g_pending.stepsLeft = static_cast<int>(std::floor(c.delay + 1e-6));
    g_pending.frac = static_cast<float>(c.delay - g_pending.stepsLeft);
    if (g_pending.frac < 0.01f) g_pending.frac = 0.f;
    report(layer, p1, c, down, limit);
    return true;
}

float beforeStep(PlayLayer* layer, float dt) {
    g_splitArmed = false;
    if (layer->m_attempts != g_attempt) {
        g_attempt = layer->m_attempts;
        window::attempt(g_attempt, false);
        g_accum = 0.f;
    }

    g_recent[g_recentAt] = dt;
    g_recentAt = (g_recentAt + 1) % 16;
    g_fullDt = *std::max_element(std::begin(g_recent), std::end(g_recent));
    g_step++;

    auto const& cfg = config();
    if (!cfg.enabled) return 0.f;
    if (g_sinceInput < 999) g_sinceInput++;

    bool waiting = g_pending.active || g_flip.active;
    // Steady timing: count full steps only, so Click Between Frames' split
    // steps don't make a wait a tick short.
    bool counts = true;
    if (cfg.steadyTiming && waiting && g_fullDt > 0.f) {
        g_accum += dt;
        if (g_accum + 1e-4f >= g_fullDt) g_accum -= g_fullDt;
        else counts = false;
    }
    else {
        g_accum = 0.f;
    }

    auto p1 = layer->m_player1;
    auto p2 = layer->m_gameState.m_isDualMode ? layer->m_player2 : nullptr;
    bool shipBlocked = !cfg.ship && p1 && (p1->m_isShip || (p2 && p2->m_isShip));
    if (shipBlocked) {
        cancel();
        if (g_held != g_fed) feed(layer, g_held);
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
            if (cfg.finerTiming && !p2 && dt >= 0.9f * g_fullDt) {
                g_splitArmed = true;
                return g_pending.frac;
            }
            // can't split this step: fire at the start of the next one (that whole tick was checked)
            g_pending.frac = 0.f;
            g_pending.elapsed++;
            return 0.f;
        }
        firePending(layer);
        return 0.f;
    }
    if (g_flip.active) {
        if (--g_flip.stepsLeft > 0) return 0.f;
        g_flip.active = false;
        if (g_flip.down != g_fed) feed(layer, g_flip.down);
        return 0.f;
    }

    if (g_autoReleased) {
        // still holding long after hold fix let go: you meant to hold, give it back
        if (g_held && ++g_autoSince > g_autoLimit) {
            g_autoReleased = false;
            feed(layer, true);
        }
        else if (!g_held) {
            g_autoReleased = false;
        }
        return 0.f;
    }
    if (p1 && holdFix(layer, p1, p2)) return 0.f;

    // keep GD in step with the button (after anything that skipped GD)
    if (g_held != g_fed) feed(layer, g_held);
    return 0.f;
}

void fireSplit(PlayLayer* layer) {
    if (!g_splitArmed) return;
    g_splitArmed = false;
    if (g_pending.active) firePending(layer);
}

void afterStep(PlayLayer* layer, float dt) {
    float warp = layer->m_gameState.m_timeWarp;
    g_dt = warp > 0.f ? dt / warp : dt;
    g_clock += g_dt;
}

double clock() {
    return g_clock;
}

void died(PlayLayer* layer) {
    auto const& cfg = config();
    if (!cfg.deathCheck || !g_lastHelp.active) return;
    long long since = g_step - g_lastHelp.step;
    g_lastHelp.active = false;
    if (since > 240) return; // more than a second ago: not related
    double seconds = since / 240.0;
    std::string what = g_lastHelp.kind == 2 ? "hold fix let go"
                     : fmt::format("a held-back {} ({})", g_lastHelp.kind == 1 ? "release" : "click", tickText(g_lastHelp.ticks));
    auto line = fmt::format("died {:.2f}s after {}", seconds, what);
    log::info("death check: {}", line);
    if (cfg.popup) popup::show(line, false);
    window::note(line);
}

void levelStarted(PlayLayer* layer) {
    g_layer = layer;
    ghost::create(layer);
    cancel();
    g_held = false;
    g_fed = false;
    g_isP1 = true;
    g_sinceInput = 999;
    g_burst = 0;
    g_accum = 0.f;
    g_lastHelp = {};
    g_attempt = layer->m_attempts;
    popup::attach(layer);
    window::attempt(g_attempt, true);
}

void levelEnded() {
    ghost::destroy();
    cancel();
    g_feeding = false;
    g_burst = 0;
    g_layer = nullptr;
}

void reset() {
    cancel();
    g_fed = false;
    g_burst = 0;
    g_accum = 0.f;
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
        feed(layer, g_pending.down);
    }
    g_flip = {};
    g_splitArmed = false;
    g_autoReleased = false;
    // don't leave the button stuck the wrong way
    if (g_held != g_fed) feed(layer, g_held);
}

} // namespace pb::buffer
