#include "Config.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace pb {

namespace {
Config g_config;
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
    c.releases = flag("buffer-releases");
    c.ship = flag("buffer-ship");
    c.popup = flag("show-feedback");
    c.freshGhost = flag("fresh-ghost");
    c.stopAtPortals = flag("stop-at-portals");
    c.smartSafety = flag("smart-safety");
    c.steadyTiming = flag("steady-timing");
    g_config = c;
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
    return fmt::format("Buffer on: up to {} ticks ({} in wave)", next, std::min(next, WAVE_CAP));
}

} // namespace pb
