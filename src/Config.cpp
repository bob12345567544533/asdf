#include "Config.hpp"

#include <Geode/Geode.hpp>

#include <chrono>

using namespace geode::prelude;

namespace pb {

namespace {
Config g_config;
bool g_off[static_cast<int>(Feature::Count)] {};

void applyOverrides(Config& c) {
    if (g_off[static_cast<int>(Feature::Finer)]) c.finerTiming = false;
    if (g_off[static_cast<int>(Feature::Remainder)]) c.remainderModel = false;
    if (g_off[static_cast<int>(Feature::Moving)]) c.movingObjects = false;
    if (g_off[static_cast<int>(Feature::Pads)]) c.throughPads = false;
    if (g_off[static_cast<int>(Feature::Orbs)]) c.throughOrbs = false;
    if (g_off[static_cast<int>(Feature::Coach)]) c.coach = false;
}

constexpr char const* MODE_KEYS[MODES] { "max-cube", "max-ship", "max-ball", "max-ufo", "max-wave", "max-robot",
                                         "max-spider", "max-swing" };
}

Config const& config() {
    return g_config;
}

void loadConfig() {
    auto mod = Mod::get();
    auto num = [&](char const* key) { return static_cast<int>(mod->getSettingValue<int64_t>(key)); };
    auto flag = [&](char const* key) { return mod->getSettingValue<bool>(key); };
    Config c;
    c.enabled = flag("enabled");
    c.maxTicks = std::clamp(num("buffer-ticks"), 0, MAX_TICKS);
    c.randomize = std::clamp(num("randomize"), 0, MAX_TICKS);
    for (int i = 0; i < MODES; ++i) c.modeMax[i] = std::clamp(num(MODE_KEYS[i]), 0, MAX_TICKS);
    c.releases = flag("buffer-releases");
    c.ship = flag("buffer-ship");
    c.popup = flag("show-feedback");
    c.practiceOnly = flag("practice-only");
    c.deathCheck = flag("death-check");
    c.coach = flag("coach");
    c.finerTiming = flag("finer-timing-v3");
    c.bestFix = flag("best-fix");
    c.confidence = flag("confidence-check");
    c.movingObjects = flag("moving-objects");
    c.holdFix = flag("hold-fix");
    c.throughPads = flag("through-pads");
    c.throughOrbs = flag("through-orbs");
    c.tapModel = flag("tap-model");
    c.selfCheck = flag("self-check");
    c.freshGhost = flag("fresh-ghost");
    c.stopAtPortals = flag("stop-at-portals");
    c.smartSafety = flag("smart-safety");
    c.steadyTiming = flag("steady-timing");
    applyOverrides(c);
    g_config = c;
}

void disableForSession(Feature feature) {
    g_off[static_cast<int>(feature)] = true;
    applyOverrides(g_config);
}

char const* featureName(Feature feature) {
    switch (feature) {
        case Feature::Finer: return "Finer Timing";
        case Feature::Remainder: return "mid-step clicks (Click Between Frames)";
        case Feature::Moving: return "See Moving Objects";
        case Feature::Pads: return "Use Pads And Portals";
        case Feature::Orbs: return "Use Orbs";
        case Feature::Coach: return "Coach";
        default: return "?";
    }
}

namespace perf {
namespace {
long long now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int g_depth = 0;
long long g_spent = 0;  // this frame, microseconds
double g_avg = 0.0;     // per frame, smoothed (ms)
int g_frames = 0;
}

Timer::Timer() : start(g_depth++ == 0 ? now() : -1) {}
Timer::~Timer() {
    --g_depth;
    if (start >= 0) g_spent += now() - start;
}

double average() { return g_avg; }

std::string frame() {
    double ms = g_spent / 1000.0;
    g_spent = 0;
    g_avg = g_avg * 0.95 + ms * 0.05;
    if (g_frames < 1000) g_frames++;
    // over ~5 ms a frame for a while: the coach's replays are the heavy part, stop them
    if (g_frames > 120 && g_avg > 5.0 && g_config.coach) {
        auto line = fmt::format("game was slowing down ({:.1f} ms a frame): Coach off for this session", g_avg);
        disableForSession(Feature::Coach);
        g_avg = 0.0;
        g_frames = 0;
        log::warn("{}", line);
        return line;
    }
    return {};
}
}

std::string cycleStage() {
    auto mod = Mod::get();
    int ticks = g_config.maxTicks;
    bool enabled = g_config.enabled && ticks > 0;
    int next;
    if (!enabled) next = 2;
    else if (ticks < 4) next = 4;
    else if (ticks < 8) next = 8;
    else if (ticks < 16) next = 16;
    else next = 0; // off
    if (next == 0) {
        mod->setSettingValue<bool>("enabled", false);
        return "Buffer off";
    }
    mod->setSettingValue<int64_t>("buffer-ticks", next);
    mod->setSettingValue<bool>("enabled", true);
    return fmt::format("Buffer on: up to {} ticks ({} in wave)", next, std::min(next, g_config.modeMax[WAVE]));
}

} // namespace pb
