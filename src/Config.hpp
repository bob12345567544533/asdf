#pragma once

#include <algorithm>
#include <string>

namespace pb {

constexpr int MAX_TICKS = 24;            // largest value the settings allow
constexpr int WAVE_CAP = 8;              // wave never waits longer than this
constexpr double TICK_MS = 1000.0 / 240.0; // one physics tick at 240 TPS

struct Config {
    bool enabled = true;
    int maxTicks = 2;      // largest delay the buffer may add
    int randomize = 3;     // pick a random safe delay up to this many ticks above the smallest safe one
    bool releases = true;  // also buffer early releases (wave, robot, ship)
    bool ship = false;     // work in ship mode too
    bool popup = true;     // in-game timing popup
    // checks (each can be switched off)
    bool freshGhost = true;
    bool stopAtPortals = true;
    bool smartSafety = true;
    bool steadyTiming = true;

    int limit(bool wave) const { return wave ? std::min(maxTicks, WAVE_CAP) : maxTicks; }
};

// Current settings (main thread).
Config const& config();

// Re-reads every setting.
void loadConfig();

// Toggle key: cycles 2 -> 4 -> 8 -> 16 ticks -> off and saves it to the
// settings. Returns a short line describing the new stage.
std::string cycleStage();

} // namespace pb
