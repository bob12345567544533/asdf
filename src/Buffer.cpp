#include "Buffer.hpp"

#include "Config.hpp"
#include "Ghost.hpp"
#include "Popup.hpp"
#include "Window.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <random>

using namespace geode::prelude;

namespace pb::buffer {

namespace {

// An input waiting to be handed to GD.
struct Pending {
    bool active = false;
    bool down = true;
    int stepsLeft = 0;
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

Pending g_pending;
Flip g_flip;
bool g_feeding = false;
bool g_held = false;       // the button as you hold it
bool g_fed = false;        // the button as GD last got it
bool g_isP1 = true;
int g_sinceInput = 999;    // steps since your last input (capped)
int g_burst = 0;           // quick inputs in a row
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
}

int percentOf(PlayLayer* layer) {
    return static_cast<int>(std::clamp(layer->getCurrentPercent(), 0.f, 100.f));
}

int modeOf(PlayerObject* p) {
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isDart) return 4;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
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

struct Choice {
    int ticks = 0;     // 0 = nothing safe, let the input through
    int base = 0;      // steps survived with no delay
    int result = 0;    // steps survived with the chosen delay
    bool random = false;
    bool loose = false; // only passed the looser rule
};

// Finds the delay for an input that would kill you as it is.
Choice choose(PlayLayer* layer, ghost::Setup const& setup, bool down, bool wave, int horizon, int limit, int base) {
    auto const& cfg = config();
    bool smart = cfg.smartSafety && !setup.flying;

    // The original mod's rule: the delayed input has to live clearly longer
    // (wave gets stricter the longer the wait).
    auto loose = [&](int d, ghost::Result const& r) {
        if (r.hitStop) return r.survived > 0 && r.survived >= base;
        if (wave) {
            int need = base + 10 + 2 * (d - 1);
            int gain = 6 + 2 * (d - 1);
            return r.survived < horizon ? r.survived >= need : (r.survived - base >= gain || r.survived >= need);
        }
        return r.survived >= std::min(base + 12, horizon);
    };
    // Smart safety (cube-like modes): the jump has to clear everything ahead or land.
    auto strict = [&](int d, ghost::Result const& r) {
        if (!smart) return loose(d, r);
        if (r.hitStop) return r.survived > 0 && r.survived >= base;
        return r.survived >= horizon;
    };

    bool strictOk[MAX_TICKS + 1] {};
    bool looseOk[MAX_TICKS + 1] {};
    int survived[MAX_TICKS + 1] {};
    int firstStrict = 0;
    int firstLoose = 0;
    for (int d = 1; d <= limit; ++d) {
        auto r = ghost::run(layer, setup, d, down, horizon);
        if (r.diedBeforeInput) break; // waiting any longer only dies sooner
        survived[d] = r.survived;
        strictOk[d] = strict(d, r);
        looseOk[d] = loose(d, r);
        if (strictOk[d] && !firstStrict) firstStrict = d;
        if (looseOk[d] && !firstLoose) firstLoose = d;
        // with a safe delay found, only the randomize range above it still matters
        if (firstStrict && d >= firstStrict + cfg.randomize) break;
    }

    Choice c;
    c.base = base;
    bool* pool = firstStrict ? strictOk : looseOk;
    int first = firstStrict ? firstStrict : firstLoose;
    if (!first) return c;
    c.loose = !firstStrict;

    // Randomize: any safe delay from the smallest one up to `randomize` above it.
    int pick = first;
    if (cfg.randomize > 0) {
        int options[MAX_TICKS + 1];
        int count = 0;
        for (int d = first; d <= std::min(limit, first + cfg.randomize); ++d) {
            if (pool[d]) options[count++] = d;
        }
        if (count > 1) pick = options[std::uniform_int_distribution<int>(0, count - 1)(rng())];
    }
    c.ticks = pick;
    c.result = survived[pick];
    c.random = pick != first;
    return c;
}

void report(PlayLayer* layer, PlayerObject* p1, Choice const& c, bool down, int limit) {
    double ms = c.ticks * TICK_MS;
    log::info("held {} back +{} ticks ({:.1f} ms): {} -> {} steps survived [limit {}]{}{}",
        down ? "press" : "release", c.ticks, ms, c.base, c.result, limit,
        c.random ? ", random" : "", c.loose ? ", loose" : "");
    if (config().popup) {
        popup::show(fmt::format("{} {:.1f} ms early (+{} tick{})", down ? "click" : "release", ms, c.ticks,
            c.ticks == 1 ? "" : "s"));
    }
    window::saved({ c.ticks, ms, modeOf(p1), !down, c.random, percentOf(layer) });
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

    auto p1 = layer->m_player1;
    PlayerObject* p2 = layer->m_gameState.m_isDualMode ? layer->m_player2 : nullptr;
    if (!cfg.enabled || cfg.maxTicks <= 0 || !p1 || p1->m_isDead || layer->m_isPlatformer || !ghost::ready()) {
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
    int limit = cfg.limit(wave);

    ghost::Setup setup;
    setup.p1 = p1;
    setup.p2 = p2;
    setup.dt = ghostDt(layer);
    setup.flying = flying;
    setup.fresh = cfg.freshGhost;

    auto now = ghost::run(layer, setup, 0, down, horizon);
    // fine as it is, or a portal / pad / orb ahead the ghost can't judge
    if (now.hitStop || now.survived >= horizon || limit <= 0) return pass();

    auto c = choose(layer, setup, down, wave, horizon, limit, now.survived);
    if (c.ticks <= 0) return pass();

    g_pending = {};
    g_pending.active = true;
    g_pending.down = down;
    g_pending.stepsLeft = c.ticks;
    report(layer, p1, c, down, limit);
    return true;
}

void beforeStep(PlayLayer* layer, float dt) {
    if (layer->m_attempts != g_attempt) {
        g_attempt = layer->m_attempts;
        window::attempt(g_attempt, false);
        g_accum = 0.f;
    }

    g_recent[g_recentAt] = dt;
    g_recentAt = (g_recentAt + 1) % 16;
    g_fullDt = *std::max_element(std::begin(g_recent), std::end(g_recent));

    auto const& cfg = config();
    if (!cfg.enabled) return;
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
        return;
    }
    if (!counts) return;

    if (g_pending.active) {
        if (g_pending.stepsLeft > 0) {
            g_pending.stepsLeft--;
            g_pending.elapsed++;
            return;
        }
        feed(layer, g_pending.down);
        g_pending.active = false;
        if (g_held != g_pending.down) {
            // you changed the button during the wait: keep the tap as long as you made it
            int tap = std::max(1, g_pending.lastChange);
            g_flip = { true, g_held, tap };
        }
        return;
    }
    if (g_flip.active) {
        if (--g_flip.stepsLeft > 0) return;
        g_flip.active = false;
        if (g_flip.down != g_fed) feed(layer, g_flip.down);
        return;
    }
    // keep GD in step with the button (after anything that skipped GD)
    if (g_held != g_fed) feed(layer, g_held);
}

void afterStep(PlayLayer* layer, float dt) {
    float warp = layer->m_gameState.m_timeWarp;
    g_dt = warp > 0.f ? dt / warp : dt;
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
    // don't leave the button stuck the wrong way
    if (g_held != g_fed) feed(layer, g_held);
}

} // namespace pb::buffer
