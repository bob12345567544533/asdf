#include "Config.hpp"

#include <Geode/Geode.hpp>

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
        default: return "?";
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
