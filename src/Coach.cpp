#include "Coach.hpp"

#include "Config.hpp"
#include "Ghost.hpp"
#include "Popup.hpp"
#include "Window.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <vector>

using namespace geode::prelude;

namespace pb::coach {

namespace {

constexpr int LOOK_BACK = 120;   // an input this many steps before the death can be what killed you (0.5 s)
constexpr int EARLIER_MAX = 16;  // "late" is looked for up to this many ticks
constexpr int LATER_MAX = 24;    // "early" up to this many
constexpr int NEW_MAX = 40;      // no input at all: how far back a click could have saved it
constexpr int MARGIN = 16;       // a replay has to live this many steps past the real death
constexpr int MAX_REPLAY = 420;  // steps: further back than this it isn't worth it
constexpr double LATE_WAIT = 0.2; // seconds after the death a click still counts as "the late click"
constexpr double SLICE_MS = 3.0; // work per frame
constexpr double RESET_MS = 8.0; // ... and at most this much right before a restart
constexpr int MAX_REPLAYS = 70;  // replays per death, at most

struct Rec {
    double time = 0.0;
    bool down = true;
    float moved = 0.f;
};
std::vector<Rec> g_inputs; // this attempt, oldest first

struct Miss {
    bool valid = false;
    long long step = 0;
    size_t index = 0; // the input it was, in g_inputs
    bool down = true;
    int needed = -1;
    int limit = 0;
};
Miss g_miss;

window::Coach g_stats;
std::map<int, int> g_deaths; // level % -> deaths there (this level)
PlayLayer* g_layer = nullptr;

enum class Stage { Gate, Shift, New, Wait, Done };

struct Job {
    bool active = false;
    Stage stage = Stage::Gate;
    Death death;
    ghost::Snapshot base;
    std::vector<ghost::Input> real; // the inputs after the snapshot, as they happened
    std::vector<float> moved;       // ... and how far the buffer had moved each
    int deathIdx = 0;               // the step of the replay you died in
    int suspect = -1;               // the input in `real` that most likely killed you
    bool pairRelease = false;       // its release moves along with it
    int candidate = 0;
    int replays = 0;
    // what came out
    bool found = false;
    bool fromNew = false; // no input of yours could be moved: a new one would have saved it
    bool late = false;
    bool release = false;
    int ticks = 0;
    // a click after the death
    double sinceDeath = 0.0;
    bool lateSeen[2] {}; // [0] a release, [1] a click
    double lateAt[2] {};
};
Job g_job;

bool holdMode(int mode) {
    return mode == WAVE || mode == SHIP || mode == ROBOT;
}

void publish() {
    int spot = -1;
    int count = 0;
    for (auto& [percent, n] : g_deaths) {
        if (n > count) {
            spot = percent;
            count = n;
        }
    }
    g_stats.spot = count >= 2 ? spot : -1;
    g_stats.spotCount = count >= 2 ? count : 0;
    window::coach(g_stats);
}

std::string tickWord(int ticks) {
    return fmt::format("{} tick{}", ticks, ticks == 1 ? "" : "s");
}

void say(std::string const& line, int tone) {
    log::info("coach: {}", line);
    window::note(line, tone);
    if (config().popup) popup::show(line, false);
}

ghost::Setup setupFor(Job const& j) {
    ghost::Setup s;
    s.p1 = j.base.player;
    s.dt = j.base.dt;
    s.heldBefore = j.base.held;
    s.fresh = true;
    s.stepsAgo = static_cast<double>(j.death.step - j.base.step);
    return s;
}

bool lives(PlayLayer* layer, Job const& j, std::vector<ghost::Input> const& inputs) {
    auto r = ghost::replay(layer, setupFor(j), inputs.data(), static_cast<int>(inputs.size()), j.deathIdx + MARGIN);
    // (something the ghost can't use a little after the spot you died at still counts as made it)
    return !r.died && (!r.hitStop || r.survived >= j.deathIdx + 4);
}

// The result is in: write the line.
void finish() {
    auto& j = g_job;
    j.active = false;
    j.stage = Stage::Done;
    if (!j.found) return;
    int percent = j.death.percent;
    double ms = j.ticks * TICK_MS;
    char const* what = j.release ? "release" : "click";
    if (j.fromNew) {
        int want = j.release ? 0 : 1;
        if (j.lateSeen[want]) {
            // the click that came after the death is the one that should have been earlier
            double total = ms + j.lateAt[want] * 1000.0;
            g_stats.late++;
            g_stats.lateMs += total;
            window::timing(total);
            say(fmt::format("{}% {} {:.0f} ms late", percent, what, total), window::Late);
        }
        else {
            say(fmt::format("{}% no {}: last chance was {:.0f} ms before", percent, what, ms), window::Late);
        }
    }
    else if (j.late) {
        g_stats.late++;
        g_stats.lateMs += ms;
        window::timing(ms);
        float moved = j.suspect >= 0 && j.suspect < static_cast<int>(j.moved.size()) ? j.moved[j.suspect] : 0.f;
        std::string tail = moved > 0.f ? fmt::format(", buffer had added +{:.1f}", moved) : "";
        say(fmt::format("{}% {} {:.0f} ms late ({}){}", percent, what, ms, tickWord(j.ticks), tail), window::Late);
    }
    else {
        g_stats.early++;
        g_stats.earlyMs += ms;
        window::timing(-ms);
        std::string tail = j.death.limit > 0 && j.ticks > j.death.limit ? fmt::format(", limit {}", j.death.limit) : "";
        say(fmt::format("{}% {} {:.0f} ms early ({}){}", percent, what, ms, tickWord(j.ticks), tail), window::Early);
    }
    publish();
}

// One replay.
void work(PlayLayer* layer) {
    auto& j = g_job;
    if (++j.replays > MAX_REPLAYS && j.stage != Stage::Wait) {
        j.stage = Stage::Done;
        return;
    }
    switch (j.stage) {
        case Stage::Gate: {
            // Play it again as it happened: only if the ghost dies where you died
            // can it say anything about what would have been different.
            auto r = ghost::replay(layer, setupFor(j), j.real.data(), static_cast<int>(j.real.size()), j.deathIdx + 3);
            if (!r.died || std::abs(r.deathStep - j.deathIdx) > 1) {
                log::info("coach: couldn't replay the death at {}% (died {} step {}, expected {}{})", j.death.percent,
                    r.died, r.deathStep, j.deathIdx, r.hitStop ? ", stopped" : "");
                j.stage = Stage::Done;
                j.active = false;
                return;
            }
            j.stage = j.suspect >= 0 ? Stage::Shift : Stage::New;
            j.candidate = 0;
            return;
        }
        case Stage::Shift: {
            // your input a little later (you were early) / earlier (you were late),
            // the smallest change first
            int k = j.candidate / 2 + 1;
            bool later = j.candidate % 2 == 0;
            j.candidate++;
            if (k > std::max(EARLIER_MAX, LATER_MAX)) {
                j.stage = Stage::New;
                j.candidate = 0;
                return;
            }
            auto const& in = j.real[j.suspect];
            double at = in.at + (later ? k : -k);
            int last = static_cast<int>(j.real.size()) - 1;
            bool paired = j.pairRelease && j.suspect < last;
            if (later) {
                if (k > LATER_MAX || at > j.deathIdx - 1) return;
                // not past your next input (its own release moves along)
                int nextIdx = j.suspect + (paired ? 2 : 1);
                double end = paired ? j.real[j.suspect + 1].at + k : at;
                if (nextIdx <= last && end >= j.real[nextIdx].at) return;
            }
            else {
                if (k > EARLIER_MAX || at < 0.0) return;
                if (j.suspect > 0 && at <= j.real[j.suspect - 1].at) return;
            }
            auto inputs = j.real;
            inputs[j.suspect].at = at;
            if (paired) inputs[j.suspect + 1].at += later ? k : -k;
            if (lives(layer, j, inputs)) {
                j.found = true;
                j.late = !later;
                j.release = !in.down;
                j.ticks = k;
                j.stage = Stage::Done;
            }
            return;
        }
        case Stage::New: {
            // nothing of yours could be moved: when was the last moment a new input saved it?
            int k = ++j.candidate;
            double at = j.deathIdx - k;
            if (k > NEW_MAX || at < 0.0 || (!j.real.empty() && at <= j.real.back().at)) {
                j.stage = Stage::Done;
                return;
            }
            // a click, or (where holding matters and you were holding) letting go
            bool down = holdMode(j.death.mode) ? !j.death.held : true;
            auto inputs = j.real;
            inputs.push_back({ at, down });
            // a tap, not a hold, where holding on would jump again
            if (down && !holdMode(j.death.mode)) inputs.push_back({ at + std::max(j.death.tap, 6), false });
            if (lives(layer, j, inputs)) {
                j.found = true;
                j.fromNew = true;
                j.late = true;
                j.release = !down;
                j.ticks = k;
                j.stage = Stage::Wait;
            }
            return;
        }
        default:
            return;
    }
}

} // namespace

void levelStarted(PlayLayer* layer) {
    g_layer = layer;
    g_inputs.clear();
    g_miss = {};
    g_job = {};
    g_stats = {};
    g_deaths.clear();
    publish();
}

void levelEnded() {
    g_layer = nullptr;
    g_inputs.clear();
    g_miss = {};
    g_job = {};
}

// Replays until the work is done or `ms` are used up.
static void workFor(PlayLayer* layer, double ms) {
    auto& j = g_job;
    auto start = std::chrono::steady_clock::now();
    while (j.active && j.stage != Stage::Wait && j.stage != Stage::Done) {
        work(layer);
        std::chrono::duration<double, std::milli> spent = std::chrono::steady_clock::now() - start;
        if (spent.count() > ms) break;
    }
}

void beforeReset(PlayLayer* layer, bool safe) {
    auto& j = g_job;
    if (!j.active || !safe || !layer || layer != g_layer || !ghost::ready()) return;
    workFor(layer, RESET_MS);
}

void reset() {
    if (g_job.active) {
        // still waiting to see how late your click comes: that can go on after the restart
        if (g_job.stage == Stage::Wait) {}
        else if (g_job.found || g_job.stage == Stage::Done) finish();
        else g_job = {};
    }
    g_inputs.clear();
    g_miss = {};
    ghost::clearSnapshots();
}

void logInput(double time, bool down, float moved) {
    if (g_inputs.size() > 4096) {
        // a very long attempt: start over (snapshots count inputs from the start)
        g_inputs.clear();
        g_miss = {};
        ghost::clearSnapshots();
    }
    g_inputs.push_back({ time, down, moved });
}

size_t inputCount() {
    return g_inputs.size();
}

void missed(long long step, bool down, int needed, int limit) {
    g_miss = { true, step, g_inputs.size(), down, needed, limit };
}

void called(bool right) {
    g_stats.calls++;
    if (right) g_stats.callHits++;
    publish();
}

void died(PlayLayer* layer, Death const& death) {
    g_deaths[death.percent]++;
    if (!config().coach || g_job.active) {
        publish();
        return;
    }

    // The buffer already said this input was early and by how much.
    if (g_miss.valid && g_miss.needed > 0 && death.step - g_miss.step <= LOOK_BACK) {
        bool pressedSince = false;
        for (size_t i = g_miss.index + 1; i < g_inputs.size(); ++i) pressedSince = pressedSince || g_inputs[i].down;
        if (!pressedSince) {
            g_stats.early++;
            g_stats.earlyMs += g_miss.needed * TICK_MS;
            window::timing(-g_miss.needed * TICK_MS);
            g_miss = {};
            publish();
            return;
        }
    }
    g_miss = {};
    publish();

    if (death.dual || !ghost::snapshotsReady() || layer->m_isPlatformer) return;
    if (layer->m_level && layer->m_level->m_twoPlayerMode) return;

    // the input that most likely did it: your last one (the last click where letting go doesn't matter)
    int n = static_cast<int>(g_inputs.size());
    bool hold = holdMode(death.mode);
    int suspect = -1;
    for (int i = n - 1; i >= 0; --i) {
        if (death.step - g_inputs[i].time > LOOK_BACK) break;
        if (hold || g_inputs[i].down) {
            suspect = i;
            break;
        }
    }
    long long want = suspect >= 0 ? static_cast<long long>(std::floor(g_inputs[suspect].time)) - EARLIER_MAX - 1
                                  : death.step - NEW_MAX - 1;
    auto snap = ghost::snapshotBefore(want);
    // early in an attempt: the first snapshot there is (smaller changes can still be tried)
    if (!snap) snap = ghost::snapshotBefore(suspect >= 0 ? static_cast<long long>(std::floor(g_inputs[suspect].time)) : death.step - 2);
    if (!snap || death.step - snap->step > MAX_REPLAY || snap->inputs > g_inputs.size()) return;

    Job j;
    j.active = true;
    j.death = death;
    j.base = *snap;
    j.deathIdx = static_cast<int>(death.step - snap->step);
    for (size_t i = snap->inputs; i < g_inputs.size(); ++i) {
        j.real.push_back({ std::max(0.0, g_inputs[i].time - static_cast<double>(snap->step)), g_inputs[i].down });
        j.moved.push_back(g_inputs[i].moved);
    }
    if (suspect >= static_cast<int>(snap->inputs)) {
        j.suspect = suspect - static_cast<int>(snap->inputs);
        auto const& in = j.real[j.suspect];
        j.pairRelease = !hold && in.down && j.suspect + 1 < static_cast<int>(j.real.size()) && !j.real[j.suspect + 1].down;
    }
    g_job = std::move(j);
}

void lateInput(bool down) {
    auto& j = g_job;
    int i = down ? 1 : 0;
    if (!j.active || j.lateSeen[i]) return;
    j.lateSeen[i] = true;
    j.lateAt[i] = j.sinceDeath;
}

void tick(PlayLayer* layer, float dt) {
    auto& j = g_job;
    if (!j.active) return;
    if (!config().coach) {
        g_job = {};
        return;
    }
    if (!layer || layer != g_layer || !ghost::ready()) {
        g_job = {};
        return;
    }
    j.sinceDeath += std::max(0.f, dt);

    // a click that's waiting in the game's input queue came after the death
    for (auto const& command : layer->m_queuedButtons) {
        if (command.m_button == PlayerButton::Jump) lateInput(command.m_isPush);
    }

    workFor(layer, SLICE_MS);
    if (!j.active) return;
    if (j.stage == Stage::Done) return finish();
    if (j.stage == Stage::Wait && (j.lateSeen[j.release ? 0 : 1] || j.sinceDeath > LATE_WAIT)) finish();
}

bool busy() {
    return g_job.active;
}

} // namespace pb::coach
